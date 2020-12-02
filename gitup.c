/*-
 * Copyright (c) 2012-2020, John Mehr <jmehr@umn.edu>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 *
 */

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/tree.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/ssl3.h>
#include <openssl/err.h>

#include <ctype.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#define GITUP_VERSION     "0.5"
#define GIT_VERSION       "2.28"
#define BUFFER_UNIT_SMALL  4096
#define BUFFER_UNIT_LARGE  1048576

struct object_node {
	RB_ENTRY(object_node) link;
	char *sha;
	int   type;
	int   index;
	int   index_delta;
	char *ref_delta_sha;
	int   pack_offset;
	char *buffer;
	int   buffer_size;
	int   data_size;
};

struct file_node {
	RB_ENTRY(file_node) link;
	mode_t  mode;
	char   *path;
	char   *sha;
};

typedef struct {
	SSL                 *ssl;
	SSL_CTX             *ctx;
	int                  socket_descriptor;
	char                *host;
	uint16_t             port;
	char                *agent;
	char                *section;
	char                *repository;
	char                *branch;
	char                *have;
	char                *want;
	char                *response;
	int                  response_blocks;
	uint32_t             response_size;
	int                  clone;
	struct object_node **object;
	int                  objects;
	char                *pack_file;
	char                *path_target;
	char                *path_work;
	char                *remote_file_old;
	char                *remote_file_new;
	int                  keep_pack_file;
	int                  use_pack_file;
	int                  verbosity;
} connector;


/*
 * node_compare
 *
 * Functions that instruct the red-black trees how to sort keys.
 */

static int
file_node_compare(const struct file_node *a, const struct file_node *b)
{
	return (strcmp(a->path, b->path));
}


static int
object_node_compare(const struct object_node *a, const struct object_node *b)
{
	return (strcmp(a->sha, b->sha));
}


/*
 * node_free
 *
 * Functions that free the memory used by tree nodes.
 */

static void
file_node_free(struct file_node *node)
{
	if (node->sha != NULL)
		free(node->sha);

	if (node->path != NULL)
		free(node->path);

	free(node);
}


static void
object_node_free(struct object_node *node)
{
	if (node->sha != NULL)
		free(node->sha);

	if (node->ref_delta_sha != NULL)
		free(node->ref_delta_sha);

	if (node->buffer != NULL)
		free(node->buffer);

	free(node);
}


static RB_HEAD(Tree_Remote_Files, file_node) Remote_Files = RB_INITIALIZER(&Remote_Files);
RB_PROTOTYPE(Tree_Remote_Files, file_node, link, file_node_compare)
RB_GENERATE(Tree_Remote_Files, file_node, link, file_node_compare)

static RB_HEAD(Tree_Local_Files, file_node) Local_Files = RB_INITIALIZER(&Local_Files);
RB_PROTOTYPE(Tree_Local_Files, file_node, link, file_node_compare)
RB_GENERATE(Tree_Local_Files, file_node, link, file_node_compare)

static RB_HEAD(Tree_Local_Directories, file_node) Local_Directories = RB_INITIALIZER(&Local_Directories);
RB_PROTOTYPE(Tree_Local_Directories, file_node, link, file_node_compare)
RB_GENERATE(Tree_Local_Directories, file_node, link, file_node_compare)

static RB_HEAD(Tree_Temp_Files, file_node) Temp_Files = RB_INITIALIZER(&Temp_Files);
RB_PROTOTYPE(Tree_Temp_Files, file_node, link, file_node_compare)
RB_GENERATE(Tree_Temp_Files, file_node, link, file_node_compare)

static RB_HEAD(Tree_Objects, object_node) Objects = RB_INITIALIZER(&Objects);
RB_PROTOTYPE(Tree_Objects, object_node, link, object_node_compare)
RB_GENERATE(Tree_Objects, object_node, link, object_node_compare)


/*
 * legible_sha
 *
 * Function that converts a 20 byte binary SHA checksum into a 40 byte human-readable SHA checksum.
 */

static char *
legible_sha(char *sha_buffer)
{
	char *sha = NULL;

	if ((sha = (char *)malloc(41)) == NULL)
		err(EXIT_FAILURE, "legible_sha: malloc");

	for (int x = 0; x < 20; x++)
		snprintf(&sha[x * 2], 3, "%02x", (unsigned char)sha_buffer[x]);

	sha[40] = '\0';

	return sha;
}


/*
 * illegible_sha
 *
 * Function that converts a 40 byte human-readable SHA checksum into a 20 byte binary SHA checksum.
 */

static char *
illegible_sha(char *sha_buffer)
{
	char *sha = NULL;

	if ((sha = (char *)malloc(20)) == NULL)
		err(EXIT_FAILURE, "illegible_sha: malloc");

	for (int x = 0; x < 20; x++)
		sha[x] = 16 * ((unsigned char)sha_buffer[x * 2] - (sha_buffer[x * 2] > 58 ? 87 : 48)) + (unsigned char)sha_buffer[x * 2 + 1] - (sha_buffer[x * 2 + 1] > 58 ? 87 : 48);

	return sha;
}


/*
 * load_file
 *
 * Procedure that loads a local file into the specified buffer.
 */

static void
load_file(char *path, char **buffer, uint32_t *buffer_size)
{
	struct stat file;
	int         fd;

	if (stat(path, &file) == -1)
		err(EXIT_FAILURE, "load_file: Cannot find %s", path);

	if (file.st_size > 0) {
		if (file.st_size > *buffer_size) {
			*buffer_size = file.st_size;

			if ((*buffer = (char *)realloc(*buffer, *buffer_size + 1)) == NULL)
				err(EXIT_FAILURE, "load_file: malloc");
		}

		if ((fd = open(path, O_RDONLY)) == -1)
			err(EXIT_FAILURE, "load_file: Cannot read %s", path);

		if (read(fd, *buffer, *buffer_size) != *buffer_size)
			err(EXIT_FAILURE, "load_file: Problem reading %s", path);

		close(fd);

		*(*buffer + *buffer_size) = '\0';
	}
}


/*
 * calculate_object_sha
 *
 * Function that adds git's "type file-size\0" header to a buffer and returns the SHA checksum.
 */

static char *
calculate_object_sha(char *buffer, uint32_t buffer_size, int type)
{
	int   digits = buffer_size, header_width = 0;
	char *sha = NULL, *sha_buffer = NULL, *temp_buffer = NULL;
	char *types[8] = { "", "commit", "tree", "blob", "tag", "", "ofs-delta", "ref-delta" };

	if ((sha_buffer = (char *)malloc(21)) == NULL)
		err(EXIT_FAILURE, "calculate_object_sha: malloc");

	if ((temp_buffer = (char *)malloc(buffer_size + 24)) == NULL)
		err(EXIT_FAILURE, "calculate_object_sha: malloc");

	/* Start with the git "type file-size\0" header. */

	header_width = strlen(types[type]) + 3;

	while ((digits /= 10) > 0)
		header_width++;

	snprintf(temp_buffer, header_width, "%s %u", types[type], buffer_size);

	/* Then add the buffer. */

	memcpy(temp_buffer + header_width, buffer, buffer_size);

	/* Calculate the SHA checksum. */

	SHA1((unsigned char *)temp_buffer, buffer_size + header_width, (unsigned char *)sha_buffer);

	sha = legible_sha(sha_buffer);

	free(sha_buffer);
	free(temp_buffer);

	return sha;
}


