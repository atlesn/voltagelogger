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

#include <fcntl.h>
#include <stdlib.h>

#include "lib/log.h"
#include "lib/common.h"
#include "lib/version.h"
#include "lib/cmdlineparser/cmdline.h"
#include "lib/rrr_strerror.h"
#include "lib/rrr_umask.h"
#include "lib/util/posix.h"
#include "lib/msgdb/msgdb_server.h"
#include "paths.h"
#include "main.h"

RRR_CONFIG_DEFINE_DEFAULT_LOG_PREFIX("rrr_msgdb");

#define RRR_GLOBAL_UMASK		S_IROTH | S_IWOTH | S_IXOTH

#define RRR_MSGDB_DEFAULT_SOCKET        RRR_RUN_DIR "/msgdb.socket"

#ifndef RRR_BUILD_TIMESTAMP
#define RRR_BUILD_TIMESTAMP 1
#endif

static const struct cmd_arg_rule cmd_rules[] = {
		{CMD_ARG_FLAG_NO_FLAG,         '\0',   "directory",             "{DIRECTORY}"},
		{CMD_ARG_FLAG_HAS_ARGUMENT,    's',    "socket",                "[-s|--socket]"},
		{CMD_ARG_FLAG_HAS_ARGUMENT,    'e',    "environment-file",      "[-e|--environment-file[=]ENVIRONMENT FILE]"},
		{CMD_ARG_FLAG_HAS_ARGUMENT,    'd',    "debuglevel",            "[-d|--debuglevel[=]DEBUG FLAGS]"},
		{CMD_ARG_FLAG_HAS_ARGUMENT,    'D',    "debuglevel-on-exit",    "[-D|--debuglevel-on-exit[=]DEBUG FLAGS]"},
		{0,                            'h',    "help",                  "[-h|--help]"},
		{0,                            'v',    "version",               "[-v|--version]"},
		{0,                            '\0',    NULL,                   NULL}
};

static int main_running = 1;
int rrr_signal_handler(int s, void *arg) {
	return rrr_signal_default_handler(&main_running, s, arg);
}

int main (int argc, const char *argv[], const char *env[]) {
	if (!rrr_verify_library_build_timestamp(RRR_BUILD_TIMESTAMP)) {
		fprintf(stderr, "Library build version mismatch.\n");
		exit(EXIT_FAILURE);
	}

	int ret = EXIT_SUCCESS;

	if (rrr_log_init() != 0) {
		goto out;
	}
	rrr_strerror_init();

	struct rrr_msgdb_server *server = NULL;
	struct rrr_signal_handler *signal_handler = NULL;
	struct cmd_data cmd;

	cmd_init(&cmd, cmd_rules, argc, argv);

	// The fork signal handler must be first
	signal_handler = rrr_signal_handler_push(rrr_signal_handler, NULL);

	rrr_signal_default_signal_actions_register();

	// Everything which might print debug stuff must be called after this
	// as the global debuglevel is 0 up to now
	if ((ret = rrr_main_parse_cmd_arguments_and_env(&cmd, env, CMD_CONFIG_DEFAULTS)) != 0) {
		goto out_cleanup_signal;
	}

	if (rrr_main_print_banner_help_and_version(&cmd, 2) != 0) {
		goto out_cleanup_signal;
	}

	const char *directory = cmd_get_value(&cmd, "directory", 0);
	const char *socket = cmd_get_value(&cmd, "socket", 0);

	if (socket == NULL) {
		socket = RRR_MSGDB_DEFAULT_SOCKET;
	}

	rrr_umask_onetime_set_global(RRR_GLOBAL_UMASK);

	RRR_DBG_1("RRR debuglevel is: %u\n", RRR_DEBUGLEVEL);
	RRR_DBG_1("Using directory '%s' and socket '%s'\n", directory, socket);

	rrr_signal_handler_set_active(RRR_SIGNALS_ACTIVE);

	if ((ret = rrr_msgdb_server_new(&server, directory, socket)) != 0) {
		goto out_cleanup_signal;
	}

	while (main_running) {
		if ((ret = rrr_msgdb_server_tick(server)) != 0) {
			break;
		}
		rrr_posix_usleep(1000);
	}

	rrr_config_set_debuglevel_on_exit();

	out_cleanup_signal:
		if (server != NULL) {
			rrr_msgdb_server_destroy(server);
		}
		rrr_signal_handler_set_active(RRR_SIGNALS_NOT_ACTIVE);
		rrr_signal_handler_remove(signal_handler);
		rrr_exit_cleanup_methods_run_and_free();
		if (ret == 0) {
			RRR_MSG_1("Exiting program without errors\n");
		}
		else {
			RRR_MSG_ERR("Exiting program following one or more errors\n");
		}
		cmd_destroy(&cmd);
		rrr_strerror_cleanup();
		rrr_log_cleanup();
	out:
		return ret;
}