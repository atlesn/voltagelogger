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

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <src/lib/array.h>
#include <src/lib/read.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "../lib/settings.h"
#include "../lib/vl_time.h"
#include "../lib/threads.h"
#include "../lib/buffer.h"
#include "../lib/messages.h"
#include "../lib/rrr_socket.h"
#include "../lib/rrr_socket_client.h"
#include "../lib/read.h"
#include "../lib/instances.h"
#include "../lib/instance_config.h"
#include "../lib/utf8.h"
#include "../global.h"

struct socket_data {
	struct rrr_instance_thread_data *thread_data;
	struct rrr_fifo_buffer buffer;
	struct rrr_fifo_buffer inject_buffer;
	char *socket_path;
	char *default_topic;
	ssize_t default_topic_length;
	int receive_rrr_message;
	int do_sync_byte_by_byte;
	struct rrr_array definitions;
	struct rrr_socket_client_collection clients;
	int socket_fd;
};

void data_cleanup(void *arg) {
	struct socket_data *data = (struct socket_data *) arg;
	rrr_fifo_buffer_clear(&data->buffer);
	rrr_fifo_buffer_clear(&data->inject_buffer);
	rrr_array_clear(&data->definitions);
	rrr_socket_client_collection_clear(&data->clients);
	RRR_FREE_IF_NOT_NULL(data->socket_path);
	RRR_FREE_IF_NOT_NULL(data->default_topic);
}

int data_init(struct socket_data *data, struct rrr_instance_thread_data *thread_data) {
	memset(data, '\0', sizeof(*data));

	data->thread_data = thread_data;

	int ret = 0;

	ret |= rrr_fifo_buffer_init(&data->buffer);
	ret |= rrr_fifo_buffer_init(&data->inject_buffer);

	if (ret != 0) {
		data_cleanup(data);
	}

	return ret;
}

int parse_config (struct socket_data *data, struct rrr_instance_config *config) {
	int ret = 0;

	// Socket path
	if (rrr_settings_get_string_noconvert(&data->socket_path, config->settings, "socket_path") != 0) {
		RRR_MSG_ERR("Error while parsing configuration parameter socket_path in socket instance %s\n", config->name);
		ret = 1;
		goto out;
	}

	struct sockaddr_un addr;
	if (strlen(data->socket_path) > sizeof(addr.sun_path) - 1) {
		RRR_MSG_ERR("Configuration parameter socket_path in socket instance %s was too long, max length is %lu bytes\n",
				config->name, sizeof(addr.sun_path) - 1);
		ret = 1;
		goto out;
	}

	// Message default topic
	if ((ret = rrr_settings_get_string_noconvert_silent(&data->default_topic, config->settings, "socket_default_topic")) != 0) {
		if (ret != RRR_SETTING_NOT_FOUND) {
			RRR_MSG_ERR("Error while parsing configuration parameter socket_default_path in socket instance %s\n", config->name);
			ret = 1;
			goto out;
		}
		ret = 0;
	}
	else {
		if (rrr_utf8_validate(data->default_topic, strlen(data->default_topic)) != 0) {
			RRR_MSG_ERR("socket_default_topic for instance %s was not valid UTF-8\n", config->name);
			ret = 1;
			goto out;
		}
		data->default_topic_length = strlen(data->default_topic);
	}

	// Receive full rrr message
	int yesno = 0;
	if (rrr_instance_config_check_yesno (&yesno, config, "socket_receive_rrr_message") == RRR_SETTING_PARSE_ERROR) {
		RRR_MSG_ERR ("mysql: Could not understand argument socket_receive_rrr_message of instance '%s', please specify 'yes' or 'no'\n",
				config->name);
		return 1;
	}
	data->receive_rrr_message = (yesno == 0 || yesno == 1 ? yesno : 0);

	// Parse expected input data
	if (rrr_instance_config_setting_exists(config, "socket_input_types")) {
		if ((ret = rrr_instance_config_parse_array_definition_from_config_silent_fail(&data->definitions, config, "socket_input_types")) != 0) {
			RRR_MSG_ERR("Could not parse configuration parameter socket_input_types in socket instance %s\n",
					config->name);
			return 1;
		}
	}

	// Sync byte by byte if parsing fails
	yesno = 0;
	if ((ret = rrr_instance_config_check_yesno(&yesno, config, "socket_sync_byte_by_byte")) != 0) {
		if (ret != RRR_SETTING_NOT_FOUND) {
			RRR_MSG_ERR("Error while parsing udpr_sync_byte_by_byte for udpreader instance %s, please use yes or no\n",
					config->name);
			ret = 1;
			goto out;
		}
		ret = 0;
	}
	data->do_sync_byte_by_byte = yesno;

	if (data->receive_rrr_message != 0 && RRR_LL_COUNT(&data->definitions) > 0) {
		RRR_MSG_ERR("Array definition cannot be specified with socket_input_types while socket_receive_rrr_message is yes in instance %s\n",
				config->name);
		return 1;
	}
	else if (data->receive_rrr_message == 0 && RRR_LL_COUNT(&data->definitions) == 0) {
		RRR_MSG_ERR("No data types defined in socket_input_types for instance %s\n",
				config->name);
		return 1;
	}

	out:
	return ret;
}