/*
 * calculate_file_sha
 *
 * Function that loads a local file and returns the SHA checksum.
 */

static char *
calculate_file_sha(char *path, ssize_t file_size, int file_mode)
{
	char     *buffer = NULL, *sha = NULL;
	uint32_t  buffer_size = 0;

	if (S_ISLNK(file_mode)) {
	} else {
		load_file(path, &buffer, &buffer_size);
		sha = calculate_object_sha(buffer, file_size, 3);
		free(buffer);
	}

	return sha;
}


/*
 * append_string
 *
 * Procedure that appends one string to another.
 */

static void
append_string(char **buffer, unsigned int *buffer_size, unsigned int *string_length, char *addendum, int addendum_size)
{
	int adjust = 0;

	while (*string_length + addendum_size > *buffer_size) {
		adjust = 1;
		*buffer_size += BUFFER_UNIT_SMALL;
	}

	if ((adjust) && ((*buffer = (char *)realloc(*buffer, *buffer_size + 1)) == NULL))
		err(EXIT_FAILURE, "append_string: realloc");

	memcpy(*buffer + *string_length, addendum, addendum_size);

	*string_length += addendum_size;

	*(*buffer + *string_length) = '\0';
}


/*
 * find_local_tree
 *
 * Procedure that recursively finds and adds local files and directories to
 * separate red-black trees.
 */

static char *
find_local_tree(connector *connection, char *base_path)
{
	DIR              *directory = NULL;
	struct stat       file;
	struct dirent    *entry = NULL;
	struct file_node *new_node, files[BUFFER_UNIT_SMALL], directories[BUFFER_UNIT_SMALL], *found = NULL;
	char             *full_path = NULL, *buffer = NULL, line[MAXNAMLEN + 8], *sha = NULL, *null = "\0";
	int               file_name_size = 0, file_count = 0, directory_count = 0;
	int               full_path_size = strlen(base_path) + MAXNAMLEN + 3;
	uint32_t          buffer_size = 0, length = 0;

	if ((full_path = (char *)malloc(full_path_size)) == NULL)
		err(EXIT_FAILURE, "find_local_tree: full_path malloc");

	/* Process the directory's contents. */

	if ((lstat(base_path, &file) != -1) && ((directory = opendir(base_path)) != NULL)) {
		while ((entry = readdir(directory)) != NULL) {
			snprintf(full_path,
				full_path_size,
				"%s/%s",
				base_path,
				entry->d_name);

			if (lstat(full_path, &file) == -1)
				err(EXIT_FAILURE, "find_local_tree: %s", full_path);

			if ((new_node = (struct file_node *)malloc(sizeof(struct file_node))) == NULL)
				err(EXIT_FAILURE, "find_local_tree: malloc");

			new_node->mode = file.st_mode;
			new_node->sha  = NULL;
			new_node->path = strdup(full_path);

			file_name_size = strlen(entry->d_name);

			if ((file_name_size == 1) && (strcmp(entry->d_name, "." ) == 0))
				continue;

			if ((file_name_size == 2) && (strcmp(entry->d_name, "..") == 0))
				continue;

			if ((file_name_size == 4) && (strcmp(entry->d_name, ".git") == 0)) {
				fprintf(stderr, " ! A .git folder was found -- gitup does not update this folder which will cause problems for the official git client.\n");
				fprintf(stderr, " ! If you wish to use gitup, please remove %s and rerun gitup.\n", full_path);

				exit(EXIT_FAILURE);
				}

			if (S_ISDIR(file.st_mode)) {
				new_node->mode = 040000;
				new_node->sha  = find_local_tree(connection, full_path);
				new_node->path = strdup(full_path);

				RB_INSERT(Tree_Local_Directories, &Local_Directories, new_node);
				directories[directory_count++] = *new_node;
			} else {
				new_node->sha = calculate_file_sha(full_path, file.st_size, file.st_mode);

				RB_INSERT(Tree_Local_Files, &Local_Files, new_node);
				files[file_count++] = *new_node;
			}
		}

		closedir(directory);
	}

	/* Reconstruct the tree object originally found in the pack file. */

	for (int x = 0; x < file_count; x++)
		RB_INSERT(Tree_Temp_Files, &Temp_Files, &files[x]);

	for (int x = 0; x < directory_count; x++)
		RB_INSERT(Tree_Temp_Files, &Temp_Files, &directories[x]);

	RB_FOREACH(found, Tree_Temp_Files, &Temp_Files) {
		snprintf(line, sizeof(line), "%o %s", found->mode, found->path + strlen(base_path) + 1);
		sha = illegible_sha(found->sha);

		append_string(&buffer, &buffer_size, &length, line, strlen(line));
		append_string(&buffer, &buffer_size, &length, null, 1);
		append_string(&buffer, &buffer_size, &length, sha, 20);

		free(sha);
		RB_REMOVE(Tree_Temp_Files, &Temp_Files, found);
	}

//	write(1, buffer, length); write(1, "\n\n\n", 3);
//	printf("%s %s\n", calculate_object_sha(buffer, length, 2), base_path);

	free(full_path);

	return calculate_object_sha(buffer, length, 2);
}


/*
 * load_object
 *
 * Procedure that loads a local file and adds it to the array/tree of pack file objects
 */

static void
load_object(connector *connection, char *sha)
{
	struct file_node *find = NULL;
	char             *buffer = NULL;
	uint32_t          buffer_size = 0, found = 0;

	/* Make sure the SHA passed in doesn't refer to a directory. */

	RB_FOREACH(find, Tree_Local_Directories, &Local_Directories)
		if (strncmp(find->sha, sha, 40) == 0) {
			found = 1;
			break;
			}

	/* Find the file the SHA references, load it and store it. */

	if (!found)
		RB_FOREACH(find, Tree_Local_Files, &Local_Files)
			if (strncmp(find->sha, sha, 40) == 0) {
				load_file(find->path, &buffer, &buffer_size);
				store_object(connection, 3, buffer, buffer_size, 0, 0, NULL);

				found = 1;
				break;
				}

	if (!found)
		errc(EXIT_FAILURE, ENOENT, "load_object: local file for object %s not found", sha);
}


/*
 * check_local_tree
 *
 * Procedure that compares the local repository tree with the data saved from the
 * last run to see if anything has been modified.
 */

static void
check_local_tree(void)
{
	struct file_node *find = NULL, *found = NULL;
	int               errors = 0;

	RB_FOREACH(find, Tree_Remote_Files, &Remote_Files) {
		found = RB_FIND(Tree_Local_Files, &Local_Files, find);

		if (found == NULL) {
			printf(" ! Local file %s is missing.\n", find->path);
			errors++;
			continue;
		}

		if (strncmp(found->sha, find->sha, 40) != 0) {
			printf(" ! Local file %s has been modified.\n", find->path);
			errors++;
		}
	}

	if (errors) {
		exit(EXIT_FAILURE);
	}
}


/*
 * ssl_connect
 *
 * Procedure that (re)establishes a connection with the server.
 */

