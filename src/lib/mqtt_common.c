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

#include <stdlib.h>
#include <string.h>

#include "ip.h"
#include "mqtt_common.h"
#include "mqtt_connection.h"
#include "mqtt_session.h"

void rrr_mqtt_common_data_destroy (struct rrr_mqtt_data *data) {
	if (data == NULL) {
		return;
	}

	if (data->connections.invalid == 0) {
		rrr_mqtt_connection_collection_destroy(&data->connections);
	}

	if (data->sessions != NULL) {
		data->sessions->methods->destroy(data->sessions);
	}

	*(data->client_name) = '\0';
	data->handler_properties = NULL;
}

static int __rrr_mqtt_common_connection_event_handler (
		struct rrr_mqtt_connection *connection,
		int event,
		void *arg
) {
	struct rrr_mqtt_data *data = arg;

	int ret = 0;
	int ret_tmp = 0;

	// session is NULL for instance after parsing CONNECT packet
	if (connection->session == NULL) {
		goto out;
	}

	switch (event) {
		case RRR_MQTT_CONNECTION_EVENT_DISCONNECT:

			break;
		case RRR_MQTT_CONNECTION_EVENT_PACKET_PARSED:
			ret_tmp = MQTT_COMMON_CALL_SESSION_HEARTBEAT(data, connection->session);
			break;
		default:
			VL_BUG("Unknown event %i in __rrr_mqtt_common_connection_event_handler\n", event);
	}

	if (ret_tmp != 0) {
		if ((ret_tmp & RRR_MQTT_SESSION_DELETED) != 0) {
			// It is normal to return DELETED from disconnect event
			if (event != RRR_MQTT_CONNECTION_EVENT_DISCONNECT) {
				VL_MSG_ERR("Session was deleted while calling session storage engine in __rrr_mqtt_common_connection_event_handler with event %i\n", event);
			}
			ret |= RRR_MQTT_CONNECTION_DESTROY_CONNECTION;
		}
		if ((ret_tmp & RRR_MQTT_SESSION_ERROR) != 0) {
			VL_MSG_ERR("Session error while calling session storage engine in __rrr_mqtt_common_connection_event_handler with event %i\n", event);
			ret |= RRR_MQTT_CONNECTION_SOFT_ERROR;
		}

		ret_tmp = ret_tmp & ~(RRR_MQTT_SESSION_ERROR|RRR_MQTT_SESSION_DELETED);

		if (ret_tmp != 0) {
			VL_MSG_ERR("Internal error while calling session storage engine in __rrr_mqtt_common_connection_event_handler with event %i return was %i\n",
					event, ret_tmp);
			ret |= RRR_MQTT_CONNECTION_INTERNAL_ERROR;
			goto out;
		}
	}

	// Call downstream event handler (broker/client)
	ret_tmp = data->event_handler(connection, event, data->event_handler_arg);
	if (ret_tmp != 0) {
		if ((ret_tmp & RRR_MQTT_CONNECTION_SOFT_ERROR) != 0) {
			ret |= RRR_MQTT_CONNECTION_SOFT_ERROR;
		}
		if ((ret_tmp & RRR_MQTT_CONNECTION_DESTROY_CONNECTION) != 0) {
			ret |= RRR_MQTT_CONNECTION_DESTROY_CONNECTION;
		}

		ret_tmp = ret_tmp & ~(RRR_MQTT_CONNECTION_SOFT_ERROR|RRR_MQTT_CONNECTION_DESTROY_CONNECTION);

		if (ret_tmp != 0) {
			VL_MSG_ERR("Internal error while calling downstream event handler in __rrr_mqtt_common_connection_event_handler with event %i return was %i\n",
					event, ret_tmp);
			ret |= RRR_MQTT_CONNECTION_INTERNAL_ERROR;
			goto out;
		}
	}

	out:
	return ret;
}

