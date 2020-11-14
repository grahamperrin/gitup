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

#define BUFFER_UNIT_SMALL 4096
#define BUFFER_UNIT_LARGE 1048576

struct object_node {
	RB_ENTRY(object_node) link;
	char *sha;
	int   type;
	int   index;
	char *ref_delta_sha;
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
	int                  socket_descriptor;
	SSL                 *ssl;
	SSL_CTX             *ctx;
	char                *address;
	uint16_t             port;
	char                *repository;
	char                *commit;
	char                *branch;
	char                *response;
	uint32_t             response_size;
	int                  response_blocks;
	struct object_node **object;
	int                  objects;
	char                *pack_file;
	char                *path_target;
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

static RB_HEAD(Tree_Local_Files, file_node) Local_Files = RB_INITIALIZER(&Local_Files);
RB_PROTOTYPE(Tree_Local_Files, file_node, link, file_node_compare)
RB_GENERATE(Tree_Local_Files, file_node, link, file_node_compare)

static RB_HEAD(Tree_Local_Directories, file_node) Local_Directories = RB_INITIALIZER(&Local_Directories);
RB_PROTOTYPE(Tree_Local_Directories, file_node, link, file_node_compare)
RB_GENERATE(Tree_Local_Directories, file_node, link, file_node_compare)

static RB_HEAD(Tree_Objects, object_node) Objects = RB_INITIALIZER(&Objects);
RB_PROTOTYPE(Tree_Objects, object_node, link, object_node_compare)
RB_GENERATE(Tree_Objects, object_node, link, object_node_compare)


/*
 * legible_sha
 *
 * Function that returns a human-readable SHA checksum.
 */

static char *
legible_sha(char *sha_buffer)
{
	char *sha = (char *)malloc(41);

	for (int x = 0; x < 20; x++)
		snprintf(&sha[x * 2], 3, "%02x", (unsigned char)sha_buffer[x]);

	sha[40] = '\0';

	return sha;
}


/*
 * calculate_sha
 *
 * Function that adds git's "type file-size\0" header and returns the SHA checksum.
 */

static char *
calculate_sha(char *buffer, uint32_t buffer_size, int type)
{
	int   digits = buffer_size, header_width = 0;
	char *sha = NULL, *sha_buffer = (char *)malloc(21);
	char *temp_buffer = (char *)malloc(buffer_size + 24);
	char *types[8] = { "", "commit", "tree", "blob", "tag", "", "ofs_delta", "ref_delta" };

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
 * Function that loads a local file, removes any revision tags and returns
 * the SHA checksum.
 */

static char *
calculate_file_sha(char *path, ssize_t file_size, int file_mode)
{
	int   fd;
	char *file_buffer, *position, *value, *eol, *sha = NULL;
	char *sha_buffer = (char *)malloc(21);

	if (S_ISLNK(file_mode)) {
	} else {
		/* Load the file into memory. */

		if ((file_buffer = (char *)malloc(file_size + 16)) == NULL)
			err(EXIT_FAILURE, "calculate_file_sha: file_buffer malloc");

		if ((fd = open(path, O_RDONLY)) == -1)
			err(EXIT_FAILURE, "calculate_file_sha: read file (%s): open", path);

		if (read(fd, file_buffer, file_size) != file_size)
			err(EXIT_FAILURE, "calculate_file_sha: read file (%s): file changed", path);

		close(fd);

		/* Remove any revision tags. */

		position = file_buffer;

		while ((position = strstr(position, "$FreeBSD:"))) {
			position += 8;
			value = strchr(position, '$');
			eol   = strchr(position, '\n');

			if ((value) && ((eol == NULL) || (value < eol))) {
				memmove(position, value, file_size - (value - file_buffer));
				file_size -= (value - position);
				file_buffer[file_size] = '\0';
			}
		}

		/* Calculate the SHA checksum. */

		sha = calculate_sha(file_buffer, file_size, 3);

		free(file_buffer);
	}

	free(sha_buffer);

	return sha;
}


/*
 * append_string
 *
 * Procedure that appends one string to another.
 */

static void
append_string(char **buffer, unsigned int *buffer_size, unsigned int *string_length, char *addendum)
{
	int adjust = 0;

	while (*string_length + strlen(addendum) > *buffer_size) {
		adjust = 1;
		*buffer_size += BUFFER_UNIT_SMALL;
	}

	if ((adjust) && ((*buffer = (char *)realloc(*buffer, *buffer_size + 1)) == NULL))
		err(EXIT_FAILURE, "append_string: realloc");

	memcpy(*buffer + *string_length, addendum, strlen(addendum));

	*string_length += strlen(addendum);

	*(*buffer + *string_length) = '\0';
}


/*
 * find_local_files_and_directories
 *
 * Procedure that recursively finds and adds local files and directories to
 * separate red-black trees.
 */

static void
find_local_files_and_directories(char *path_base, const char *path_target)
{
	DIR              *directory;
	struct stat       file;
	struct dirent    *entry;
	struct file_node *new_node;
	char             *full_path;
	int               full_path_size, file_name_size;

	/* Construct the file's full path. */

	full_path_size = strlen(path_base) + strlen(path_target) + MAXNAMLEN + 3;

	if ((full_path = (char *)malloc(full_path_size)) == NULL)
		err(EXIT_FAILURE, "find_local_files_and_directories: full_path malloc");

	snprintf(full_path, full_path_size, "%s%s", path_base, path_target);

	/* Process the directory's contents. */

	if (lstat(full_path, &file) != -1) {
		if (S_ISDIR(file.st_mode)) {

			/* Keep track of the local directories, ignoring path_base. */

			if (strlen(path_target)) {
				new_node       = (struct file_node *)malloc(sizeof(struct file_node));
				new_node->path = strdup(full_path);
				new_node->sha  = 0;

				RB_INSERT(Tree_Local_Directories, &Local_Directories, new_node);
			}

			/* Recursively process the contents of the directory. */

			if ((directory = opendir(full_path)) != NULL) {
				while ((entry = readdir(directory)) != NULL) {
					file_name_size = strlen(entry->d_name);

					if ((file_name_size == 1) && (strcmp(entry->d_name, "." ) == 0))
						continue;

					if ((file_name_size == 2) && (strcmp(entry->d_name, "..") == 0))
						continue;

					if ((file_name_size == 4) && (strcmp(entry->d_name, ".git") == 0))
						continue;

					snprintf(full_path,
						full_path_size,
						"%s/%s",
						path_target,
						entry->d_name);

					find_local_files_and_directories(path_base, full_path);
				}

				closedir(directory);
			}
		} else {
			new_node       = (struct file_node *)malloc(sizeof(struct file_node));
			new_node->path = strdup(path_target);
			new_node->sha  = calculate_file_sha(full_path, file.st_size, file.st_mode);
			new_node->mode = file.st_mode;

			printf("?? %o\t%s\t%s\n", new_node->mode, new_node->sha, new_node->path);

			RB_INSERT(Tree_Local_Files, &Local_Files, new_node);
		}
	}

	free(full_path);
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

	if ((error = getaddrinfo(connection->address, type, &hints, &start)))
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
	char  read_buffer[BUFFER_UNIT_SMALL + 1];
	char *position = NULL, *next_chunk = NULL, *data_start = NULL;
	int   chunk_size = -1, bytes_expected = 0, error = 0;
	int   bytes_read = 0, total_bytes_read = 0;
	int   bytes_sent = 0, total_bytes_sent = 0;
	int   bytes_to_write = strlen(command);

	bzero(connection->response, connection->response_blocks * BUFFER_UNIT_SMALL + 1);
	bzero(read_buffer, BUFFER_UNIT_SMALL + 1);

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

		if (connection->verbosity > 2)
			fprintf(stderr, "\r==> bytes sent: %d", total_bytes_sent);
		}

	if (connection->verbosity > 2)
		fprintf(stderr, "\n");

	/* Process the response. */

	while (chunk_size) {
		bytes_read = SSL_read(connection->ssl, read_buffer, BUFFER_UNIT_SMALL);

		if (bytes_read == 0) break;

		if (bytes_read < 0)
			err(EXIT_FAILURE, "process_command: SSL_read error: %d\n", SSL_get_error(connection->ssl, error));

		memcpy(connection->response + total_bytes_read, read_buffer, bytes_read + 1);
		total_bytes_read += bytes_read;
		connection->response[total_bytes_read] = '\0';

		if (connection->verbosity > 2)
			fprintf(stderr, "\r==> bytes read: %d\tbytes_expected:%d\ttotal_bytes_read:%d", bytes_read, bytes_expected, total_bytes_read);

		/* Find the boundary between the header and the data. */

		if (chunk_size == -1) {
			if ((position = strnstr(connection->response, "\r\n\r\n", total_bytes_read)) == NULL) {
				continue;
				}
			else {
				bytes_expected = position - connection->response + 4;
				position += 2;
				data_start = position;
				}
			}

		while (bytes_expected < total_bytes_read) {
			chunk_size = strtol(position + 2, (char **)NULL, 16);

			/* Check to see if the stream has ended or if the current chunk hasn't been fully fetched. */

			if ((chunk_size == 0) || (bytes_expected + chunk_size > total_bytes_read))
 				break;

			/* Find the start of the next chunk. */

			next_chunk = strnstr(position + 2, "\r\n", total_bytes_read - (position + 2 - connection->response));

			if (next_chunk == NULL)
				break;
			else
				next_chunk += 2;

			/* Remove the chunk length marker. */

			memmove(position, next_chunk, total_bytes_read - (next_chunk - connection->response));
			total_bytes_read -= (next_chunk - position);

			position += chunk_size;
			bytes_expected += chunk_size;
			}
		}

	if (strstr(connection->response, "HTTP/1.1 200 OK") != connection->response)
		err(EXIT_FAILURE, "process_command: read failure:\n%s\n", connection->response);

	if (connection->verbosity > 2)
		fprintf(stderr, "\n");

	/* Remove the header. */

	connection->response_size = total_bytes_read - (data_start - connection->response);
	memmove(connection->response, data_start, connection->response_size);
	connection->response[connection->response_size] = '\0';
}


/*
 * build_fetch_request
 *
 * Function that constructs the command to the fetch the pack data.
 */

static char *
build_fetch_request(connector *connection)
{
	struct file_node *file;
	unsigned int      command_size = 0, command_buffer_size = BUFFER_UNIT_LARGE;
	char             *command = (char *)malloc(BUFFER_UNIT_LARGE);
	char              want[BUFFER_UNIT_SMALL], have[51];

	/* Start with the "wants". */

	snprintf(want,
		BUFFER_UNIT_SMALL,
		"0065want %s no-progress side-band-64k ref-delta agent=git/2.28\n"
		"0032want %s\n"
//		"000cdeepen 1"
//		"0034shallow %s"
		"0000",
		connection->commit,
//		connection->commit,
		connection->commit
		);

	/* Pre-determine the length of the command for inclusion in the header. */

	command_size = strlen(want) + strlen("0009done\n0000");

	RB_FOREACH(file, Tree_Local_Files, &Local_Files)
		command_size += 50;

	snprintf(command,
		BUFFER_UNIT_LARGE,
		"POST %s/git-upload-pack HTTP/1.1\n"
		"Host: github.com\n"
		"Accept: */*\n"
		"User-Agent: git/2.28\n"
		"Content-type: application/x-git-upload-pack-request\n"
		"Content-length: %d\n"
		"\r\n"
		"%s",
		connection->repository,
		command_size,
		want
		);

	command_size = strlen(command);

	/* Loop through the local files adding each to the "haves". */

	RB_FOREACH(file, Tree_Local_Files, &Local_Files) {
		snprintf(have, sizeof(have), "0032have %s\n\n", file->sha);
		append_string(&command, &command_buffer_size, &command_size, have);
		}

	/* Finish the request. */

	append_string(&command, &command_buffer_size, &command_size, "0009done\n0000");

	if (connection->verbosity > 2)
		fprintf(stderr, "%s\n\n", command);

	return command;
}


/*
 * get_commit_hash
 */

static void
get_commit_hash(connector *connection)
{
	char command[BUFFER_UNIT_SMALL], full_branch[BUFFER_UNIT_SMALL], *position = NULL;

	/* Get the list of commits from the server. */

	snprintf(command,
		BUFFER_UNIT_SMALL,
		"GET %s/info/refs?service=git-upload-pack HTTP/1.1\n"
		"Host: github.com\n"
		"User-Agent: git/2.28\n"
		"\r\n",
		connection->repository
		);

	process_command(connection, command);

	/* Change all \0 characters to \n to make it easy to find the entry. */

	for (uint32_t x = 0; x < connection->response_size; x++)
		if (connection->response[x] == '\0')
			connection->response[x] = '\n';

	snprintf(full_branch, BUFFER_UNIT_SMALL, " refs/heads/%s\n", connection->branch);

	position = strstr(connection->response, full_branch);

	if (position == NULL)
		err(EXIT_FAILURE, "get_commit_hash: %s doesn't exist in %s", connection->branch, connection->repository);

	/* Extract and save the commit hash. */

	connection->commit = (char *)malloc(41);
	memcpy(connection->commit, position - 40, 40);
	connection->commit[40] = '\0';

	printf("Fetching %s %s (%s)\n", connection->repository, connection->branch, connection->commit);
}


/*
 * fetch_pack
 *
 * Procedure that loads a local copy of the pack data or fetches it from the server.
 */

static void
fetch_pack(connector *connection)
{
	char        *fetch  = NULL, *pack_start = NULL, sha_buffer[20], path[BUFFER_UNIT_SMALL];
	struct stat  pack_file;
	int          fd, chunk_size = 1, position = 0, pack_size = 0;

	connection->response_size = 0;

	/* If a pack file has been specified, attempt to load it. */

	if ((connection->pack_file) && (lstat(connection->pack_file, &pack_file) != -1))
		if ((fd = open(connection->pack_file, O_RDONLY)) != -1) {
			connection->response_size = pack_file.st_size;

			read(fd, connection->response, connection->response_size);
			close(fd);

			pack_size = connection->response_size - 31;
		}

	/* If no pack data has been loaded, fetch it from the server. */

	if (connection->response_size == 0) {
		fetch = build_fetch_request(connection);
		process_command(connection, fetch);
		free(fetch);

		/* Find the start of the pack data. */

		if ((pack_start = strstr(connection->response, "PACK")) == NULL)
			err(EXIT_FAILURE, "unpack_objects: %s\n", connection->response);

		pack_start -= 5;

		/* Remove the chunk size markers from the pack data. */

		position = pack_start - connection->response;

		while (chunk_size > 0) {
			chunk_size = strtol(connection->response + position, (char **)NULL, 16);

			if (chunk_size == 0)
				break;

			memmove(connection->response + position, connection->response + position + 5, connection->response_size - position);
			connection->response_size -= 5;
			position += chunk_size - 5;
		}

		/* Remove anything before the 'PACK'. */

		connection->response_size -= (pack_start - connection->response);
		memmove(connection->response, pack_start, connection->response_size);
		pack_size = connection->response_size - 31;

		/* Save the pack data for testing. */

		snprintf(path, sizeof(path), "./temp/pack");

		if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC)) == -1)
			err(EXIT_FAILURE, "write file failure %s", path);

		chmod(path, 0644);
		write(fd, connection->response, connection->response_size);
		close(fd);
	}

	/* Verify the pack data checksum. */

	SHA1((unsigned char *)connection->response, pack_size, (unsigned char *)sha_buffer);

	if (memcmp(connection->response + pack_size, sha_buffer, 20) != 0)
		errc(EXIT_FAILURE, 80, "unpack_objects: pack checksum mismatch - expected %s, received %s", legible_sha(connection->response + pack_size), legible_sha(sha_buffer));
}