static void
ssl_connect(connector *connection)
{
	struct addrinfo hints, *start, *temp;
	struct timeval  timeout;
	int             error, option;
	char            type[10];

	if (connection->socket_descriptor)
		if (close(connection->socket_descriptor) != 0)
			if (errno != EBADF)
				err(EXIT_FAILURE, "ssl_connect: close_connection");

	snprintf(type, sizeof(type), "%d", connection->port);

	bzero(&hints, sizeof(struct addrinfo));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	if ((error = getaddrinfo(connection->host, type, &hints, &start)))
		errx(EXIT_FAILURE, "%s", gai_strerror(error));

	connection->socket_descriptor = -1;

	while (start) {
		temp = start;

		if (connection->socket_descriptor < 0) {
			if ((connection->socket_descriptor = socket(temp->ai_family, temp->ai_socktype, temp->ai_protocol)) < 0)
				err(EXIT_FAILURE, "ssl_connect: socket failure");

			if (connect(connection->socket_descriptor, temp->ai_addr, temp->ai_addrlen) < 0)
				err(EXIT_FAILURE, "ssl_connect: connect failure - %d", errno);
		}

		start = temp->ai_next;
		freeaddrinfo(temp);
	}

	if (SSL_library_init() == 0)
		err(EXIT_FAILURE, "ssl_connect: SSL_library_init");

	SSL_load_error_strings();
	connection->ctx = SSL_CTX_new(SSLv23_client_method());
	SSL_CTX_set_mode(connection->ctx, SSL_MODE_AUTO_RETRY);
	SSL_CTX_set_options(connection->ctx, SSL_OP_ALL | SSL_OP_NO_TICKET);

	if ((connection->ssl = SSL_new(connection->ctx)) == NULL)
		err(EXIT_FAILURE, "ssl_connect: SSL_new");

	SSL_set_fd(connection->ssl, connection->socket_descriptor);

	while ((error = SSL_connect(connection->ssl)) == -1)
		fprintf(stderr, "ssl_connect: SSL_connect error:%d\n", SSL_get_error(connection->ssl, error));

	option = 1;

	if (setsockopt(connection->socket_descriptor, SOL_SOCKET, SO_KEEPALIVE, &option, sizeof(int)))
		err(EXIT_FAILURE, "ssl_connect: setsockopt SO_KEEPALIVE error");

	option = BUFFER_UNIT_LARGE;

	if (setsockopt(connection->socket_descriptor, SOL_SOCKET, SO_SNDBUF, &option, sizeof(int)))
		err(EXIT_FAILURE, "ssl_connect: setsockopt SO_SNDBUF error");

	if (setsockopt(connection->socket_descriptor, SOL_SOCKET, SO_RCVBUF, &option, sizeof(int)))
		err(EXIT_FAILURE, "ssl_connect: setsockopt SO_RCVBUF error");

	bzero(&timeout, sizeof(struct timeval));
	timeout.tv_sec = 300;

	if (setsockopt(connection->socket_descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(struct timeval)))
		err(EXIT_FAILURE, "ssl_connect: setsockopt SO_RCVTIMEO error");

	if (setsockopt(connection->socket_descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(struct timeval)))
		err(EXIT_FAILURE, "ssl_connect: setsockopt SO_SNDTIMEO error");
}


/*
 * process_command
 *
 * Procedure that sends a command to the server and processes the response.
 */

static void
process_command(connector *connection, char *command)
{
	char  read_buffer[BUFFER_UNIT_SMALL];
	char *marker_start = NULL, *marker_end = NULL, *data_start = NULL;
	int   chunk_size = -1, bytes_expected = 0, marker_offset = 0, data_start_offset = 0;
	int   bytes_read = 0, total_bytes_read = 0, bytes_to_move = 0;
	int   bytes_sent = 0, total_bytes_sent = 0, check_bytes = 0;
	int   bytes_to_write = strlen(command), error = 0, twirl = 0;
	char  twirly[4] = { '|', '/', '-', '\\' };

	/* Transmit the command to the server. */

	ssl_connect(connection);

	while (total_bytes_sent < bytes_to_write) {
		bytes_sent = SSL_write(
			connection->ssl,
			command + total_bytes_sent,
			bytes_to_write - total_bytes_sent);

		if (bytes_sent <= 0) {
			if ((bytes_sent < 0) && ((errno == EINTR) || (errno == 0)))
				continue;
			else
				err(EXIT_FAILURE, "process_command: send command");
		}

		total_bytes_sent += bytes_sent;

		if (connection->verbosity > 1)
			fprintf(stderr, "\r==> bytes sent: %d", total_bytes_sent);
		}

	if (connection->verbosity > 1)
		fprintf(stderr, "\n");

	/* Process the response. */

	while (chunk_size) {
		bytes_read = SSL_read(connection->ssl, read_buffer, BUFFER_UNIT_SMALL);

		if (bytes_read == 0)
			break;

		if (bytes_read < 0)
			err(EXIT_FAILURE, "process_command: SSL_read error: %d\n", SSL_get_error(connection->ssl, error));

		/* Expand the buffer if needed, preserving the position and data_start if the buffer moves. */

		if (total_bytes_read + bytes_read > connection->response_blocks * BUFFER_UNIT_LARGE) {
			marker_offset     = marker_start - connection->response;
			data_start_offset = data_start   - connection->response;

			if ((connection->response = (char *)realloc(connection->response, ++connection->response_blocks * BUFFER_UNIT_LARGE)) == NULL)
				err(EXIT_FAILURE, "process_command: connection->response realloc");

			marker_start = connection->response + marker_offset;
			data_start   = connection->response + data_start_offset;
		}

		/* Add the bytes received to the buffer. */

		memcpy(connection->response + total_bytes_read, read_buffer, bytes_read + 1);
		total_bytes_read += bytes_read;
		connection->response[total_bytes_read] = '\0';

		if (connection->verbosity > 1)
			fprintf(stderr, "\r==> bytes read:%d\tbytes_expected:%d\ttotal_bytes_read:%d", bytes_read, bytes_expected, total_bytes_read);

		/* Find the boundary between the header and the data. */

		if (chunk_size == -1) {
			if ((marker_start = strnstr(connection->response, "\r\n\r\n", total_bytes_read)) == NULL) {
				continue;
			} else {
				bytes_expected = marker_start - connection->response + 4;
				marker_start += 2;
				data_start = marker_start;
			}
		}

		while (total_bytes_read + chunk_size > bytes_expected) {
			/* Make sure the whole chunk marker has been read. */

			check_bytes = total_bytes_read - (marker_start + 2 - connection->response);

			if (check_bytes < 0)
				break;

			marker_end = strnstr(marker_start + 2, "\r\n", check_bytes);

			if (marker_end == NULL)
				break;

			/* Remove the chunk length marker. */

			chunk_size    = strtol(marker_start, (char **)NULL, 16);
			bytes_to_move = total_bytes_read - (marker_end + 2 - connection->response) + 1;

			if (bytes_to_move < 0)
				break;

			memmove(marker_start, marker_end + 2, bytes_to_move);
			total_bytes_read -= (marker_end + 2 - marker_start);

			if (chunk_size == 0)
				break;

			marker_start += chunk_size;
			bytes_expected += chunk_size;

			if (connection->verbosity == 1)
				fprintf(stderr, "%c\r", twirly[twirl++ % 4]);
		}
	}

	if (connection->verbosity > 1)
		fprintf(stderr, "\n");

	/* Remove the header. */

	connection->response_size = total_bytes_read - (data_start - connection->response);
	memmove(connection->response, data_start, connection->response_size);
	connection->response[connection->response_size] = '\0';
}


