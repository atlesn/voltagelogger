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

#ifndef RRR_EVENT_H
#define RRR_EVENT_H

#include <stdint.h>
#include <pthread.h>

#define RRR_EVENT_FUNCTION_ARGS \
	uint16_t *amount, uint8_t flags, void *arg

#define RRR_EVENT_FUNCTION_PERIODIC_ARGS \
	void *arg

struct rrr_event {
	uint8_t function;
	uint8_t flags;
	uint16_t amount;
};

struct rrr_event_queue {
	struct rrr_event queue[0xff];
	int (*functions[0xff])(RRR_EVENT_FUNCTION_ARGS);
	uint8_t queue_rpos;
	uint8_t queue_wpos;
};

void rrr_event_function_set (
		struct rrr_event_queue *handle,
		uint8_t code,
		int (*function)(RRR_EVENT_FUNCTION_ARGS)
);
int rrr_event_dispatch (
		struct rrr_event_queue *queue,
		pthread_mutex_t *mutex,
		pthread_cond_t *cond,
		int (*function_periodic)(RRR_EVENT_FUNCTION_PERIODIC_ARGS),
		void *arg
);
void rrr_event_pass (
		struct rrr_event_queue *queue,
		pthread_mutex_t *mutex,
		pthread_cond_t *cond,
		uint8_t function,
		uint8_t flags,
		uint16_t amount
);

#endif /* RRR_EVENT_H */