/*
 * store_object
 *
 * Procedure that creates a new object and stores it in the array and lookup tree.
 */

static void
store_object(connector *connection, int type, char *buffer, int buffer_size, char *ref_delta_sha)
{
	struct object_node *object = NULL;

	if (connection->objects % BUFFER_UNIT_SMALL == 0)
		if ((connection->object = (struct object_node **)realloc(connection->object, (connection->objects + BUFFER_UNIT_SMALL) * sizeof(struct object_node **))) == NULL)
			err(EXIT_FAILURE, "store_object: realloc");

	if ((object = (struct object_node *)malloc(sizeof(struct object_node))) == NULL)
		err(EXIT_FAILURE, "store_object: malloc");

	object->index         = connection->objects;
	object->type          = type;
	object->sha           = calculate_sha(buffer, buffer_size, type);
	object->ref_delta_sha = (ref_delta_sha != NULL ? legible_sha(ref_delta_sha) : NULL);
	object->buffer        = buffer;
	object->buffer_size   = buffer_size;
	object->data_size     = buffer_size;

	if (connection->verbosity > 1)
		fprintf(stdout, "###### %04d-%d\t%u\t%s -> %s\n", object->index, object->type, object->data_size, object->sha, object->ref_delta_sha);

	connection->object[connection->objects++] = object;

	RB_INSERT(Tree_Objects, &Objects, object);
}