/*
 * send_command
 *
 * Function that constructs the command to the fetch the full pack data.
 */

static void
send_command(connector *connection, char *want)
{
	char *command   = NULL;
	int   want_size = strlen(want);

	if ((command = (char *)malloc(BUFFER_UNIT_SMALL + want_size)) == NULL)
		err(EXIT_FAILURE, "send_command: malloc");

	snprintf(command,
		BUFFER_UNIT_LARGE,
		"POST %s/git-upload-pack HTTP/1.1\n"
		"Host: github.com\n"
		"User-Agent: git/%s\n"
		"Accept-encoding: deflate, gzip\n"
		"Content-type: application/x-git-upload-pack-request\n"
		"Accept: application/x-git-upload-pack-result\n"
		"Git-Protocol: version=2\n"
		"Content-length: %d\n"
		"\r\n"
		"%s",
		connection->repository,
		GIT_VERSION,
		want_size,
		want
		);

	if (connection->verbosity > 1)
		fprintf(stderr, "%s\n\n", command);

	process_command(connection, command);

	free(command);
}


/*
 * initiate_clone
 *
 * Function that constructs and executes the command to the fetch the full pack data.
 */

static void
initiate_clone(connector *connection)
{
	char want[BUFFER_UNIT_SMALL];

	snprintf(want,
		BUFFER_UNIT_SMALL,
		"0011command=fetch"
		"%04lx%s0001"
		"000fno-progress"
		"000dofs-delta"
		"0034shallow %s"
		"0032want %s\n"
		"0032want %s\n"
		"0009done\n0000",
		strlen(connection->agent) + 4,
		connection->agent,
		connection->want,
		connection->want,
		connection->want);

	send_command(connection, want);
}


/*
 * initiate_pull
 *
 * Function that constructs and executes the command to the fetch the incremental pack data.
 */

static void
initiate_pull(connector *connection)
{
	struct file_node *find = NULL, *found = NULL;
	unsigned int      want_size = 0, want_buffer_size = BUFFER_UNIT_LARGE;
	char             *want = NULL, have[51], *done = "0009done\n0000";

	if ((want = (char *)malloc(want_buffer_size)) == NULL)
		err(EXIT_FAILURE, "initiate_pull: malloc");

	/* Start with the basic pull command. */

	snprintf(want,
		BUFFER_UNIT_SMALL,
		"0011command=fetch"
		"%04lx%s0001"
		"000dthin-pack"
		"000fno-progress"
		"000dofs-delta"
		"0034shallow %s"
		"0034shallow %s"
		"000cdeepen 1"
		"0032want %s\n"
		"0032have %s\n",
		strlen(connection->agent) + 4,
		connection->agent,
		connection->want,
		connection->have,
		connection->want,
		connection->have);

	want_size = strlen(want);

	/* Loop through the local files, adding any missing or modified files to the wants. */
/*
	RB_FOREACH(find, Tree_Remote_Files, &Remote_Files) {
		found = RB_FIND(Tree_Local_Files, &Local_Files, find);

		if ((found == NULL) || (strncmp(found->sha, find->sha, 40) != 0)) {
			snprintf(have, sizeof(have), "0032want %s\n", find->sha);
			append_string(&want, &want_buffer_size, &want_size, have, strlen(have));
		}
	}
*/
	/* Finish the request. */

	append_string(&want, &want_buffer_size, &want_size, done, strlen(done));

	send_command(connection, want);
}


/*
 * get_commit_details
 */

static void
get_commit_details(connector *connection)
{
	char command[BUFFER_UNIT_SMALL], full_branch[BUFFER_UNIT_SMALL], *position = NULL, *end = NULL;

	/* Get the list of commits from the server. */

	snprintf(command,
		BUFFER_UNIT_SMALL,
		"GET %s/info/refs?service=git-upload-pack HTTP/1.1\n"
		"Host: github.com\n"
		"User-Agent: git/%s\n"
		"\r\n",
		connection->repository,
		GIT_VERSION);

	process_command(connection, command);

	/* Change all \0 characters to \n to make it easy to find the data. */

	for (uint32_t x = 0; x < connection->response_size; x++)
		if (connection->response[x] == '\0')
			connection->response[x] = '\n';

	/* Extract the agent. */

	position = strstr(connection->response, "agent=");
	end      = strstr(position, "\n");

	*end = '\0';
	connection->agent = strdup(position);
	*end = '\n';

	/* Extract the "want" checksum. */

	if (connection->want == NULL) {
		snprintf(full_branch, BUFFER_UNIT_SMALL, " refs/heads/%s\n", connection->branch);

		position = strstr(connection->response, full_branch);

		if (position == NULL)
			errc(EXIT_FAILURE, EINVAL, "get_commit_details: %s doesn't exist in %s", connection->branch, connection->repository);

		if ((connection->want = (char *)malloc(41)) == NULL)
			err(EXIT_FAILURE, "get_commit_details: malloc");

		memcpy(connection->want, position - 40, 40);
		connection->want[40] = '\0';

		if (connection->verbosity)
			printf("# Want: %s\n", connection->want);
	}

	/* Create the pack file name. */

	if (connection->keep_pack_file) {
		int pack_file_name_size = strlen(connection->section) + 47;

		if ((connection->pack_file = (char *)malloc(pack_file_name_size)) == NULL)
			err(EXIT_FAILURE, "get_commit_details: malloc");

		snprintf(connection->pack_file, pack_file_name_size, "%s-%s.pack", connection->section, connection->want);

		if (connection->verbosity)
			fprintf(stderr, "# Saving pack file: %s\n", connection->pack_file);
	}
}


/*
 * fetch_pack
 *
 * Procedure that loads a local copy of the pack data or fetches it from the server.
 */

static void
fetch_pack(connector *connection)
{
	char        *pack_start = NULL, sha_buffer[20], path[BUFFER_UNIT_SMALL];
	struct stat  pack_file, remote_file;
	int          fd, chunk_size = 1, pack_size = 0, source = 0, target = 0;

	connection->response_size = 0;

	/* If a pack file has been specified, attempt to load it. */

	if ((connection->use_pack_file) && (lstat(connection->pack_file, &pack_file) != -1)) {
		load_file(connection->pack_file, &connection->response, &(connection->response_size));
		pack_size = connection->response_size - 20;
		}

	/* If we're pulling and the remote file exists, check the local tree for problems. */

	if ((stat(connection->remote_file_old, &remote_file) == 0) && (connection->clone == 0))
		check_local_tree();

	/* If no pack data has been loaded, fetch it from the server. */

	if (connection->response_size == 0) {
		if ((stat(connection->remote_file_old, &remote_file) != 0) || (connection->clone))
			initiate_clone(connection);
		else
			initiate_pull(connection);

		/* Find the start of the pack data and remove the header. */

		if ((pack_start = strstr(connection->response, "PACK")) == NULL)
			errc(EXIT_FAILURE, EFTYPE, "unpack_objects: %s\n", connection->response);

		pack_start -= 5;
		connection->response_size -= (pack_start - connection->response + 11);
		memmove(connection->response, connection->response + 8, 4);

		/* Remove the chunk size markers from the pack data. */

		source = pack_start - connection->response;

		while (chunk_size > 0) {
			chunk_size = strtol(connection->response + source, (char **)NULL, 16);

			if (chunk_size == 0)
				break;

			memmove(connection->response + target, connection->response + source + 5, chunk_size - 5);
			target += chunk_size - 5;
			source += chunk_size;
			connection->response_size -= 5;
		}

		connection->response_size += 5;
		pack_size = connection->response_size - 20;
	}

	/* Verify the pack data checksum. */

	SHA1((unsigned char *)connection->response, pack_size, (unsigned char *)sha_buffer);

	if (memcmp(connection->response + pack_size, sha_buffer, 20) != 0)
		errc(EXIT_FAILURE, EAUTH, "unpack_objects: pack checksum mismatch - expected %s, received %s", legible_sha(connection->response + pack_size), legible_sha(sha_buffer));

	/* Save the pack data. */

	if (connection->keep_pack_file) {
		if ((fd = open(connection->pack_file, O_WRONLY | O_CREAT | O_TRUNC)) == -1)
			err(EXIT_FAILURE, "write file failure %s", path);

		chmod(connection->pack_file, 0644);
		write(fd, connection->response, connection->response_size);
		close(fd);
	}
}


