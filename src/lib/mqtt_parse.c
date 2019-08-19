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
#include <endian.h>
#include <ctype.h>

#include "mqtt_packet.h"
#include "mqtt_parse.h"
#include "mqtt_property.h"
#include "mqtt_subscription.h"
#include "utf8.h"

#define PARSE_CHECK_END_RAW(end,final_end)										\
	((end) > (final_end))

#define PARSE_CHECK_END_AND_RETURN_RAW(end,final_end)							\
	do { if (PARSE_CHECK_END_RAW(end, final_end)) {								\
		return RRR_MQTT_PARSE_INCOMPLETE;										\
	}} while (0)

#define PARSE_CHECK_TARGET_END()												\
	PARSE_CHECK_END_RAW((end)+1,(session)->buf+(session)->target_size)

#define PARSE_CHECK_END_AND_RETURN(end,session)									\
	PARSE_CHECK_END_AND_RETURN_RAW((end),(session)->buf+(session)->buf_size)

#define PASTE(x, y) x ## y

#define PARSE_BEGIN(type)																\
	int ret = RRR_MQTT_PARSE_OK;														\
	const char *start = NULL; (void)(start);											\
	const char *end = NULL; (void)(end);												\
	ssize_t bytes_parsed = 0; (void)(bytes_parsed);										\
	uint16_t blob_length = 0; (void)(blob_length);										\
	ssize_t payload_length = 0; (void)(payload_length);									\
	struct PASTE(rrr_mqtt_p_,type) *type = NULL;									\
	if (RRR_MQTT_PARSE_STATUS_PAYLOAD_IS_DONE(session)) {										\
		VL_BUG("rrr_mqtt_parse called for same packet again after payload was done\n");	\
	}																					\
	if (RRR_MQTT_PARSE_VARIABLE_HEADER_IS_DONE(session)) {								\
		goto parse_payload;																\
	}																					\
	start = end = session->buf + session->variable_header_pos

#define PARSE_REQUIRE_PROTOCOL_VERSION()												\
	if (session->protocol_version == NULL) {											\
		return RRR_MQTT_PARSE_INCOMPLETE;												\
	}

#define PARSE_ALLOCATE(type)															\
	if (session->packet == NULL) {														\
		session->packet = session->type_properties->allocate (							\
			session->type_properties,													\
			session->protocol_version													\
		);																				\
		if (session->packet == NULL) {													\
			VL_MSG_ERR("Could not allocate packet of type %s while parsing\n",			\
				session->type_properties->name);										\
			return RRR_MQTT_PARSE_INTERNAL_ERROR;										\
		}																				\
	}																					\
	type = (struct PASTE(rrr_mqtt_p_,type) *) session->packet; (void)(type)

#define PARSE_PACKET_ID(target) 										\
	start = end;														\
	end = start + 2;													\
	PARSE_CHECK_END_AND_RETURN(end,session);							\
	(target)->packet_identifier = be16toh(*((uint16_t *) start))

#define PARSE_PREPARE(bytes)											\
	start = end;														\
	end = start + bytes;												\
	PARSE_CHECK_END_AND_RETURN(end,session)

#define PARSE_U8_RAW(target)						\
	PARSE_PREPARE(1);								\
	(target) = *((uint8_t*) start);

#define PARSE_U16_RAW(target)						\
	PARSE_PREPARE(2);								\
	(target) = be16toh(*((uint16_t*) start));

#define PARSE_U8(type,target)						\
	PARSE_U8_RAW((type)->target)

#define PARSE_U16(type,target)						\
	PARSE_U16_RAW((type)->target)

#define PARSE_CHECK_V5(type)									\
	((type)->protocol_version->id >= RRR_MQTT_VERSION_5)

#define PARSE_VALIDATE_QOS(qos)													\
	if ((qos) > 2) {															\
		VL_MSG_ERR("Invalid QoS flags %u in %s packet\n",						\
			(qos), RRR_MQTT_P_GET_TYPE_NAME(session->packet));					\
		return RRR_MQTT_PARSE_PARAMETER_ERROR;									\
	}

#define PARSE_VALIDATE_RETAIN(retain)											\
	if ((retain) > 2) {															\
		VL_MSG_ERR("Invalid retain flags %u in %s packet\n",					\
			(retain), RRR_MQTT_P_GET_TYPE_NAME(session->packet));				\
		return RRR_MQTT_PARSE_PARAMETER_ERROR;									\
	}

#define PARSE_VALIDATE_RESERVED(reserved, value)								\
	if ((reserved) != value) {													\
		VL_MSG_ERR("Invalid reserved flags %u in %s packet, must be %u\n",		\
			(reserved), RRR_MQTT_P_GET_TYPE_NAME(session->packet), (value));	\
		return RRR_MQTT_PARSE_PARAMETER_ERROR;									\
	}

#define PARSE_VALIDATE_ZERO_RESERVED(reserved)									\
		PARSE_VALIDATE_RESERVED(reserved, 0)

