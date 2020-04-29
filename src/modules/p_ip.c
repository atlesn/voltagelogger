/*

Read Route Record

Copyright (C) 2019-2020 Atle Solbakken atle@goliathdns.no

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
#include <src/lib/array.h>
#include <unistd.h>

#include "../lib/settings.h"
#include "../lib/vl_time.h"
#include "../lib/threads.h"
#include "../lib/buffer.h"
#include "../lib/messages.h"
#include "../lib/ip.h"
#include "../lib/array.h"
#include "../lib/type.h"
#include "../lib/rrr_socket.h"
#include "../lib/instances.h"
#include "../lib/instance_config.h"
#include "../lib/utf8.h"
#include "../lib/read.h"
#include "../lib/poll_helper.h"
#include "../lib/map.h"
#include "../lib/stats_instance.h"
#include "../global.h"

#define IP_DEFAULT_PORT		2222
#define IP_DEFAULT_PROTOCOL	RRR_IP_UDP

struct ip_data {
	struct rrr_instance_thread_data *thread_data;
	struct rrr_fifo_buffer send_buffer;
	struct rrr_fifo_buffer inject_buffer;
	struct rrr_fifo_buffer delivery_buffer;
	unsigned int source_udp_port;
	unsigned int source_tcp_port;
	struct rrr_ip_data ip_udp;
	struct rrr_ip_data ip_tcp_listen;
	int ip_tcp_default_target_fd;
	int ip_tcp_preserve_connections;
	struct rrr_array definitions;
	struct rrr_read_session_collection read_sessions_udp;
	struct rrr_read_session_collection read_sessions_tcp;
	int do_sync_byte_by_byte;
	int do_send_rrr_message;
	int do_force_target;
	int do_extract_rrr_messages;
	char *default_topic;
	char *target_host;
	unsigned int target_port;
	int target_protocol;
	ssize_t default_topic_length;
	struct rrr_map array_send_tags;
	uint64_t messages_count_read;
	uint64_t messages_count_polled;
	uint64_t read_error_count;
};

void data_cleanup(void *arg) {
	struct ip_data *data = (struct ip_data *) arg;
	rrr_fifo_buffer_invalidate(&data->send_buffer);
	rrr_fifo_buffer_invalidate(&data->inject_buffer);
	rrr_fifo_buffer_invalidate(&data->delivery_buffer);
	rrr_array_clear(&data->definitions);
	rrr_read_session_collection_clear(&data->read_sessions_udp);
	rrr_read_session_collection_clear(&data->read_sessions_tcp);
	RRR_FREE_IF_NOT_NULL(data->default_topic);
	RRR_FREE_IF_NOT_NULL(data->target_host);
	rrr_map_clear(&data->array_send_tags);
}

int data_init(struct ip_data *data, struct rrr_instance_thread_data *thread_data) {
	memset(data, '\0', sizeof(*data));

	data->thread_data = thread_data;

	int ret = 0;

	ret |= rrr_fifo_buffer_init_custom_free(&data->send_buffer, rrr_ip_buffer_entry_destroy_void);
	ret |= rrr_fifo_buffer_init_custom_free(&data->inject_buffer, rrr_ip_buffer_entry_destroy_void);
	ret |= rrr_fifo_buffer_init_custom_free(&data->delivery_buffer, rrr_ip_buffer_entry_destroy_void);

	if (ret != 0) {
		data_cleanup(data);
	}

	return ret;
}

struct ip_poll_delete_callback_data {
	struct ip_data *ip_data;
	int (*callback)(RRR_MODULE_POLL_CALLBACK_SIGNATURE);
	struct rrr_fifo_callback_args *poll_data;
};

static int __poll_delete_extract_msg_callback (RRR_FIFO_CALLBACK_ARGS) {
	struct ip_poll_delete_callback_data *ip_callback_data = callback_data->private_data;
//	struct ip_data *ip_data = ip_callback_data->ip_data;

	(void)(size);

	int ret = 0;

	struct rrr_ip_buffer_entry *entry = (struct rrr_ip_buffer_entry *) data;

	ret = ip_callback_data->callback (
			ip_callback_data->poll_data,
			entry->message,
			entry->data_length
	);

	// Ownership of message pointer is handed over to callback
	entry->message = NULL;
	rrr_ip_buffer_entry_destroy(entry);

	return ret;
}

static int poll_delete (RRR_MODULE_POLL_SIGNATURE) {
	struct ip_data *ip_data = data->private_data;

	struct ip_poll_delete_callback_data callback_data = {
			ip_data,
			callback,
			poll_data
	};

	struct rrr_fifo_callback_args fifo_args = {
			ip_data->thread_data,
			&callback_data,
			0
	};

	return rrr_fifo_read_clear_forward (
			&ip_data->delivery_buffer,
			NULL,
			__poll_delete_extract_msg_callback,
			&fifo_args,
			wait_milliseconds
	);
}

static int __poll_extract_msg_callback (RRR_FIFO_CALLBACK_ARGS) {
	struct ip_poll_delete_callback_data *ip_callback_data = callback_data->private_data;
//	struct ip_data *ip_data = ip_callback_data->ip_data;

	(void)(size);

	int ret = 0;

	struct rrr_ip_buffer_entry *entry = (struct rrr_ip_buffer_entry *) data;

	ret = ip_callback_data->callback (
			ip_callback_data->poll_data,
			entry->message,
			entry->data_length
	);

	return ret;
}

static int poll (RRR_MODULE_POLL_SIGNATURE) {
	struct ip_data *ip_data = data->private_data;

	struct ip_poll_delete_callback_data callback_data = {
			ip_data,
			callback,
			poll_data
	};

	struct rrr_fifo_callback_args fifo_args = {
			ip_data->thread_data,
			&callback_data,
			0
	};

	return rrr_fifo_search(&ip_data->delivery_buffer, __poll_extract_msg_callback, &fifo_args, wait_milliseconds);
}

static int poll_delete_ip (RRR_MODULE_POLL_SIGNATURE) {
	struct ip_data *ip_data = data->private_data;

	return rrr_fifo_read_clear_forward (
			&ip_data->delivery_buffer,
			NULL,
			callback,
			poll_data,
			wait_milliseconds
	);
}

int config_parse_port (struct ip_data *data, struct rrr_instance_config *config) {
	int ret = 0;

	rrr_setting_uint tmp_uint;

	ret = rrr_instance_config_read_port_number (&tmp_uint, config, "ip_udp_port");
	if (ret != 0) {
		if (ret == RRR_SETTING_PARSE_ERROR) {
			RRR_MSG_ERR("Could not parse ip_udp_port for instance %s\n", config->name);
			ret = 1;
			goto out;
		}
		else if (ret == RRR_SETTING_NOT_FOUND) {
			// Listening not being done
			ret = 0;
		}
		else {
			RRR_MSG_ERR("Error while parsing ip_udp_port setting for instance %s\n", config->name);
			ret = 1;
			goto out;
		}
	}
	data->source_udp_port = tmp_uint;

	ret = rrr_instance_config_read_port_number (&tmp_uint, config, "ip_tcp_port");
	if (ret != 0) {
		if (ret == RRR_SETTING_PARSE_ERROR) {
			RRR_MSG_ERR("Could not parse ip_tcp_port for instance %s\n", config->name);
			ret = 1;
			goto out;
		}
		else if (ret == RRR_SETTING_NOT_FOUND) {
			// Listening not being done
			ret = 0;
		}
		else {
			RRR_MSG_ERR("Error while parsing ip_tcp_port setting for instance %s\n", config->name);
			ret = 1;
			goto out;
		}
	}
	data->source_tcp_port = tmp_uint;

	ret = rrr_instance_config_read_port_number (&tmp_uint, config, "ip_target_port");
	if (ret != 0) {
		if (ret == RRR_SETTING_PARSE_ERROR) {
			RRR_MSG_ERR("Could not parse ip_remote_port for instance %s\n", config->name);
			ret = 1;
			goto out;
		}
		else if (ret == RRR_SETTING_NOT_FOUND) {
			// No remote port specified
			ret = 0;
		}
		else {
			RRR_MSG_ERR("Error while parsing ip_remote_port setting for instance %s\n", config->name);
			ret = 1;
			goto out;
		}
	}
	data->target_port = tmp_uint;

	out:
	return ret;
}

int parse_config (struct ip_data *data, struct rrr_instance_config *config) {
	int ret = 0;
	char *protocol = NULL;

	// Parse listen and target port
	if ((ret = config_parse_port (data, config)) != 0) {
		goto out;
	}

	// Default target protocol
	if ((ret = rrr_settings_get_string_noconvert_silent(&protocol, config->settings, "ip_target_protocol")) != 0) {
		if (ret != RRR_SETTING_NOT_FOUND) {
			RRR_MSG_ERR("Error while parsing configuration parameter ip_target_protocol in ip instance %s\n", config->name);
			ret = 1;
			goto out;
		}
		ret = 0;
		data->target_protocol = IP_DEFAULT_PROTOCOL;
	}
	else {
		if (strcasecmp(protocol, "udp") == 0) {
			data->target_protocol = RRR_IP_UDP;
		}
		else if (strcasecmp(protocol, "tcp") == 0) {
			data->target_protocol = RRR_IP_TCP;
		}
		else {
			RRR_MSG_ERR("Unknown protocol '%s' specified in ip_target_protocol in ip instance %s. Must be tcp or udp.\n",
					protocol, config->name);
			ret = 1;
			goto out;
		}
	}

	// Default target host
	if ((ret = rrr_settings_get_string_noconvert_silent(&data->target_host, config->settings, "ip_target_host")) != 0) {
		if (ret != RRR_SETTING_NOT_FOUND) {
			RRR_MSG_ERR("Error while parsing configuration parameter ip_target_host in ip instance %s\n", config->name);
			ret = 1;
			goto out;
		}
		ret = 0;
	}

	if (data->target_port != 0 && (data->target_host == NULL || *(data->target_host) == '\0')) {
		RRR_MSG_ERR("ip_target_port was set but ip_target_host was not, both of them must be either set or left unset in ip instance %s\n", config->name);
		ret = 1;
		goto out;
	}

	if (data->target_port == 0 && (data->target_host != NULL && *(data->target_host) != '\0')) {
		RRR_MSG_ERR("ip_target_host was set but ip_target_port was not, both of them must be either set or left unset in ip instance %s\n", config->name);
		ret = 1;
		goto out;
	}

	// Parse expected input data
	if ((ret = rrr_instance_config_parse_array_definition_from_config_silent_fail(&data->definitions, config, "ip_input_types")) != 0) {
		if (ret != RRR_SETTING_NOT_FOUND) {
			RRR_MSG_ERR("Could not parse command line argument ip_input_types in udp\n");
			ret = 1;
			goto out;
		}
	}

	if (data->definitions.node_count > 0 && data->source_udp_port == 0 && data->source_tcp_port == 0) {
		RRR_MSG_ERR("ip_input_types was set but ip_port was not, this is an invalid configuraton in ip instance %s\n", config->name);
		ret = 1;
		goto out;
	}
	else if (data->definitions.node_count == 0) {
		// Listening disabled
	}

	// Message default topic
	if ((ret = rrr_settings_get_string_noconvert_silent(&data->default_topic, config->settings, "ip_default_topic")) != 0) {
		if (ret != RRR_SETTING_NOT_FOUND) {
			RRR_MSG_ERR("Error while parsing configuration parameter ip_default_topic in ip instance %s\n", config->name);
			ret = 1;
			goto out;
		}
		ret = 0;
	}
	else {
		if (rrr_utf8_validate(data->default_topic, strlen(data->default_topic)) != 0) {
			RRR_MSG_ERR("ip_default_topic for instance %s was not valid UTF-8\n", config->name);
			ret = 1;
			goto out;
		}
		data->default_topic_length = strlen(data->default_topic);
	}

	// Sync byte by byte if parsing fails
	int yesno = 0;
	if ((ret = rrr_instance_config_check_yesno(&yesno, config, "ip_sync_byte_by_byte")) != 0) {
		if (ret != RRR_SETTING_NOT_FOUND) {
			RRR_MSG_ERR("Error while parsing ip_sync_byte_by_byte for ip instance %s, please use yes or no\n", config->name);
			ret = 1;
			goto out;
		}
		ret = 0;
	}
	else {
		data->do_sync_byte_by_byte = yesno;
	}

	// Send complete RRR message
	yesno = 0;
	if ((ret = rrr_instance_config_check_yesno(&yesno, config, "ip_send_rrr_message")) != 0) {
		if (ret != RRR_SETTING_NOT_FOUND) {
			RRR_MSG_ERR("Error while parsing ip_send_rrr_message for ip instance %s, please use yes or no\n", config->name);
			ret = 1;
			goto out;
		}
		ret = 0;
	}
	else {
		data->do_send_rrr_message = yesno;
	}

	// Force target
	yesno = 0;
	if ((ret = rrr_instance_config_check_yesno(&yesno, config, "ip_force_target")) != 0) {
		if (ret != RRR_SETTING_NOT_FOUND) {
			RRR_MSG_ERR("Error while parsing ip_force_target for ip instance %s, please use yes or no\n", config->name);
			ret = 1;
			goto out;
		}
		ret = 0;
	}
	else {
		data->do_force_target = yesno;
	}

	if (data->do_force_target != 0 && data->target_port == 0) {
		RRR_MSG_ERR("ip_force_target was set to yes but no target was specified in ip_target_host and ip_target_port in ip instance %s\n",
				config->name);
		ret = 1;
		goto out;
	}

	// Extract RRR messages from arrays
	yesno = 0;
	if ((ret = rrr_instance_config_check_yesno(&yesno, config, "ip_extract_rrr_messages")) != 0) {
		if (ret != RRR_SETTING_NOT_FOUND) {
			RRR_MSG_ERR("Error while parsing ip_extract_rrr_messages for ip instance %s, please use yes or no\n", config->name);
			ret = 1;
			goto out;
		}
		ret = 0;
	}
	else {
		data->do_extract_rrr_messages = yesno;
	}

	// Array columns to send if we receive array messages from other modules
	ret = rrr_instance_config_parse_comma_separated_to_map(&data->array_send_tags, config, "ip_array_send_tags");
	if (ret != 0 && ret != RRR_SETTING_NOT_FOUND) {
		RRR_MSG_ERR("Error while parsing ip_array_send_tags of instance %s\n", config->name);
		goto out;
	}
	RRR_DBG_1("%i blob write columns specified for ip instance %s\n", RRR_MAP_COUNT(&data->array_send_tags), config->name);

	out:
	RRR_FREE_IF_NOT_NULL(protocol);
	return ret;
}

struct ip_read_callback_data {
	struct ip_data *ip_data;
	const struct rrr_ip_buffer_entry *entry_orig;
	int protocol;
};

int read_data_receive_message_callback (struct rrr_message *message, void *arg) {
	struct ip_read_callback_data *callback_data = arg;
	struct ip_data *data = callback_data->ip_data;

	int ret = 0;

	struct rrr_ip_buffer_entry *new_entry = NULL;

	if (rrr_ip_buffer_entry_new (
			&new_entry,
			MSG_TOTAL_SIZE(message),
			&callback_data->entry_orig->addr,
			callback_data->entry_orig->addr_len,
			callback_data->protocol,
			message
	) != 0) {
		RRR_MSG_ERR("Could not create new ip buffer entry in read_data_receive_message_callback\n");
		ret = 1;
		goto out;
	}

	RRR_DBG_3("ip instance %s created a message with timestamp %llu size %lu\n",
			INSTANCE_D_NAME(data->thread_data), (long long unsigned int) message->timestamp, (long unsigned int) sizeof(*message));

	// Now managed by ip buffer entry
	message = NULL;

	rrr_fifo_buffer_write(&data->delivery_buffer, (char*)new_entry, sizeof(*new_entry));

	// Now managed by fifo buffer
	new_entry = NULL;

	data->messages_count_read++;

	out:
	if (new_entry != NULL) {
		rrr_ip_buffer_entry_destroy(new_entry);
	}
	if (message != NULL) {
		free(message);
	}
	return ret;
}

int read_data_receive_extract_messages_callback (const struct rrr_array *array, void *arg) {
	struct ip_read_callback_data *callback_data = arg;
	struct ip_data *data = callback_data->ip_data;

	int ret = 0;

	int found_messages = 0;
	RRR_LL_ITERATE_BEGIN(array, const struct rrr_type_value);
		if (RRR_TYPE_IS_MSG(node->definition->type)) {
			const struct rrr_message *message = (struct rrr_message *) node->data;
			struct rrr_message *message_new = rrr_message_duplicate(message);
			if (message_new == NULL) {
				RRR_MSG_ERR("Could not allocate new message in ip read_data_receive_array_callback\n");
				ret = 1;
				goto out;
			}

			if ((ret = read_data_receive_message_callback(message_new, arg)) != 0) {
				goto out;
			}

			found_messages++;
		}
	RRR_LL_ITERATE_END();

	RRR_DBG_3("ip instance %s extracted %i RRR messages from an array\n",
			INSTANCE_D_NAME(data->thread_data), found_messages);

	if (found_messages == 0) {
		RRR_MSG_ERR("No RRR message found in array definition in ip instance %s\n",
				INSTANCE_D_NAME(data->thread_data));
		ret = 1;
		goto out;
	}

	out:
	return ret;
}

int read_raw_data_callback (struct rrr_ip_buffer_entry *entry, void *arg) {
	struct ip_data *data = arg;
	int ret = 0;

	struct ip_read_callback_data callback_data = {
			data,
			entry,
			entry->protocol
	};

	if (data->do_extract_rrr_messages) {
		ret = rrr_array_parse_from_buffer_with_callback (
			entry->message,
			entry->data_length,
			&data->definitions,
			read_data_receive_extract_messages_callback,
			&callback_data
		);
	}
	else {
		ret = rrr_array_new_message_from_buffer_with_callback (
			entry->message,
			entry->data_length,
			data->default_topic,
			data->default_topic_length,
			&data->definitions,
			read_data_receive_message_callback,
			&callback_data
		);
	}

	if (ret != 0) {
		if (ret == RRR_ARRAY_PARSE_SOFT_ERR) {
			RRR_MSG_ERR("Could not create message in ip instance %s read_data_callback, soft error probably caused by invalid input data\n",
					INSTANCE_D_NAME(data->thread_data));
			ret = 0;
		}
		else {
			RRR_MSG_ERR("Could not create message in ip instance %s read_data_callback\n",
					INSTANCE_D_NAME(data->thread_data));
		}
		goto out;
	}

	out:
	rrr_ip_buffer_entry_destroy_void(entry);
	return ret;
}

int inject_callback(struct rrr_fifo_callback_args *poll_data, char *data, unsigned long int size) {
	RRR_DBG_4("ip inject callback size %lu\n", size);
	struct ip_data *ip_data = poll_data->private_data;
	return read_raw_data_callback((struct rrr_ip_buffer_entry *) data, ip_data);
}

int read_loop (struct ip_data *data, int handle_soft_error, int fd, struct rrr_read_session_collection *read_sessions) {
	int ret = 0;

	for (int i = 0; i < 10; i++) {
		if ((ret = rrr_ip_receive_array (
				read_sessions,
				fd,
				RRR_READ_F_NO_SLEEPING,
				&data->definitions,
				data->do_sync_byte_by_byte,
				read_raw_data_callback,
				data,
				NULL
		)) != 0) {
			if (ret == RRR_ARRAY_PARSE_SOFT_ERR) {
				if (handle_soft_error) {
					break;
				}
				RRR_MSG_ERR("Received invalid data in ip_receive_packets in ip instance %s\n",
						INSTANCE_D_NAME(data->thread_data));
				// Don't allow invalid data to stop processing
				ret = 0;
				data->read_error_count++;
			}
			else {
				RRR_MSG_ERR("Error from ip_receive_packets in ip instance %s return was %i\n",
						INSTANCE_D_NAME(data->thread_data), ret);
				ret = 1;
				goto out;
			}
		}
	}

	out:
	return ret;
}

int tcp_read_data (struct ip_data *data, struct rrr_ip_accept_data_collection *accept_data_collection) {
	int ret = 0;
	if (data->source_tcp_port == 0) {
		goto out;
	}

	struct rrr_ip_accept_data *accept_data = NULL;

	if (rrr_ip_accept (
			&accept_data,
			&data->ip_tcp_listen,
			"ip",
			0
	) != 0) {
		RRR_MSG_ERR("Error while accepting TCP connection in ip instance %s\n", INSTANCE_D_NAME(data->thread_data));
		ret = 1;
		goto out;
	}

	if (accept_data != NULL) {
		RRR_LL_APPEND(accept_data_collection, accept_data);
		accept_data = NULL;
	}

	RRR_LL_ITERATE_BEGIN(accept_data_collection, struct rrr_ip_accept_data);
		if ((ret = read_loop (data, 1, node->ip_data.fd, &data->read_sessions_tcp)) != 0) {
			if (ret == RRR_SOCKET_SOFT_ERROR) {
				RRR_MSG_ERR("Closing tcp connection following error in ip instance %s\n", INSTANCE_D_NAME(data->thread_data));
				RRR_LL_ITERATE_SET_DESTROY();
				ret = 0;
			}
		}
	RRR_LL_ITERATE_END_CHECK_DESTROY(accept_data_collection, 0; rrr_ip_accept_data_close_and_destroy(node));

	out:
	return ret;
}

int udp_read_data (struct ip_data *data) {
	int ret = 0;

	if (data->source_udp_port > 0) {
		if ((ret = read_loop (data, 0, data->ip_udp.fd, &data->read_sessions_udp)) != 0) {
			goto out;
		}
	}

	out:
	return ret;
}

static int inject (RRR_MODULE_INJECT_SIGNATURE) {
	struct ip_data *data = thread_data->private_data;
	RRR_DBG_2("ip: writing data from inject function\n");

	if (data->inject_buffer.invalid) {
		return 1;
	}

	rrr_fifo_buffer_write(&data->inject_buffer, (char *) message, sizeof(*message));

	return 0;
}

static int poll_callback_final (struct ip_data *data, struct rrr_ip_buffer_entry *entry) {
	data->messages_count_polled++;
	rrr_fifo_buffer_write(&data->send_buffer, (char *) entry, sizeof(*entry));
	return 0;
}

static int poll_callback (struct rrr_fifo_callback_args *poll_data, char *data, unsigned long int size) {
	struct rrr_instance_thread_data *thread_data = poll_data->source;
	struct ip_data *private_data = thread_data->private_data;
	struct rrr_message *message = (struct rrr_message *) data;
	struct rrr_ip_buffer_entry *entry = NULL;

	RRR_DBG_3 ("ip instance %s: Result from buffer: timestamp %" PRIu64 " size %lu\n",
			INSTANCE_D_NAME(thread_data), message->timestamp, size);

	if (rrr_ip_buffer_entry_new (
			&entry,
			MSG_TOTAL_SIZE(message),
			NULL,
			0,
			0,
			message
	) != 0) {
		RRR_MSG_ERR("Could not create ip buffer entry in ip poll_callback\n");
		free(data);
		return 1;
	}

	return poll_callback_final(private_data, entry);
}

static int poll_callback_ip (struct rrr_fifo_callback_args *poll_data, char *data, unsigned long int size) {
	struct rrr_instance_thread_data *thread_data = poll_data->source;
	struct ip_data *private_data = thread_data->private_data;
	struct rrr_ip_buffer_entry *entry = (struct rrr_ip_buffer_entry *) data;

	RRR_DBG_3 ("ip instance %s: Result from buffer ip: size %lu\n",
			INSTANCE_D_NAME(thread_data), size);

	return poll_callback_final(private_data, entry);
}

struct input_callback_data {
	struct ip_data *ip_data;
	struct rrr_ip_accept_data_collection *tcp_connect_data;
};

static int input_callback(struct rrr_fifo_callback_args *poll_data, char *data, unsigned long int size) {
	struct rrr_instance_thread_data *thread_data = poll_data->source;
	struct input_callback_data *input_callback_data = poll_data->private_data;
	struct ip_data *ip_data = input_callback_data->ip_data;
	struct rrr_ip_accept_data_collection *tcp_connect_data = input_callback_data->tcp_connect_data;
	struct rrr_ip_accept_data *accept_data_tmp = NULL;
	int ret = RRR_FIFO_OK;

	(void)(size);

	struct rrr_ip_buffer_entry *entry = (struct rrr_ip_buffer_entry *) data;

	char *tmp_data = NULL; // Freed upon function return

	const void *send_data = NULL; // Just a pointer to data managed elsewhere, not freed
	ssize_t send_size = 0;

	int err;
	const struct rrr_ip_accept_data *accept_data = NULL;
	struct rrr_array array_tmp = {0};
	struct rrr_message *message = entry->message;

	// We modify the data in the buffer here, no need to copy as the memory is always
	// freed after this function.
	if (ip_data->do_send_rrr_message != 0) {
		if (entry->data_length < (long int) sizeof(*message) - 1) {
			RRR_MSG_ERR("ip instance %s had send_rrr_message set but received a message which was too short (%li<%li), dropping it\n",
					INSTANCE_D_NAME(thread_data), entry->data_length, (long int) sizeof(*message));
			ret = 0; // Non-critical error
			goto out;
		}

		ssize_t final_size = MSG_TOTAL_SIZE(message);

		if (entry->data_length != final_size) {
			RRR_BUG("message size mismatch in ip input_callback %li vs %li\n", entry->data_length, final_size);
		}

		RRR_DBG_3 ("ip instance %s sends packet with rrr message timestamp from %" PRIu64 " size %li\n",
				INSTANCE_D_NAME(thread_data), message->timestamp, final_size);

		rrr_message_prepare_for_network(message);

		rrr_socket_msg_populate_head (
				(struct rrr_socket_msg *) message,
				RRR_SOCKET_MSG_TYPE_MESSAGE,
				final_size,
				0
		);

		rrr_socket_msg_checksum_and_to_network_endian (
				(struct rrr_socket_msg *) message
		);

		send_data = message;
		send_size = final_size;
	}
	else {
		if (!MSG_IS_ARRAY(message)) {
			if (RRR_MAP_COUNT(&ip_data->array_send_tags) > 0) {
				RRR_MSG_ERR("ip instance %s received a non-array message while setting ip_array_send_tags was defined, dropping it\n",
						INSTANCE_D_NAME(thread_data));
				ret = 0; // Non-critical error
				goto out;
			}

			send_data = message->data;
			send_size = MSG_DATA_LENGTH(message);

			if (send_size == 0) {
				ret = 0; // Nothing to send
				goto out;
			}

			RRR_DBG_3 ("ip instance %s sends packet with raw data from message with timestamp from %" PRIu64 " %li bytes\n",
					INSTANCE_D_NAME(thread_data), message->timestamp, send_size);
		}
		else {
			int tag_count = RRR_MAP_COUNT(&ip_data->array_send_tags);

			if (rrr_array_message_to_collection(&array_tmp, message) != 0) {
				RRR_MSG_ERR("Could not convert array message to collection in ip instance %s\n", INSTANCE_D_NAME(thread_data));
				ret = 1; // Probably bug in some other module or with array parsing
				goto out;
			}

			RRR_FREE_IF_NOT_NULL(tmp_data);
			ssize_t target_size = 0;
			int found_tags = 0;
			struct rrr_map *tag_map = (tag_count > 0 ? &ip_data->array_send_tags : NULL);
			if (rrr_array_selected_tags_export (
					&tmp_data,
					&target_size,
					&found_tags,
					&array_tmp,
					tag_map
			) != 0) {
				RRR_MSG_ERR("Error while converting array to raw in ip instance %s\n", INSTANCE_D_NAME(thread_data));
				ret = 1; // Probably bug in some other module or with array parsing
				goto out;
			}

			if (tag_count != 0 && found_tags != tag_count) {
				RRR_MSG_ERR("Array message to send in ip instance %s did not contain all tags specified in configuration, dropping it (%i tags missing)\n",
						INSTANCE_D_NAME(thread_data), tag_count - found_tags);
				ret = 0; // Non-critical
				goto out;
			}

			RRR_DBG_3 ("ip instance %s sends packet with array data from message with timestamp from %" PRIu64 " %i array tags size %li\n",
					INSTANCE_D_NAME(thread_data), message->timestamp, found_tags, target_size);

			send_data = tmp_data;
			send_size = target_size;
		}
	}

	if ((ip_data->target_port != 0 && (ip_data->target_host == NULL || *(ip_data->target_host) == '\0')) ||
	    (ip_data->target_port == 0 && (ip_data->target_host != NULL && *(ip_data->target_host) != '\0'))
	) {
		RRR_BUG("Invalid target_port/target_host configuration in ip input_callback\n");
	}

	// Configuration validation should produce an error if do_force_target is set
	// but no target_port/target_host
	if (ip_data->do_force_target == 1 || entry->addr_len == 0) {
		if (ip_data->target_port == 0) {
			RRR_MSG_ERR("Warning: A message from a sender in ip instance %s had no address information and we have no default remote host set, dropping it\n",
					INSTANCE_D_NAME(thread_data));
			goto out;
		}

		if (ip_data->target_protocol == RRR_IP_TCP) {
			accept_data = rrr_ip_accept_data_collection_find_by_fd (
					tcp_connect_data,
					ip_data->ip_tcp_default_target_fd
			);

			if (accept_data == NULL) {
				if (rrr_ip_network_connect_tcp_ipv4_or_ipv6(&accept_data_tmp, ip_data->target_port, ip_data->target_host) != 0) {
					RRR_MSG_ERR("Could not connect with TCP to remote %s port %u in ip instance %s\n",
							ip_data->target_host, ip_data->target_port, INSTANCE_D_NAME(thread_data));
					ret = 1;
					goto out;
				}
				accept_data = accept_data_tmp;
				RRR_LL_APPEND(tcp_connect_data, accept_data_tmp);
				accept_data_tmp = NULL;
			}

			goto ip_tcp_send;
		}
		else {
			ret = rrr_ip_network_sendto_udp_ipv4_or_ipv6 (
				&ip_data->ip_udp,
				ip_data->target_port,
				ip_data->target_host,
				(void *) send_data, // Cast away const OK
				send_size
			);
		}
	}
	else {
		if (entry->protocol == RRR_IP_TCP) {
			accept_data = rrr_ip_accept_data_collection_find (
					tcp_connect_data,
					&entry->addr,
					entry->addr_len
			);

			if (accept_data == NULL) {
				if (rrr_ip_network_connect_tcp_ipv4_or_ipv6_raw(
						&accept_data_tmp,
						&entry->addr,
						entry->addr_len
				) != 0) {
					RRR_MSG_ERR("Could not connect to remote in ip instance %s, dropping message\n",
							INSTANCE_D_NAME(thread_data));
					ret = 0;
					goto out;
				}
				accept_data = accept_data_tmp;
				RRR_LL_APPEND(tcp_connect_data, accept_data_tmp);
				accept_data_tmp = NULL;
			}

			goto ip_tcp_send;
		}
		else {
			int err;
			ret = rrr_ip_send (
				&err,
				ip_data->ip_udp.fd,
				&entry->addr,
				entry->addr_len,
				(void *) send_data, // Cast away const OK
				send_size
			);
		}
	}

	if (ret != 0) {
		RRR_MSG_ERR("Could not send data in ip instance %s", INSTANCE_D_NAME(thread_data));
		ret = 1;
		goto out;
	}

	goto out;
	ip_tcp_send:
		if ((ret = rrr_socket_connect_nonblock_postcheck(accept_data->ip_data.fd)) != 0) {
			if (ret == RRR_SOCKET_SOFT_ERROR) {
				RRR_DBG_3("Connection not ready while sending in ip instance %s, putting message back into send queue\n",
						INSTANCE_D_NAME(thread_data));
				goto out_put_back;
			}

			RRR_DBG_1("Connection problem with TCP connection, dropping message in ip instance %s\n",
					INSTANCE_D_NAME(thread_data));
			ret = 0;
		}
		else if ((ret = rrr_ip_send(&err, accept_data->ip_data.fd, NULL, 0, (void*) send_data, send_size)) != 0) {
			if (ret == RRR_SOCKET_SOFT_ERROR) {
				if (err == EAGAIN || err == EWOULDBLOCK) {
					RRR_DBG_1("Sending of message to remote blocked for ip instance %s, putting message back into send queue\n",
							INSTANCE_D_NAME(thread_data));
					ret = 0;
					goto out_put_back;
				}

				RRR_MSG_ERR("Connection problem with TCP connection while sending, dropping message in ip instance %s\n",
						INSTANCE_D_NAME(thread_data));
				ret = 0;
				// No goto
			}
			else {
				RRR_MSG_ERR("Error while sending tcp message in ip instance %s\n",
						INSTANCE_D_NAME(thread_data));
				ret = 1;
				goto out;
			}
		}

		// Only one message per connection? Close if message was sent
		if (!ip_data->ip_tcp_preserve_connections) {
			rrr_ip_accept_data_collection_close_and_remove_by_fd(tcp_connect_data, accept_data->ip_data.fd);
		}
	goto out;

	out_put_back:
		rrr_fifo_buffer_write(&ip_data->send_buffer, data, size);
		data = NULL; // Prevents FREE below

		// Don't stop and block others, continue reading from the buffer in
		// case there are other targets
		ret = 0;

	out:
		if (data != NULL) {
			ret |= RRR_FIFO_SEARCH_FREE;
		}
		if (accept_data_tmp != NULL) {
			rrr_ip_accept_data_close_and_destroy_void(accept_data_tmp);
		}
		RRR_FREE_IF_NOT_NULL(tmp_data);
		rrr_array_clear(&array_tmp);
		return ret;
}

static void *thread_entry_ip (struct rrr_thread *thread) {
	struct rrr_instance_thread_data *thread_data = thread->private_data;
	struct ip_data *data = thread_data->private_data = thread_data->private_memory;
	struct poll_collection poll;
	struct poll_collection poll_ip;

	struct rrr_ip_accept_data_collection tcp_accept_data = {0};
	struct rrr_ip_accept_data_collection tcp_connect_data = {0};

	if (data_init(data, thread_data) != 0) {
		RRR_MSG_ERR("Could not initalize data in ip instance %s\n", INSTANCE_D_NAME(thread_data));
		pthread_exit(0);
	}

	RRR_DBG_1 ("ip thread data is %p\n", thread_data);

	poll_collection_init(&poll_ip);
	poll_collection_init(&poll);
	RRR_STATS_INSTANCE_INIT_WITH_PTHREAD_CLEANUP_PUSH;
	pthread_cleanup_push(poll_collection_clear_void, &poll_ip);
	pthread_cleanup_push(poll_collection_clear_void, &poll);
	pthread_cleanup_push(data_cleanup, data);
	pthread_cleanup_push(rrr_thread_set_stopping, thread);

	rrr_thread_set_state(thread, RRR_THREAD_STATE_INITIALIZED);
	rrr_thread_signal_wait(thread_data->thread, RRR_THREAD_SIGNAL_START);
	// Don't set running here, wait until listening has started

	if (parse_config(data, thread_data->init_data.instance_config) != 0) {
		RRR_MSG_ERR("Configuration parsing failed for ip instance %s\n", thread_data->init_data.module->instance_name);
		goto out_message;
	}

	rrr_instance_config_check_all_settings_used(thread_data->init_data.instance_config);

	poll_add_from_thread_senders_ignore_error(&poll, thread_data, RRR_POLL_POLL_DELETE|RRR_POLL_NO_SENDERS_OK);
	poll_add_from_thread_senders_ignore_error(&poll_ip, thread_data, RRR_POLL_POLL_DELETE_IP|RRR_POLL_NO_SENDERS_OK);
	poll_remove_senders_also_in(&poll, &poll_ip);

	int has_senders = (poll_collection_count(&poll) + poll_collection_count(&poll_ip) > 0 ? 1 : 0);

	if (has_senders == 0 && RRR_LL_COUNT(&data->definitions) == 0) {
		RRR_MSG_ERR("Error: ip instance %s has no senders defined and also has no array definition. Cannot do anything with this configuration.\n",
				INSTANCE_D_NAME(thread_data));
		goto out_message;
	}

	if (data->source_udp_port == 0) {
		if (rrr_ip_network_start_udp_ipv4_nobind(&data->ip_udp) != 0) {
			RRR_MSG_ERR("Could not initialize network in ip instance %s\n", INSTANCE_D_NAME(thread_data));
			goto out_message;
		}
		RRR_DBG_1("ip instance %s started, not listening on any UDP port\n", INSTANCE_D_NAME(thread_data));
	}
	else {
		data->ip_udp.port = data->source_udp_port;
		if (rrr_ip_network_start_udp_ipv4(&data->ip_udp) != 0) {
			RRR_MSG_ERR("Could not initialize UDP network in ip instance %s\n", INSTANCE_D_NAME(thread_data));
			goto out_message;
		}
		RRR_DBG_1("ip instance %s listening on and/or sending from UDP port %d\n",
				INSTANCE_D_NAME(thread_data), data->source_udp_port);
	}

	if (data->source_tcp_port > 0) {
		data->ip_tcp_listen.port = data->source_tcp_port;
		if (rrr_ip_network_start_tcp_ipv4_and_ipv6(&data->ip_tcp_listen, 10) != 0) {
			RRR_MSG_ERR("Could not initialize TCP network in ip instance %s\n", INSTANCE_D_NAME(thread_data));
			goto out_message;
		}
		RRR_DBG_1("ip instance %s listening on TCP port %d\n",
				INSTANCE_D_NAME(thread_data), data->source_tcp_port);
	}

	pthread_cleanup_push(rrr_ip_network_cleanup, &data->ip_udp);
	pthread_cleanup_push(rrr_ip_network_cleanup, &data->ip_tcp_listen);
	pthread_cleanup_push(rrr_ip_accept_data_collection_clear_void, &tcp_accept_data);
	pthread_cleanup_push(rrr_ip_accept_data_collection_clear_void, &tcp_connect_data);

	rrr_thread_set_state(thread, RRR_THREAD_STATE_RUNNING);

	RRR_STATS_INSTANCE_POST_DEFAULT_STICKIES;

	uint64_t prev_read_error_count = 0;
	uint64_t prev_read_count = 0;
	uint64_t prev_polled_count = 0;

	uint64_t next_stats_time = 0;
	unsigned int tick = 0;
	while (!rrr_thread_check_encourage_stop(thread_data->thread)) {
		rrr_update_watchdog_time(thread_data->thread);

		if (has_senders != 0) {
			if (poll_do_poll_delete_simple (&poll, thread_data, poll_callback, 0) != 0) {
				break;
			}
			if (poll_do_poll_delete_ip_simple (&poll_ip, thread_data, poll_callback_ip, 0) != 0) {
				break;
			}
		}

		struct input_callback_data input_callback_data = {
			data,
			&tcp_connect_data
		};

		struct rrr_fifo_callback_args callback_args = {
			thread_data,
			&input_callback_data,
			0
		};

		if (rrr_fifo_read_clear_forward(&data->send_buffer, NULL, input_callback, &callback_args, 0) != 0) {
			RRR_MSG_ERR("Error while iterating input buffer in ip instance %s\n", INSTANCE_D_NAME(thread_data));
			break;
		}

		if (RRR_LL_COUNT(&data->definitions) > 0) {
			if (udp_read_data(data) != 0) {
				break;
			}
			if (tcp_read_data(data, &tcp_accept_data) != 0) {
				break;
			}
			struct rrr_fifo_callback_args callback_data = {NULL, data, 0};
			if (rrr_fifo_read_clear_forward(&data->inject_buffer, NULL, inject_callback, &callback_data, 0) != 0) {
				RRR_MSG_ERR("Error from inject buffer in ip instance %s\n", INSTANCE_D_NAME(data->thread_data));
				break;
			}

		}

		// Sleep if nothing happened
		if (prev_read_count == data->messages_count_read &&
			prev_polled_count == data->messages_count_polled &&
			prev_read_error_count == data->read_error_count
		) {
			usleep(25000);
		}

		uint64_t time_now = rrr_time_get_64();

		if (stats != NULL && time_now > next_stats_time) {
			rrr_stats_instance_update_rate(stats, 1, "read_error_count", data->read_error_count);
			rrr_stats_instance_update_rate(stats, 2, "read_count", data->messages_count_read);
			rrr_stats_instance_update_rate(stats, 3, "polled_count", data->messages_count_polled);
			rrr_stats_instance_post_unsigned_base10_text (
					stats,
					"delivery_buffer_count",
					0,
					rrr_fifo_buffer_get_entry_count(&data->delivery_buffer)
			);
			tick = 0;
			data->read_error_count = 0;
			data->messages_count_read = 0;
			data->messages_count_polled = 0;
			next_stats_time = time_now + 1000000;

			if (rrr_fifo_buffer_get_entry_count(&data->delivery_buffer) > 10000) {
				RRR_DBG_1("Enabling ratelimit on buffer in ip instance %s due to slow reader\n", INSTANCE_D_NAME(thread_data));
				rrr_fifo_buffer_set_do_ratelimit(&data->delivery_buffer, 1);
			}
		}

		prev_read_error_count = data->read_error_count;
		prev_read_count = data->messages_count_read;
		prev_polled_count = data->messages_count_polled;

		tick++;
	}

	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);

	out_message:

	RRR_DBG_1 ("ip instance %s stopping\n", thread_data->init_data.instance_config->name);
	// Set running in case we failed before getting around to do that
	if (!rrr_thread_check_state(thread, RRR_THREAD_STATE_RUNNING)) {
		rrr_thread_set_state(thread, RRR_THREAD_STATE_RUNNING);
	}
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	RRR_STATS_INSTANCE_CLEANUP_WITH_PTHREAD_CLEANUP_POP;
	pthread_exit(0);
}

static int test_config (struct rrr_instance_config *config) {
	struct ip_data data;
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
	thread_entry_ip,
	NULL,
	poll,
	NULL,
	poll_delete,
	poll_delete_ip,
	test_config,
	inject,
	NULL
};

static const char *module_name = "ip";

__attribute__((constructor)) void load(void) {
}

void init(struct rrr_instance_dynamic_data *data) {
		data->module_name = module_name;
		data->type = RRR_MODULE_TYPE_FLEXIBLE;
		data->operations = module_operations;
		data->dl_ptr = NULL;
		data->private_data = NULL;
		data->start_priority = RRR_THREAD_START_PRIORITY_NETWORK;
}

void unload(void) {
}