/*
 * store_object
 *
 * Procedure that creates a new object and stores it in the array and lookup tree.
 */

static void
store_object(connector *connection, int type, char *buffer, int buffer_size, int pack_offset, int index_delta, char *ref_delta_sha)
{
	struct object_node *object = NULL, find;
	char               *sha = calculate_object_sha(buffer, buffer_size, type);

	find.sha = sha;

	if ((object = RB_FIND(Tree_Objects, &Objects, &find)) != NULL) {
		free(sha);
	} else {
		if (connection->objects % BUFFER_UNIT_SMALL == 0)
			if ((connection->object = (struct object_node **)realloc(connection->object, (connection->objects + BUFFER_UNIT_SMALL) * sizeof(struct object_node **))) == NULL)
				err(EXIT_FAILURE, "store_object: realloc");

		if ((object = (struct object_node *)malloc(sizeof(struct object_node))) == NULL)
			err(EXIT_FAILURE, "store_object: malloc");

		object->index         = connection->objects;
		object->type          = type;
		object->sha           = sha;
		object->pack_offset   = pack_offset;
		object->index_delta   = index_delta;
		object->ref_delta_sha = (ref_delta_sha ? legible_sha(ref_delta_sha) : NULL);
		object->buffer        = buffer;
		object->buffer_size   = buffer_size;
		object->data_size     = buffer_size;

		if (connection->verbosity > 1)
			fprintf(stdout, "###### %05d-%d\t%d\t%u\t%s\t%d\t%s\n", object->index, object->type, object->pack_offset, object->data_size, object->sha, object->index_delta, object->ref_delta_sha);

		if (type < 6)
			RB_INSERT(Tree_Objects, &Objects, object);

		connection->object[connection->objects++] = object;
/*
		char full_path[BUFFER_UNIT_SMALL];
		snprintf(full_path, sizeof(full_path), "./temp/b%05d-%d-%s.out", object->index, object->type, object->sha);

		int fd = open(full_path, O_WRONLY | O_CREAT | O_TRUNC);
		chmod(full_path, 0644);
		write(fd, object->buffer, object->data_size);
		close(fd);
*/
	}
}


/*
 * unpack_objects
 *
 * Procedure that extracts all of the objects from the pack data.
 */

static void
unpack_objects(connector *connection)
{
	int            buffer_size = 0, total_objects = 0, object_type = 0, position = 4, index_delta = 0;
	int            pack_offset = 0, lookup_offset = 0, stream_code = 0, version = 0, stream_bytes = 0;
	char          *buffer = NULL, *ref_delta_sha = NULL;
	uint32_t       file_size = 0, file_bits = 0;
	unsigned char  zlib_out[16384];

	/* Check the pack version number. */

	version   = (unsigned char)connection->response[position + 3];
	position += 4;

	if (version != 2)
		errc(EXIT_FAILURE, EFTYPE, "unpack_objects: pack version %d not supported", version);

	/* Determine the number of objects in the pack data. */

	for (int x = 0; x < 4; x++, position++)
		total_objects = (total_objects << 8) + (unsigned char)connection->response[position];

	if (connection->verbosity > 1)
		fprintf(stderr, "\npack version: %d, total_objects: %d, pack_size:% d\n\n", version, total_objects, connection->response_size);

	/* Unpack the objects. */

	while ((position < connection->response_size) && (total_objects-- > 0)) {
		object_type   = (unsigned char)connection->response[position] >> 4 & 0x07;
		pack_offset   = position;
		index_delta   = 0;
		file_size     = 0;
		stream_bytes  = 0;
		ref_delta_sha = NULL;

		/* Extract the file size. */

		do {
			file_bits  = connection->response[position] & (stream_bytes == 0 ? 0x0F : 0x7F);
			file_size += (stream_bytes == 0 ? file_bits : file_bits << (4 + 7 * (stream_bytes - 1)));
			stream_bytes++;
		}
		while (connection->response[position++] & 0x80);

		/* Find the object->index referred to by the ofs-delta. */

		if (object_type == 6) {
			lookup_offset = 0;
			index_delta   = connection->objects;

			do lookup_offset = (lookup_offset << 7) + (connection->response[position] & 0x7F) + 1;
			while (connection->response[position++] & 0x80);

			while (--index_delta > 0)
				if (pack_offset - lookup_offset + 1 == connection->object[index_delta]->pack_offset)
					break;

			if (index_delta == 0)
				errc(EXIT_FAILURE, EINVAL, "Cannot find ofs-delta base object");
		}

		/* Extract ref-delta checksum. */

		if (object_type == 7) {
			if ((ref_delta_sha = (char *)malloc(21)) == NULL)
				err(EXIT_FAILURE, "unpack_objects: malloc");

			memcpy(ref_delta_sha, connection->response + position, 20);
			position += 20;
		}

		/* Inflate and store the object. */

		buffer      = NULL;
		buffer_size = 0;

		z_stream stream = {
			.zalloc   = Z_NULL,
			.zfree    = Z_NULL,
			.opaque   = Z_NULL,
			.avail_in = connection->response_size - position,
			.next_in  = (unsigned char *)(connection->response + position),
			};

		stream_code = inflateInit(&stream);

		if (stream_code == Z_DATA_ERROR)
			errc(EXIT_FAILURE, EILSEQ, "unpack_objects: zlib data stream failure");

		do {
			stream.avail_out = 16384,
			stream.next_out  = zlib_out;
			stream_code      = inflate(&stream, Z_NO_FLUSH);
			stream_bytes     = 16384 - stream.avail_out;

			if ((buffer = (char *)realloc(buffer, buffer_size + stream_bytes)) == NULL)
				err(EXIT_FAILURE, "unpack_objects: realloc");

			memcpy(buffer + buffer_size, zlib_out, stream_bytes);
			buffer_size += stream_bytes;
		}
		while (stream.avail_out == 0);

		inflateEnd(&stream);
		position += stream.total_in;

		store_object(connection, object_type, buffer, buffer_size, pack_offset, index_delta, ref_delta_sha);

		if (ref_delta_sha != NULL)
			free(ref_delta_sha);
	}
}


/*
 * unpack_delta_integer
 *
 * Function that reconstructs a 32 bit integer from the data stream.
 */

