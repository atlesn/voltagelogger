/*

Read Route Record

Copyright (C) 2020-2021 Atle Solbakken atle@goliathdns.no

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
#include "../lib/poll_helper.h"
#include "../lib/instance_config.h"
#include "../lib/instances.h"
#include "../lib/threads.h"
#include "../lib/message_broker.h"
#include "../lib/array.h"
#include "../lib/string_builder.h"
#include "../lib/http/http_client.h"
#include "../lib/http/http_client_config.h"
#include "../lib/http/http_query_builder.h"
#include "../lib/http/http_session.h"
#include "../lib/http/http_transaction.h"
#include "../lib/http/http_util.h"
#include "../lib/net_transport/net_transport_config.h"
#include "../lib/net_transport/net_transport.h"
#include "../lib/messages/msg_msg.h"
#include "../lib/message_holder/message_holder.h"
#include "../lib/message_holder/message_holder_struct.h"
#include "../lib/message_holder/message_holder_collection.h"
#include "../lib/helpers/nullsafe_str.h"
#include "../lib/json/json.h"
#include "../lib/msgdb/msgdb_client.h"

#define RRR_HTTPCLIENT_DEFAULT_SERVER                    "localhost"
#define RRR_HTTPCLIENT_DEFAULT_PORT                      0 // 0=automatic
#define RRR_HTTPCLIENT_DEFAULT_REDIRECTS_MAX             5
#define RRR_HTTPCLIENT_LIMIT_REDIRECTS_MAX               500
#define RRR_HTTPCLIENT_READ_MAX_SIZE                     1 * 1024 * 1024 * 1024 // 1 GB
#define RRR_HTTPCLIENT_DEFAULT_KEEPALIVE_MAX_S           5
#define RRR_HTTPCLIENT_JSON_MAX_LEVELS                   4
#define RRR_HTTPCLIENT_DEFAULT_MSGDB_RETRY_INTERVAL_S    30

struct httpclient_data {
	struct rrr_instance_runtime_data *thread_data;
	struct rrr_msg_holder_collection defer_queue;
	struct rrr_msg_holder_collection msgdb_queue;

	struct rrr_msgdb_client_conn msgdb_conn;

	int do_no_data;
	int do_rrr_msg_to_array;
	int do_drop_on_error;
	int do_receive_part_data;
	int do_receive_json_data;

	int do_endpoint_from_topic;
	int do_endpoint_from_topic_force;

	char *endpoint_tag;
	int do_endpoint_tag_force;

	char *server_tag;
	int do_server_tag_force;

	char *port_tag;
	int do_port_tag_force;

	rrr_setting_uint message_timeout_us;
	rrr_setting_uint message_ttl_us;

	rrr_setting_uint redirects_max;
	rrr_setting_uint keepalive_s_max;

	char *msgdb_socket;
	rrr_setting_uint msgdb_poll_interval_us;

	struct rrr_net_transport_config net_transport_config;

	struct rrr_net_transport *keepalive_transport_plain;
	struct rrr_net_transport *keepalive_transport_tls;

	struct rrr_http_client_request_data request_data;

	// Array fields, server name etc.
	struct rrr_http_client_config http_client_config;
};

static void httpclient_data_cleanup(void *arg) {
	struct httpclient_data *data = arg;

	if (data->keepalive_transport_plain != NULL) {
		rrr_net_transport_destroy(data->keepalive_transport_plain);
	}
	if (data->keepalive_transport_tls != NULL) {
		rrr_net_transport_destroy(data->keepalive_transport_tls);
	}
	rrr_msgdb_client_close(&data->msgdb_conn);
	rrr_http_client_request_data_cleanup(&data->request_data);
	rrr_net_transport_config_cleanup(&data->net_transport_config);
	rrr_http_client_config_cleanup(&data->http_client_config);
	rrr_msg_holder_collection_clear(&data->defer_queue);
	rrr_msg_holder_collection_clear(&data->msgdb_queue);
	RRR_FREE_IF_NOT_NULL(data->endpoint_tag);
	RRR_FREE_IF_NOT_NULL(data->server_tag);
	RRR_FREE_IF_NOT_NULL(data->port_tag);
	RRR_FREE_IF_NOT_NULL(data->msgdb_socket);
}

struct httpclient_transaction_data {
	char *msg_topic;
	struct rrr_msg_holder *entry;
};

static void httpclient_dbl_ptr_free_if_not_null (
		void *arg
) {
	void *ptr = *((void **) arg);
	RRR_FREE_IF_NOT_NULL(ptr);
}

static int httpclient_transaction_data_new (
		struct httpclient_transaction_data **target,
		const char *topic,
		size_t topic_len,
		struct rrr_msg_holder *entry
) {
	int ret = 0;

	*target = NULL;

	struct httpclient_transaction_data *result = malloc(sizeof(*result));
	if (result == NULL) {
		RRR_MSG_0("Could not allocate memory in rrr_httpclient_transaction_data_new\n");
		ret = 1;
		goto out;
	}

	memset(result, '\0', sizeof(*result));

	if ((result->msg_topic = malloc(topic_len + 1)) == NULL) {
		RRR_MSG_0("Could not allocate memory for topic in rrr_httpclient_transaction_data_new\n");
		ret = 1;
		goto out_free;
	}

	if (topic != NULL && topic_len != 0) {
		memcpy(result->msg_topic, topic, topic_len);
	}
	result->msg_topic[topic_len] = '\0';
	result->entry = entry;

	*target = result;

	goto out;
	out_free:
		free(result);
	out:
		return ret;
}

static void httpclient_transaction_destroy (struct httpclient_transaction_data *target) {
	RRR_FREE_IF_NOT_NULL(target->msg_topic);

	// Assuming that entry has recursive lock
	rrr_msg_holder_decref(target->entry);

	free(target);
}

static void httpclient_transaction_destroy_void (void *target) {
	httpclient_transaction_destroy(target);
}

static void httpclient_transaction_destroy_void_dbl_ptr (void *target) {
	struct httpclient_transaction_data **transaction_data = target;
	if (*transaction_data != NULL) {
		httpclient_transaction_destroy(*transaction_data);
	}
}

struct httpclient_create_message_from_response_data_nullsafe_callback_data {
	struct rrr_msg_holder *new_entry;
	const struct httpclient_transaction_data *transaction_data;
};

static int httpclient_create_message_from_response_data_nullsafe_callback (
		const void *str,
		rrr_length len,
		void *arg
) {
	struct httpclient_create_message_from_response_data_nullsafe_callback_data *callback_data = arg;

	int ret = 0;

	if ((ret = rrr_msg_msg_new_with_data (
			(struct rrr_msg_msg **) &callback_data->new_entry->message,
			MSG_TYPE_MSG,
			MSG_CLASS_DATA,
			rrr_time_get_64(),
			callback_data->transaction_data->msg_topic,
			(callback_data->transaction_data->msg_topic != NULL ? strlen(callback_data->transaction_data->msg_topic) : 0),
			str,
			len
	)) != 0) {
		goto out;
	}

	callback_data->new_entry->data_length = MSG_TOTAL_SIZE((struct rrr_msg_msg *) callback_data->new_entry->message);

	out:
	return ret;
}

struct httpclient_create_message_from_response_data_callback_data {
	struct httpclient_data *httpclient_data;
	const struct httpclient_transaction_data *transaction_data;
	const struct rrr_nullsafe_str *response_data;
};

static int httpclient_create_message_from_response_data_callback (
		struct rrr_msg_holder *new_entry,
		void *arg
) {
	struct httpclient_create_message_from_response_data_callback_data *callback_data = arg;

	int ret = RRR_MESSAGE_BROKER_OK;

	if (rrr_nullsafe_str_len(callback_data->response_data) > 0xffffffff) { // Eight f's
		RRR_MSG_0("HTTP length too long in httpclient_create_message_callback, max is 0xffffffff\n");
		ret = RRR_MESSAGE_BROKER_DROP;
		goto out;
	}

	struct httpclient_create_message_from_response_data_nullsafe_callback_data nullsafe_callback_data = {
			new_entry,
			callback_data->transaction_data
	};

	if ((ret = rrr_nullsafe_str_with_raw_do_const (
			callback_data->response_data,
			httpclient_create_message_from_response_data_nullsafe_callback,
			&nullsafe_callback_data
	)) != 0) {
		RRR_MSG_0("Failed to create message in httpclient_create_message_callback\n");
		ret = RRR_MESSAGE_BROKER_ERR;
		goto out;
	}

	out:
	rrr_msg_holder_unlock(new_entry);
	return ret;
}

struct httpclient_final_callback_data {
	struct httpclient_data *httpclient_data;
};

static int httpclient_final_callback_receive_data (
		struct httpclient_data *httpclient_data,
		const struct httpclient_transaction_data *transaction_data,
		const struct rrr_nullsafe_str *response_data
) {
	struct httpclient_create_message_from_response_data_callback_data callback_data_broker = {
			httpclient_data,
			transaction_data,
			response_data
	};

	return rrr_message_broker_write_entry (
			INSTANCE_D_BROKER_ARGS(httpclient_data->thread_data),
			NULL,
			0,
			0,
			httpclient_create_message_from_response_data_callback,
			&callback_data_broker
	);
}

struct httpclient_create_message_from_json_broker_callback_data {
	struct httpclient_data *httpclient_data;
	const struct httpclient_transaction_data *transaction_data;
	const struct rrr_array *array;
};

static int httpclient_create_message_from_json_callback (
		struct rrr_msg_holder *new_entry,
		void *arg
) {
	struct httpclient_create_message_from_json_broker_callback_data *callback_data = arg;

	int ret = RRR_MESSAGE_BROKER_OK;

	if ((ret = rrr_array_new_message_from_collection (
			(struct rrr_msg_msg **) &new_entry->message,
			callback_data->array,
			rrr_time_get_64(),
			callback_data->transaction_data->msg_topic,
			(callback_data->transaction_data->msg_topic != NULL ? strlen(callback_data->transaction_data->msg_topic) : 0)
	)) != 0) {
		RRR_MSG_0("Failed to create array message in httpclient_create_message_from_json_callback of httpclient instance %s\n",
				INSTANCE_D_NAME(callback_data->httpclient_data->thread_data));
		goto out;
	}

	new_entry->data_length = MSG_TOTAL_SIZE((struct rrr_msg_msg *) new_entry->message);

	out:
	rrr_msg_holder_unlock(new_entry);
	return ret;
}

struct httpclient_create_message_from_json_callback_data {
	struct httpclient_data *httpclient_data;
	const struct httpclient_transaction_data *transaction_data;
};

static int httpclient_create_message_from_json_array_callback (
		const struct rrr_array *array,
		void *arg
) {
	struct httpclient_create_message_from_json_callback_data *callback_data = arg;

	struct httpclient_create_message_from_json_broker_callback_data callback_data_broker = {
			callback_data->httpclient_data,
			callback_data->transaction_data,
			array
	};

	return rrr_message_broker_write_entry (
			INSTANCE_D_BROKER_ARGS(callback_data->httpclient_data->thread_data),
			NULL,
			0,
			0,
			httpclient_create_message_from_json_callback,
			&callback_data_broker
	);
}

static int httpclient_create_message_from_json_nullsafe_callback (
		const void *str,
		rrr_nullsafe_len len,
		void *arg
) {
	struct httpclient_create_message_from_json_callback_data *callback_data = arg;

	int ret = 0;

	if ((ret = rrr_json_to_arrays (
			str,
			len,
			RRR_HTTPCLIENT_JSON_MAX_LEVELS,
			httpclient_create_message_from_json_array_callback,
			callback_data
	)) != 0) {
		// Let hard error only propagate
		if (ret == RRR_JSON_PARSE_INCOMPLETE || ret == RRR_JSON_PARSE_ERROR) {
			RRR_DBG_2("HTTP client instance %s: JSON parsing of data from server failed, possibly invalid data\n",
					INSTANCE_D_NAME(callback_data->httpclient_data->thread_data));
			ret = 0;
		}

		if (ret != 0) {
			RRR_MSG_0("HTTP client instance %s: JSON parsing of data from server failed with a hard error\n",
					INSTANCE_D_NAME(callback_data->httpclient_data->thread_data));
		}
	}

	return ret;
}

static int httpclient_final_callback_receive_json (
		struct httpclient_data *httpclient_data,
		const struct httpclient_transaction_data *transaction_data,
		const struct rrr_nullsafe_str *response_data
) {
	struct httpclient_create_message_from_json_callback_data callback_data = {
			httpclient_data,
			transaction_data
	};

	return rrr_nullsafe_str_with_raw_do_const (
			response_data,
			httpclient_create_message_from_json_nullsafe_callback,
			&callback_data
	);
}

static void httpclient_msgdb_conn_ensure_with_callback (
		struct httpclient_data *data,
		int (*callback)(struct httpclient_data *data, void *arg),
		void *callback_arg
) {
	if (rrr_msgdb_client_open (&data->msgdb_conn, data->msgdb_socket) != 0) {
		RRR_MSG_0("Warning: Connection to msgdb on socket '%s' failed in httpclient instance %s\n",
			data->msgdb_socket, INSTANCE_D_NAME(data->thread_data));
		return;
	}

	if (callback(data, callback_arg) != 0) {
		rrr_msgdb_client_close(&data->msgdb_conn);
	}
}

static int httpclient_msgdb_poll_callback_get_msg (struct httpclient_data *data, const struct rrr_type_value *path) {
	int ret = 0;

	struct rrr_msg_holder *entry = NULL;
	char *topic_tmp = NULL;
	struct rrr_msg_msg *msg_tmp = NULL;

	if ((ret = path->definition->to_str(&topic_tmp, path)) != 0) {
		goto out;
	}

	if (rrr_msgdb_client_cmd_get(&msg_tmp, &data->msgdb_conn, topic_tmp)) {
		// Don't return failure on this error
		goto out;
	}

	RRR_DBG_3("httpclient instance %s retrieved message with timestamp %" PRIu64 " topic '%s' from msgdb\n",
			INSTANCE_D_NAME(data->thread_data),
			msg_tmp->timestamp,
			topic_tmp
	);

	if ((ret = rrr_msg_holder_new (
			&entry,
			MSG_TOTAL_SIZE(msg_tmp),
			NULL,
			0,
			0,
			msg_tmp
	)) != 0) {
		goto out;
	}

	msg_tmp = NULL;

	entry->send_time = rrr_time_get_64();

	rrr_msg_holder_incref(entry);
	RRR_LL_APPEND(&data->defer_queue, entry);

	out:
	if (entry != NULL) {
		rrr_msg_holder_decref(entry);
	}
	RRR_FREE_IF_NOT_NULL(msg_tmp);
	RRR_FREE_IF_NOT_NULL(topic_tmp);
	return ret;
}

static int httpclient_msgdb_poll_callback (struct httpclient_data *data, void *callback_arg) {
	(void)(callback_arg);

	int ret = 0;

	struct rrr_array paths = {0};

	if ((ret = rrr_msgdb_client_cmd_idx(&paths, &data->msgdb_conn, "/")) != 0) {
		goto out;
	}

	RRR_LL_ITERATE_BEGIN(&paths, struct rrr_type_value);
		if (node->tag == NULL || strcmp(node->tag, "file") != 0) {
			RRR_LL_ITERATE_NEXT();
		}

		if ((ret = httpclient_msgdb_poll_callback_get_msg (data, node)) != 0) {
			RRR_LL_ITERATE_BREAK();
		}
	RRR_LL_ITERATE_END();

	out:
	rrr_array_clear(&paths);
	return ret;
}

static void httpclient_msgdb_poll (struct httpclient_data *data) {
	httpclient_msgdb_conn_ensure_with_callback(data, httpclient_msgdb_poll_callback, NULL);
}

static int httpclient_msgdb_delete_callback (struct httpclient_data *data, void *callback_arg) {
	const struct rrr_msg_msg *msg = callback_arg;

	int ret = 0;

	char *topic_tmp = NULL;

	if ((ret = rrr_msg_msg_topic_get(&topic_tmp, msg)) != 0) {
		goto out;
	}

	ret = rrr_msgdb_client_cmd_del(&data->msgdb_conn, topic_tmp);

	out:
	RRR_FREE_IF_NOT_NULL(topic_tmp);
	return ret;
}

static void httpclient_msgdb_delete (struct httpclient_data *data, const struct rrr_msg_msg *msg) {
	httpclient_msgdb_conn_ensure_with_callback(data, httpclient_msgdb_delete_callback, (void *) msg); // Cast away const OK
}

static int httpclient_msgdb_queue_process_callback (struct httpclient_data *data, void *callback_arg) {
	(void)(callback_arg);

	int ret = RRR_HTTP_OK;

	RRR_LL_ITERATE_BEGIN(&data->msgdb_queue, struct rrr_msg_holder);
		rrr_msg_holder_lock(node);
		pthread_cleanup_push(rrr_msg_holder_unlock_void, node);

		struct rrr_msg_msg *msg = node->message;
		MSG_SET_TYPE(msg, MSG_TYPE_PUT);

		if ((ret = rrr_msgdb_client_send(&data->msgdb_conn, msg)) == 0) {
			int positive_ack = 0;
			if ((ret = rrr_msgdb_client_await_ack(&positive_ack, &data->msgdb_conn)) != 0) {
				RRR_LL_ITERATE_LAST();
			}
			else if (positive_ack == 1) {
				RRR_LL_ITERATE_SET_DESTROY();
			}
		}
		else {
			RRR_LL_ITERATE_LAST();
		}

		pthread_cleanup_pop(1); // Unlock
	RRR_LL_ITERATE_END_CHECK_DESTROY(&data->msgdb_queue, 0; rrr_msg_holder_decref(node));

	return ret;
}

static void httpclient_msgdb_queue_process (struct httpclient_data *data) {
	if (RRR_LL_COUNT(&data->msgdb_queue) == 0) {
		return;
	}

	httpclient_msgdb_conn_ensure_with_callback(data, httpclient_msgdb_queue_process_callback, NULL);
}

#define HTTPCLIENT_NOTIFY_MSGDB_ENSURE_ACTIVE() \
	if (data->msgdb_socket == NULL || data->http_client_config.method != RRR_HTTP_METHOD_PUT) return

#define HTTPCLIENT_NOTIFY_MSGDB_ENSURE_TOPIC() \
	if (MSG_TOPIC_LENGTH((struct rrr_msg_msg *) entry_locked->message) == 0) return

static void httpclient_msgdb_notify_remove_from_queue(struct httpclient_data *data, struct rrr_msg_holder *entry_locked) {
	HTTPCLIENT_NOTIFY_MSGDB_ENSURE_ACTIVE();
	HTTPCLIENT_NOTIFY_MSGDB_ENSURE_TOPIC();

	// Call this method ONLY when the entry is guaranteed to be removed imminently from
	// the defer queue, as it otherwise would be part of two lists at the same time
	// causing corruption.

	rrr_msg_holder_incref_while_locked(entry_locked);
	RRR_LL_APPEND(&data->msgdb_queue, entry_locked);
}

static void httpclient_msgdb_notify_timeout(struct httpclient_data *data, struct rrr_msg_holder *entry_locked) {
	HTTPCLIENT_NOTIFY_MSGDB_ENSURE_ACTIVE();
	HTTPCLIENT_NOTIFY_MSGDB_ENSURE_TOPIC();

	httpclient_msgdb_delete(data, entry_locked->message);
}

static void httpclient_msgdb_notify_complete(struct httpclient_data *data, struct rrr_msg_holder *entry_locked) {
	HTTPCLIENT_NOTIFY_MSGDB_ENSURE_ACTIVE();
	HTTPCLIENT_NOTIFY_MSGDB_ENSURE_TOPIC();

	httpclient_msgdb_delete(data, entry_locked->message);
}

static int httpclient_final_callback (
		RRR_HTTP_CLIENT_FINAL_CALLBACK_ARGS
) {
	struct httpclient_data *httpclient_data = arg;
	struct httpclient_transaction_data *transaction_data = transaction->application_data;

	int ret = RRR_HTTP_OK;

	RRR_DBG_3("HTTP response %i from server in httpclient instance %s: data size %" PRIrrrl "\n",
			transaction->response_part->response_code,
			INSTANCE_D_NAME(httpclient_data->thread_data),
			rrr_nullsafe_str_len(response_data)
	);

	if (transaction->response_part->response_code < 200 || transaction->response_part->response_code > 299) {
		RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(method,transaction->request_part->request_method_str_nullsafe);
		RRR_MSG_0("Error response while fetching HTTP: %i %s (request was %s %s)\n",
				transaction->response_part->response_code,
				(transaction->response_part->response_str != NULL ? transaction->response_part->response_str : "-"),
				RRR_HTTP_METHOD_TO_STR_CONFORMING(transaction->method),
				transaction->endpoint_str
		);
	}
	else if (transaction->method == RRR_HTTP_METHOD_PUT) {
		rrr_msg_holder_lock(transaction_data->entry);
		httpclient_msgdb_notify_complete(httpclient_data, transaction_data->entry);
		rrr_msg_holder_unlock(transaction_data->entry);
	}

	if (httpclient_data->do_receive_part_data) {
		RRR_DBG_3("httpclient instance %s creating message with HTTP response data\n",
				INSTANCE_D_NAME(httpclient_data->thread_data));

		ret |= httpclient_final_callback_receive_data(httpclient_data, transaction->application_data, response_data);
	}

	if (httpclient_data->do_receive_json_data) {
		RRR_DBG_3("httpclient instance %s creating messages with JSON data\n",
				INSTANCE_D_NAME(httpclient_data->thread_data));

		ret |= httpclient_final_callback_receive_json(httpclient_data, transaction->application_data, response_data);
	}

	return ret;
}

static int httpclient_transaction_field_add (
		struct httpclient_data *data,
		struct rrr_http_transaction *transaction,
		const struct rrr_type_value *value,
		const char *tag_to_use
) {
	int ret = 0;

	struct rrr_http_query_builder query_builder;

	char *buf_tmp = NULL;

	if ((rrr_http_query_builder_init(&query_builder)) != 0) {
		RRR_MSG_0("Could not initialize query builder in httpclient_add_multipart_array_value\n");
		ret = 1;
		goto out;
	}

	RRR_DBG_3("HTTP add array value with tag '%s' type '%s'\n",
			(tag_to_use != NULL ? tag_to_use : "(no tag)"), value->definition->identifier);

	if (RRR_TYPE_IS_MSG(value->definition->type)) {
		rrr_length buf_size = 0;

		if (rrr_type_value_allocate_and_export(&buf_tmp, &buf_size, value) != 0) {
			RRR_MSG_0("Error while exporting RRR message in httpclient_add_multipart_array_value\n");
			ret = 1;
			goto out_cleanup_query_builder;
		}

		ret = rrr_http_transaction_query_field_add (
				transaction,
				tag_to_use,
				buf_tmp,
				buf_size,
				RRR_MESSAGE_MIME_TYPE
		);
	}
	else if (RRR_TYPE_IS_STR(value->definition->type)) {
		// MUST be signed due to decrement counting. Also, do not get
		// export length as it will add two bytes for quotes ""
		int64_t buf_size = value->total_stored_length;
		const char *buf = value->data;

		// Remove trailing 0's
		while (buf_size > 0 && buf[buf_size - 1] == '\0') {
			buf_size--;
		}

		ret = rrr_http_transaction_query_field_add (
				transaction,
				tag_to_use,
				buf,
				buf_size,
				"text/plain"
		);
	}
	else if (RRR_TYPE_IS_BLOB(value->definition->type)) {
		ret = rrr_http_transaction_query_field_add (
				transaction,
				tag_to_use,
				value->data,
				value->total_stored_length,
				"application/octet-stream"
		);
	}
	else {
		int value_was_empty_dummy = 0;

		// BLOB and STR must be treated as special case above, this
		// function would otherwise modify the data by escaping
		if ((ret = rrr_http_query_builder_append_type_value_as_escaped_string (
				&value_was_empty_dummy,
				&query_builder,
				value,
				0
		)) != 0) {
			RRR_MSG_0("Error while exporting non-BLOB in httpclient_add_multipart_array_value\n");
			goto out_cleanup_query_builder;
		}

		ret = rrr_http_transaction_query_field_add (
				transaction,
				tag_to_use,
				rrr_http_query_builder_buf_get(&query_builder),
				rrr_http_query_builder_wpos_get(&query_builder),
				"text/plain"
		);
	}

	if (ret != 0) {
		RRR_MSG_0("Could not add data to HTTP query in instance %s\n", INSTANCE_D_NAME(data->thread_data));
		goto out_cleanup_query_builder;
	}

	out_cleanup_query_builder:
		rrr_http_query_builder_cleanup(&query_builder);
	out:
		RRR_FREE_IF_NOT_NULL(buf_tmp);
		return ret;
}

static int httpclient_message_values_get (
		struct rrr_array *target_array,
		const struct rrr_msg_msg *message
) {
	int ret = 0;

	uint16_t array_version_dummy;
	if (rrr_array_message_append_to_collection(&array_version_dummy, target_array, message) != 0) {
		RRR_MSG_0("Error while converting message to collection in httpclient_get_values_from_message\n");
		ret = RRR_HTTP_SOFT_ERROR;
		goto out;
	}

	out:
	return ret;
}

static int httpclient_get_metadata_from_message (
		struct rrr_array *target_array,
		const struct rrr_msg_msg *message
) {
	int ret = 0;

	// Push timestamp
	if (rrr_array_push_value_u64_with_tag(target_array, "timestamp", message->timestamp) != 0) {
		RRR_MSG_0("Could not create timestamp array value in httpclient_get_values_from_message\n");
		ret = RRR_HTTP_HARD_ERROR;
		goto out;
	}

	// Push topic
	if (MSG_TOPIC_LENGTH(message) > 0) {
		if (rrr_array_push_value_str_with_tag_with_size (
				target_array,
				"topic",
				MSG_TOPIC_PTR(message),
				MSG_TOPIC_LENGTH(message)
		) != 0) {
			RRR_MSG_0("Could not create topic array value in httpclient_get_values_from_message\n");
			ret = RRR_HTTP_HARD_ERROR;
			goto out;
		}
	}

	// Push data
	if (MSG_DATA_LENGTH(message) > 0) {
		if (rrr_array_push_value_blob_with_tag_with_size (
				target_array,
				"data",
				MSG_DATA_PTR(message),
				MSG_DATA_LENGTH(message)
		) != 0) {
			RRR_MSG_0("Could not create data array value in httpclient_get_values_from_message\n");
			ret = RRR_HTTP_HARD_ERROR;
			goto out;
		}
	}

	out:
	return ret;
}

static int httpclient_session_query_prepare_callback_process_override (
		char **result,
		struct httpclient_data *data,
		const struct rrr_array *array,
		const char *tag,
		int do_force,
		const char *debug_name
) {
	int ret = RRR_HTTP_OK;

	*result = NULL;

	char *data_to_free = NULL;

	const struct rrr_type_value *value = rrr_array_value_get_by_tag_const(array, tag);
	if (value == NULL) {
		// Use default if force is not enabled
	}
	else {
		if (value->definition->to_str == NULL) {
			RRR_MSG_0("Warning: Received message in httpclient instance %s where the specified type of the %s tagged '%s' in the message was of type '%s' which cannot be used as a string\n",
					INSTANCE_D_NAME(data->thread_data),
					debug_name,
					tag,
					value->definition->identifier
			);
		}
		else if (value->definition->to_str(&data_to_free, value) != 0) {
			RRR_MSG_0("Warning: Failed to convert array value tagged '%s' to string for use as %s in httpserver instance %s\n",
					tag,
					debug_name,
					INSTANCE_D_NAME(data->thread_data)
			);
		}
	}

	if (data_to_free == NULL && do_force) {
		RRR_MSG_0("Warning: Received message in httpclient instance %s with missing/unusable %s tag '%s' (which is enforced in configuration), dropping it\n",
				INSTANCE_D_NAME(data->thread_data),
				debug_name,
				tag
		);
		ret = RRR_HTTP_SOFT_ERROR;
		goto out;
	}

	*result = data_to_free;
	data_to_free = NULL;

	out:
	RRR_FREE_IF_NOT_NULL(data_to_free);
	return ret;
}

struct httpclient_prepare_callback_data {
	struct httpclient_data *data;
	const struct rrr_msg_msg *message;
	const struct rrr_array *array_from_msg;
	int no_destination_override;
};

#define HTTPCLIENT_PREPARE_OVERRIDE(name)                                                \
  do {if (  data->RRR_PASTE(name,_tag) != NULL &&                                        \
        (ret = httpclient_session_query_prepare_callback_process_override (              \
            &RRR_PASTE(name,_to_free),                                                   \
            data,                                                                        \
            array_from_msg,                                                              \
            data->RRR_PASTE(name,_tag),                                                  \
            data->RRR_PASTE_3(do_,name,_tag_force),                                      \
            RRR_QUOTE(name)                                                              \
        )) != 0) { goto out; }} while (0)

static int httpclient_overrides_server_and_port_get_from_message (
		char **server_override,
		uint16_t *port_override,
		struct httpclient_data *data,
		const struct rrr_array *array_from_msg
) {
	int ret = 0;

	*server_override = NULL;
	// DO NOT set *port_ovveride to zero here, leave it as is

	char *server_to_free = NULL;
	char *port_to_free = NULL;

	HTTPCLIENT_PREPARE_OVERRIDE(server);
	HTTPCLIENT_PREPARE_OVERRIDE(port);

	if (port_to_free != NULL) {
		char *end = NULL;
		unsigned long long port = strtoull(port_to_free, &end, 10);
		if (end == NULL || *end != '\0' || port == 0 || port > 65535) {
			RRR_MSG_0("Warning: Invalid override port value of '%s' in message to httpclient instance %s, dropping it\n",
					port_to_free, INSTANCE_D_NAME(data->thread_data));
			ret = RRR_HTTP_SOFT_ERROR;
			goto out;
		}
		*port_override = port;
	}

	*server_override = server_to_free;
	server_to_free = NULL;

	out:
	RRR_FREE_IF_NOT_NULL(server_to_free);
	RRR_FREE_IF_NOT_NULL(port_to_free);
	return ret;
}

static int httpclient_connection_prepare_callback (
		RRR_HTTP_CLIENT_CONNECTION_PREPARE_CALLBACK_ARGS
) {
	struct httpclient_prepare_callback_data *callback_data = arg;
	struct httpclient_data *data = callback_data->data;

	return httpclient_overrides_server_and_port_get_from_message (
			server_override,
			port_override,
			data,
			callback_data->array_from_msg
	);
}

static int httpclient_session_query_prepare_callback_process_endpoint_from_topic_override (
		char **target,
		struct httpclient_data *data,
		const struct rrr_msg_msg *message
) {
	int ret = 0;

	struct rrr_string_builder *string_builder = NULL;

	if (MSG_TOPIC_LENGTH(message) == 0) {
		if (data->do_endpoint_from_topic_force) {
			RRR_DBG_2("No topic was set in message received in httpclient instance %s while endpoint from topic force was enabled, dropping it\n",
				INSTANCE_D_NAME(data->thread_data));
			ret = RRR_HTTP_SOFT_ERROR;
		}
		goto out;
	}

	if ((ret = rrr_string_builder_new(&string_builder)) != 0) {
		RRR_MSG_0("Could not create string builder in httpclient_session_query_prepare_callback_process_endpoint_from_topic_override\n");
		goto out;
	}

	if ((ret = rrr_string_builder_append(string_builder, "/")) != 0) {
		RRR_MSG_0("Failed to append to string builder in httpclient_session_query_prepare_callback_process_endpoint_from_topic_override\n");
		goto out;
	}

	if ((ret = rrr_string_builder_append_raw(string_builder, MSG_TOPIC_PTR(message), MSG_TOPIC_LENGTH(message))) != 0) {
		RRR_MSG_0("Failed to append to string builder in httpclient_session_query_prepare_callback_process_endpoint_from_topic_override\n");
		goto out;
	}

	*target = rrr_string_builder_buffer_takeover(string_builder);


	out:
	if (string_builder != NULL) {
		rrr_string_builder_destroy(string_builder);
	}
	return ret;
}

static int httpclient_session_query_prepare_callback (
		RRR_HTTP_CLIENT_QUERY_PREPARE_CALLBACK_ARGS
) {
	struct httpclient_prepare_callback_data *callback_data = arg;
	struct httpclient_data *data = callback_data->data;
	const struct rrr_msg_msg *message = callback_data->message;

	*query_string = NULL;
	*endpoint_override = NULL;

	int ret = RRR_HTTP_OK;

	char *endpoint_to_free = NULL;
	struct rrr_array array_to_send_tmp = {0};

	array_to_send_tmp.version = RRR_ARRAY_VERSION;

	if (!callback_data->no_destination_override) {
		if (data->do_endpoint_from_topic) {
			if ((ret = httpclient_session_query_prepare_callback_process_endpoint_from_topic_override (
					&endpoint_to_free,
					data,
					message
			)) != 0) {
				goto out;
			}
		}
		else {
			const struct rrr_array *array_from_msg = callback_data->array_from_msg;
			HTTPCLIENT_PREPARE_OVERRIDE(endpoint);
		}
	}

	if (data->do_no_data == 0) {
		rrr_array_append_from(&array_to_send_tmp, callback_data->array_from_msg);

		if (data->do_rrr_msg_to_array) {
			if ((ret = httpclient_get_metadata_from_message(&array_to_send_tmp, message))) {
				goto out;
			}
		}
	}

	if (data->do_no_data != 0 && (RRR_MAP_COUNT(&data->http_client_config.tags) + RRR_LL_COUNT(&array_to_send_tmp) > 0)) {
		RRR_BUG("BUG: HTTP do_no_data is set but tags map and array are not empty in httpclient_session_query_prepare_callback\n");
	}

	if ((ret = rrr_http_transaction_keepalive_set (
			transaction,
			1
	)) != 0) {
		RRR_MSG_0("Failed to set keep-alive in httpclient_session_query_prepare_callback\n");
		ret = 1;
		goto out;
	}

	if (RRR_MAP_COUNT(&data->http_client_config.tags) == 0) {
		// Add all array fields
		RRR_LL_ITERATE_BEGIN(&array_to_send_tmp, const struct rrr_type_value);
			if ((ret = httpclient_transaction_field_add (
					data,
					transaction,
					node,
					node->tag // NULL allowed
			)) != RRR_HTTP_OK) {
				goto out;
			}
		RRR_LL_ITERATE_END();
	}
	else {
		// Add chosen array fields
		RRR_MAP_ITERATE_BEGIN(&data->http_client_config.tags);
			const struct rrr_type_value *value = rrr_array_value_get_by_tag_const(callback_data->array_from_msg, node_tag);
			if (value == NULL) {
				RRR_MSG_0("Could not find array tag %s while adding HTTP query values in instance %s.\n",
						node_tag, INSTANCE_D_NAME(data->thread_data));
				ret = RRR_HTTP_SOFT_ERROR;
				goto out;
			}

			// If value is set in map, tag is to be translated
			const char *tag_to_use = (node_value != NULL && *node_value != '\0') ? node_value : node_tag;

			if ((ret = httpclient_transaction_field_add (
					data,
					transaction,
					value,
					tag_to_use
			)) != RRR_HTTP_OK) {
				goto out;
			}
		RRR_MAP_ITERATE_END();
	}

	RRR_MAP_ITERATE_BEGIN(&data->http_client_config.fields);
		RRR_DBG_3("HTTP add field value with tag '%s' value '%s'\n",
				node_tag, node_value != NULL ? node_value : "(no value)");
		if ((ret = rrr_http_transaction_query_field_add (
				transaction,
				node_tag,
				node_value,
				strlen(node_value),
				"text/plain"
		)) != RRR_HTTP_OK) {
			goto out;
		}
	RRR_MAP_ITERATE_END();

	if (RRR_DEBUGLEVEL_3) {
		RRR_MSG_3("HTTP using method %s\n", RRR_HTTP_METHOD_TO_STR(transaction->method));
		rrr_http_transaction_query_fields_dump(transaction);
	}

	{
		const char *endpoint_to_print = (endpoint_to_free != NULL ? endpoint_to_free : data->http_client_config.endpoint);
		RRR_DBG_2("HTTP client instance %s sending request from message with timestamp %" PRIu64 " endpoint %s\n",
				INSTANCE_D_NAME(data->thread_data),
				message->timestamp,
				endpoint_to_print
		);
	}

	*endpoint_override = endpoint_to_free;
	endpoint_to_free = NULL;

	out:
		rrr_array_clear(&array_to_send_tmp);
		RRR_FREE_IF_NOT_NULL(endpoint_to_free);
		return ret;
}

static int httpclient_request_send (
		struct httpclient_data *data,
		struct rrr_http_client_request_data *request_data,
		struct rrr_msg_holder *entry,
		rrr_biglength remaining_redirects,
		int no_destination_override
) {
	struct rrr_msg_msg *message = entry->message;

	int ret = RRR_HTTP_OK;

	struct rrr_array array_from_msg_tmp = {0};
	struct httpclient_transaction_data *transaction_data = NULL;

	pthread_cleanup_push(rrr_array_clear_void, &array_from_msg_tmp);

	array_from_msg_tmp.version = RRR_ARRAY_VERSION;

	if ((ret = httpclient_transaction_data_new (
			&transaction_data,
			MSG_TOPIC_PTR(message),
			MSG_TOPIC_LENGTH(message),
			entry
	)) != 0) {
		goto out_cleanup_array;
	}

	rrr_msg_holder_incref_while_locked(entry);

	pthread_cleanup_push(httpclient_transaction_destroy_void_dbl_ptr, &transaction_data);

	if (MSG_IS_ARRAY(message)) {
		if ((ret = httpclient_message_values_get(&array_from_msg_tmp, message)) != RRR_HTTP_OK) {
			goto out_cleanup_transaction_data;
		}
	}

	struct httpclient_prepare_callback_data prepare_callback_data = {
			data,
			message,
			&array_from_msg_tmp,
			no_destination_override
	};

	request_data->upgrade_mode = RRR_HTTP_UPGRADE_MODE_HTTP2;

	// Debug message for sending a request is in query prepare callback

	ret = rrr_http_client_request_send (
			request_data,
			&data->keepalive_transport_plain,
			&data->keepalive_transport_tls,
			&data->net_transport_config,
			remaining_redirects,
			httpclient_connection_prepare_callback,
			&prepare_callback_data,
			httpclient_session_query_prepare_callback,
			&prepare_callback_data,
			(void **) &transaction_data,
			httpclient_transaction_destroy_void
	);

	// Do not add anything here, let return value from last function call propagate

	out_cleanup_transaction_data:
		pthread_cleanup_pop(1);
	out_cleanup_array:
		pthread_cleanup_pop(1);
		return ret;
}

static int httpclient_redirect_callback (
		RRR_HTTP_CLIENT_REDIRECT_CALLBACK_ARGS
) {
	struct httpclient_data *data = arg;
	struct httpclient_transaction_data *transaction_data = transaction->application_data;

	int ret = 0;

	struct rrr_http_client_request_data request_data = {0};
	struct rrr_array array_from_msg_tmp = {0};
	char *server_override = NULL;
	uint16_t port_override = 0;

	rrr_msg_holder_lock(transaction_data->entry);

	pthread_cleanup_push(rrr_msg_holder_unlock_void, transaction_data->entry);
	pthread_cleanup_push(rrr_http_client_request_data_cleanup_void, &request_data);
	pthread_cleanup_push(rrr_array_clear_void, &array_from_msg_tmp);
	pthread_cleanup_push(httpclient_dbl_ptr_free_if_not_null, &server_override);

	struct rrr_msg_msg *message = transaction_data->entry->message;

	if (MSG_IS_ARRAY(message)) {
		if ((ret = httpclient_message_values_get(&array_from_msg_tmp, message)) != RRR_HTTP_OK) {
			goto out;
		}
	}

	if ((ret =  httpclient_overrides_server_and_port_get_from_message (
			&server_override,
			&port_override,
			data,
			&array_from_msg_tmp
	)) != 0) {
		goto out;
	}

	// Default from config
	if ((ret = rrr_http_client_request_data_reset_from_request_data (&request_data, &data->request_data)) != 0) {
		goto out;
	}

	// Overrides from message excluding endpoint which is part ov the redirect
	if ((ret = rrr_http_client_request_data_reset_from_raw (
			&request_data,
			server_override,
			port_override
	)) != 0) {
		goto out;
	}

	// Overrides from redirect URI which may be multiple parameters
	if ((ret = rrr_http_client_request_data_reset_from_uri (&request_data, uri)) != 0) {
		RRR_MSG_0("Error while updating target from redirect response URI in httpclient instance %s, return was %i\n",
				INSTANCE_D_NAME(data->thread_data), ret);
		goto out;
	}

	// printf("port: %u transport force: %i\n", request_data.http_port, request_data.transport_force);

	// It is safe to call back into net transport ctx as we are not in ctx while handling redirects.
	// This function will incref the entry as needed.
	// We assume that http client lib has already decref'd remaining redirects by 1,
	if ((ret = httpclient_request_send (
			data,
			&request_data,
			transaction_data->entry,
			transaction->remaining_redirects,
			1 // No destination override (endpoint, server etc. from message)
	)) != 0) {
		RRR_MSG_0("Failed to send HTTP request following redirect response in httpclient instance %s, return was %i\n",
				INSTANCE_D_NAME(data->thread_data), ret);
		goto out;
	}

	out:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);

	// Don't let soft error propagate (would cause the whole thread to shut down)
	return (ret & ~(RRR_HTTP_SOFT_ERROR));
}

static int httpclient_poll_callback(RRR_MODULE_POLL_CALLBACK_SIGNATURE) {
	struct rrr_instance_runtime_data *thread_data = arg;
	struct httpclient_data *data = thread_data->private_data;
	struct rrr_msg_msg *message = entry->message;

	if (RRR_DEBUGLEVEL_3) {
		char *topic_tmp = NULL;

		if (rrr_msg_msg_topic_get(&topic_tmp, message) != 0 ) {
			RRR_MSG_0("Warning: Error while getting topic from message in httpclient_poll_callback\n");
		}

		RRR_DBG_3("httpclient instance %s received message with timestamp %" PRIu64 " topic '%s'\n",
				INSTANCE_D_NAME(thread_data),
				message->timestamp,
				(topic_tmp != NULL ? topic_tmp : "(none)")
		);

		RRR_FREE_IF_NOT_NULL(topic_tmp);
	}

	// Important : Set send_time for correct timeout behavior
	entry->send_time = rrr_time_get_64();

	int ret = RRR_FIFO_SEARCH_GIVE;

	// No incref, we return GIVE
	RRR_LL_APPEND(&data->defer_queue, entry);
	rrr_msg_holder_unlock(entry);
	return ret;
}

static int httpclient_data_init (
		struct httpclient_data *data,
		struct rrr_instance_runtime_data *thread_data
) {
	int ret = 0;

	memset(data, '\0', sizeof(*data));

	data->thread_data = thread_data;

	rrr_http_client_request_data_init(&data->request_data);

	goto out;
//	out_cleanup_data:
//		httpclient_data_cleanup(httpclient_data);
	out:
		return ret;
}

#define HTTPCLIENT_OVERRIDE_TAG_GET(parameter)                                                                                    \
    RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_UTF8_DEFAULT_NULL("http_" RRR_QUOTE(parameter) "_tag", RRR_PASTE(parameter,_tag));         \
    RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_YESNO("http_" RRR_QUOTE(parameter) "_tag_force", RRR_PASTE_3(do_,parameter,_tag_force), 0) \

#define HTTPCLIENT_OVERRIDE_TAG_VALIDATE(parameter)                                                                               \
    do {if (data->RRR_PASTE_3(do_,parameter,_tag_force) != 0) {                                                                   \
        if (data->RRR_PASTE(parameter,_tag) == NULL) {                                                                            \
            RRR_MSG_0("http_" RRR_QUOTE(parameter) " was 'yes' in httpclient instance %s but no tag was specified in http_" RRR_QUOTE(parameter) "_tag\n", \
                    config->name);                                                                                                \
            ret = 1;                                                                                                              \
        }                                                                                                                         \
        if (RRR_INSTANCE_CONFIG_EXISTS("http_" RRR_QUOTE(parameter))) {                                                           \
            RRR_MSG_0("http_" RRR_QUOTE(parameter) "_tag_force was 'yes' in httpclient instance %s while http_" RRR_QUOTE(parameter) " was also set, this is a configuration error\n", \
                    config->name);                                                                                                \
            ret = 1;                                                                                                              \
        }                                                                                                                         \
        if (ret != 0) { goto out; }}} while(0)

static int httpclient_parse_config (
		struct httpclient_data *data,
		struct rrr_instance_config_data *config
) {
	int ret = 0;

	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_YESNO("http_no_data", do_no_data, 0);
	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_YESNO("http_rrr_msg_to_array", do_rrr_msg_to_array, 0);
	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_YESNO("http_drop_on_error", do_drop_on_error, 0);
	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_YESNO("http_receive_part_data", do_receive_part_data, 0);
	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_YESNO("http_receive_json_data", do_receive_json_data, 0);

	// Deprecated option http_keepalive
	RRR_INSTANCE_CONFIG_IF_EXISTS_THEN("http_keepalive",
			RRR_MSG_0("Warning: Parameter http_keepalive is deprecated and has no effect. Use http_max_keepalive_s to control connection lifetime.\n"));

	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_UNSIGNED("http_ttl_seconds", message_ttl_us, 0);
	data->message_ttl_us *= 1000 * 1000;
	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_UNSIGNED("http_message_timeout_ms", message_timeout_us, 0);
	data->message_timeout_us *= 1000;

	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_UNSIGNED("http_max_redirects", redirects_max, RRR_HTTPCLIENT_DEFAULT_REDIRECTS_MAX);
	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_UNSIGNED("http_max_keepalive_s", keepalive_s_max, RRR_HTTPCLIENT_DEFAULT_KEEPALIVE_MAX_S);

	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_UTF8_DEFAULT_NULL("http_msgdb_socket", msgdb_socket);
	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_UNSIGNED("http_msgdb_poll_interval_s", msgdb_poll_interval_us, RRR_HTTPCLIENT_DEFAULT_MSGDB_RETRY_INTERVAL_S);

	data->msgdb_poll_interval_us *= 1000 * 1000;

	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_YESNO("http_endpoint_from_topic", do_endpoint_from_topic, 0);
	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_YESNO("http_endpoint_from_topic_force", do_endpoint_from_topic_force, 0);

	HTTPCLIENT_OVERRIDE_TAG_GET(endpoint);
	HTTPCLIENT_OVERRIDE_TAG_GET(server);
	HTTPCLIENT_OVERRIDE_TAG_GET(port);

	if (data->redirects_max > RRR_HTTPCLIENT_LIMIT_REDIRECTS_MAX) {
		RRR_MSG_0("Setting http_max_redirects of instance %s oustide range, maximum is %i\n",
				config->name, RRR_HTTPCLIENT_LIMIT_REDIRECTS_MAX);
		ret = 1;
		goto out;
	}

	if (data->do_no_data) {
		if (RRR_MAP_COUNT(&data->http_client_config.tags) > 0) {
			RRR_MSG_0("Setting http_no_data in instance %s was 'yes' while http_tags was also set. This is an error.\n",
					config->name);
			ret = 1;
		}
		if (data->do_rrr_msg_to_array) {
			RRR_MSG_0("Setting http_no_data in instance %s was 'yes' while http_rrr_msg_to_array was also 'yes'. This is an error.\n",
					config->name);
			ret = 1;
		}
		if (ret != 0) {
			goto out;
		}
	}

	if (rrr_http_client_config_parse (
			&data->http_client_config,
			config,
			"http",
			RRR_HTTPCLIENT_DEFAULT_SERVER,
			RRR_HTTPCLIENT_DEFAULT_PORT,
			0, // <-- Disable fixed tags and fields
			1, // <-- Enable endpoint
			0  // No raw data
	) != 0) {
		ret = 1;
		goto out;
	}

	{
		if (data->do_endpoint_from_topic_force && !data->do_endpoint_from_topic) {
			RRR_MSG_0("http_endpoint_from_topic_force was 'yes' while http_endpoint_from_topic was not in httpclient instance %s, this is an invalid configuration.\n",
					config->name);
			ret = 1;
		}
		if (data->do_endpoint_from_topic && RRR_INSTANCE_CONFIG_EXISTS("http_endpoint_tag")) {
			RRR_MSG_0("http_endpoint_from_topic_force was 'yes' while http_endpoint_tag was set in httpclient instance %s, this is an invalid configuration.\n",
					config->name);
			ret = 1;
		}
		if (ret != 0) {
			goto out;
		}
	}

	HTTPCLIENT_OVERRIDE_TAG_VALIDATE(endpoint);
	HTTPCLIENT_OVERRIDE_TAG_VALIDATE(server);
	HTTPCLIENT_OVERRIDE_TAG_VALIDATE(port);

	if (rrr_net_transport_config_parse (
			&data->net_transport_config,
			config,
			"http",
			1,
			RRR_NET_TRANSPORT_BOTH
	) != 0) {
		ret = 1;
		goto out;
	}

	out:
	return ret;
}

static void httpclient_defer_queue_process (struct httpclient_data *data) {
	if (RRR_LL_COUNT(&data->defer_queue) ==  0) {
		return;
	}

	int send_timeout_count = 0;
	RRR_LL_ITERATE_BEGIN(&data->defer_queue, struct rrr_msg_holder);
		int ret_tmp = RRR_HTTP_OK;

		if (rrr_thread_signal_encourage_stop_check(INSTANCE_D_THREAD(data->thread_data))) {
			RRR_LL_ITERATE_BREAK();
		}
		rrr_thread_watchdog_time_update(INSTANCE_D_THREAD(data->thread_data));

		rrr_msg_holder_lock(node);
		pthread_cleanup_push(rrr_msg_holder_unlock_void, node);

		if ((data->message_timeout_us != 0 && rrr_time_get_64() > node->send_time + data->message_timeout_us) ||
		    (data->message_ttl_us != 0 && rrr_time_get_64() > ((struct rrr_msg_msg *) node->message)->timestamp + data->message_ttl_us)
		) {
				send_timeout_count++;
				httpclient_msgdb_notify_timeout(data, node);
				RRR_LL_ITERATE_SET_DESTROY();
		}
		else if ((ret_tmp = httpclient_request_send (
				data,
				&data->request_data,
				node,
				data->redirects_max,
				0 // Destination override performed as needed
		)) != RRR_HTTP_OK) {
			if (ret_tmp == RRR_HTTP_BUSY) {
				// Try again
			}
			else if (data->msgdb_socket != NULL && data->http_client_config.method == RRR_HTTP_METHOD_PUT) {
//				RRR_MSG_2("httpclient instance %s deferring failed PUT-message to msgdb on-disk storage\n",
//					INSTANCE_D_NAME(data->thread_data));
				httpclient_msgdb_notify_remove_from_queue(data, node);
				RRR_LL_ITERATE_SET_DESTROY();
			}
			else {
				if (ret_tmp == RRR_HTTP_SOFT_ERROR) {
					rrr_posix_usleep(500000); // 500ms to avoid spamming server when there are errors
					// Try again
					/*
					* TODO : Implement, was removed by mistake during refactoring
					-               if (data->do_drop_on_error) {
					-                       RRR_MSG_0("Dropping message per configuration after error in httpclient instance %s\n",
					-                                       INSTANCE_D_NAME(data->thread_data));
					-                       ret = RRR_HTTP_OK;
					-               }
					*/

				}
				else {
					RRR_MSG_0("Hard error while iterating defer queue in httpclient instance %s, deleting message\n",
							INSTANCE_D_NAME(data->thread_data));
					httpclient_msgdb_notify_remove_from_queue(data, node);
					RRR_LL_ITERATE_SET_DESTROY();
					// Delete message
				}
			}
		}
		else {
			httpclient_msgdb_notify_remove_from_queue(data, node);
			RRR_LL_ITERATE_SET_DESTROY();
		}

		pthread_cleanup_pop(1); // Unlock
	RRR_LL_ITERATE_END_CHECK_DESTROY(&data->defer_queue, 0; rrr_msg_holder_decref(node));

	if (send_timeout_count > 0) {
		RRR_MSG_0("Send timeout for %i messages in httpclient instance %s\n",
				send_timeout_count,
				INSTANCE_D_NAME(data->thread_data));
	}
}

