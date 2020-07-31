/*

Read Route Record

Copyright (C) 2020 Atle Solbakken atle@goliathdns.no

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
#include <pthread.h>
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>

#include "../lib/log.h"

#include "../lib/instance_config.h"
#include "../lib/threads.h"
#include "../lib/instances.h"
#include "../lib/messages.h"
#include "../lib/message_broker.h"
#include "../lib/stats/stats_instance.h"
#include "../lib/random.h"
#include "../lib/array.h"
#include "../lib/rrr_strerror.h"
#include "../lib/ip/ip.h"
#include "../lib/message_holder/message_holder.h"
#include "../lib/message_holder/message_holder_struct.h"
#include "../lib/util/linked_list.h"
#include "../lib/util/gnu.h"
#include "../lib/util/rrr_time.h"

// No trailing or leading /
#define RRR_JOURNAL_TOPIC_PREFIX "rrr/journal"
#define RRR_JOURNAL_HOSTNAME_MAX_LEN 256

struct journal_queue_entry {
	RRR_LL_NODE(struct journal_queue_entry);
	uint64_t timestamp;
	struct rrr_array array;
};

struct journal_queue {
	RRR_LL_HEAD(struct journal_queue_entry);
};

struct journal_data {
	struct rrr_instance_thread_data *thread_data;

	int do_generate_test_messages;
	int log_hook_handle;

	pthread_mutex_t delivery_lock;
	struct journal_queue delivery_queue;
	int is_in_hook;
	int error_in_hook;

	uint64_t count_suppressed;
	uint64_t count_total;
	uint64_t count_processed;

	char *hostname;
};

static int journal_queue_entry_new (struct journal_queue_entry **target) {
	struct journal_queue_entry *node = NULL;

	*target = NULL;

	if ((node = malloc(sizeof(*node))) == NULL) {
		RRR_MSG_0("Could not allocate memory in journal_queue_entry_new\n");
		return 1;
	}

	memset(node, '\0', sizeof(*node));

	*target = node;

	return 0;
}

static void journal_queue_entry_destroy (struct journal_queue_entry *node) {
	rrr_array_clear(&node->array);
	free(node);
}

static int journal_data_init(struct journal_data *data, struct rrr_instance_thread_data *thread_data) {

	// memset 0 is done in preload function, DO NOT do that here

	data->thread_data = thread_data;

	return 0;
}

static void journal_data_cleanup(void *arg) {
	struct journal_data *data = (struct journal_data *) arg;

	// DO NOT cleanup delivery_lock here, that is done in a separate function

	RRR_FREE_IF_NOT_NULL(data->hostname);

	pthread_mutex_lock(&data->delivery_lock);
	RRR_LL_DESTROY(&data->delivery_queue, struct journal_queue_entry, journal_queue_entry_destroy(node));
	pthread_mutex_unlock(&data->delivery_lock);
}

static int journal_parse_config (struct journal_data *data, struct rrr_instance_config *config) {
	int ret = 0;

	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_YESNO("journal_generate_test_messages", do_generate_test_messages, 0);
	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_UTF8_DEFAULT_NULL("journal_hostname", hostname);

	if (data->hostname == NULL || *(data->hostname) == '\0') {
		char hostname[RRR_JOURNAL_HOSTNAME_MAX_LEN+1];
		int ret = 0;
		if ((ret = gethostname(hostname, sizeof(hostname))) != 0) {
			RRR_MSG_0("Could not get system hostname in journal instance %s: %s\n",
					INSTANCE_D_NAME(data->thread_data), rrr_strerror(errno));
			ret = 1;
			goto out;
		}

		RRR_FREE_IF_NOT_NULL(data->hostname);
		if ((data->hostname = strdup(hostname)) == NULL) {
			RRR_MSG_0("Could not allocate memory for hostname in journal_parse_config\n");
			ret = 1;
			goto out;
		}
	}

	out:
	return ret;
}

// Lock must be initialized before other locks start to provide correct memory fence
static int journal_preload (struct rrr_thread *thread) {
	struct rrr_instance_thread_data *thread_data = thread->private_data;
	struct journal_data *data = thread_data->private_data = thread_data->private_memory;

	int ret = 0;

	memset(data, '\0', sizeof(*data));

	pthread_mutexattr_t attr;
	if (pthread_mutexattr_init(&attr) != 0) {
		RRR_MSG_0("Could not initialize mutexattr in journal_preload\n");
		ret = 1;
		goto out;
	}

	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);

	if (pthread_mutex_init(&data->delivery_lock, &attr) != 0) {
		RRR_MSG_0("Could not initialize lock in journal_preload\n");
		ret = 1;
		goto out_cleanup_attr;
	}

	out_cleanup_attr:
		pthread_mutexattr_destroy(&attr);
	out:
		return ret;
}

// Note : Context here is ANY thread
static void journal_log_hook (
		unsigned short loglevel_translated,
		const char *prefix,
		const char *message,
		void *private_arg
) {
	struct journal_data *data = private_arg;

	// This is a recursive lock
	pthread_mutex_lock(&data->delivery_lock);

	struct journal_queue_entry *entry = NULL;

	data->count_total++;

	if (	rrr_config_global.debuglevel != 0 &&
			rrr_config_global.debuglevel != RRR_DEBUGLEVEL_1 &&
			loglevel_translated > RRR_RFC5424_LOGLEVEL_ERROR
	) {
		// These messages must be suppressed to avoid generating new messages when processing log
		// messages created in this module
		data->count_suppressed++;
		goto out_unlock;
	}

	// In case of errors printed by the functions below, prevent recursion
	if (data->is_in_hook) {
		data->count_suppressed++;
		goto out_unlock;
	}

	data->count_processed++;

	data->is_in_hook = 1;

	if ((journal_queue_entry_new(&entry)) != 0) {
		goto out_unlock;
	}

	int ret = 0;
	ret |= rrr_array_push_value_u64_with_tag(&entry->array, "log_level_translated", loglevel_translated);
	ret |= rrr_array_push_value_str_with_tag(&entry->array, "log_prefix", prefix);
	ret |= rrr_array_push_value_str_with_tag(&entry->array, "log_message", message);

	if (ret != 0) {
		// Set error flag and leave is_in_hook set to prevent more errors before the threads exit
		data->error_in_hook = 1;
		goto out_unlock;
	}

	entry->timestamp = rrr_time_get_64();

	RRR_LL_APPEND(&data->delivery_queue, entry);
	entry = NULL;

	data->is_in_hook = 0;

	out_unlock:
		if (entry != NULL) {
			journal_queue_entry_destroy(entry);
		}
		pthread_mutex_unlock(&data->delivery_lock);
		return;
}

static int journal_write_message_callback (struct rrr_message_holder *entry, void *arg) {
	struct journal_data *data = arg;

	int ret = 0;

	char *topic_tmp = NULL;
	char *topic_tmp_final = NULL;
	struct rrr_message *reading = NULL;
	struct journal_queue_entry *queue_entry = NULL;

	pthread_mutex_lock (&data->delivery_lock);

	queue_entry = RRR_LL_SHIFT(&data->delivery_queue);

	if (queue_entry == NULL) {
		ret = RRR_MESSAGE_BROKER_DROP;
		goto out;
	}

	if (rrr_array_push_value_str_with_tag(&queue_entry->array, "log_hostname", data->hostname) != 0) {
		RRR_MSG_0("Could not push hostname to message in journal_write_message_callback of instance %s\n",
				INSTANCE_D_NAME(data->thread_data));
		ret = RRR_MESSAGE_BROKER_ERR;
		goto out;
	}

	struct rrr_type_value *prefix_value = rrr_array_value_get_by_tag(&queue_entry->array, "log_prefix");

	if (prefix_value == NULL || !RRR_TYPE_IS_STR_EXCACT(prefix_value->definition->type)) {
		RRR_BUG("BUG: log_prefix not set or of wrong type in journal_write_message_callback\n");
	}

	if (rrr_type_definition_str.to_str(&topic_tmp, prefix_value) != 0) {
		RRR_MSG_0("Could not get string from log prefix in journal_write_message_callback\n");
		ret = RRR_MESSAGE_BROKER_ERR;
		goto out;
	}

	if (rrr_asprintf(&topic_tmp_final, "%s/%s", RRR_JOURNAL_TOPIC_PREFIX, topic_tmp) < 0) {
		RRR_MSG_0("Could not allocate memory for prefix in journal_write_message_callback\n");
		ret = RRR_MESSAGE_BROKER_ERR;
		goto out;
	}

//	printf ("topic: %s\n", topic_tmp_final);

	if (rrr_array_new_message_from_collection (
				&reading,
				&queue_entry->array,
				queue_entry->timestamp,
				topic_tmp_final,
				strlen(topic_tmp_final)
	) != 0) {
		RRR_MSG_0("Could create new message in journal_write_message_callback\n");
		ret = RRR_MESSAGE_BROKER_ERR;
		goto out;
	}

	entry->message = reading;
	entry->data_length = MSG_TOTAL_SIZE(reading);

	reading = NULL;

	if (RRR_LL_COUNT(&data->delivery_queue) > 0) {
		ret = RRR_MESSAGE_BROKER_AGAIN;
	}

	out:
	if (queue_entry != NULL) {
		journal_queue_entry_destroy(queue_entry);
	}
	pthread_mutex_unlock (&data->delivery_lock);
	RRR_FREE_IF_NOT_NULL(topic_tmp_final);
	RRR_FREE_IF_NOT_NULL(topic_tmp);
	RRR_FREE_IF_NOT_NULL(reading);
	rrr_message_holder_unlock(entry);
	return ret;
}

static void journal_unregister_handle(void *arg) {
	struct journal_data *data = (struct journal_data *) arg;
	if (data->log_hook_handle != 0) {
		rrr_log_hook_unregister(data->log_hook_handle);
		data->log_hook_handle = 0;
	}
}

static void journal_delivery_lock_cleanup(void *arg) {
	struct journal_data *data = (struct journal_data *) arg;
	pthread_mutex_destroy(&data->delivery_lock);
}

static void *thread_entry_journal (struct rrr_thread *thread) {
	struct rrr_instance_thread_data *thread_data = thread->private_data;
	struct journal_data *data = thread_data->private_data = thread_data->private_memory;

	// This cleanup must happen after the hook is unregistered
	pthread_cleanup_push(journal_delivery_lock_cleanup, data);

	if (journal_data_init(data, thread_data) != 0) {
		RRR_MSG_0("Could not initialize data in journal instance %s\n", INSTANCE_D_NAME(thread_data));
		pthread_exit(0);
	}

	RRR_DBG_1 ("journal thread data is %p\n", thread_data);

	pthread_cleanup_push(journal_data_cleanup, data);
	pthread_cleanup_push(journal_unregister_handle, data);

	rrr_thread_set_state(thread, RRR_THREAD_STATE_INITIALIZED);
	rrr_thread_signal_wait(thread_data->thread, RRR_THREAD_SIGNAL_START);
	rrr_thread_set_state(thread, RRR_THREAD_STATE_RUNNING);

	if (journal_parse_config(data, thread_data->init_data.instance_config) != 0) {
		RRR_MSG_0("Configuration parse failed for instance %s\n", INSTANCE_D_NAME(thread_data));
		goto out_cleanup;
	}

	rrr_instance_config_check_all_settings_used(thread_data->init_data.instance_config);

	rrr_log_hook_register(&data->log_hook_handle, journal_log_hook, data);

	if (rrr_config_global.debuglevel != 0 && rrr_config_global.debuglevel != RRR_DEBUGLEVEL_1) {
		RRR_DBG_1("Note: journal instance %s will suppress some messages due to debuglevel other than 1 being active\n",
				INSTANCE_D_NAME(thread_data));
	}

	uint64_t time_start = rrr_time_get_64();

	uint64_t prev_suppressed = 0;
	uint64_t prev_total = 0;
	uint64_t prev_processed = 0;

	uint64_t next_test_msg_time = 0;

	while (!rrr_thread_check_encourage_stop(thread_data->thread)) {
		rrr_thread_update_watchdog_time(thread_data->thread);

		if (data->error_in_hook) {
			RRR_MSG_0("Error encountered inside log hook of journal instance %s, exiting\n",
					INSTANCE_D_NAME(thread_data));
			break;
		}

		uint64_t time_now = rrr_time_get_64();

		if (data->do_generate_test_messages) {
			if (time_now > next_test_msg_time) {
				RRR_MSG_1("Log test message from journal instance %s per configuration\n", INSTANCE_D_NAME(thread_data));
				next_test_msg_time = time_now + 1000000; // 1000 ms
			}
		}

		if (rrr_message_broker_write_entry (
				INSTANCE_D_BROKER(thread_data),
				INSTANCE_D_HANDLE(thread_data),
				NULL,
				0,
				0,
				journal_write_message_callback,
				data
		)) {
			RRR_MSG_0("Could not create new message in journal instance %s\n",
					INSTANCE_D_NAME(thread_data));
			break;
		}

		if (time_now - time_start > 1000000) {
			time_start = time_now;
			rrr_stats_instance_update_rate (INSTANCE_D_STATS(thread_data), 0, "processed", data->count_processed - prev_processed);
			rrr_stats_instance_update_rate (INSTANCE_D_STATS(thread_data), 1, "suppressed", data->count_suppressed - prev_suppressed);
			rrr_stats_instance_update_rate (INSTANCE_D_STATS(thread_data), 2, "total", data->count_total - prev_total);

			prev_processed = data->count_processed;
			prev_suppressed = data->count_suppressed;
			prev_total = data->count_total;
		}

		rrr_posix_usleep (50000); // 50 ms
	}

	out_cleanup:
	RRR_DBG_1 ("Thread journal instance %s exiting\n", INSTANCE_D_MODULE_NAME(thread_data));
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);

	pthread_exit(0);
}

static int test_config (struct rrr_instance_config *config) {
	struct journal_data data;
	int ret = 0;
	memset(&data, '\0', sizeof(data));
	ret = journal_parse_config(&data, config);
	journal_data_cleanup(&data);
	return ret;
}

static struct rrr_module_operations module_operations = {
	journal_preload,
	thread_entry_journal,
	NULL,
	test_config,
	NULL,
	NULL
};

static const char *module_name = "journal";

__attribute__((constructor)) void load(void) {
}

void init(struct rrr_instance_dynamic_data *data) {
		data->module_name = module_name;
		data->type = RRR_MODULE_TYPE_SOURCE;
		data->operations = module_operations;
		data->dl_ptr = NULL;
		data->private_data = NULL;
}

void unload(void) {
}