#define PARSE_PROPERTIES_IF_V5(type,target)														\
	do {if (PARSE_CHECK_V5(type)) {																\
		start = end;																			\
		ret = __rrr_mqtt_parse_properties(&(type)->target, session, start, &bytes_parsed);		\
		if (ret != 0) {																			\
			if (ret != RRR_MQTT_PARSE_INCOMPLETE) {												\
				VL_MSG_ERR("Error while parsing properties of MQTT packet of type %s\n",		\
					RRR_MQTT_P_GET_TYPE_NAME(type));											\
			}																					\
			return ret;																			\
		}																						\
		end = start + bytes_parsed;																\
	}} while (0)

#define PARSE_UTF8(type,target)																	\
	start = end;																				\
	RRR_FREE_IF_NOT_NULL(type->target);															\
	if ((ret = __rrr_mqtt_parse_utf8 (															\
			&type->target,																		\
			start,																				\
			session->buf + session->buf_size,													\
			&bytes_parsed																		\
	)) != 0) {																					\
		if (ret != RRR_MQTT_PARSE_INCOMPLETE) {													\
			VL_MSG_ERR("Error while parsing UTF8 of MQTT message of type %s\n",					\
					RRR_MQTT_P_GET_TYPE_NAME(type));											\
		}																						\
		return ret;																				\
	}																							\
	end = start + bytes_parsed

#define PARSE_BLOB(type,target)																	\
	start = end;																				\
	RRR_FREE_IF_NOT_NULL(type->target);															\
	if ((ret = __rrr_mqtt_parse_blob (															\
			&type->target,																		\
			start,																				\
			session->buf + session->buf_size,													\
			&bytes_parsed,																		\
			&blob_length																		\
	)) != 0) {																					\
		if (ret != RRR_MQTT_PARSE_INCOMPLETE) {													\
			VL_MSG_ERR("Error while parsing blob of MQTT message of type %s\n",					\
					RRR_MQTT_P_GET_TYPE_NAME(type));											\
		}																						\
		return ret;																				\
	}																							\
	end = start + bytes_parsed

#define PARSE_CHECK_ZERO_PAYLOAD()																\
	payload_length = session->target_size - session->payload_pos;								\
	if (payload_length < 0) {																	\
		VL_BUG("Payload length was < 0 while parsing\n");										\
	}																							\
	if (payload_length == 0) {																	\
		goto parse_done;																		\
	}

#define PARSE_PAYLOAD_SAVE_CHECKPOINT()															\
	session->payload_checkpoint = end - session->buf

#define PARSE_END_HEADER_BEGIN_PAYLOAD_AT_CHECKPOINT(type)										\
	RRR_MQTT_PARSE_STATUS_SET(session,RRR_MQTT_PARSE_STATUS_VARIABLE_HEADER_DONE);				\
	PARSE_PAYLOAD_SAVE_CHECKPOINT();															\
	session->payload_pos = end - session->buf;													\
	goto parse_payload;																			\
	parse_payload:																				\
	type = (struct PASTE(rrr_mqtt_p_,type) *) session->packet;							\
	end = session->buf + session->payload_checkpoint

#define PARSE_END_PAYLOAD()																		\
	goto parse_done;																			\
	parse_done:																					\
	RRR_MQTT_PARSE_STATUS_SET(session,RRR_MQTT_PARSE_STATUS_PAYLOAD_DONE);						\
	RRR_MQTT_PARSE_STATUS_SET(session,RRR_MQTT_PARSE_STATUS_VARIABLE_HEADER_DONE);				\
	return ret;

#define PARSE_END_NO_HEADER(type)																	\
	if (!PARSE_CHECK_TARGET_END()) {																\
		VL_MSG_ERR("Data after fixed header in mqtt packet type %s which has no variable header\n",	\
				session->type_properties->name);													\
	}																								\
	PARSE_END_HEADER_BEGIN_PAYLOAD_AT_CHECKPOINT(type);												\
	PARSE_END_PAYLOAD()


void rrr_mqtt_parse_session_destroy (
		struct rrr_mqtt_parse_session *session
) {
	if (session->buf == NULL) {
		return;
	}

	if (session->packet != NULL) {
//		printf ("Packet refcount in rrr_mqtt_parse_session_destroy: %i\n", rrr_mqtt_p_get_refcount(session->packet));
		RRR_MQTT_P_DECREF(session->packet);
		session->packet = NULL;
	}
}

void rrr_mqtt_parse_session_init (
		struct rrr_mqtt_parse_session *session
) {
	memset(session, '\0', sizeof(*session));
}

void rrr_mqtt_parse_session_update (
		struct rrr_mqtt_parse_session *session,
		const char *buf,
		ssize_t buf_size,
		const struct rrr_mqtt_p_protocol_version *protocol_version
) {
	session->buf = buf;
	session->buf_size = buf_size;

	// May be NULL before CONNECT packet has been received or sent
	session->protocol_version = protocol_version;
}

