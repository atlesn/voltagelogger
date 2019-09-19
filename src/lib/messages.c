/*

Read Route Record

Copyright (C) 2018-2019 Atle Solbakken atle@goliathdns.no

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

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include "utf8.h"
#include "rrr_endian.h"
#include "rrr_socket.h"
#include "messages.h"
#include "../global.h"

// {MSG|MSG_ACK|MSG_TAG}:{AVG|MAX|MIN|POINT|INFO}:{CRC32}:{LENGTH}:{TIMESTAMP_FROM}:{TIMESTAMP_TO}:{DATA}

struct vl_message *message_new_reading (
		vl_u64 reading_millis,
		vl_u64 time
) {
	struct vl_message *res;

	if (message_new_empty (
			&res,
			MSG_TYPE_MSG,
			0,
			MSG_CLASS_POINT,
			time,
			time,
			reading_millis,
			0,
			0
	) != 0) {
		return NULL;
	}

	return res;
}

struct vl_message *message_new_array (
	vl_u64 time,
	vl_u16 topic_length,
	vl_u32 data_length
) {
	struct vl_message *res;

	if (message_new_empty (
			(struct vl_message **) &res,
			MSG_TYPE_MSG,
			0,
			MSG_CLASS_ARRAY,
			time,
			time,
			0,
			topic_length,
			data_length
	) != 0) {
		return NULL;
	}

	return res;
}

int message_new_empty (
		struct vl_message **final_result,
		vl_u16 type,
		vl_u16 type_flags,
		vl_u32 class,
		vl_u64 timestamp_from,
		vl_u64 timestamp_to,
		vl_u64 data_numeric,
		vl_u16 topic_length,
		vl_u32 data_length
) {
	ssize_t total_size = sizeof(struct vl_message) - 1 + topic_length + data_length;
	// -1 because the char which points to the data holds 1 byte
	struct vl_message *result = malloc(total_size);
	if (result == NULL) {
		VL_MSG_ERR("Could not allocate memory in new_empty_message\n");
		return 1;
	}

	memset(result, '\0', total_size);

	rrr_socket_msg_populate_head (
			(struct rrr_socket_msg *) result,
			RRR_SOCKET_MSG_TYPE_VL_MESSAGE,
			total_size,
			0
	);

	result->type = type;
	result->type_flags = type_flags;
	result->class = class;
	result->timestamp_from = timestamp_from;
	result->timestamp_to = timestamp_to;
	result->data_numeric = data_numeric;
	result->topic_length = topic_length;

	*final_result = result;

	return 0;
}

int message_new_with_data (
		struct vl_message **final_result,
		vl_u16 type,
		vl_u16 type_flags,
		vl_u32 class,
		vl_u64 timestamp_from,
		vl_u64 timestamp_to,
		vl_u64 data_numeric,
		const char *topic,
		vl_u16 topic_length,
		const char *data,
		vl_u32 data_length
) {
	if (message_new_empty (
			final_result,
			type,
			type_flags,
			class,
			timestamp_from,
			timestamp_to,
			data_numeric,
			topic_length,
			data_length
	) != 0) {
		return 1;
	}

	memcpy (MSG_TOPIC_PTR(*final_result), topic, topic_length);
	memcpy (MSG_DATA_PTR(*final_result), data, data_length);

	return 0;
}

int message_to_string (
	char **final_target,
	struct vl_message *message
) {
	int ret = 0;

	char *target = malloc(128);
	if (target == NULL) {
		VL_MSG_ERR("Could not allocate memory in message_to_string\n");
		ret = 1;
		goto out;
	}

	const char *type;
	switch (message->type) {
	case MSG_TYPE_MSG:
		type = MSG_TYPE_MSG_STRING;
		break;
	case MSG_TYPE_ACK:
		type = MSG_TYPE_ACK_STRING;
		break;
	case MSG_TYPE_TAG:
		type = MSG_TYPE_TAG_STRING;
		break;
	default:
		VL_MSG_ERR ("Unknown type %" PRIu32 " in message while converting to string\n", message->type);
		ret = 1;
		goto out;
	}

	const char *class;
	switch (message->class) {
	case MSG_CLASS_POINT:
		class = MSG_CLASS_POINT_STRING;
		break;
	case MSG_CLASS_AVG:
		class = MSG_CLASS_AVG_STRING;
		break;
	case MSG_CLASS_MAX:
		class = MSG_CLASS_MAX_STRING;
		break;
	case MSG_CLASS_MIN:
		class = MSG_CLASS_MIN_STRING;
		break;
	case MSG_CLASS_INFO:
		class = MSG_CLASS_INFO_STRING;
		break;
	case MSG_CLASS_ARRAY:
		class = MSG_CLASS_ARRAY_STRING;
		break;
	default:
		VL_MSG_ERR ("Unknown class %" PRIu32 " in message while converting to string\n", message->class);
		ret = 1;
		goto out;
	}

	sprintf(target, "%s:%s:%" PRIu64 ":%" PRIu64 ":%" PRIu64,
			type,
			class,
			message->timestamp_from,
			message->timestamp_to,
			message->data_numeric
	);

	*final_target = target;
	target = NULL;

	out:
	RRR_FREE_IF_NOT_NULL(target);
	return ret;
}

void flip_endianess_64(vl_u64 *value) {
	vl_u64 result = 0;

	result |= (*value & 0x00000000000000ff) << 56;
	result |= (*value & 0x000000000000ff00) << 40;
	result |= (*value & 0x0000000000ff0000) << 24;
	result |= (*value & 0x00000000ff000000) << 8;
	result |= (*value & 0x000000ff00000000) >> 8;
	result |= (*value & 0x0000ff0000000000) >> 24;
	result |= (*value & 0x00ff000000000000) >> 40;
	result |= (*value & 0xff00000000000000) >> 56;

	*value = result;
}

void flip_endianess_32(vl_u32 *value) {
	vl_u32 result = 0;

	result |= (*value & 0x000000ff) << 24;
	result |= (*value & 0x0000ff00) << 8;
	result |= (*value & 0x00ff0000) >> 8;
	result |= (*value & 0xff000000) >> 24;

	*value = result;
}

static int __message_validate (const struct vl_message *message){
	int ret = 0;

	if (message->msg_size < sizeof(*message) - 1 ||
			MSG_TOTAL_SIZE(message) != message->msg_size
	) {
		VL_MSG_ERR("Received a message in message_validate with invalid header size fields (%" PRIu32 " and %" PRIu32 ")\n",
				message->msg_size, MSG_TOTAL_SIZE(message));
		ret = 1;
		goto out;
	}
	if (!MSG_CLASS_OK(message)) {
		VL_MSG_ERR("Invalid class %u in message to message_validate\n", message->class);
		ret = 1;
	}
	if (!MSG_TYPE_OK(message)) {
		VL_MSG_ERR("Invalid type %u in message to message_validate\n", message->type);
		ret = 1;
	}
	if (rrr_utf8_validate(MSG_TOPIC_PTR(message), MSG_TOPIC_LENGTH(message)) != 0) {
		VL_MSG_ERR("Invalid topic for message in message_validate, not valid UTF-8\n");
		ret = 1;
	}

	out:
	return ret;
}

int message_to_host_and_verify (struct vl_message *message, ssize_t expected_size) {
	if (expected_size < ((ssize_t) sizeof(*message)) - 1) {
		VL_MSG_ERR("Message was too short in message_to_host_and_verify\n");
		return 1;
	}
	message->type = be16toh(message->type);
	message->type_flags = be16toh(message->type_flags);
	message->class = be16toh(message->class);
	message->version = be16toh(message->version);
	message->timestamp_from = be64toh(message->timestamp_from);
	message->timestamp_to = be64toh(message->timestamp_to);
	message->data_numeric = be64toh(message->data_numeric);
	message->topic_length = be16toh(message->topic_length);

	if (MSG_TOTAL_SIZE(message) != (unsigned int) expected_size) {
		VL_MSG_ERR("Size mismatch of message in message_to_host_and_verify actual size was %li stated size was %u\n",
				expected_size, MSG_TOTAL_SIZE(message));
		return 1;
	}

	return __message_validate(message);
}

void message_prepare_for_network (struct vl_message *message) {
	message->type = htobe16(message->type);
	message->type_flags = htobe16(message->type_flags);
	message->class = htobe16(message->class);
	message->version = htobe16(message->version);
	message->timestamp_from = htobe64(message->timestamp_from);
	message->timestamp_to = htobe64(message->timestamp_to);
	message->data_numeric = htobe64(message->data_numeric);
	message->topic_length = htobe16(message->topic_length);

	if (VL_DEBUGLEVEL_6) {
		VL_DEBUG_MSG("Message prepared for network: ");
		for (unsigned int i = 0; i < sizeof(*message); i++) {
			unsigned char *buf = (unsigned char *) message;
			VL_DEBUG_MSG("%x-", *(buf + i));
		}
		VL_DEBUG_MSG("\n");
	}
/*
	if (message_to_string (message, buf+1, buf_size) != 0) {
		VL_MSG_ERR ("ipclient: Error while converting message to string\n");
		return 1;
	}
*/
}