int read_data_receive_message_callback (struct rrr_message **message_ptr, void *arg) {
	struct socket_data *data = arg;
	struct rrr_message *message = *message_ptr;

	if (MSG_TOPIC_LENGTH(message) == 0 && data->default_topic != NULL) {
		if (rrr_message_set_topic(&message, data->default_topic, strlen(data->default_topic)) != 0) {
			RRR_MSG_ERR("Could not set topic of message in read_data_receive_message_callback of instance %s\n",
					INSTANCE_D_NAME(data->thread_data));
			goto out_err;
		}
	}

	rrr_fifo_buffer_write(&data->buffer, (char*)message, MSG_TOTAL_SIZE(message));
	RRR_DBG_3("socket created a message with timestamp %llu size %lu\n",
			(long long unsigned int) message->timestamp, (long unsigned int) sizeof(*message));

	return 0;

	out_err:
		return 1;
}

int read_raw_data_callback(struct rrr_read_session *read_session, void *arg) {
	struct socket_data *data = arg;

	return rrr_array_new_message_from_buffer_with_callback (
			read_session->rx_buf_ptr,
			read_session->rx_buf_wpos,
			data->default_topic,
			data->default_topic_length,
			&data->definitions,
			read_data_receive_message_callback,
			data
	);
}

int socket_read_data(struct socket_data *data) {
	if (data->receive_rrr_message != 0) {
		struct rrr_read_common_receive_message_callback_data callback_data = {
				read_data_receive_message_callback,
				NULL,
				data
		};
		return rrr_socket_client_collection_read (
				&data->clients,
				sizeof(struct rrr_socket_msg),
				4096,
				0,
				RRR_SOCKET_READ_METHOD_RECVFROM | RRR_SOCKET_READ_USE_TIMEOUT,
				rrr_read_common_get_session_target_length_from_message_and_checksum,
				NULL,
				rrr_read_common_receive_message_callback,
				&callback_data
		);
	}
	else {
		struct rrr_read_common_get_session_target_length_from_array_data callback_data = {
				&data->definitions,
				data->do_sync_byte_by_byte
		};
		return rrr_socket_client_collection_read (
				&data->clients,
				sizeof(struct rrr_socket_msg),
				4096,
				0,
				RRR_SOCKET_READ_METHOD_RECVFROM,
				rrr_read_common_get_session_target_length_from_array,
				&callback_data,
				read_raw_data_callback,
				data
		);
	}
}

static int socket_start (struct socket_data *data) {
	int ret = 0;

	char socket_name[64 + 1];
	snprintf(socket_name, 64, "socket for instance %s", INSTANCE_D_NAME(data->thread_data));
	socket_name[64] = '\0';

	int fd = 0;
	if (rrr_socket_unix_create_bind_and_listen(&fd, socket_name, data->socket_path, 10, 1, 0) != 0) {
		RRR_MSG_ERR("Could not create socket in socket_start of instance %s\n", INSTANCE_D_NAME(data->thread_data));
		ret = 1;
		goto out;
	}

	RRR_DBG_1("socket instance %s listening on %s\n",
			INSTANCE_D_NAME(data->thread_data), data->socket_path);

	data->socket_fd = fd;

	rrr_socket_client_collection_init(&data->clients, fd, socket_name);

	out:
	return ret;
}