static int __rrr_mqtt_parse_variable_int (uint32_t *target, ssize_t *bytes_parsed, const void *buf, ssize_t len) {
	ssize_t pos = 0;
	uint32_t result = 0;
	uint32_t exponent = 1;
	uint8_t carry = 1;

	*target = 0;
	*bytes_parsed = 0;

	if (len == 0) {
		VL_BUG("__rrr_mqtt_packet_parse_variable_int called with zero length");
	}

	while (carry) {
		if (pos == len) {
			/* Could not finish the value, input too short */
			return RRR_MQTT_PARSE_INCOMPLETE;
		}
		if (pos > 3) {
			/* Only four bytes allowed */
			return RRR_MQTT_PARSE_OVERFLOW;
		}

		uint8_t current = *((uint8_t *) (buf + pos));
		uint8_t value = current & 0x7f;
		carry = current & 0x80;

		result += (value * exponent);

		exponent *= 128;
		pos++;
	}

	*target = result;
	*bytes_parsed = pos;

	return RRR_MQTT_PARSE_OK;
}

static int __rrr_mqtt_parse_blob (
		char **target, const char *start, const char *final_end, ssize_t *bytes_parsed, uint16_t *blob_length
) {
	if (*target != NULL) {
		VL_BUG ("target was not NULL in __rrr_mqtt_parse_blob\n");
	}

	const char *end = start + 2;
	*bytes_parsed = 2;

	PARSE_CHECK_END_AND_RETURN_RAW(end,final_end);
	*blob_length = be16toh(*((uint16_t *) start));

	*target = malloc((*blob_length) + 1);
	if (*target == NULL){
		VL_MSG_ERR("Could not allocate memory for UTF8 in __rrr_mqtt_parse_utf8\n");
		return RRR_MQTT_PARSE_INTERNAL_ERROR;
	}
	**target = '\0';

	start = end;
	end = start + *blob_length;
/*	{
		char buf_debug[(*blob_length) + 1];
		int length_available = final_end - start;
		if (length_available > 0) {
			int length_print = *blob_length > length_available ? length_available : *blob_length;
			memcpy(buf_debug, start, length_print);
			buf_debug[length_print] = '\0';
			printf ("blob string: %s\n", buf_debug);
		}
	}*/
	PARSE_CHECK_END_AND_RETURN_RAW(end,final_end);

	memcpy(*target, start, *blob_length);
	(*target)[*blob_length] = '\0';

	*bytes_parsed += *blob_length;

	return RRR_MQTT_PARSE_OK;
}

struct parse_utf8_validate_callback_data {
	uint32_t character;
	int has_illegal_character;
};

static int __rrr_mqtt_parse_utf8_validate_callback (uint32_t character, void *arg) {
	struct parse_utf8_validate_callback_data *data = arg;
	if (character == 0 || (character >= 0xD800 && character <= 0xDFFF)) {
		data->has_illegal_character = 1;
		data->character = character;
		return 1;
	}
	return 0;
}

static int __rrr_mqtt_parse_utf8 (
		char **target, const char *start, const char *final_end, ssize_t *bytes_parsed
) {
	uint16_t utf8_length = 0;
	int ret = __rrr_mqtt_parse_blob(target, start, final_end, bytes_parsed, &utf8_length);
	if (ret != RRR_MQTT_PARSE_OK) {
		return ret;
	}

	struct parse_utf8_validate_callback_data callback_data = {0, 0};
	if (rrr_utf8_validate_and_iterate(*target, utf8_length, __rrr_mqtt_parse_utf8_validate_callback, &callback_data) != 0) {
		VL_MSG_ERR ("Malformed UTF-8 detected in UTF8-data\n");
		if (callback_data.has_illegal_character == 1){
			VL_MSG_ERR("Illegal character 0x%04x\n", callback_data.character);
		}
		return RRR_MQTT_PARSE_PARAMETER_ERROR;
	}

	return RRR_MQTT_PARSE_OK;
}

#define RRR_PROPERTY_PARSER_DEFINITION \
		struct rrr_mqtt_property *target, struct rrr_mqtt_parse_session *session, \
		const char *start, ssize_t *bytes_parsed_final

static int __rrr_mqtt_parse_property_save_uint32 (struct rrr_mqtt_property *target, uint32_t value) {
	target->data = malloc(sizeof(value));
	if (target->data == NULL) {
		VL_MSG_ERR("Could not allocate memory in __rrr_mqtt_property_parse_integer\n");
		return RRR_MQTT_PARSE_INTERNAL_ERROR;
	}

	target->internal_data_type = RRR_MQTT_PROPERTY_DATA_TYPE_INTERNAL_UINT32;
	target->length = sizeof(value);
	memcpy (target->data, &value, sizeof(value));

	return RRR_MQTT_PARSE_OK;
}