static void *thread_entry_httpclient (struct rrr_thread *thread) {
	struct rrr_instance_runtime_data *thread_data = thread->private_data;
	struct httpclient_data *data = thread_data->private_data = thread_data->private_memory;

	if (httpclient_data_init(data, thread_data) != 0) {
		RRR_MSG_0("Could not initialize thread_data in httpclient instance %s\n", INSTANCE_D_NAME(thread_data));
		pthread_exit(0);
	}

	RRR_DBG_1 ("httpclient thread thread_data is %p\n", thread_data);

	pthread_cleanup_push(httpclient_data_cleanup, data);

	rrr_thread_start_condition_helper_nofork(thread);

	if (httpclient_parse_config(data, INSTANCE_D_CONFIG(thread_data)) != 0) {
		goto out_message;
	}

	rrr_instance_config_check_all_settings_used(thread_data->init_data.instance_config);

	RRR_DBG_1 ("httpclient started thread %p\n", thread_data);

	enum rrr_http_transport http_transport_force = RRR_HTTP_TRANSPORT_ANY;

	switch (data->net_transport_config.transport_type) {
		case RRR_NET_TRANSPORT_TLS:
			http_transport_force = RRR_HTTP_TRANSPORT_HTTPS;
			 break;
		case RRR_NET_TRANSPORT_PLAIN:
			http_transport_force = RRR_HTTP_TRANSPORT_HTTP;
			 break;
		default:
			http_transport_force = RRR_HTTP_TRANSPORT_ANY;
			break;
	};

	if (rrr_http_client_request_data_reset (
			&data->request_data,
			http_transport_force,
			data->http_client_config.method,
			RRR_HTTP_UPGRADE_MODE_HTTP2,
			data->http_client_config.do_plain_http2,
			RRR_HTTP_CLIENT_USER_AGENT
	) != 0) {
		RRR_MSG_0("Could not initialize http client request data in httpclient instance %s\n",
				INSTANCE_D_NAME(thread_data));
		goto out_message;
	}

	if (rrr_http_client_request_data_reset_from_config (
			&data->request_data,
			&data->http_client_config
	) != 0) {
		RRR_MSG_0("Could not store HTTP client configuration in httpclient instance %s\n",
				INSTANCE_D_NAME(data->thread_data));
		goto out_message;
	}

	unsigned int consecutive_nothing_happened = 0; // NO NOT use signed
	uint64_t prev_bytes_total = 0;
	uint64_t prev_msgdb_index_time = rrr_time_get_64();

	while (rrr_thread_signal_encourage_stop_check(thread) != 1) {
		rrr_thread_watchdog_time_update(thread);

		const uint64_t time_now = rrr_time_get_64();

		if (data->msgdb_socket != NULL) {
			httpclient_msgdb_queue_process(data);
			if (prev_msgdb_index_time + data->msgdb_poll_interval_us < time_now) {
				httpclient_msgdb_poll(data);
				prev_msgdb_index_time = time_now;
			}
		}

		httpclient_defer_queue_process(data);

		uint64_t bytes_total = 0;

		// We are allowed to pass NULL transport pointers
		if (rrr_http_client_tick (
				&bytes_total,
				data->keepalive_transport_plain,
				data->keepalive_transport_tls,
				RRR_HTTPCLIENT_READ_MAX_SIZE,
				RRR_HTTPCLIENT_DEFAULT_KEEPALIVE_MAX_S * 1000,
				httpclient_final_callback,
				data,
				httpclient_redirect_callback,
				data,
				NULL,
				NULL,
				NULL,
				NULL,
				NULL,
				NULL
		) != 0) {
			RRR_MSG_0("httpclient instance %s error while ticking\n", INSTANCE_D_NAME(thread_data));
			goto out_message;
		}

		if (prev_bytes_total == bytes_total) {
			consecutive_nothing_happened++;
			if (consecutive_nothing_happened > 100) {
				rrr_posix_usleep(30000); // 30 ms
			}
			else if (consecutive_nothing_happened > 20) {
				rrr_posix_usleep(100); // 0.1 ms
			}
		}
		else {
			consecutive_nothing_happened = 0;
		}
		prev_bytes_total = bytes_total;

		if (RRR_LL_COUNT(&data->defer_queue) < 100) {
			if (rrr_poll_do_poll_search(thread_data, &thread_data->poll, httpclient_poll_callback, thread_data, 0) != 0) {
				RRR_MSG_0("Error while polling in httpclient instance %s\n",
						INSTANCE_D_NAME(thread_data));
				break;
			}
		}
	}

	out_message:
	RRR_DBG_1 ("Thread httpclient %p exiting\n", thread);

	pthread_cleanup_pop(1);
	pthread_exit(0);
}

static struct rrr_module_operations module_operations = {
		NULL,
		thread_entry_httpclient,
		NULL,
		NULL,
		NULL
};

static const char *module_name = "httpclient";

__attribute__((constructor)) void load(void) {
}

void init(struct rrr_instance_module_data *data) {
	data->private_data = NULL;
	data->module_name = module_name;
	data->type = RRR_MODULE_TYPE_PROCESSOR;
	data->operations = module_operations;
	data->dl_ptr = NULL;
}

void unload(void) {
	RRR_DBG_1 ("Destroy httpclient module\n");
}
