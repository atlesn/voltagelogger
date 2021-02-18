/*

Read Route Record

Copyright (C) 2019-2021 Atle Solbakken atle@goliathdns.no

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

#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "poll_helper.h"
#include "instances.h"
#include "buffer.h"
#include "message_broker.h"
#include "message_holder/message_holder_struct.h"
#include "message_holder/message_holder.h"
#include "messages/msg_msg.h"

static int __poll_collection_entry_destroy (
		struct rrr_message_broker *message_broker,
		struct rrr_poll_collection_entry *entry
) {
	rrr_message_broker_costumer_decref(message_broker, entry->message_broker_handle);
	free(entry);
	return 0;
}

void rrr_poll_collection_clear (
		struct rrr_message_broker *message_broker,
		struct rrr_poll_collection *collection
) {
	RRR_LL_DESTROY(collection,struct rrr_poll_collection_entry, __poll_collection_entry_destroy(message_broker, node));
}

int rrr_poll_collection_add (
		unsigned int *flags_result,
		struct rrr_poll_collection *collection,
		struct rrr_message_broker *message_broker,
		const char *costumer_name
) {
	int ret = 0;
	*flags_result = 0;

	struct rrr_message_broker_costumer *handle = rrr_message_broker_costumer_find_by_name(message_broker, costumer_name);
	if (handle == NULL) {
		RRR_MSG_0("Could not find message broker costumer '%s' in rrr_poll_collection_add\n", costumer_name);
		ret = RRR_POLL_ERR;
		goto out;
	}

	struct rrr_poll_collection_entry *entry = malloc(sizeof(*entry));
	if (entry == NULL) {
		RRR_MSG_0("Could not allocate memory inn rrr_poll_collection_add\n");
		ret = RRR_POLL_ERR;
		goto out;
	}

	memset(entry, '\0', sizeof(*entry));

	entry->message_broker = message_broker;
	entry->message_broker_handle = handle;

	rrr_message_broker_costumer_incref(message_broker, handle);

	RRR_LL_APPEND(collection, entry);

	out:
	return ret;
}

struct poll_add_from_senders_callback_data {
	struct rrr_message_broker *broker;
	struct rrr_poll_collection *collection;
	struct rrr_instance *faulty_sender;
};

static int __poll_collection_add_from_senders_callback (
		struct rrr_instance *instance,
		void *arg
) {
	int ret = 0;

	struct poll_add_from_senders_callback_data *data = arg;

	unsigned int flags_result;

	if ((ret = rrr_poll_collection_add (
			&flags_result,
			data->collection,
			data->broker,
			INSTANCE_M_NAME(instance)
	)) == RRR_POLL_NOT_FOUND) {
		data->faulty_sender = instance;
		ret = 1;
	}
	else if (ret != 0) {
		RRR_MSG_0("Error while adding senders to collection in __poll_collection_add_from_senders_callback\n");
		ret = 1;
	}

	return ret;
}

int rrr_poll_do_poll_discard (
		int *discarded_count,
		struct rrr_instance_runtime_data *thread_data,
		struct rrr_poll_collection *collection
) {
	int ret = 0;

	(void)(thread_data);

	*discarded_count = 0;

	RRR_LL_ITERATE_BEGIN(collection, struct rrr_poll_collection_entry);
		int ret_tmp;

		struct rrr_poll_collection_entry *entry = node;

		int discarded_count_tmp = 0;

		ret_tmp = rrr_message_broker_poll_discard (
				&discarded_count_tmp,
				entry->message_broker_handle,
				INSTANCE_D_HANDLE(thread_data)
		);

		(*discarded_count) += discarded_count_tmp;

		if (	(ret_tmp & RRR_FIFO_CALLBACK_ERR) ==  RRR_FIFO_CALLBACK_ERR ||
				(ret_tmp & RRR_FIFO_GLOBAL_ERR) == RRR_FIFO_GLOBAL_ERR
		) {
			ret = 1;
			RRR_LL_ITERATE_BREAK();
		}
		else if (ret_tmp != 0) {
			RRR_BUG("BUG: Unknown return value %i when polling in rrr_poll_do_poll_discard\n",
					ret_tmp);
		}
	RRR_LL_ITERATE_END();

	return ret;
}

static int __rrr_poll_intermediate_callback_topic_filter (
		int *does_match,
		struct rrr_instance_runtime_data *thread_data,
		struct rrr_msg_holder *entry
) {
	int ret = 0;

	*does_match = 0;

	if (rrr_msg_msg_topic_match (
			does_match,
			(const struct rrr_msg_msg *) entry->message,
			INSTANCE_D_TOPIC(thread_data)
	) != 0) {
		RRR_MSG_0("Error while matching topic against topic filter while polling in instance %s\n",
				INSTANCE_D_NAME(thread_data));
		ret = RRR_MESSAGE_BROKER_ERR;
		goto out;
	}

	if (RRR_DEBUGLEVEL_3) {
		RRR_DBG_3("Result of topic match while polling in instance %s with topic filter is '%s': %s\n",
				INSTANCE_D_NAME(thread_data),
				INSTANCE_D_TOPIC_STR(thread_data),
				(does_match ? "MATCH" : "MISMATCH/DROPPED")
		);
	}

	out:
	return ret;
}

struct rrr_poll_intermediate_callback_data {
	uint16_t *amount;
	struct rrr_instance_runtime_data *thread_data;
	int (*callback)(RRR_MODULE_POLL_CALLBACK_SIGNATURE);
	void *callback_arg;
	int do_poll_delete;
};

static int __rrr_poll_intermediate_callback (
		RRR_MODULE_POLL_CALLBACK_SIGNATURE
) {
	struct rrr_poll_intermediate_callback_data *callback_data = arg;

	int ret = RRR_MESSAGE_BROKER_OK;

	int does_match = 1;

	if (callback_data->thread_data->init_data.topic_first_token != NULL) {
		if ((ret = __rrr_poll_intermediate_callback_topic_filter(&does_match, callback_data->thread_data, entry)) != 0) {
			goto out;
		}
	}

	if (does_match) {
		// Callback unlocks
		ret = callback_data->callback(entry, callback_data->callback_arg);
		goto out_no_unlock;
	}
	else if (!callback_data->do_poll_delete) {
		ret |= RRR_FIFO_SEARCH_GIVE | RRR_FIFO_SEARCH_FREE;
	}

	out:
		rrr_msg_holder_unlock(entry);
	out_no_unlock:
		if (*callback_data->amount == 0) {
			RRR_BUG("BUG: Amount was 0 in __rrr_poll_do_poll\n");
		}
		if (--(*callback_data->amount) == 0) {
			ret |= RRR_FIFO_SEARCH_STOP;
		}
		return ret;
}

static int __rrr_poll_do_poll (
		uint16_t *amount,
		struct rrr_instance_runtime_data *thread_data,
		struct rrr_poll_collection *collection,
		int (*callback)(RRR_MODULE_POLL_CALLBACK_SIGNATURE),
		void *callback_arg,
		unsigned int wait_milliseconds,
		int do_poll_delete
) {
	int ret = 0;

	struct rrr_poll_intermediate_callback_data callback_data = {
		amount,
		thread_data,
		callback,
		callback_arg,
		do_poll_delete
	};

	if (RRR_LL_COUNT(collection) == 0 && wait_milliseconds > 0) {
		rrr_posix_usleep(wait_milliseconds * 1000);
	}

	RRR_LL_ITERATE_BEGIN(collection, struct rrr_poll_collection_entry);
		int ret_tmp;

		struct rrr_poll_collection_entry *entry = node;

		int message_broker_flags = 0;

		if (!(INSTANCE_D_INSTANCE(thread_data)->misc_flags & RRR_INSTANCE_MISC_OPTIONS_DISABLE_BACKSTOP)) {
			message_broker_flags |= RRR_MESSAGE_BROKER_POLL_F_CHECK_BACKSTOP;
		}

		if (do_poll_delete) {
			ret_tmp = rrr_message_broker_poll_delete (
					entry->message_broker_handle,
					INSTANCE_D_HANDLE(thread_data),
					message_broker_flags,
					__rrr_poll_intermediate_callback,
					&callback_data,
					wait_milliseconds
			);
		}
		else {
			ret_tmp = rrr_message_broker_poll (
					entry->message_broker_handle,
					INSTANCE_D_HANDLE(thread_data),
					message_broker_flags,
					__rrr_poll_intermediate_callback,
					&callback_data,
					wait_milliseconds
			);
		}

		if ( (ret_tmp & RRR_FIFO_CALLBACK_ERR) ||
		     (ret_tmp & RRR_FIFO_GLOBAL_ERR)
		) {
			ret = 1;
			RRR_LL_ITERATE_BREAK();
		}
		else if (ret_tmp != 0) {
			RRR_BUG("BUG: Unknown return value %i when polling in rrr_poll_do_poll_delete\n",
					ret_tmp);
		}

		if (*amount == 0) {
			RRR_LL_ITERATE_BREAK();
		}
	RRR_LL_ITERATE_END();

	return ret;
}

int rrr_poll_do_poll_delete (
		uint16_t *amount,
		struct rrr_instance_runtime_data *thread_data,
		struct rrr_poll_collection *collection,
		int (*callback)(RRR_MODULE_POLL_CALLBACK_SIGNATURE),
		unsigned int wait_milliseconds
) {
	return __rrr_poll_do_poll (amount, thread_data, collection, callback, thread_data, wait_milliseconds, 1);
}

int rrr_poll_do_poll_search (
		uint16_t *amount,
		struct rrr_instance_runtime_data *thread_data,
		struct rrr_poll_collection *collection,
		int (*callback)(RRR_MODULE_POLL_CALLBACK_SIGNATURE),
		void *callback_arg,
		unsigned int wait_milliseconds
) {
	return __rrr_poll_do_poll (amount, thread_data, collection, callback, callback_arg, wait_milliseconds, 0);
}

int rrr_poll_collection_count (
		struct rrr_poll_collection *collection
) {
	return collection->node_count;
}

int rrr_poll_add_from_thread_senders (
		struct rrr_instance **faulty_sender,
		struct rrr_poll_collection *collection,
		struct rrr_instance_runtime_data *thread_data
) {
	int ret = 0;

	*faulty_sender = NULL;

	struct poll_add_from_senders_callback_data callback_data = {
			INSTANCE_D_BROKER(thread_data),
			collection,
			NULL
	};

	ret = rrr_instance_friend_collection_iterate (
			thread_data->init_data.senders,
			__poll_collection_add_from_senders_callback,
			&callback_data
	);

	if (ret != 0) {
		*faulty_sender = callback_data.faulty_sender;
	}

	return ret;
}