static uint32_t
unpack_delta_integer(char *data, int *position, int bits)
{
	uint32_t result = 0, read_bytes = 0, temp = 0, mask = 8;

	/* Determine how many bytes in the stream are needed. */

	do if (bits & mask) read_bytes++;
	while (mask >>= 1);

	/* Put the bytes in their proper position based on the bit field passed in. */

	if (read_bytes > 0) {
		temp = read_bytes;
		mask = 3;

		do {
			if (bits & (1 << mask))
				result += ((unsigned char)data[*position + --temp] << (mask * 8));
		}
		while (mask-- > 0);

		*position += read_bytes;
	}

	return result;
}


/*
 * unpack_variable_length_integer
 *
 * Function that reconstructs a variable length integer from the data stream.
 */

static uint32_t
unpack_variable_length_integer(char *data, int *position)
{
	uint32_t result = 0, count = 0;

	do result += (data[*position] & 0x7F) << (7 * count++);
	while (data[(*position)++] & 0x80);

	return result;
}


/*
 * apply_deltas
 *
 * Procedure that applies the changes in all of the delta objects to their
 * base objects.
 */

static void
apply_deltas(connector *connection)
{
	int                 position = 0, instruction = 0, length_bits = 0, offset_bits = 0, delta_count = -1;
	int                 deltas[BUFFER_UNIT_SMALL], merge_buffer_size = 0, layer_buffer_size = 0;
	char               *start, *merge_buffer = NULL, *layer_buffer = NULL;
	uint32_t            offset = 0, length = 0, old_file_size = 0, new_file_size = 0, new_position = 0;
	struct object_node *delta, *base, lookup;

	for (int o = 0; o < connection->objects; o++) {
		merge_buffer = NULL;
		delta        = connection->object[o];
		delta_count  = 0;

		if (delta->type < 6)
			continue;

		/* Follow the chain of ofs-deltas down to the base object. */

		while (delta->type == 6) {
			deltas[delta_count++] = delta->index;
			delta = connection->object[delta->index_delta];
		}

		/* Lookup the base object and setup the merge buffer. */

		lookup.sha = delta->sha;

		if ((base = RB_FIND(Tree_Objects, &Objects, &lookup)) == NULL)
			errc(EXIT_FAILURE, ENOENT, "apply_deltas: can't find %05d -> %d/%s\n", delta->index, delta->index_delta, delta->ref_delta_sha);

		if ((merge_buffer = (char *)malloc(base->buffer_size)) == NULL)
			err(EXIT_FAILURE, "apply_deltas: malloc");

		memcpy(merge_buffer, base->buffer, base->buffer_size);
		merge_buffer_size = base->buffer_size;

		/* Loop though the deltas to be applied. */

		for (int x = delta_count - 1; x >= 0; x--) {
			delta         = connection->object[deltas[x]];
			position      = 0;
			new_position  = 0;
			old_file_size = unpack_variable_length_integer(delta->buffer, &position);
			new_file_size = unpack_variable_length_integer(delta->buffer, &position);

			/* Make sure the layer buffer is large enough. */

			if (new_file_size > layer_buffer_size) {
				layer_buffer_size = new_file_size;

				if ((layer_buffer = (char *)realloc(layer_buffer, layer_buffer_size)) == NULL)
					err(EXIT_FAILURE, "apply_deltas: realloc");
			}

			/* Loop through the copy/insert instructions and build up the layer buffer. */

			while (position < delta->data_size) {
				instruction = (unsigned char)delta->buffer[position++];

				if (instruction & 0x80) {
					length_bits = (instruction & 0x70) >> 4;
					offset_bits = (instruction & 0x0F);

					offset = unpack_delta_integer(delta->buffer, &position, offset_bits);
					start  = merge_buffer + offset;
					length = unpack_delta_integer(delta->buffer, &position, length_bits);

					if (length == 0)
						length = 65536;
				} else {
					offset    = position;
					start     = delta->buffer + offset;
					length    = instruction;
					position += length;
				}

				if (new_position + length > new_file_size)
					errc(EXIT_FAILURE, ERANGE, "apply_deltas: position overflow -- %u + %u > %u", new_position, length, new_file_size);

				memcpy(layer_buffer + new_position, start, length);
				new_position += length;
			}

			/* Make sure the merge buffer is large enough. */

			if (new_file_size > merge_buffer_size) {
				merge_buffer_size = new_file_size;

				if ((merge_buffer = (char *)realloc(merge_buffer, merge_buffer_size)) == NULL)
					err(EXIT_FAILURE, "apply_deltas: realloc");
			}

			/* Store the layer buffer in the merge buffer for the next loop iteration. */

			memcpy(merge_buffer, layer_buffer, new_file_size);
		}

		/* Store the completed object. */

		store_object(connection, base->type, merge_buffer, new_file_size, 0, 0, NULL);
	}
}


/*
 * extract_tree_item
 *
 * Procedure that extracts mode/path/sha items in a tree and returns them in a new file_node.
 */

static void
extract_tree_item(struct file_node *file, char **position)
{
	int path_size = 0;

	/* Extract the file mode. */

	file->mode = strtol(*position, (char **)NULL, 8);
	*position  = strchr(*position, ' ') + 1;

	/* Extract the file path. */

	path_size = strlen(*position) + 1;
	snprintf(file->path, path_size, "%s", *position);
	*position += path_size;

	/* Extract the file SHA checksum. */

	for (int x = 0; x < 20; x++)
		snprintf(&file->sha[x * 2], 3, "%02x", (unsigned char)*(*position)++);

	file->sha[40] = '\0';
}


/*
 * save_tree
 */

static void
save_tree(connector *connection, char *sha, char *base_path)
{
	struct object_node object, *found_object = NULL, *tree = NULL;
	struct file_node   file, *found_file = NULL, *new_file_node = NULL;
	char               full_path[BUFFER_UNIT_SMALL], *position = NULL;
	int                fd;

	if ((mkdir(base_path, 0755) == -1) && (errno != EEXIST))
		err(EXIT_FAILURE, "save_tree: Cannot create %s", base_path);

	if ((file.path = (char *)malloc(BUFFER_UNIT_SMALL + 1)) == NULL)
		err(EXIT_FAILURE, "save_tree: malloc");

	if ((file.sha = (char *)malloc(41)) == NULL)
		err(EXIT_FAILURE, "save_tree: malloc");

	object.sha = sha;

	if ((tree = RB_FIND(Tree_Objects, &Objects, &object)) == NULL)
		errc(EXIT_FAILURE, ENOENT, "save_tree: tree %s - %s cannot be found", base_path, object.sha);

	/* Process the tree items. */

	position = tree->buffer;

	while (position - tree->buffer < tree->data_size) {
		extract_tree_item(&file, &position);

		snprintf(full_path, sizeof(full_path), "%s/%s", base_path, file.path);

		/* Recursively walk the trees and save the files/links. */

		if (S_ISDIR(file.mode)) {
			save_tree(connection, file.sha, full_path);
		} else {
			/* Locate the pack file object and local copy of the file. */

			memcpy(object.sha, file.sha, 41);
			memcpy(file.path, full_path, sizeof(full_path));

			found_object = RB_FIND(Tree_Objects, &Objects, &object);
			found_file   = RB_FIND(Tree_Local_Files, &Local_Files, &file);

			if (found_object == NULL)
				errc(EXIT_FAILURE, ENOENT, "save_tree: file %s - %s cannot be found", full_path, object.sha);

			/* If the local file hasn't changed, skip it. */

			if ((found_file != NULL) && (strncmp(found_object->sha, found_file->sha, 40) == 0))
				continue;

			/* Otherwise save it. */

			if (connection->verbosity)
				printf(" %c %s\n", (found_file == NULL ? '+' : '*'), full_path);

			if (S_ISLNK(file.mode)) {
				if (symlink(found_object->buffer, full_path) == -1)
					err(EXIT_FAILURE, "save_tree: symlink failure %s -> %s", full_path, found_object->buffer);
			} else {
				if (((fd = open(full_path, O_WRONLY | O_CREAT | O_TRUNC)) == -1) && (errno != EEXIST))
					err(EXIT_FAILURE, "save_tree: write file failure %s", full_path);

				chmod(full_path, file.mode);
				write(fd, found_object->buffer, found_object->data_size);
				close(fd);

				/* Add/update the file details to the remote files tree. */

				if (found_file == NULL) {
					if ((new_file_node = (struct file_node *)malloc(sizeof(struct file_node))) == NULL)
						err(EXIT_FAILURE, "save_tree: malloc");

					new_file_node->mode = file.mode;
					new_file_node->sha  = strdup(found_object->sha);
					new_file_node->path = strdup(full_path);

					RB_INSERT(Tree_Remote_Files, &Remote_Files, new_file_node);
				} else {
					free(found_file->sha);

					found_file->mode = file.mode;
					found_file->sha  = strdup(found_object->sha);
				}
			}
		}
	}

	free(file.sha);
	free(file.path);
}