static int __rrr_mqtt_parse_property_integer (struct rrr_mqtt_property *target, const char *start, ssize_t length) {
	int ret = RRR_MQTT_PARSE_OK;

	if (length > 4) {
		VL_BUG("Too many bytes in __rrr_mqtt_property_parse_integer\n");
	}

	union {
		uint32_t result;
		uint8_t bytes[4];
	} int_merged;

	int_merged.result = 0;

	int wpos = 3;
	int rpos = length - 1;
	while (rpos >= 0) {
		int_merged.bytes[wpos] = *((uint8_t *) (start + rpos));
		wpos--;
		rpos--;
	}

	int_merged.result = be32toh(int_merged.result);

	if ((ret = __rrr_mqtt_parse_property_save_uint32(target, int_merged.result)) != 0) {
		return ret;
	}

	target->length = length;

	return ret;
}

static int __rrr_mqtt_parse_property_one (RRR_PROPERTY_PARSER_DEFINITION) {
	int ret = RRR_MQTT_PARSE_OK;

	PARSE_CHECK_END_AND_RETURN(start + 1,session);

	ret = __rrr_mqtt_parse_property_integer(target, start, 1);
	if (ret != RRR_MQTT_PARSE_OK) {
		return ret;
	}

	*bytes_parsed_final = 1;

	return ret;
}

static int __rrr_mqtt_parse_property_two (RRR_PROPERTY_PARSER_DEFINITION) {
	int ret = RRR_MQTT_PARSE_OK;

	PARSE_CHECK_END_AND_RETURN(start + 2,session);

	ret = __rrr_mqtt_parse_property_integer(target, start, 2);
	if (ret != RRR_MQTT_PARSE_OK) {
		return ret;
	}

	*bytes_parsed_final = 2;

	return ret;
}

static int __rrr_mqtt_parse_property_four (RRR_PROPERTY_PARSER_DEFINITION) {
	int ret = RRR_MQTT_PARSE_OK;

	PARSE_CHECK_END_AND_RETURN(start + 4,session);

	ret = __rrr_mqtt_parse_property_integer(target, start, 4);
	if (ret != RRR_MQTT_PARSE_OK) {
		return ret;
	}

	*bytes_parsed_final = 4;

	return ret;
}

static int __rrr_mqtt_parse_property_vint (RRR_PROPERTY_PARSER_DEFINITION) {
	int ret = RRR_MQTT_PARSE_OK;

	uint32_t result = 0;

	ret = __rrr_mqtt_parse_variable_int(&result, bytes_parsed_final, start, session->buf_size - (start - session->buf));
	if (ret != RRR_MQTT_PARSE_OK) {
		return ret;
	}

	if ((ret = __rrr_mqtt_parse_property_save_uint32(target, result)) != 0) {
		return ret;
	}

	return ret;
}

static int __rrr_mqtt_parse_property_blob (RRR_PROPERTY_PARSER_DEFINITION) {
	int ret = RRR_MQTT_PARSE_OK;

	uint16_t blob_length = 0;
	ssize_t bytes_parsed = 0;

	if ((ret = __rrr_mqtt_parse_blob(&target->data, start, session->buf + session->buf_size, &bytes_parsed, &blob_length)) != 0) {
		return ret;
	}

	target->length = blob_length;
	target->internal_data_type = RRR_MQTT_PROPERTY_DATA_TYPE_INTERNAL_BLOB;

	bytes_parsed += blob_length;

	*bytes_parsed_final = bytes_parsed;

	return ret;
}

static int __rrr_mqtt_parse_property_utf8 (RRR_PROPERTY_PARSER_DEFINITION) {
	int ret = 0;

	ret = __rrr_mqtt_parse_utf8 (&target->data, start, session->buf + session->buf_size, bytes_parsed_final);

	return ret;
}

static int __rrr_mqtt_parse_property_2utf8 (RRR_PROPERTY_PARSER_DEFINITION) {
	int ret = 0;

	ssize_t bytes_parsed = 0;
	*bytes_parsed_final = 0;

	if ((ret = rrr_mqtt_property_new(&target->sibling, target->definition)) != 0) {
		return ret;
	}

	if ((ret = __rrr_mqtt_parse_property_utf8 (target, session, start, &bytes_parsed)) != 0) {
		return ret;
	}

	*bytes_parsed_final += bytes_parsed;

	start = start + bytes_parsed;
	if ((ret = __rrr_mqtt_parse_property_utf8 (target->sibling, session, start, &bytes_parsed)) != 0) {
		return ret;
	}

	*bytes_parsed_final += bytes_parsed;

	return ret;
}

static int (* const property_parsers[]) (RRR_PROPERTY_PARSER_DEFINITION) = {
		NULL,
		__rrr_mqtt_parse_property_one,
		__rrr_mqtt_parse_property_two,
		NULL,
		__rrr_mqtt_parse_property_four,
		__rrr_mqtt_parse_property_vint,
		__rrr_mqtt_parse_property_blob,
		__rrr_mqtt_parse_property_utf8,
		__rrr_mqtt_parse_property_2utf8
};

