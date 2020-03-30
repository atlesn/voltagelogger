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

#ifndef RRR_INSTANCE_CONFIG_H
#define RRR_INSTANCE_CONFIG_H

#include "../global.h"
#include "settings.h"

struct rrr_array;
struct rrr_map;

struct rrr_instance_config {
	char *name;
	struct rrr_instance_settings *settings;
};

static inline int rrr_instance_config_setting_exists (
		struct rrr_instance_config *source,
		const char *name
) {
	return rrr_settings_exists(source->settings, name);
}

static inline int rrr_instance_config_get_string_noconvert (
		char **target,
		struct rrr_instance_config *source,
		const char *name
) {
	return rrr_settings_get_string_noconvert(target, source->settings, name);
}

static inline int rrr_instance_config_get_string_noconvert_silent (
		char **target,
		struct rrr_instance_config *source,
		const char *name
) {
	return rrr_settings_get_string_noconvert_silent(target, source->settings, name);
}

static inline int rrr_instance_config_read_unsigned_integer (
		rrr_setting_uint *target,
		struct rrr_instance_config *source,
		const char *name
) {
	return rrr_settings_read_unsigned_integer (target, source->settings, name);
}

static inline int rrr_instance_config_check_yesno (
		int *result,
		struct rrr_instance_config *source,
		const char *name
) {
	return rrr_settings_check_yesno (result, source->settings, name);
}

static inline int rrr_instance_config_traverse_split_commas_silent_fail (
		struct rrr_instance_config *source,
		const char *name,
		int (*callback)(const char *value, void *arg),
		void *arg
) {
	return rrr_settings_traverse_split_commas_silent_fail (source->settings, name, callback, arg);
}

static inline int rrr_instance_config_split_commas_to_array (
		struct rrr_settings_list **target,
		struct rrr_instance_config *source,
		const char *name
) {
	return rrr_settings_split_commas_to_array (target, source->settings, name);
}

static inline int rrr_instance_config_dump (
		struct rrr_instance_config *source
) {
	return rrr_settings_dump (source->settings);
}

void rrr_instance_config_destroy (
		struct rrr_instance_config *config
);

struct rrr_instance_config *rrr_instance_config_new (
		const char *name_begin,
		const int name_length,
		const int max_settings
);

int rrr_instance_config_read_port_number (
		rrr_setting_uint *target,
		struct rrr_instance_config *source,
		const char *name
);

int rrr_instance_config_check_all_settings_used (
		struct rrr_instance_config *config
);

int rrr_instance_config_parse_array_definition_from_config_silent_fail (
		struct rrr_array *target,
		struct rrr_instance_config *config,
		const char *cmd_key
);

int rrr_instance_config_parse_comma_separated_associative_to_map (
		struct rrr_map *target,
		struct rrr_instance_config *config,
		const char *cmd_key,
		const char *delimeter
);

int rrr_instance_config_parse_comma_separated_to_map (
		struct rrr_map *target,
		struct rrr_instance_config *config,
		const char *cmd_key
);

#endif /* RRR_INSTANCE_CONFIG_H */
