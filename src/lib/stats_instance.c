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

#include <stddef.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>

#include "stats_engine.h"
#include "stats_instance.h"
#include "stats_message.h"
#include "../global.h"

#define RRR_INSTANCE_POST_ARGUMENTS									\
	struct rrr_stats_instance *instance,							\
	const char *path_postfix,										\
	int sticky

int rrr_stats_instance_new (struct rrr_stats_instance **result, struct rrr_stats_engine *engine, const char *name) {
	int ret = 0;
	*result = NULL;

	struct rrr_stats_instance *instance = malloc(sizeof(*instance));
	if (instance == NULL) {
		VL_MSG_ERR("Could not allocate memory in rrr_stats_instance_new\n");
		ret = 1;
		goto out;
	}

	memset(instance, '\0', sizeof(*instance));

	if (pthread_mutex_init(&instance->lock, 0) != 0) {
		VL_MSG_ERR("Could not initialize mutex in  rrr_stats_instance_new\n");
		ret = 1;
		goto out_free;
	}

	if ((instance->name = strdup(name)) == NULL) {
		VL_MSG_ERR("Could not save instance name in rrr_stats_instance_new\n");
		ret = 1;
		goto out_destroy_mutex;
	}

	if (rrr_stats_engine_handle_obtain(&instance->stats_handle, engine) != 0) {
		VL_MSG_ERR("Could not obtain stats handle in rrr_stats_instance_new\n");
		ret = 1;
		goto out_free_name;
	}

	instance->engine = engine;

	*result = instance;
	goto out;

	out_free_name:
		RRR_FREE_IF_NOT_NULL(instance->name);
	out_destroy_mutex:
		pthread_mutex_destroy(&instance->lock);
	out_free:
		RRR_FREE_IF_NOT_NULL(instance);

	out:
	return ret;
}

void rrr_stats_instance_destroy (struct rrr_stats_instance *instance) {
	rrr_stats_engine_handle_unregister(instance->engine, instance->stats_handle);
	RRR_FREE_IF_NOT_NULL(instance->name);
	pthread_mutex_destroy(&instance->lock);
	free(instance);
}

void rrr_stats_instance_destroy_void (void *instance) {
	rrr_stats_instance_destroy(instance);
}

int rrr_stats_instance_post_text (
		RRR_INSTANCE_POST_ARGUMENTS,
		const char *text
) {
	int ret = 0;
	struct rrr_stats_message message;

	if (rrr_stats_message_init (
			&message,
			RRR_STATS_MESSAGE_TYPE_TEXT,
			(sticky != 0 ? RRR_STATS_MESSAGE_FLAGS_STICKY : 0),
			path_postfix,
			text,
			strlen(text) +1
	) != 0) {
		VL_MSG_ERR("Could not initialize statistics message in rrr_stats_message_post_text\n");
		ret = 1;
		goto out;
	}

	strcpy(message.data, text);

	if ((ret = rrr_stats_engine_post_message(instance->engine, instance->stats_handle, &message)) != 0) {
		VL_MSG_ERR("Error returned from post function in rrr_stats_message_post_text\n");
		ret = 1;
		goto out;
	}

	out:
	return ret;
}