static void socket_stop (void *arg) {
	struct socket_data *data = arg;
	if (data->socket_fd != 0) {
		rrr_socket_close(data->socket_fd);
	}
	rrr_socket_client_collection_clear(&data->clients);
}

static void *thread_entry_socket (struct rrr_thread *thread) {
	struct rrr_instance_thread_data *thread_data = thread->private_data;
	struct socket_data *data = thread_data->private_data = thread_data->private_memory;

	pthread_cleanup_push(data_cleanup, data);

	if (data_init(data, thread_data) != 0) {
		RRR_MSG_ERR("Could not initalize data in socket instance %s\n",
				INSTANCE_D_NAME(thread_data));
		pthread_exit(0);
	}

	RRR_DBG_1 ("Socket thread data is %p\n", thread_data);

//	pthread_cleanup_push(rrr_thread_set_stopping, thread);
	pthread_cleanup_push(socket_stop, data);

	rrr_thread_set_state(thread, RRR_THREAD_STATE_INITIALIZED);
	rrr_thread_signal_wait(thread_data->thread, RRR_THREAD_SIGNAL_START);
	rrr_thread_set_state(thread, RRR_THREAD_STATE_RUNNING);

	if (parse_config(data, thread_data->init_data.instance_config) != 0) {
		RRR_MSG_ERR("Configuration parsing failed for socket instance %s\n",
				INSTANCE_D_NAME(thread_data));
		goto out_message;
	}

	rrr_instance_config_check_all_settings_used(thread_data->init_data.instance_config);

	if (socket_start(data) != 0) {
		RRR_MSG_ERR("Could not start socket in socket instance %s\n",
				INSTANCE_D_NAME(thread_data));
		goto out_message;
	}

	RRR_DBG_2("socket instance %s listening on socket %s\n",
			INSTANCE_D_NAME(thread_data), data->socket_path);

	while (!rrr_thread_check_encourage_stop(thread_data->thread)) {
		rrr_thread_update_watchdog_time(thread_data->thread);

		if (rrr_socket_client_collection_accept_simple(&data->clients) != 0) {
			break;
		}

		int err = 0;
		if ((err = socket_read_data(data)) != 0) {
			if (err == RRR_SOCKET_SOFT_ERROR) {
				// Upon receival of invalid data, we must close the socket as sizes of
				// the messages and boundaries might be out of sync
				RRR_MSG_ERR("Invalid data received in socket instance %s, socket must be closed\n",
						INSTANCE_D_NAME(thread_data));
			}
			else {
				RRR_MSG_ERR("Error while reading data in socket instance %s, return was %i\n",
						INSTANCE_D_NAME(thread_data), err);
			}
			break;
		}
	}

	out_message:
	RRR_DBG_1 ("socket instance %s received encourage stop\n",
			INSTANCE_D_NAME(thread_data));

	pthread_cleanup_pop(1);
//	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_exit(0);
}

static int test_config (struct rrr_instance_config *config) {
	struct socket_data data;
	int ret = 0;
	if ((ret = data_init(&data, NULL)) != 0) {
		goto err;
	}
	ret = parse_config(&data, config);
	data_cleanup(&data);
	err:
	return ret;
}

static struct rrr_module_operations module_operations = {
	NULL,
	thread_entry_socket,
	NULL,
	test_config,
	NULL,
	NULL
};

static const char *module_name = "socket";

__attribute__((constructor)) void load(void) {
}

void init(struct rrr_instance_dynamic_data *data) {
		data->module_name = module_name;
		data->type = RRR_MODULE_TYPE_SOURCE;
		data->operations = module_operations;
		data->dl_ptr = NULL;
		data->private_data = NULL;
		data->start_priority = RRR_THREAD_START_PRIORITY_NETWORK;
}

void unload(void) {
}


