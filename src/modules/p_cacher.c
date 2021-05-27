/*

Read Route Record

Copyright (C) 2021 Atle Solbakken atle@goliathdns.no

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <inttypes.h>

#include "../lib/log.h"
#include "../lib/allocator.h"

#include "../lib/poll_helper.h"
#include "../lib/instance_config.h"
#include "../lib/instances.h"
#include "../lib/threads.h"
#include "../lib/message_broker.h"
#include "../lib/event/event.h"
#include "../lib/messages/msg_msg.h"
#include "../lib/message_holder/message_holder.h"
#include "../lib/message_holder/message_holder_struct.h"
#include "../lib/msgdb/msgdb_client.h"

struct cacher_data {
	struct rrr_instance_runtime_data *thread_data;

	struct rrr_msgdb_client_conn msgdb_conn;

	char *msgdb_socket;
	char *request_tag;

	rrr_setting_uint message_ttl_seconds;
	uint64_t message_ttl_us;

	int do_forward_requests;
	int do_forward_data;
	int do_forward_other;
	int do_no_update;
};

static void cacher_data_init(struct cacher_data *data, struct rrr_instance_runtime_data *thread_data) {
	memset(data, '\0', sizeof(*data));
	data->thread_data = thread_data;
}

static void cacher_data_cleanup(void *arg) {
	struct cacher_data *data = arg;
	rrr_msgdb_client_close(&data->msgdb_conn);
	RRR_FREE_IF_NOT_NULL(data->msgdb_socket);
	RRR_FREE_IF_NOT_NULL(data->request_tag);
}

struct cacher_get_from_msgdb_callback_data {
	struct cacher_data *data;
	const char *tag;
};

struct cacher_get_from_msgdb_broker_callback_data {
	struct rrr_msg_msg **msg_ptr;
};

static int cacher_get_from_msgdb_broker_callback (struct rrr_msg_holder *new_entry, void *arg) {
	struct cacher_get_from_msgdb_broker_callback_data *callback_data = arg;

	rrr_msg_holder_set_data_unlocked(new_entry, *callback_data->msg_ptr, MSG_TOTAL_SIZE(*callback_data->msg_ptr));
	*callback_data->msg_ptr = NULL;

	rrr_msg_holder_unlock(new_entry);
	return 0;
}

static int cacher_get_from_msgdb_callback (
		struct rrr_msgdb_client_conn *conn,
		void *arg
) {
	struct cacher_get_from_msgdb_callback_data *callback_data = arg;

	int ret = 0;

	struct rrr_msg_msg *msg_tmp = NULL;

	if ((ret = rrr_msgdb_client_cmd_get(&msg_tmp, conn, callback_data->tag))) {
		goto out;
	}

	if (msg_tmp != NULL) {
		struct cacher_get_from_msgdb_broker_callback_data broker_callback_data = {
			&msg_tmp
		};

		if ((ret = rrr_message_broker_write_entry (
				INSTANCE_D_BROKER_ARGS(callback_data->data->thread_data),
				NULL,
				0,
				0,
				cacher_get_from_msgdb_broker_callback,
				&broker_callback_data,
				INSTANCE_D_CANCEL_CHECK_ARGS(callback_data->data->thread_data)
		)) != 0) {
			goto out;
		}
	}

	out:
	RRR_FREE_IF_NOT_NULL(msg_tmp);
	return ret;
}

static int cacher_get_from_msgdb (
		struct cacher_data *data,
		const char *tag
) {
	int ret = 0;

	struct cacher_get_from_msgdb_callback_data callback_data = {
		data,
		tag
	};

	if ((ret = rrr_msgdb_client_conn_ensure_with_callback (
			&data->msgdb_conn,
			data->msgdb_socket,
			INSTANCE_D_EVENTS(data->thread_data),
			cacher_get_from_msgdb_callback,
			&callback_data
	)) != 0) {
		RRR_MSG_0("Failed to get message from  message DB in cacher_get_id_from_msgdb\n");
		goto out;
	}

	out:
	return ret;
}

static int cacher_send_to_msgdb_callback_final (
		struct rrr_msgdb_client_conn *conn,
		void *arg
) {
	struct rrr_msg_msg *msg = arg;

	int ret = 0;

	MSG_SET_TYPE(msg, MSG_TYPE_PUT);

	if ((ret = rrr_msgdb_client_send(conn, msg)) != 0) {	
		RRR_DBG_7("Failed to send message to msgdb in cacher_send_to_msgdb_callback, return from send was %i\n",
			ret);
		goto out;
	}

	int positive_ack = 0;
	if ((ret = rrr_msgdb_client_await_ack(&positive_ack, conn)) != 0 || positive_ack == 0) {
		RRR_DBG_7("Failed to send message to msgdb in cacher_send_to_msgdb_callback, return from await ack was %i positive ack was %i\n",
			ret, positive_ack);
		ret = 1; // Ensure failure is returned upon negative ACK
		goto out;
	}

	out:
	return ret;
}

static int cacher_send_to_msgdb (
	struct cacher_data *data,
	struct rrr_msg_msg *msg
) {
	int ret = 0;

	char *topic_tmp = NULL;

	if (data->msgdb_socket == NULL) {
		goto out;
	}

	if ((ret = rrr_msg_msg_topic_get(&topic_tmp, msg)) != 0) {
		RRR_MSG_0("Failed to get topic from message in cacher_send_to_msgdb_callback\n");
		goto out;
	}

	if ((ret = rrr_msgdb_client_conn_ensure_with_callback (
			&data->msgdb_conn,
			data->msgdb_socket,
			INSTANCE_D_EVENTS(data->thread_data),
			cacher_send_to_msgdb_callback_final,
			msg
	)) != 0) {
		RRR_MSG_0("Failed to send message to message DB in cacher_send_to_msgdb_callback\n");
		goto out;
	}

	out:
	RRR_FREE_IF_NOT_NULL(topic_tmp);
	return ret;
}

static int cacher_process (
		int *do_forward,
		struct cacher_data *data,
		struct rrr_msg_holder *entry
) {
	int ret = 0;

	struct rrr_msg_msg *msg = entry->message;
	char *topic_tmp = NULL;

	if (data->message_ttl_us > 0 && !rrr_msg_msg_ttl_ok(msg, data->message_ttl_us)) {
		RRR_MSG_0("Warning: Received message in cacher instance %s with expired TTL, limit is set to %" PRIrrrbl " seconds. Dropping message.\n",
				INSTANCE_D_NAME(data->thread_data), data->message_ttl_seconds);
		goto out;
	}

	if (MSG_TOPIC_LENGTH(msg) == 0) {
		if (data->do_forward_other) {
			RRR_DBG_2("cacher instance %s forwarding other message with timestamp %" PRIu64 " without topic\n",
					INSTANCE_D_NAME(data->thread_data),
					msg->timestamp
			);
			*do_forward = 1;
		}
		else {
			RRR_MSG_0("Warning: Received a message in cacher instance %s without a topic, dropping it per configuration\n",
				INSTANCE_D_NAME(data->thread_data));
		}
		goto out;
	}

	if ((ret = rrr_msg_msg_topic_get(&topic_tmp, msg)) != 0) {
		RRR_MSG_0("Failed to get topic from message in cacher_process\n");
		goto out;
	}

	if (data->request_tag != NULL && rrr_array_message_has_tag(msg, data->request_tag)) {
		RRR_DBG_2("cacher instance %s request message with timestamp %" PRIu64 " with topic '%s'%s\n",
				INSTANCE_D_NAME(data->thread_data),
				msg->timestamp,
				topic_tmp,
				data->do_forward_requests ? " (and forwarding)" : ""
		);

		ret = cacher_get_from_msgdb(data, topic_tmp);

		if (data->do_forward_requests) {
			*do_forward = 1;
		}

		goto out;
	}

	if (data->do_no_update) {
		if (data->do_forward_other) {
			RRR_DBG_2("cacher instance %s forwarding other message with timestamp %" PRIu64 " (updates are disabled)\n",
					INSTANCE_D_NAME(data->thread_data),
					msg->timestamp
			);
			*do_forward = 1;
		}
		else {
			RRR_MSG_0("Warning: Received a message in cacher instance %s which will be dropped without processing (updates and forwarding is disabled and message is not a reqest)\n",
				INSTANCE_D_NAME(data->thread_data));
		}
		goto out;
	}

	RRR_DBG_2("cacher instance %s storing data message with timestamp %" PRIu64 " with topic '%s'%s\n",
			INSTANCE_D_NAME(data->thread_data),
			msg->timestamp,
			topic_tmp,
			data->do_forward_data ? " (and forwarding)" : ""
	);

	if ((ret = cacher_send_to_msgdb(data, msg)) != 0) {
		goto out;
	}

	if (data->do_forward_data) {
		*do_forward = 1;
	}

	out:
	RRR_FREE_IF_NOT_NULL(topic_tmp);
	return ret;
}

static int cacher_poll_callback (RRR_MODULE_POLL_CALLBACK_SIGNATURE) {
	struct rrr_instance_runtime_data *thread_data = arg;
	struct cacher_data *data = thread_data->private_data;

	int ret = 0;

	// We check stuff with the watchdog in case we are slow to process messages
	if (rrr_thread_signal_encourage_stop_check(INSTANCE_D_THREAD(data->thread_data))) {
		ret = RRR_FIFO_SEARCH_STOP;
		goto out;
	}
	rrr_thread_watchdog_time_update(INSTANCE_D_THREAD(data->thread_data));

	// Do not produce errors for message process failures, just drop them

	int do_forward = 0;
	if (cacher_process(&do_forward, data, entry) != 0) {
		RRR_MSG_0("Warning: Failed to process message in cacher instance %s\n",
			INSTANCE_D_NAME(data->thread_data));
		goto out;
	}

	if (do_forward && (ret = rrr_message_broker_incref_and_write_entry_unsafe_no_unlock (
			INSTANCE_D_BROKER_ARGS(data->thread_data), 
			entry,
			INSTANCE_D_CANCEL_CHECK_ARGS(data->thread_data)
	)) != 0) {
		RRR_MSG_0("Failed to write entry in cacher_poll_callback of instance %s\n",
			INSTANCE_D_NAME(data->thread_data));
		goto out;
	}

	out:
		rrr_msg_holder_unlock(entry);
		return ret;
}

static int cacher_event_broker_data_available (RRR_EVENT_FUNCTION_ARGS) {
	struct rrr_thread *thread = arg;
	struct rrr_instance_runtime_data *thread_data = thread->private_data;

	return rrr_poll_do_poll_delete (amount, thread_data, cacher_poll_callback, 0);
}

static int cacher_event_periodic (void *arg) {
	struct rrr_thread *thread = arg;
	struct rrr_instance_runtime_data *thread_data = thread->private_data;
	struct cacher_data *data = thread_data->private_data;

	if (data->message_ttl_seconds != 0) {
		RRR_MSG_0("TODO: Implement TTL check in msgdb\n");
	}

	return rrr_thread_signal_encourage_stop_check_and_update_watchdog_timer_void(thread);
}

static int cacher_parse_config (struct cacher_data *data, struct rrr_instance_config_data *config) {
	int ret = 0;

	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_UTF8_DEFAULT_NULL("cacher_msgdb_socket", msgdb_socket);
	if (data->msgdb_socket == NULL || *(data->msgdb_socket) == '\0') {
		RRR_MSG_0("Required aramenter cacher_msgdb_socket missing in cacher instance %s\n");
		ret = 1;
		goto out;
	}

	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_UTF8_DEFAULT_NULL("cacher_request_tag", request_tag);
	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_YESNO("cacher_forward_requests", do_forward_requests, 0);
	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_YESNO("cacher_forward_data", do_forward_data, 0);
	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_YESNO("cacher_forward_other", do_forward_other, 0);
	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_YESNO("cacher_no_update", do_no_update, 0);

	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_UNSIGNED("cacher_ttl_seconds", message_ttl_seconds, 0);
	data->message_ttl_us = data->message_ttl_seconds * 1000 * 1000;

	out:
	return ret;
}

static void *thread_entry_cacher (struct rrr_thread *thread) {
	struct rrr_instance_runtime_data *thread_data = thread->private_data;
	struct cacher_data *data = thread_data->private_data = thread_data->private_memory;

	RRR_DBG_1 ("cacher thread thread_data is %p\n", thread_data);

	cacher_data_init(data, thread_data);

	pthread_cleanup_push(cacher_data_cleanup, data);

	rrr_thread_start_condition_helper_nofork(thread);

	if (cacher_parse_config(data, INSTANCE_D_CONFIG(thread_data)) != 0) {
		goto out_message;
	}

	rrr_instance_config_check_all_settings_used(thread_data->init_data.instance_config);

	RRR_DBG_1 ("cacher instance %s started thread\n",
			INSTANCE_D_NAME(thread_data));

	rrr_event_dispatch (
			INSTANCE_D_EVENTS(thread_data),
			1 * 1000 * 1000, // 1 s
			cacher_event_periodic,
			thread
	);

	out_message:
	pthread_cleanup_pop(1);

	RRR_DBG_1 ("Thread cacher %p exiting\n", thread);
	pthread_exit(0);
}

static struct rrr_module_operations module_operations = {
		NULL,
		thread_entry_cacher,
		NULL,
		NULL,
		NULL
};

static const char *module_name = "cacher";

__attribute__((constructor)) void load(void) {
}

static struct rrr_instance_event_functions event_functions = {
	cacher_event_broker_data_available
};

void init(struct rrr_instance_module_data *data) {
	data->private_data = NULL;
	data->module_name = module_name;
	data->type = RRR_MODULE_TYPE_PROCESSOR;
	data->operations = module_operations;
	data->event_functions = event_functions;
}

void unload(void) {
	RRR_DBG_1 ("Destroy cacher module\n");
}

