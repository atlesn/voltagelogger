/*

Voltage Logger

Copyright (C) 2018 Atle Solbakken atle@goliathdns.no

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
#include <bdl/bdl.h>

#include "../modules.h"
#include "../lib/messages.h"
#include "../lib/threads.h"
#include "../lib/buffer.h"

// Should not be smaller than module max
#define VL_BLOCKDEV_MAX_SENDERS VL_MODULE_MAX_SENDERS

void poll_callback(void *caller_data, char *data, unsigned long int size) {
	struct module_thread_data *thread_data = caller_data;
	struct vl_message *reading = (struct vl_message *) data;
	printf ("blockdev: Result from buffer: %s measurement %" PRIu64 " size %lu\n", reading->data, reading->data_numeric, size);
	free(data);
}

static void *thread_entry_blockdev(struct vl_thread_start_data *start_data) {
	struct module_thread_data *thread_data = start_data->private_arg;
	thread_data->thread = start_data->thread;
	unsigned long int senders_count = thread_data->senders_count;

	printf ("blockdev thread data is %p\n", thread_data);

	pthread_cleanup_push(thread_set_stopping, start_data->thread);

	thread_set_state(start_data->thread, VL_THREAD_STATE_RUNNING);

	if (senders_count > VL_BLOCKDEV_MAX_SENDERS) {
		fprintf (stderr, "Too many senders for blockdev module, max is %i\n", VL_BLOCKDEV_MAX_SENDERS);
		goto out_message;
	}

	int (*poll[VL_BLOCKDEV_MAX_SENDERS])(struct module_thread_data *data, void (*callback)(void *caller_data, char *data, unsigned long int size), struct module_thread_data *caller_data);


	for (int i = 0; i < senders_count; i++) {
		printf ("blockdev: found sender %p\n", thread_data->senders[i]);
		poll[i] = thread_data->senders[i]->module->operations.poll_delete;

		if (poll[i] == NULL) {
			fprintf (stderr, "blockdev cannot use this sender, lacking poll delete function.\n");
			goto out_message;
		}
	}

	printf ("blockdev started thread %p\n", thread_data);
	if (senders_count == 0) {
		fprintf (stderr, "Error: Sender was not set for blockdev processor module\n");
		goto out_message;
	}


	for (int i = 0; i < senders_count; i++) {
		while (thread_get_state(thread_data->senders[i]->thread) != VL_THREAD_STATE_RUNNING && thread_check_encourage_stop(thread_data->thread) != 1) {
			update_watchdog_time(thread_data->thread);
			printf ("blockdev: Waiting for source thread to become ready\n");
			usleep (5000);
		}
	}

	while (thread_check_encourage_stop(thread_data->thread) != 1) {
		update_watchdog_time(thread_data->thread);

		int err = 0;

		printf ("blockdev polling data\n");
		for (int i = 0; i < senders_count; i++) {
			int res = poll[i](thread_data->senders[i], poll_callback, thread_data);
			if (!(res >= 0)) {
				printf ("blockdev module received error from poll function\n");
				err = 1;
				break;
			}
		}

		if (err != 0) {
			break;
		}
		usleep (1249000); // 1249 ms
	}

	out_message:
	printf ("Thread blockdev %p exiting\n", thread_data->thread);

	out:
	pthread_cleanup_pop(1);
	pthread_exit(0);
}

static struct module_operations module_operations = {
		thread_entry_blockdev,
		NULL,
		NULL,
		NULL
};

static const char *module_name = "blockdev";

__attribute__((constructor)) void load() {
}

void init(struct module_dynamic_data *data) {
	data->private_data = NULL;
	data->name = module_name;
	data->type = VL_MODULE_TYPE_PROCESSOR;
	data->operations = module_operations;
	data->dl_ptr = NULL;
}

void unload(struct module_dynamic_data *data) {
	printf ("Destroy blockdev module\n");
}