static int __rrr_mqtt_parse_properties (
		struct rrr_mqtt_property_collection *target,
		struct rrr_mqtt_parse_session *session,
		const char *start,
		ssize_t *bytes_parsed_final
) {
	int ret = 0;
	const char *end = NULL;

	*bytes_parsed_final = 0;

	uint32_t property_length = 0;
	ssize_t bytes_parsed = 0;
	ssize_t bytes_parsed_total = 0;

	rrr_mqtt_property_collection_destroy(target);

	ret = __rrr_mqtt_parse_variable_int(&property_length, &bytes_parsed, start, (session->buf_size - (start - session->buf)));

	if (ret != RRR_MQTT_PARSE_OK) {
		if (ret == RRR_MQTT_PARSE_OVERFLOW) {
			VL_MSG_ERR("Overflow while parsing property length variable int\n");
		}
		return ret;
	}

	bytes_parsed_total += bytes_parsed;
	start += bytes_parsed;
	while (1) {
		end = start + 1;
		PARSE_CHECK_END_AND_RETURN(end,session);

		uint8_t type = *((uint8_t *) start);

		const struct rrr_mqtt_property_definition *property_def = rrr_mqtt_property_get_definition(type);
		if (property_def == NULL) {
			VL_MSG_ERR("Unknown mqtt property field found: 0x%02x\n", type);
			return RRR_MQTT_PARSE_PARAMETER_ERROR;
		}

		struct rrr_mqtt_property *property = NULL;
		if ((ret = rrr_mqtt_property_new(&property, property_def)) != 0) {
			return RRR_MQTT_PARSE_INTERNAL_ERROR;
		}

		start = end;
		ret = property_parsers[property_def->type](property, session, start, &bytes_parsed);
		if (ret != 0) {
			rrr_mqtt_property_destroy(property);
			return ret;
		}

		rrr_mqtt_property_collection_add (target, property);

		bytes_parsed_total += bytes_parsed;
		start = end + bytes_parsed;
	}

	*bytes_parsed_final = bytes_parsed_total;

	return ret;
}

static int __rrr_mqtt_parse_protocol_version_validate_name (
		const struct rrr_mqtt_p_protocol_version *protocol_version,
		const char *string
) {
	int len = strlen(string);

	char uppercase[len + 1];
	uppercase[len] = '\0';

	const char *input = string;
	char *output = uppercase;
	while (*input) {
		*output = toupper(*input);
		output++;
		input++;
	}

	if (strcmp(protocol_version->name, uppercase) == 0) {
		return 0;
	}

	return 1;
}

int rrr_mqtt_parse_connect (struct rrr_mqtt_parse_session *session) {
	PARSE_BEGIN(connect);

	PARSE_PREPARE(2);
	uint16_t protocol_name_length = be16toh(*((uint16_t *) start));

	if (protocol_name_length > 6) {
		VL_MSG_ERR("Protocol name in connect packet was too long\n");
		return RRR_MQTT_PARSE_PARAMETER_ERROR;
	}

	PARSE_PREPARE(protocol_name_length);

	char name_buf[7];
	strncpy(name_buf, start, protocol_name_length);
	name_buf[protocol_name_length] = '\0';

	PARSE_PREPARE(1);
	uint8_t protocol_version_id = *((uint8_t *) start);

	const struct rrr_mqtt_p_protocol_version *protocol_version = rrr_mqtt_p_get_protocol_version(protocol_version_id);
	if (protocol_version == NULL) {
		VL_MSG_ERR("MQTT protocol version could not be found, input name was '%s' version was '%u'\n",
				name_buf, protocol_version_id);
		return RRR_MQTT_PARSE_PARAMETER_ERROR;
	}

	if (__rrr_mqtt_parse_protocol_version_validate_name(protocol_version, name_buf) != 0) {
		VL_MSG_ERR("MQTT protocol version name mismatch, input name was '%s' version was '%u'. Expected name '%s'\n",
				name_buf, protocol_version_id, protocol_version->name);
		return RRR_MQTT_PARSE_PARAMETER_ERROR;
	}

	session->protocol_version = protocol_version;

	PARSE_ALLOCATE(connect);

	// CONNECT FLAGS
	PARSE_U8(connect,connect_flags);

	PARSE_VALIDATE_ZERO_RESERVED(RRR_MQTT_P_CONNECT_GET_FLAG_RESERVED(connect));

	if (RRR_MQTT_P_CONNECT_GET_FLAG_WILL(connect) == 0) {
		if (RRR_MQTT_P_CONNECT_GET_FLAG_WILL_QOS(connect) != 0 || RRR_MQTT_P_CONNECT_GET_FLAG_WILL_RETAIN(connect) != 0) {
			VL_MSG_ERR("WILL flag of mqtt connect packet was zero, but not WILL_QOS and WILL_RETAIN\n");
			return RRR_MQTT_PARSE_PARAMETER_ERROR;
		}
	}

	if (connect->protocol_version->id < RRR_MQTT_VERSION_5) {
		if (RRR_MQTT_P_CONNECT_GET_FLAG_PASSWORD(connect) == 1 && RRR_MQTT_P_CONNECT_GET_FLAG_USER_NAME(connect) == 0) {
			VL_MSG_ERR("Password flag was set in mqtt connect packet but not username flag. Not allowed for protocol version <5\n");
			return RRR_MQTT_PARSE_PARAMETER_ERROR;
		}
	}

	PARSE_U16(connect,keep_alive);
	PARSE_PROPERTIES_IF_V5(connect,properties);

	PARSE_END_HEADER_BEGIN_PAYLOAD_AT_CHECKPOINT(connect);

	PARSE_UTF8(connect,client_identifier);

	if (RRR_MQTT_P_CONNECT_GET_FLAG_WILL(connect) != 0) {
		PARSE_PROPERTIES_IF_V5(connect,will_properties);
		PARSE_UTF8(connect,will_topic);
		PARSE_BLOB(connect,will_message);
	}

	if (RRR_MQTT_P_CONNECT_GET_FLAG_USER_NAME(connect) != 0) {
		PARSE_UTF8(connect,username);
	}

	if (RRR_MQTT_P_CONNECT_GET_FLAG_PASSWORD(connect) != 0) {
		PARSE_UTF8(connect,password);
	}

	PARSE_END_PAYLOAD();
 }