int rrr_mqtt_common_data_init (struct rrr_mqtt_data *data,
		const char *client_name,
		const struct rrr_mqtt_type_handler_properties *handler_properties,
		int (*session_initializer)(struct rrr_mqtt_session_collection **sessions, void *arg),
		void *session_initializer_arg,
		int (*event_handler)(struct rrr_mqtt_connection *connection, int event, void *arg),
		void *event_handler_arg,
		uint64_t close_wait_time_usec,
		int max_socket_connections
) {
	int ret = 0;

	memset (data, '\0', sizeof(*data));

	if (strlen(client_name) > RRR_MQTT_DATA_CLIENT_NAME_LENGTH) {
		VL_MSG_ERR("Client name was too long in rrr_mqtt_data_init\n");
		ret = 1;
		goto out;
	}

	data->event_handler = event_handler;
	data->event_handler_arg = event_handler_arg;
	data->close_wait_time_usec = close_wait_time_usec;
	data->handler_properties = handler_properties;
	strcpy(data->client_name, client_name);

	if (rrr_mqtt_connection_collection_init (
			&data->connections,
			max_socket_connections,
			__rrr_mqtt_common_connection_event_handler,
			data
	) != 0) {
		VL_MSG_ERR("Could not initialize connection collection in rrr_mqtt_data_new\n");
		ret = 1;
		goto out;
	}

	if (session_initializer (&data->sessions, session_initializer_arg) != 0) {
		VL_MSG_ERR("Could not initialize session data in rrr_mqtt_data_new\n");
		ret = 1;
		goto out_destroy_connections;
	}

	goto out;

	out_destroy_connections:
		rrr_mqtt_connection_collection_destroy(&data->connections);

	out:
		return ret;
}

int rrr_mqtt_common_data_register_connection (
		struct rrr_mqtt_data *data,
		const struct ip_accept_data *accept_data
) {
	int ret = 0;

	struct rrr_mqtt_connection *connection;

	ret = rrr_mqtt_connection_collection_new_connection (
			&connection,
			&data->connections,
			&accept_data->ip_data,
			&accept_data->addr,
			data->close_wait_time_usec
	);

	return ret;
}

struct handle_packets_callback {
	struct rrr_mqtt_data *data;
	struct rrr_mqtt_connection *connection;
	int handler_return;
};