/*
 * save_objects
 *
 * Procedure that commits the objects and trees to disk.
 */

static void
save_objects(connector *connection)
{
	struct object_node *object = NULL, lookup;
	struct file_node   *file = NULL;
	char               *tree = NULL, temp[BUFFER_UNIT_SMALL];
	int                 fd = 0;

	/* Find the tree object referenced in the commit. */

	lookup.sha = connection->want;

	if ((object = RB_FIND(Tree_Objects, &Objects, &lookup)) == NULL)
		errc(EXIT_FAILURE, EINVAL, "save_objects: can't find %s\n", connection->want);

	if (memcmp(object->buffer, "tree ", 5) != 0)
		errc(EXIT_FAILURE, EINVAL, "save_objects: first object is not a commit");

	if ((tree = (char *)malloc(41)) == NULL)
		err(EXIT_FAILURE, "save_objects: malloc");

	memcpy(tree, object->buffer + 5, 40);
	tree[40] = '\0';

	/* Recursively start processing the tree. */

	save_tree(connection, tree, connection->path_target);

	/* Save the new list of remote files. */

	if ((fd = open(connection->remote_file_new, O_WRONLY | O_CREAT | O_TRUNC)) == -1)
		err(EXIT_FAILURE, "write file failure %s", connection->remote_file_new);

	chmod(connection->remote_file_new, 0644);
	write(fd, connection->want, strlen(connection->want));
	write(fd, "\n", 1);

	RB_FOREACH(file, Tree_Remote_Files, &Remote_Files) {
		snprintf(temp, BUFFER_UNIT_SMALL, "%o\t%s\t%s\n", file->mode, file->sha, file->path);
		write(fd, temp, strlen(temp));
	}

	close(fd);
	free(tree);
}


/*
 * set_configuration_parameters
 *
 * Procedure that parses a line of text from the config file, allocates
 * space and stores the values.
 */

static void
set_configuration_parameters(connector *connection, char *buffer, size_t length, const char *section)
{
	char *bracketed_section, *item, *line;

	if ((bracketed_section = (char *)malloc(strlen(section) + 4)) == NULL)
		err(EXIT_FAILURE, "set_configuration bracketed_section malloc");

	snprintf(bracketed_section, strlen(section) + 4, "[%s]\n", section);

	if ((item = strstr(buffer, bracketed_section)) == NULL)
		errc(EXIT_FAILURE, EINVAL, "Cannot find [%s] in gitup.conf", section);

	item += strlen(bracketed_section);

	while ((line = strsep(&item, "\n"))) {
		if ((strlen(line) == 0) || (line[0] == '['))
			break;

		if (line[0] == '#')
			continue;

		if (strstr(line, "host=") == line)
			connection->host = strdup(line + 5);

		if (strstr(line, "port=") == line)
			connection->port = strtol(line + 5, (char **)NULL, 10);

		if (strstr(line, "repository=") == line)
			connection->repository = strdup(line + 11);

		if (strstr(line, "branch=") == line)
			connection->branch = strdup(line + 7);

		if (strstr(line, "target=") == line)
			connection->path_target = strdup(line + 7);

		if (strstr(line, "work_directory=") == line)
			connection->path_work = strdup(line + 15);

		if (strstr(line, "verbosity=") == line)
			connection->verbosity = strtol(line + 10, (char **)NULL, 10);
	}

	/* Put the returns that strsep took out back in for the next run. */

	for (int x = 0; x < length; x++)
		if (buffer[x] == '\0')
			buffer[x] = '\n';

	free(bracketed_section);
}


/*
 * load_configuration
 *
 * Procedure that loads the section options from gitup.conf
 */

static void
load_configuration(connector *connection, char *configuration_file, char *section)
{
	char     *buffer      = NULL;
	uint32_t  buffer_size = 0;

	load_file(configuration_file, &buffer, &buffer_size);

	set_configuration_parameters(connection, buffer, buffer_size, "defaults");
	set_configuration_parameters(connection, buffer, buffer_size, section);

	connection->section = strdup(section);

	free(buffer);
}


/*
 * usage
 *
 * Procedure that prints a summary of command line options and exits.
 */

static void
usage(char *configuration_file)
{
	fprintf(stderr, "Usage: gitup <section> [options]\n\n");
	fprintf(stderr, "  Please see %s for the list of <section> options.\n\n", configuration_file);
	fprintf(stderr, "  Options:\n");
	fprintf(stderr, "    -c  Force gitup to clone the repository.\n");
	fprintf(stderr, "    -h  Override the 'have' checksum.\n");
	fprintf(stderr, "    -k  Path to save a copy of the pack data.\n");
	fprintf(stderr, "    -u  Path to load a copy of the pack data, skipping the download.\n");
	fprintf(stderr, "    -v  How verbose the output should be (0 = no output, 1 = the default\n");
	fprintf(stderr, "          normal output, 2 = also show debugging information.\n");
	fprintf(stderr, "    -V  Display gitup's version number and exit.\n");
	fprintf(stderr, "    -w  Override the 'want' checksum.\n");
	fprintf(stderr, "\n");

	exit(EXIT_FAILURE);
}


/*
 * main
 *
 * A lightweight, dependency-free program to clone/pull a git repository.
 */