int rrr_mqtt_parse_connack (struct rrr_mqtt_parse_session *session) {
	int ret = 0;
	return ret;
}

int rrr_mqtt_parse_publish (struct rrr_mqtt_parse_session *session) {
	PARSE_BEGIN(publish);
	PARSE_REQUIRE_PROTOCOL_VERSION();
	PARSE_ALLOCATE(publish);

	publish = (struct rrr_mqtt_p_publish *) session->packet;

	publish->dup = RRR_MQTT_P_PUBLISH_GET_FLAG_DUP(session);
	publish->qos = RRR_MQTT_P_PUBLISH_GET_FLAG_QOS(session);
	publish->retain = RRR_MQTT_P_PUBLISH_GET_FLAG_RETAIN(session);

	PARSE_VALIDATE_QOS(publish->qos);

	if (publish->qos == 0 && publish->dup != 0) {
		VL_MSG_ERR("Received a PUBLISH packet of QoS 0, but DUP was non zero\n");
		return RRR_MQTT_PARSE_PARAMETER_ERROR;
	}

	// PARSE TOPIC
	PARSE_UTF8(publish,topic);
	PARSE_PACKET_ID(publish);
	PARSE_PROPERTIES_IF_V5(publish,properties);

	PARSE_END_HEADER_BEGIN_PAYLOAD_AT_CHECKPOINT(publish);

	PARSE_CHECK_ZERO_PAYLOAD();

	// TODO : Implement maximum size of payload. We still however require a client to actually
	//        send the data before we allocate huge amounts of memory

	// The memory of a large payload is continiously being read in. We don't do anything until the
	// complete packet has been read, after which we order the read data to be moved to the
	// assembled_data-member of the packet. Memory will after that be managed by the packet.

	if (session->buf_size == session->target_size) {
		goto parse_done;
	}
	else if (session->buf_size > session->target_size) {
		VL_BUG("Read too many bytes in rrr_mqtt_parse_publish %li > %li\n",
				session->buf_size, session->target_size);
	}

	return RRR_MQTT_PARSE_INCOMPLETE;

	PARSE_END_PAYLOAD();
}

int rrr_mqtt_parse_puback (struct rrr_mqtt_parse_session *session) {
	int ret = 0;
	return ret;
}

int rrr_mqtt_parse_pubrec (struct rrr_mqtt_parse_session *session) {
	int ret = 0;
	return ret;
}

int rrr_mqtt_parse_pubrel (struct rrr_mqtt_parse_session *session) {
	int ret = 0;
	return ret;
}

int rrr_mqtt_parse_pubcomp (struct rrr_mqtt_parse_session *session) {
	int ret = 0;
	return ret;
}