static int __rrr_mqtt_connection_handle_packets_callback (struct fifo_callback_args *callback_data, char *data, unsigned long int size) {
	// Remember to ALWAYS return FIFO_SEARCH_FREE
	int ret = FIFO_SEARCH_FREE;

	(void)(size);

	struct handle_packets_callback *handle_packets_data = callback_data->private_data;
	struct rrr_mqtt_data *mqtt_data = handle_packets_data->data;
	struct rrr_mqtt_connection *connection = handle_packets_data->connection;
	struct rrr_mqtt_p_packet *packet = (struct rrr_mqtt_p_packet *) data;

	if (RRR_MQTT_P_GET_TYPE(packet) == RRR_MQTT_P_TYPE_CONNECT) {
		if (!RRR_MQTT_CONNECTION_STATE_RECEIVE_CONNECT_IS_ALLOWED(connection)) {
			VL_MSG_ERR("Received a CONNECT packet while not allowed in __rrr_mqtt_connection_handle_packets_callback\n");
			ret |= FIFO_CALLBACK_ERR|FIFO_SEARCH_STOP;
			goto out;
		}
	}
	else if (RRR_MQTT_P_GET_TYPE(packet) == RRR_MQTT_P_TYPE_CONNACK) {
		if (!RRR_MQTT_CONNECTION_STATE_RECEIVE_CONNACK_IS_ALLOWED(connection)) {
			VL_MSG_ERR("Received a CONNACK packet while not allowed in __rrr_mqtt_connection_handle_packets_callback\n");
			ret |= FIFO_CALLBACK_ERR|FIFO_SEARCH_STOP;
			goto out;
		}
	}
	else if (!RRR_MQTT_CONNECTION_STATE_RECEIVE_ANY_IS_ALLOWED(connection)) {
		VL_MSG_ERR("Received a %s packet while only CONNECT was allowed in __rrr_mqtt_connection_handle_packets_callback\n",
				RRR_MQTT_P_GET_TYPE_NAME(packet));
		ret |= FIFO_CALLBACK_ERR|FIFO_SEARCH_STOP;
		goto out;
	}

	if (mqtt_data->handler_properties[RRR_MQTT_P_GET_TYPE(packet)].handler == NULL) {
		VL_MSG_ERR("No handler specified for packet type %i\n", RRR_MQTT_P_GET_TYPE(packet));
		ret |= FIFO_CALLBACK_ERR|FIFO_SEARCH_STOP;
		goto out;
	}

	VL_DEBUG_MSG_3 ("Handling packet of type %s\n", RRR_MQTT_P_GET_TYPE_NAME(packet));
	int tmp = mqtt_data->handler_properties[RRR_MQTT_P_GET_TYPE(packet)].handler(mqtt_data, connection, packet);

	if (tmp != RRR_MQTT_CONNECTION_OK) {
		if ((tmp & RRR_MQTT_CONNECTION_DESTROY_CONNECTION) != 0) {
			handle_packets_data->handler_return = RRR_MQTT_CONNECTION_DESTROY_CONNECTION;
			ret |= FIFO_CALLBACK_ERR | FIFO_SEARCH_STOP;
		}
		if ((tmp & RRR_MQTT_CONNECTION_SOFT_ERROR) != 0) {
			handle_packets_data->handler_return = RRR_MQTT_CONNECTION_SOFT_ERROR;
			ret |= FIFO_CALLBACK_ERR | FIFO_SEARCH_STOP;
		}

		tmp = tmp & ~(RRR_MQTT_CONNECTION_SOFT_ERROR|RRR_MQTT_CONNECTION_DESTROY_CONNECTION);
		if (tmp != 0) {
			handle_packets_data->handler_return = RRR_MQTT_CONNECTION_INTERNAL_ERROR;
			ret |=  FIFO_CALLBACK_ERR | FIFO_SEARCH_STOP;
		}
	}

	out:
	return ret | FIFO_SEARCH_FREE;
}

static int __rrr_mqtt_common_handle_packets (
		struct rrr_mqtt_connection *connection,
		void *arg
) {
	int ret = RRR_MQTT_CONNECTION_OK;

	/* There can be multiple parse threads, make sure we do not block */
	if (RRR_MQTT_CONNECTION_TRYLOCK(connection) != 0) {
		ret = RRR_MQTT_CONNECTION_BUSY;
		goto out_nolock;
	}

	if (	!RRR_MQTT_CONNECTION_STATE_RECEIVE_ANY_IS_ALLOWED(connection) &&
			!RRR_MQTT_CONNECTION_STATE_RECEIVE_CONNECT_IS_ALLOWED(connection) &&
			!RRR_MQTT_CONNECTION_STATE_RECEIVE_CONNACK_IS_ALLOWED(connection)
	) {
		ret = RRR_MQTT_CONNECTION_BUSY;
		goto out;
	}

	struct rrr_mqtt_data *data = arg;

	struct handle_packets_callback callback_data = {
			data, connection, RRR_MQTT_CONNECTION_OK
	};

	struct fifo_callback_args fifo_callback_data = {
			NULL, &callback_data, 0
	};

	ret = fifo_read_clear_forward (
			&connection->receive_queue.buffer,
			NULL,
			__rrr_mqtt_connection_handle_packets_callback,
			&fifo_callback_data,
			0
	);

	if (ret == FIFO_GLOBAL_ERR) {
		VL_MSG_ERR("Buffer error while handling mqtt packets from client, must exit.\n");
		ret = RRR_MQTT_CONNECTION_INTERNAL_ERROR;
		goto out;
	}
	else if (ret != FIFO_OK) {
		ret = callback_data.handler_return;
		if ((ret & RRR_MQTT_CONNECTION_SOFT_ERROR) != 0) {
			VL_MSG_ERR("Soft error while handling packets from mqtt client, destroying connection.\n");
			// Always set DESTROY on SOFT ERROR
			ret |= RRR_MQTT_CONNECTION_DESTROY_CONNECTION|RRR_MQTT_CONNECTION_SOFT_ERROR;
		}

		int ret_old = ret;

		ret = ret & ~(RRR_MQTT_CONNECTION_SOFT_ERROR|RRR_MQTT_CONNECTION_DESTROY_CONNECTION);
		if (ret != 0) {
			VL_MSG_ERR("Internal error while handling packets from mqtt client, must exit. Return is %i.\n", ret);
			ret = RRR_MQTT_CONNECTION_INTERNAL_ERROR;
			goto out;
		}

		ret |= ret_old;
	}

	out:
	RRR_MQTT_CONNECTION_UNLOCK(connection);

	out_nolock:
	return ret;
}