/*
 * unpack_objects
 *
 * Procedure that extracts all of the objects from the pack data.
 */

static void
unpack_objects(connector *connection)
{
	int                 objects = 0, buffer_size = 0, total_objects = 0;
	int                 version = 0, object_type = 0, stream_code = 0, stream_bytes = 0;
	char               *buffer = NULL, *ref_delta_sha_buffer = NULL;
	uint32_t            file_size = 0, file_bits = 0, position = 4;
	unsigned char       zlib_out[16384];
	struct object_node *new_object = NULL;

	/* Check the pack version number. */

	version   = (unsigned char)connection->response[position + 3];
	position += 4;

	if (version != 2)
		errc(EXIT_FAILURE, 79, "unpack_objects: pack version %d not supported", version);

	/* Determine the number of objects in the pack data. */

	for (int x = 0; x < 4; x++, position++)
		total_objects = (total_objects << 8) + (unsigned char)connection->response[position];

	if (connection->verbosity > 2)
		fprintf(stderr, "\npack version: %d, total_objects: %d, pack_size:% d\n\n", version, total_objects, connection->response_size);

	/* Unpack the objects. */

	while ((position < connection->response_size) && (total_objects-- > 0)) {
		object_type = (unsigned char)connection->response[position] >> 4 & 0x07;

		/* Extract the file size. */

		file_size = stream_bytes = 0;

		do {
			file_bits  = connection->response[position] & (stream_bytes == 0 ? 0x0F : 0x7F);
			file_size += (stream_bytes == 0 ? file_bits : file_bits << (4 + 7 * (stream_bytes - 1)));
			stream_bytes++;
		}
		while (connection->response[position++] & 0x80);

		/* Ignore ofs_deltas for now. */

		if (object_type == 6) {
			while ((unsigned char)connection->response[position] & 0x80) position++;
			position++;
		}

		/* Extract ref_delta SHA checksum. */

		if (object_type == 7) {
			ref_delta_sha_buffer = (char *)malloc(21);
			memcpy(ref_delta_sha_buffer, connection->response + position, 20);
			position += 20;
		}
		else ref_delta_sha_buffer = NULL;

		/* Inflate the object. */

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
			err(EXIT_FAILURE, "unpack_objects: zlib data stream failure");

		do {
			stream.avail_out = 16384,
			stream.next_out  = zlib_out;
			stream_code      = inflate(&stream, Z_NO_FLUSH);
			stream_bytes     = 16384 - stream.avail_out;

			buffer = (char *)realloc(buffer, buffer_size + stream_bytes);
			memcpy(buffer + buffer_size, zlib_out, stream_bytes);
			buffer_size += stream_bytes;
		}
		while (stream.avail_out == 0);

		inflateEnd(&stream);
		position += stream.total_in;

		/* Store the object. */

		if (connection->objects % BUFFER_UNIT_SMALL == 0)
			if ((connection->object = (struct object_node **)realloc(connection->object, (connection->objects + BUFFER_UNIT_SMALL) * sizeof(struct object_node **))) == NULL)
				err(EXIT_FAILURE, "unpack_objects: object realloc");

		new_object                = (struct object_node *)malloc(sizeof(struct object_node));
		new_object->index         = objects++;
		new_object->type          = object_type;
		new_object->sha           = calculate_sha(buffer, buffer_size, object_type);
		new_object->ref_delta_sha = (ref_delta_sha_buffer ? legible_sha(ref_delta_sha_buffer) : NULL);
		new_object->buffer        = buffer;
		new_object->buffer_size   = buffer_size;
		new_object->data_size     = buffer_size;

		if (connection->verbosity > 2)
			fprintf(stderr, "###### %04d-%d\t%u\t%s -> %s\n", new_object->index, new_object->type, file_size, new_object->sha, new_object->ref_delta_sha);

		connection->object[connection->objects++] = new_object;

		RB_INSERT(Tree_Objects, &Objects, new_object);
	}
}