int rrr_mqtt_parse_subscribe (struct rrr_mqtt_parse_session *session) {
	PARSE_BEGIN(subscribe);

	PARSE_REQUIRE_PROTOCOL_VERSION();
	PARSE_ALLOCATE(subscribe);

	PARSE_PACKET_ID(subscribe);
	PARSE_PROPERTIES_IF_V5(subscribe,properties);

	PARSE_END_HEADER_BEGIN_PAYLOAD_AT_CHECKPOINT(subscribe);

	/* If we need several attempts to parse the SUBSCRIBE-packet, the subscriptions parsed in the
	 * previous rounds are parsed again and overwritten. We do however skip to our payload position
	 * checkpoint to avoid doing this with all of the subscriptions, only at most one should actually
	 * be overwritten. */
	while (!PARSE_CHECK_TARGET_END()) {
		PARSE_UTF8(subscribe,data_tmp);

		uint8_t subscription_flags = 0;
		uint8_t reserved = 0;
		uint8_t retain = 0;
		uint8_t rap = 0;
		uint8_t nl = 0;
		uint8_t qos = 0;

		PARSE_U8_RAW(subscription_flags);

		reserved = RRR_MQTT_SUBSCRIPTION_GET_FLAG_RAW_RESERVED(subscription_flags);
		retain = RRR_MQTT_SUBSCRIPTION_GET_FLAG_RAW_RETAIN(subscription_flags);
		rap = RRR_MQTT_SUBSCRIPTION_GET_FLAG_RAW_RAP(subscription_flags);
		nl = RRR_MQTT_SUBSCRIPTION_GET_FLAG_RAW_NL(subscription_flags);
		qos = RRR_MQTT_SUBSCRIPTION_GET_FLAG_RAW_QOS(subscription_flags);

		PARSE_VALIDATE_QOS(qos);
		PARSE_VALIDATE_ZERO_RESERVED(reserved);

		if (PARSE_CHECK_V5(subscribe)) {
			PARSE_VALIDATE_RETAIN(retain);
		}
		else {
			PARSE_VALIDATE_ZERO_RESERVED(retain);
			PARSE_VALIDATE_ZERO_RESERVED(rap);
			PARSE_VALIDATE_ZERO_RESERVED(nl);
		}

		struct rrr_mqtt_subscription *subscription = NULL;
		ret = rrr_mqtt_subscription_new (&subscription, subscribe->data_tmp, retain, rap, nl, qos);
		if (ret != 0) {
			VL_MSG_ERR("Could not allocate subscription in rrr_mqtt_parse_subscribe\n");
			return RRR_MQTT_PARSE_INTERNAL_ERROR;
		}

		ret = rrr_mqtt_subscription_collection_append_unique (subscribe->subscriptions, &subscription);
		if (ret != RRR_MQTT_SUBSCRIPTION_OK) {
			rrr_mqtt_subscription_destroy(subscription);
			VL_MSG_ERR("Error while adding subscription to collection in rrr_mqtt_parse_subscribe\n");
			return RRR_MQTT_PARSE_INTERNAL_ERROR;
		}

		PARSE_PAYLOAD_SAVE_CHECKPOINT();
	}

	goto parse_done;

	PARSE_END_PAYLOAD();
}

int rrr_mqtt_parse_suback (struct rrr_mqtt_parse_session *session) {
	int ret = 0;
	return ret;
}

int rrr_mqtt_parse_unsubscribe (struct rrr_mqtt_parse_session *session) {
	int ret = 0;
	return ret;
}

int rrr_mqtt_parse_unsuback (struct rrr_mqtt_parse_session *session) {
	int ret = 0;
	return ret;
}

int rrr_mqtt_parse_pingreq (struct rrr_mqtt_parse_session *session) {
	PARSE_BEGIN(pingreq);

	PARSE_REQUIRE_PROTOCOL_VERSION();
	PARSE_ALLOCATE(pingreq);

	PARSE_END_NO_HEADER(pingreq);
}

int rrr_mqtt_parse_pingresp (struct rrr_mqtt_parse_session *session) {
	PARSE_BEGIN(pingresp);

	PARSE_REQUIRE_PROTOCOL_VERSION();
	PARSE_ALLOCATE(pingresp);

	PARSE_END_NO_HEADER(pingresp);
}

int rrr_mqtt_parse_disconnect (struct rrr_mqtt_parse_session *session) {
	PARSE_BEGIN(disconnect);
	PARSE_REQUIRE_PROTOCOL_VERSION();
	PARSE_ALLOCATE(disconnect);

	if (session->protocol_version->id < 5) {
		// Non-zero length NOT allowed for V3.1
		if (session->target_size - session->variable_header_pos != 0) {
			VL_MSG_ERR("Received MQTT V3.1 DISCONNECT packet with non-zero remaining length %li\n",
					session->target_size - session->variable_header_pos);
			return RRR_MQTT_PARSE_PARAMETER_ERROR;
		}
		goto parse_done;
	}
	else if (session->target_size - session->variable_header_pos == 0) {
		// Zero or non-zero length allowed for V5
		goto parse_done;
	}

	PARSE_PREPARE(1);
	disconnect->disconnect_reason_code = *((uint8_t*) start);

	PARSE_PROPERTIES_IF_V5(disconnect,properties);

	PARSE_END_HEADER_BEGIN_PAYLOAD_AT_CHECKPOINT(disconnect);
	PARSE_END_PAYLOAD();
}

int rrr_mqtt_parse_auth (struct rrr_mqtt_parse_session *session) {
	int ret = 0;
	return ret;
}


#define RRR_MQTT_PARSE_GET_TYPE(p)			(((p)->type & ((uint8_t) 0xF << 4)) >> 4)
#define RRR_MQTT_PARSE_GET_TYPE_FLAGS(p)	((p)->type & ((uint8_t) 0xF))