static int __rrr_mqtt_common_read_and_parse (
		struct rrr_mqtt_connection *connection,
		void *arg
) {
	int ret = RRR_MQTT_CONNECTION_OK;

	struct rrr_mqtt_data *data = arg;
	(void)(data);

	if (RRR_MQTT_CONNECTION_STATE_IS_DISCONNECTED_OR_DISCONNECT_WAIT(connection)) {
		goto out;
	}

	// Do not block while reading a large message, read only 4K each time. This also
	// goes for threaded reading, the connection lock must be released often to allow
	// for other iterators to check stuff.
	ret = rrr_mqtt_connection_iterator_ctx_read (connection, RRR_MQTT_SYNCHRONIZED_READ_STEP_MAX_SIZE);
	if ((ret & RRR_MQTT_CONNECTION_INTERNAL_ERROR) != 0) {
		VL_MSG_ERR("Internal error while reading data from mqtt client. Closing down server.\n");
		ret =  RRR_MQTT_CONNECTION_INTERNAL_ERROR;
		goto out;
	}
	if ((ret & (RRR_MQTT_CONNECTION_DESTROY_CONNECTION|RRR_MQTT_CONNECTION_SOFT_ERROR)) != 0) {
		VL_MSG_ERR("Error while reading data from mqtt client, destroying connection.\n");
		ret = RRR_MQTT_CONNECTION_DESTROY_CONNECTION|RRR_MQTT_CONNECTION_SOFT_ERROR;
		goto out;
	}

	ret = rrr_mqtt_connection_iterator_ctx_parse (connection);
	if ((ret & RRR_MQTT_CONNECTION_INTERNAL_ERROR) != 0) {
		VL_MSG_ERR("Internal error while parsing data from mqtt client. Closing down server.\n");
		ret =  RRR_MQTT_CONNECTION_INTERNAL_ERROR;
		goto out;
	}
	if ((ret & (RRR_MQTT_CONNECTION_DESTROY_CONNECTION|RRR_MQTT_CONNECTION_SOFT_ERROR)) != 0) {
		VL_MSG_ERR("Error while parsing data from mqtt client, destroying connection.\n");
		ret = RRR_MQTT_CONNECTION_DESTROY_CONNECTION|RRR_MQTT_CONNECTION_SOFT_ERROR;
		goto out;
	}

	ret = rrr_mqtt_connection_iterator_ctx_check_finalize (connection);
	if ((ret & RRR_MQTT_CONNECTION_INTERNAL_ERROR) != 0) {
		VL_MSG_ERR("Internal error while finalizing data from mqtt client. Closing down server.\n");
		ret =  RRR_MQTT_CONNECTION_INTERNAL_ERROR;
		goto out;
	}
	if ((ret & (RRR_MQTT_CONNECTION_DESTROY_CONNECTION|RRR_MQTT_CONNECTION_SOFT_ERROR)) != 0) {
		VL_MSG_ERR("Error while finalizing data from mqtt client, destroying connection.\n");
		ret = RRR_MQTT_CONNECTION_DESTROY_CONNECTION|RRR_MQTT_CONNECTION_SOFT_ERROR;
		goto out;
	}

	out:
	return ret;
}

