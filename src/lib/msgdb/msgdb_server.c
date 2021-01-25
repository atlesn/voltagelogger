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

#include <stdlib.h>
#include <string.h>

#include "../log.h"
#include "msgdb_common.h"
#include "msgdb_server.h"
#include "../read.h"
#include "../messages/msg_msg.h"
#include "../socket/rrr_socket.h"
#include "../socket/rrr_socket_client.h"

struct rrr_msgdb_server {
	char *directory;
	int fd;
	struct rrr_socket_client_collection clients;
};

int rrr_msgdb_server_new (
	struct rrr_msgdb_server **result,
	const char *directory,
	const char *socket
) {
	int ret = 0;

	struct rrr_msgdb_server *server = NULL;
	int fd = 0;

	if ((ret = rrr_socket_unix_create_bind_and_listen (
		&fd,
		"msgdb_server",
		socket,
		10, // Number of clients
		1,  // Do nonblock
		0,  // No mkstemp
		1   // Do unlink if exists
	)) != 0) {
		RRR_MSG_0("Failed to create listening socket '%s' in message database server\n", socket);
		goto out;
	}

	if ((server = malloc(sizeof(*server))) == NULL) {
		RRR_MSG_0("Could not allocate memory for server in rrr_msgdb_server_new\n");
		ret = 1;
		goto out_close;
	}

	memset(server, '\0', sizeof(*server));

	if ((server->directory = strdup(directory)) == NULL) {
		RRR_MSG_0("Could not allocate memory for directory in rrr_msgdb_server_new\n");
		ret = 1;
		goto out_free;
	}

	if ((ret = rrr_socket_client_collection_init(&server->clients, fd, "msgdb_server")) != 0) {
		goto out_free_directory;
	}

	server->fd = fd;

	*result = server;

	goto out;
	out_free_directory:
		free(server->directory);
	out_free:
		free(server);
	out_close:
		rrr_socket_close(fd);
	out:
		return ret;
}

void rrr_msgdb_server_destroy (
	struct rrr_msgdb_server *server
) {
	RRR_FREE_IF_NOT_NULL(server->directory);
	rrr_socket_close(server->fd);
	rrr_socket_client_collection_clear(&server->clients);
	free(server);
}

struct rrr_msgdb_server_client {
	int prev_ctrl_msg_type;
};

static int __rrr_msgdb_server_client_new (
	struct rrr_msgdb_server_client **target,
	void *arg
) {
	(void)(arg);

	*target = NULL;

	struct rrr_msgdb_server_client *client = malloc(sizeof(*client));
	if (client == NULL) {
		RRR_MSG_0("Could not allocate memory in __rrr_msgdb_server_client_new\n");
		return 1;
	}

	memset (client, '\0', sizeof(*client));

	*target = client;

	return 0;
}

static int __rrr_msgdb_server_client_new_void (
	void **target,
	void *arg
) {
	return __rrr_msgdb_server_client_new((struct rrr_msgdb_server_client **) target, arg);
}

static void __rrr_msgdb_server_client_destroy (
	struct rrr_msgdb_server_client *client
) {
	free(client);
}

static void __rrr_msgdb_server_client_destroy_void (
	void *arg
) {
	return __rrr_msgdb_server_client_destroy(arg);
}

static int __rrr_msgdb_server_read_msg_msg_callback (
		struct rrr_msg_msg **msg,
		void *arg,
		void *private_data
) {
	return 0;
}

static int __rrr_msgdb_server_read_msg_ctrl_callback (
		const struct rrr_msg *msg,
		void *arg,
		void *private_data
) {
	struct rrr_msgdb_server_client *client = private_data;

	if (RRR_MSG_CTRL_F_HAS(msg, RRR_MSGDB_CTRL_F_PUT)) {
		RRR_DBG_3("Received control message PUT\n");
	}
	else {
		RRR_MSG_0("Received unknown control message %u\n", RRR_MSG_CTRL_FLAGS(msg));
		return RRR_MSGDB_SOFT_ERROR;
	}

	client->prev_ctrl_msg_type = RRR_MSG_CTRL_FLAGS(msg);

	return 0;
}

int rrr_msgdb_server_tick (
	struct rrr_msgdb_server *server
) {
	int ret = 0;

	if ((ret = rrr_socket_client_collection_accept (
		&server->clients,
		__rrr_msgdb_server_client_new_void,
		NULL,
		__rrr_msgdb_server_client_destroy_void
	)) != 0) {
		goto out;
	}

	if ((ret = rrr_socket_client_collection_read_message (
			&server->clients,
			4096,
			RRR_SOCKET_READ_METHOD_RECVFROM | RRR_SOCKET_READ_CHECK_POLLHUP,
			__rrr_msgdb_server_read_msg_msg_callback,
			NULL,
			NULL,
			__rrr_msgdb_server_read_msg_ctrl_callback,
			server
	)) != 0) {
		goto out;
	}

	out:
	return ret;
}