struct vl_message *message_duplicate_no_data_with_size (
		const struct vl_message *message,
		ssize_t topic_length,
		ssize_t data_length
) {
	ssize_t new_total_size = (sizeof (struct vl_message) - 1 + topic_length + data_length);

	struct vl_message *ret = malloc(new_total_size);
	if (ret == NULL) {
		VL_MSG_ERR("Could not allocate memory in message_duplicate\n");
		return NULL;
	}

	memset(ret, '\0', new_total_size);
	memcpy(ret, message, sizeof(*ret) - 2);

	ret->topic_length = topic_length;
	ret->network_size = new_total_size;
	ret->msg_size = new_total_size;

	return ret;
}

struct vl_message *message_duplicate (
		const struct vl_message *message
) {
	struct vl_message *ret = malloc(MSG_TOTAL_SIZE(message));
	if (ret == NULL) {
		VL_MSG_ERR("Could not allocate memory in message_duplicate\n");
		return NULL;
	}
	memcpy(ret, message, MSG_TOTAL_SIZE(message));
	return ret;
}

struct vl_message *message_duplicate_no_data (
		struct vl_message *message
) {
	ssize_t new_size = sizeof(struct vl_message) - 1 + MSG_TOPIC_LENGTH(message);
	struct vl_message *ret = malloc(new_size);
	if (ret == NULL) {
		VL_MSG_ERR("Could not allocate memory in message_duplicate\n");
		return NULL;
	}
	memcpy(ret, message, new_size);
	ret->network_size = new_size;
	ret->msg_size = new_size;
	return ret;
}

int message_set_topic (
		struct vl_message **message,
		const char *topic,
		ssize_t topic_len
) {
	struct vl_message *ret = message_duplicate_no_data_with_size(*message, topic_len, MSG_DATA_LENGTH(*message));
	if (ret == NULL) {
		VL_MSG_ERR("Could not allocate memory in message_set_topic\n");
		return 1;
	}

	memcpy(MSG_TOPIC_PTR(ret), topic, topic_len);
	memcpy(MSG_DATA_PTR(ret), MSG_DATA_PTR(*message), MSG_DATA_LENGTH(*message));

	free(*message);
	*message = ret;

	return 0;
}