static int __rrr_mqtt_common_send (
		struct rrr_mqtt_connection *connection,
		void *arg
) {
	int ret = RRR_MQTT_CONNECTION_OK;

	struct rrr_mqtt_data *data = arg;

	(void)(data);

	/* There can be multiple parse threads, make sure we do not block */
	if (RRR_MQTT_CONNECTION_TRYLOCK(connection) != 0) {
		ret = RRR_MQTT_CONNECTION_BUSY;
		goto out_nolock;
	}

	ret = rrr_mqtt_connection_iterator_ctx_send_packets(connection);

	RRR_MQTT_CONNECTION_UNLOCK(connection);
	out_nolock:

	return ret;
}


int rrr_mqtt_common_read_parse_handle (struct rrr_mqtt_data *data) {
	int ret = 0;

	ret = rrr_mqtt_connection_collection_iterate(&data->connections, __rrr_mqtt_common_read_and_parse, data);

	if ((ret & (RRR_MQTT_CONNECTION_SOFT_ERROR|RRR_MQTT_CONNECTION_DESTROY_CONNECTION)) != 0) {
		VL_MSG_ERR("Soft error in rrr_mqtt_common_read_parse_handle (one or more connections had to be closed)\n");
		ret = 0;
	}
	if ((ret & RRR_MQTT_CONNECTION_INTERNAL_ERROR) != 0) {
		VL_MSG_ERR("Internal error received in rrr_mqtt_common_read_parse_handle while reading and parsing\n");
		ret = 1;
		goto out;
	}

	ret = rrr_mqtt_connection_collection_iterate(&data->connections, __rrr_mqtt_common_handle_packets, data);

	if ((ret & (RRR_MQTT_CONNECTION_SOFT_ERROR|RRR_MQTT_CONNECTION_DESTROY_CONNECTION)) != 0) {
		VL_MSG_ERR("Soft error in rrr_mqtt_common_read_parse_handle while handling packets (one or more connections had to be closed)\n");
		ret = 0;
	}
	if ((ret & RRR_MQTT_CONNECTION_INTERNAL_ERROR) != 0) {
		VL_MSG_ERR("Internal error received in rrr_mqtt_common_read_parse_handle while handling packets\n");
		ret = 1;
		goto out;
	}

	ret = rrr_mqtt_connection_collection_iterate(&data->connections, __rrr_mqtt_common_send, data);

	if ((ret & (RRR_MQTT_CONNECTION_SOFT_ERROR|RRR_MQTT_CONNECTION_DESTROY_CONNECTION)) != 0) {
		VL_MSG_ERR("Soft error in rrr_mqtt_common_read_parse_handle while sending packets (one or more connections had to be closed)\n");
		ret = 0;
	}
	if ((ret & RRR_MQTT_CONNECTION_INTERNAL_ERROR) != 0) {
		VL_MSG_ERR("Internal error received in rrr_mqtt_common_read_parse_handle while sending packets\n");
		ret = 1;
		goto out;
	}

	ret = rrr_mqtt_connection_collection_iterate(&data->connections, rrr_mqtt_connection_iterator_ctx_housekeeping, data);

	if ((ret & (RRR_MQTT_CONNECTION_SOFT_ERROR|RRR_MQTT_CONNECTION_DESTROY_CONNECTION)) != 0) {
		VL_MSG_ERR("Soft error in rrr_mqtt_common_read_parse_handle while doing housekeeping (one or more connections had to be closed)\n");
		ret = 0;
	}
	if ((ret & RRR_MQTT_CONNECTION_INTERNAL_ERROR) != 0) {
		VL_MSG_ERR("Internal error received in rrr_mqtt_common_read_parse_handle while doing housekeeping\n");
		ret = 1;
		goto out;
	}

	out:
	return ret;
}
