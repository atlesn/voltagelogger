/*

Read Route Record

Copyright (C) 2019 Atle Solbakken atle@goliathdns.no

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

#ifndef RRR_MQTT_BROKER_H
#define RRR_MQTT_BROKER_H

#include <netinet/in.h>
#include <sys/socket.h>
#include <inttypes.h>
#include <pthread.h>

#include "ip.h"
#include "mqtt_common.h"

#define RRR_MQTT_BROKER_CLIENT_PREFIX "mqtt-client-"
#define RRR_MQTT_MAX_GENERATED_CLIENT_IDS 65535

struct rrr_mqtt_listen_fd {
	struct rrr_mqtt_listen_fd *next;
	struct ip_data ip;
};

struct rrr_mqtt_listen_fd_collection {
	struct rrr_mqtt_listen_fd *first;
	pthread_mutex_t lock;
};

struct rrr_mqtt_broker_data {
	/* MUST be first */
	struct rrr_mqtt_data mqtt_data;

	struct rrr_mqtt_listen_fd_collection listen_fds;

	pthread_mutex_t client_serial_lock;
	uint32_t client_serial;
};

int rrr_mqtt_broker_accept_connections (struct rrr_mqtt_broker_data *data);
void rrr_mqtt_broker_destroy (struct rrr_mqtt_broker_data *broker);
int rrr_mqtt_broker_new (struct rrr_mqtt_broker_data **broker, const char *client_name);
int rrr_mqtt_broker_listen_ipv4_and_ipv6 (
		struct rrr_mqtt_broker_data *broker,
		int port,
		int max_connections
);
void rrr_mqtt_broker_stop_listening (struct rrr_mqtt_broker_data *broker);

/* Run all tasks in sequence, simply call repeatedly for non-threaded operation */
int rrr_mqtt_broker_synchronized_tick (struct rrr_mqtt_broker_data *data);

#endif /* RRR_MQTT_BROKER_H */