int
main(int argc, char **argv)
{
	struct object_node *object = NULL;
	struct file_node   *file   = NULL;
	int                 option = 0, length = 0, count = 0;
	uint32_t            remote_file_size = 0;
	char               *configuration_file = "./gitup.conf";
	char               *line = NULL, *sha = NULL, *path = NULL, *remote_files = NULL;
	struct stat         pack_file, temp_file;

	connector connection = {
		.ssl               = NULL,
		.ctx               = NULL,
		.socket_descriptor = 0,
		.host              = NULL,
		.port              = 0,
		.agent             = NULL,
		.section           = NULL,
		.repository        = NULL,
		.branch            = NULL,
		.have              = NULL,
		.want              = NULL,
		.response          = NULL,
		.response_blocks   = 0,
		.response_size     = 0,
		.clone             = 0,
		.object            = NULL,
		.objects           = 0,
		.pack_file         = NULL,
		.path_target       = NULL,
		.path_work         = NULL,
		.remote_file_old   = NULL,
		.remote_file_new   = NULL,
		.keep_pack_file    = 0,
		.use_pack_file     = 0,
		.verbosity         = 1,
		};

	/* Process the command line parameters. */

	if (argc < 2)
		usage(configuration_file);

	if (argv[1][0] == '-') {
		if (argv[1][1] == 'V') {
			fprintf(stdout, "gitup version %s\n", GITUP_VERSION);
			exit(EXIT_SUCCESS);
		}
		else usage(configuration_file);
	} else {
		load_configuration(&connection, configuration_file, argv[1]);
		optind = 2;
	}

	while ((option = getopt(argc, argv, "ch:ku:Vv:w:")) != -1)
		switch (option) {
			case 'c':
				connection.clone = 1;
				break;
			case 'h':
				connection.have = strdup(optarg);
				break;
			case 'k':
				connection.keep_pack_file = 1;
				break;
			case 'u':
				connection.use_pack_file = 1;
				connection.pack_file     = strdup(optarg);

				/* Try and extract the want from the file name. */

				int   length    = strlen(optarg);
				char *start     = optarg;
				char *temp      = optarg;
				char *extension = strnstr(optarg, ".pack", length);

				while ((temp = strchr(start, '/')) != NULL)
					start = temp + 1;

				char *want = strnstr(start, connection.section, length - (start - optarg));

				if (want == NULL)
					break;
				else
					want += strlen(connection.section) + 1;

				if (extension != NULL)
					*extension = '\0';

				if (strlen(want) == 40)
					connection.want = strdup(want);

				break;
			case 'w':
				connection.want = strdup(optarg);
				break;
			case 'v':
				connection.verbosity = strtol(optarg, (char **)NULL, 10);
				break;
		}

	/* Continue setting up the environment. */

	if (stat(connection.path_target, &temp_file) == -1)
		connection.clone = 1;

	if ((mkdir(connection.path_work, 0755) == -1) && (errno != EEXIST))
		err(EXIT_FAILURE, "Cannot create %s", connection.path_work);

	find_local_tree(&connection, connection.path_target);

	/* Load the list of remote files and checksums, if they exist. */

	length = strlen(connection.path_work) + MAXNAMLEN;

	if ((connection.remote_file_old = (char *)malloc(length)) == NULL)
		err(EXIT_FAILURE, "main connection.remote_file_old malloc");

	if ((connection.remote_file_new = (char *)malloc(length)) == NULL)
		err(EXIT_FAILURE, "main connection.remote_file_new malloc");

	snprintf(connection.remote_file_old, length, "%s/%s", connection.path_work, argv[1]);
	snprintf(connection.remote_file_new, length, "%s/%s.new", connection.path_work, argv[1]);

	if (stat(connection.remote_file_old, &temp_file) != -1) {
		load_file(connection.remote_file_old, &remote_files, &remote_file_size);

		while ((line = strsep(&remote_files, "\n"))) {
			if (count++ == 0)
				connection.have = strdup(line);

			if (strlen(line) <= 42)
				continue;

			/* Split the line and save the data into the remote files tree. */

			if ((file = (struct file_node *)malloc(sizeof(struct file_node))) == NULL)
				err(EXIT_FAILURE, "main: malloc");

			sha  = strchr(line, '\t') + 1;
			path = strchr(sha,  '\t') + 1;

			*(sha -  1) = '\0';
			*(sha + 40) = '\0';

			file->mode = strtol(line, (char **)NULL, 8);
			file->sha  = strdup(sha);
			file->path = strdup(path);

			RB_INSERT(Tree_Remote_Files, &Remote_Files, file);
		}

		free(remote_files);
	}

	/* Display connection parameters. */

	if (connection.verbosity) {
		fprintf(stderr, "# Host: %s\n", connection.host);
		fprintf(stderr, "# Port: %d\n", connection.port);
		fprintf(stderr, "# Repository: %s\n", connection.repository);
		fprintf(stderr, "# Branch: %s\n", connection.branch);
		fprintf(stderr, "# Target: %s\n", connection.path_target);

		if (connection.have)
			fprintf(stderr, "# Have: %s\n", connection.have);

		if (connection.want)
			fprintf(stderr, "# Want: %s\n", connection.want);

		if (connection.use_pack_file)
			fprintf(stderr, "# Using pack file: %s\n", connection.pack_file);
	}

	/* Execute the fetch, unpack, apply deltas and save. */

	if ((connection.use_pack_file == 0) || ((connection.use_pack_file == 1) && (lstat(connection.pack_file, &pack_file) == -1)))
		get_commit_details(&connection);

	fetch_pack(&connection);
	unpack_objects(&connection);

	if (connection.objects > 0) {
		apply_deltas(&connection);
		save_objects(&connection);

		remove(connection.remote_file_old);

		if ((rename(connection.remote_file_new, connection.remote_file_old)) != 0)
			err(EXIT_FAILURE, "Cannot rename %s", connection.remote_file_old);
	}

	/* Wrap it all up. */

	RB_FOREACH(object, Tree_Objects, &Objects)
		RB_REMOVE(Tree_Objects, &Objects, object);

	RB_FOREACH(file, Tree_Local_Files, &Local_Files)
		file_node_free(RB_REMOVE(Tree_Local_Files, &Local_Files, file));

	RB_FOREACH(file, Tree_Remote_Files, &Remote_Files)
		file_node_free(RB_REMOVE(Tree_Remote_Files, &Remote_Files, file));

	RB_FOREACH(file, Tree_Local_Directories, &Local_Directories)
		file_node_free(RB_REMOVE(Tree_Local_Directories, &Local_Directories, file));

	for (int o = 0; o < connection.objects; o++) {
		if (connection.verbosity > 1)
			fprintf(stdout, "###### %05d-%d\t%d\t%u\t%s\t%d\t%s\n", connection.object[o]->index, connection.object[o]->type, connection.object[o]->pack_offset, connection.object[o]->data_size, connection.object[o]->sha, connection.object[o]->index_delta, connection.object[o]->ref_delta_sha);

		object_node_free(connection.object[o]);
	}

	if (connection.response)
		free(connection.response);

	if (connection.object)
		free(connection.object);

	if (connection.host)
		free(connection.host);

	if (connection.agent)
		free(connection.agent);

	if (connection.section)
		free(connection.section);

	if (connection.repository)
		free(connection.repository);

	if (connection.branch)
		free(connection.branch);

	if (connection.have)
		free(connection.have);

	if (connection.want)
		free(connection.want);

	if (connection.pack_file)
		free(connection.pack_file);

	if (connection.path_target)
		free(connection.path_target);

	if (connection.path_work)
		free(connection.path_work);

	if (connection.remote_file_old)
		free(connection.remote_file_old);

	if (connection.remote_file_new)
		free(connection.remote_file_new);

	if (connection.ssl) {
		SSL_shutdown(connection.ssl);
		SSL_CTX_free(connection.ctx);
		SSL_free(connection.ssl);
	}

	return (0);
}