int rrr_mqtt_packet_parse (
		struct rrr_mqtt_parse_session *session
) {
	int ret = 0;

	/*
	 * We might return 0 on error if the error is data-related and it's
	 * the client's fault. In that case, we only set the error status flag.
	 * On other horrendous errors, we return 1.
	 */

	if (session->buf == NULL) {
		VL_BUG("buf was NULL in rrr_mqtt_packet_parse\n");
	}
	if (RRR_MQTT_PARSE_IS_ERR(session)) {
		VL_BUG("rrr_mqtt_packet_parse called with error flag set, connection should have been closed.\n");
	}
	if (RRR_MQTT_PARSE_IS_COMPLETE(session)) {
		VL_BUG("rrr_mqtt_packet_parse called while parsing was complete\n");
	}

	if (session->buf_size < 2) {
		goto out;
	}

	if (!RRR_MQTT_PARSE_FIXED_HEADER_IS_DONE(session)) {
		const struct rrr_mqtt_p_header *header = (const struct rrr_mqtt_p_header *) session->buf;

		if (RRR_MQTT_PARSE_GET_TYPE(header) == 0) {
			VL_MSG_ERR("Received 0 header type in rrr_mqtt_packet_parse\n");
			RRR_MQTT_PARSE_STATUS_SET_ERR(session);
			goto out;
		}

		const struct rrr_mqtt_p_type_properties *properties = rrr_mqtt_p_get_type_properties(RRR_MQTT_PARSE_GET_TYPE(header));

		VL_DEBUG_MSG_3("Received mqtt packet of type %u name %s\n",
				properties->type_id, properties->name);

		if (properties->has_reserved_flags != 0 && RRR_MQTT_PARSE_GET_TYPE_FLAGS(header) != properties->flags) {
			VL_MSG_ERR("Invalid reserved flags %u received in mqtt packet of type %s\n",
					RRR_MQTT_PARSE_GET_TYPE_FLAGS(header),
					properties->name
			);
			RRR_MQTT_PARSE_STATUS_SET_ERR(session);
			goto out;
		}

		uint32_t remaining_length = 0;
		ssize_t bytes_parsed = 0;
		if ((ret = __rrr_mqtt_parse_variable_int (
				&remaining_length,
				&bytes_parsed,
				header->length,
				session->buf_size - sizeof(header->type))
		) != 0) {
			if (ret == RRR_MQTT_PARSE_INCOMPLETE) {
				/* Not enough bytes were read */
				ret = 0;
				goto out;
			}
			else {
				VL_MSG_ERR("Parse error in packet fixed header remaining length of type %s\n",
						properties->name);
				RRR_MQTT_PARSE_STATUS_SET_ERR(session);
				goto out;
			}
		}

		session->variable_header_pos = sizeof(header->type) + bytes_parsed;
		session->target_size = sizeof(header->type) + bytes_parsed + remaining_length;
		session->type = RRR_MQTT_PARSE_GET_TYPE(header);
		session->type_flags = RRR_MQTT_PARSE_GET_TYPE_FLAGS(header);
		session->type_properties = properties;

		VL_DEBUG_MSG_3 ("parsed a packet fixed header of type %s\n",
				properties->name);

		RRR_MQTT_PARSE_STATUS_SET(session,RRR_MQTT_PARSE_STATUS_FIXED_HEADER_DONE);
	}

	if (!RRR_MQTT_PARSE_VARIABLE_HEADER_IS_DONE(session)) {
		session->header_parse_attempts++;
		if (session->header_parse_attempts > 10) {
			VL_MSG_ERR("Could not parse packet of type %s after 10 attempts, input might be too short or CONNECT missing\n",
					session->type_properties->name);
			RRR_MQTT_PARSE_STATUS_SET_ERR(session);
			goto out;
		}
	}

	if ((ret = session->type_properties->parse(session)) != RRR_MQTT_PARSE_OK) {
		if (ret == RRR_MQTT_PARSE_INCOMPLETE) {
			/* Not enough bytes were read or CONNECT is not yet handled (protocol version not set) */
			ret = 0;
			goto out;
		}
		else {
			VL_MSG_ERR("Error from mqtt parse function of type %s\n",
					session->type_properties->name);
			ret = 1;
			goto out;
		}
	}

	/* Type parser might haver set error flag */
	if (RRR_MQTT_PARSE_IS_ERR(session)) {
		goto out;
	}

	if (RRR_MQTT_PARSE_STATUS_PAYLOAD_IS_DONE(session)) {
		RRR_MQTT_PARSE_STATUS_SET(session,RRR_MQTT_PARSE_STATUS_COMPLETE);
	}

	out:
	if (ret != 0) {
		RRR_MQTT_PARSE_STATUS_SET(session,RRR_MQTT_PARSE_STATUS_ERR);
	}
	return ret;
}

int rrr_mqtt_packet_parse_finalize (
		struct rrr_mqtt_p **packet,
		struct rrr_mqtt_parse_session *session
) {
	int ret = 0;

	if (session->packet == NULL || !RRR_MQTT_PARSE_STATUS_PAYLOAD_IS_DONE(session)) {
		VL_BUG("Invalid preconditions for rrr_mqtt_packet_parse_finalize\n");
	}

	session->packet->type_flags = session->type_flags;
	*packet = session->packet;
	session->packet = NULL;

	rrr_mqtt_parse_session_destroy(session);

	return ret;
}