/*
 * save_objects
 */

static void
save_objects()
{
	struct object_node *object = NULL;
	char                path[BUFFER_UNIT_SMALL];
	int                 fd;

	RB_FOREACH(object, Tree_Objects, &Objects) {
		snprintf(path, sizeof(path), "./temp/b%04d-%d-%s.out", object->index, object->type, object->sha);

		if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC)) == -1)
			err(EXIT_FAILURE, "save_objects: write file failure %s", path);

		chmod(path, 0644);
		write(fd, object->buffer, object->data_size);
		close(fd);
	}
}


/*
 * main
 *
 * A lightweight, dependency-free program to clone/pull a git repository.
 */

int
main(void)
{
	struct object_node *object = NULL;
	struct file_node   *file   = NULL;

	connector connection = {
		.ssl               = NULL,
		.ctx               = NULL,
		.socket_descriptor = 0,
		.address           = "github.com",
		.port              = 443,
		.response_blocks   = 1024,
		.response_size     = 0,
		.verbosity         = 3,
//		.repository        = "/johnmehr/svnup",
		.repository        = "/johnmehr/gitup",
//		.repository        = "/johnmehr/test",
//		.repository        = "/freebsd/freebsd-ci",
		.branch            = "main",
//		.path_target       = "/work/code/gitup/test",
		.path_target       = "/work/code/gitup/svnup",
		.pack_file         = "/work/code/gitup/temp/pack",
		};

	if ((connection.response = (char *)malloc(connection.response_blocks * BUFFER_UNIT_SMALL + 1)) == NULL)
		err(EXIT_FAILURE, "main: connection.response malloc");

	find_local_files_and_directories(connection.path_target, "");

	/* Initialize connection with the server and get the latest revision number. */

	get_commit_hash(&connection);
	fetch_pack(&connection);
	unpack_objects(&connection);
	save_objects();

	/* Wrap it all up. */

	RB_FOREACH(object, Tree_Objects, &Objects)
		RB_REMOVE(Tree_Objects, &Objects, object);

	RB_FOREACH(file, Tree_Local_Files, &Local_Files)
		file_node_free(RB_REMOVE(Tree_Local_Files, &Local_Files, file));

	RB_FOREACH(file, Tree_Local_Directories, &Local_Directories)
		file_node_free(RB_REMOVE(Tree_Local_Directories, &Local_Directories, file));

	for (int o = 0; o < connection.objects; o++) {
		if (connection.verbosity > 2)
			fprintf(stderr, "###### %04d-%d\t%u\t%s -> %s\n", connection.object[o]->index, connection.object[o]->type, connection.object[o]->data_size, connection.object[o]->sha, connection.object[o]->ref_delta_sha);

		object_node_free(connection.object[o]);
		}

	if (connection.response)
		free(connection.response);

	if (connection.object)
		free(connection.object);

	if (connection.commit)
		free(connection.commit);

	if (connection.ssl) {
		SSL_shutdown(connection.ssl);
		SSL_CTX_free(connection.ctx);
		SSL_free(connection.ssl);
	}

	return 0;
}