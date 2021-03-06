/*
 * epoll-based echo server. Uses epoll(7) to multiplex connections.
 *
 * TODO:
 *  - block data receiving when receive buffer is full (use circular buffers)
 *  - do not copy receive buffer into send buffer when send buffer data is
 *      still valid
 *
 * 2012-2017, Operating Systems
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/sendfile.h>
#include <libaio.h>
#include <sys/eventfd.h>

#include "aws.h"
#include "util.h"
#include "debug.h"
#include "sock_util.h"
#include "w_epoll.h"
#include "http-parser/http_parser.h"

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

enum connection_state {
	STATE_DATA_RECEIVED,
	STATE_DATA_SENT,
	STATE_CONNECTION_CLOSED
};

/* structure acting as a connection handler */
struct connection {
	int sockfd;
	/* buffers used for receiving messages and then echoing them back */
	char recv_buffer[BUFSIZ];
	size_t recv_len;
	char send_buffer[BUFSIZ];
	size_t send_len;
	enum connection_state state;

    int fd;
    char path[BUFSIZ];
	int file_size;
	int ready_dynamic;
	int dynamic_sending;
	io_context_t ctx;
	int efd;
	struct iocb iocb;
	struct iocb *piocb;
	char *data_buffer; /* fd -> sockfd */
	size_t data_len;
};
typedef struct connection conn;
char request_path[BUFSIZ];
http_parser request_parser;


static int on_path_cb(http_parser *p, const char *buf, size_t len)
{
	assert(p == &request_parser);
	memcpy(request_path, buf, len);

	return 0;
}

static http_parser_settings settings_on_path = {
	/* on_message_begin */ 0,
	/* on_header_field */ 0,
	/* on_header_value */ 0,
	/* on_path */ on_path_cb,
	/* on_url */ 0,
	/* on_fragment */ 0,
	/* on_query_string */ 0,
	/* on_body */ 0,
	/* on_headers_complete */ 0,
	/* on_message_complete */ 0
};


/*
 * Initialize connection structure on given socket.
 */

static struct connection *connection_create(int sockfd)
{
	struct connection *conn = malloc(sizeof(*conn));

	DIE(conn == NULL, "malloc");

	conn->sockfd = sockfd;
	memset(conn->recv_buffer, 0, BUFSIZ);
	memset(conn->send_buffer, 0, BUFSIZ);
	conn->recv_len = 0;
	conn->send_len = 0;
	conn->fd = -1;
	conn->file_size = 0;
	conn->ready_dynamic = 0;
	conn->dynamic_sending = 0;


	return conn;
}

/*
 * Remove connection handler.
 */

static void connection_remove(struct connection *conn)
{
	close(conn->sockfd);
	conn->state = STATE_CONNECTION_CLOSED;
	free(conn);
}

/*
 * Handle a new connection request on the server socket.
 */

static void handle_new_connection(void)
{
	static int sockfd;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	struct sockaddr_in addr;
	struct connection *conn;
	int rc;

	/* accept new connection */
	sockfd = accept(listenfd, (SSA *) &addr, &addrlen);
	DIE(sockfd < 0, "accept");

	dlog(LOG_ERR, "Accepted connection from: %s:%d\n",
		inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    
    fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);

	/* instantiate new connection handler */
	conn = connection_create(sockfd);
    memset(conn->path, 0, BUFSIZ);
    conn->fd = -1;

	/* add socket to epoll */
	rc = w_epoll_add_ptr_in(epollfd, sockfd, conn);
	DIE(rc < 0, "w_epoll_add_in");
}

/*
 * Receive message on socket.
 * Store message in recv_buffer in struct connection.
 */

static enum connection_state receive_message(struct connection *conn)
{
	ssize_t bytes_recv;
	int rc;
	char abuffer[64];

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		goto remove_connection;
	}

	bytes_recv = recv(conn->sockfd, conn->recv_buffer + conn->recv_len, BUFSIZ, 0);
	if (bytes_recv < 0) {		/* error in communication */
		dlog(LOG_ERR, "Error in communication from: %s\n", abuffer);
		goto remove_connection;
	}
	if (bytes_recv == 0) {		/* connection closed */
		dlog(LOG_INFO, "Connection closed to %s\n", abuffer);
		goto remove_connection;
	}

    conn->recv_len += bytes_recv;


	dlog(LOG_DEBUG, "Received message from: %s\n", abuffer);
	printf("--\n%s--\n", conn->recv_buffer);

	conn->state = STATE_DATA_RECEIVED;

	return STATE_DATA_RECEIVED;

remove_connection:
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	/* remove current connection */
	connection_remove(conn);

	return STATE_CONNECTION_CLOSED;
}

static int get_filesize(int fd) {
	struct stat buf;
	fstat(fd, &buf);
	return buf.st_size;
}

/*
 * Send message on socket.
 * Store message in send_buffer in struct connection.
 */

static enum connection_state send_message(struct connection *conn)
{
	ssize_t bytes_sent;
	int rc;
	char abuffer[64];

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		goto remove_connection;
	}

	if (conn->send_len > 0) {
		bytes_sent = send(conn->sockfd, conn->send_buffer, conn->send_len, 0);

		if (bytes_sent < 0) {		/* error in communication */
			dlog(LOG_ERR, "Error in communication to %s\n", abuffer);
			goto remove_connection;
		}
		if (bytes_sent == 0) {		/* connection closed */
			dlog(LOG_INFO, "Connection closed to %s\n", abuffer);
			goto remove_connection;
		}

		conn->send_len -= bytes_sent;
		strncpy(conn->send_buffer, conn->send_buffer + bytes_sent, conn->send_len);


		dlog(LOG_DEBUG, "Sending message to %s\n", abuffer);

		printf("--\n%s--\n", conn->send_buffer);

		if(conn->send_len == 0) {
			dlog(LOG_DEBUG, "Sent header to %s\n", abuffer);
		}

		return 0;
	}
	
	/* Starting to send file */
	if (conn->fd != -1) {
		/* Send using sendfile - case 1 */
		if (strstr(request_path, STATIC) != NULL) {
			if (conn->file_size != 0) {
				rc = sendfile(conn->sockfd, conn->fd, NULL, conn->file_size);
				DIE(rc < 0, "sendfile");
				conn->file_size -= rc;
				return 0;
			} else {
				dlog(LOG_DEBUG, "Completed STATIC send file to %s\n", abuffer);
				goto remove_connection;
			}
		/* Send using async IO - case 2 */
		} else if (strstr(request_path, DYNAMIC) != NULL) {
			if (conn->ready_dynamic == 0 && conn->dynamic_sending == 0) {
				conn->data_buffer = malloc(conn->file_size);
				conn->efd = eventfd(0, 0);
				conn->ready_dynamic = 1;
				memset(&conn->iocb, 0, sizeof(conn->iocb));
				io_prep_pread(&conn->iocb, conn->fd, conn->data_buffer, conn->file_size, 0);
				conn->piocb = &conn->iocb;
				io_set_eventfd(&conn->iocb, conn->efd);
				rc = w_epoll_add_ptr_in(epollfd, conn->efd, conn);
				DIE(rc < 0, "w_epoll_add_ptr_in");
				rc = io_setup(1, &conn->ctx);
				DIE(rc < 0, "io_setup");
				rc = io_submit(conn->ctx, 1, &conn->piocb);
				DIE(rc < 0, "io_submit");
				return 0;
			} else if (conn->ready_dynamic == 0 && conn->dynamic_sending == 1 && conn->file_size > 0) {
				int total_sent;
				total_sent = send(conn->sockfd, conn->data_buffer, conn->file_size, 0);
				dlog(LOG_INFO, "Sending message to %s:\n%s\n", abuffer, conn->data_buffer);
				conn->file_size -= total_sent;
				conn->data_buffer += total_sent;
				return 0;
			} else if (conn->file_size == 0) {
				io_destroy(conn->ctx);
				conn->ctx = 0;
				close(conn->efd);
				conn->efd = -1;
				dlog(LOG_INFO, "Connection closed to %s\n", abuffer);
				goto remove_connection;
				
			}
		}
		
	}


	/* all done - remove out notification */
	rc = w_epoll_update_ptr_in(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_update_ptr_in");

	conn->state = STATE_DATA_SENT;

	return STATE_DATA_SENT;

remove_connection:

	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	/* remove current connection */
	connection_remove(conn);

	return STATE_CONNECTION_CLOSED;
}

/*
 * Handle a client request on a client connection.
 */

static char *get_peer(int sockfd) {
	struct sockaddr_in saddr;
	socklen_t slen;
	int rc;
	rc = getpeername(sockfd, (struct sockaddr *) &saddr, &slen);
	if(rc == -1) 
		return NULL;
	char *ret = malloc(BUFSIZ);
	sprintf(ret, "%s:%d", inet_ntoa(saddr.sin_addr), ntohs(saddr.sin_port));
	return ret;
}

static void try_open(struct connection **conn) {
	(*conn)->fd = open((*conn)->path, O_RDWR);
	memset((*conn)->send_buffer, 0, BUFSIZ);
	if ((*conn)->fd == -1) {
		sprintf((*conn)->send_buffer, ERR_MSG);
		(*conn)->send_len = strlen(ERR_MSG);
	} else {
		sprintf((*conn)->send_buffer, OK_MSG);
		(*conn)->send_len = strlen(OK_MSG);
		(*conn)->file_size = get_filesize((*conn)->fd);
	}
}

static void handle_client_request(struct connection *conn)
{
	int rc;
	enum connection_state ret_state;

	ret_state = receive_message(conn);
	if (ret_state == STATE_CONNECTION_CLOSED)
		return;


    if(strstr(conn->recv_buffer, "\r\n\r\n") == NULL) {
        return;
    }
	char *peer = get_peer(conn->sockfd);
	dlog(LOG_INFO, "Received HTTP request from %s:\n%s\n", peer, conn->recv_buffer);
	free(peer);

	/* Parse the request */
	http_parser_init(&request_parser, HTTP_REQUEST);
	int bytes_parsed = http_parser_execute(&request_parser, &settings_on_path, conn->recv_buffer, conn->recv_len);
	dlog(LOG_INFO, "Parsed HTTP request (%d bytes): \n%s\n", bytes_parsed, request_path);

	/* Open the file in the request */
	memset(conn->path, 0, BUFSIZ);
	sprintf(conn->path, "%s%s", AWS_DOCUMENT_ROOT, request_path + 1);
	try_open(&conn);

	/* add socket to epoll for out events */
	rc = w_epoll_update_ptr_inout(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_add_ptr_inout");
}

int main(void)
{
	int rc;

	/* init multiplexing */
	epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create");

	/* create server socket */
	listenfd = tcp_create_listener(AWS_LISTEN_PORT,
		DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener");

	rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc < 0, "w_epoll_add_fd_in");

	dlog(LOG_INFO, "Server waiting for connections on port %d\n",
		AWS_LISTEN_PORT);

	/* server main loop */
	while (1) {
		struct epoll_event rev;

		/* wait for events */
		rc = w_epoll_wait_infinite(epollfd, &rev);
		DIE(rc < 0, "w_epoll_wait_infinite");

		/*
		 * switch event types; consider
		 *   - new connection requests (on server socket)
		 *   - socket communication (on connection sockets)
		 */

		if (rev.data.fd == listenfd) {
			dlog(LOG_DEBUG, "New connection\n");
			if (rev.events & EPOLLIN)
				handle_new_connection();
		} else {
			struct connection *conn = rev.data.ptr;
			if(conn->ready_dynamic == 1) {
				u_int64_t efd_ops = 0;
				rc = read(conn->efd, &efd_ops, sizeof(efd_ops));
				DIE(rc < 0, "read eventfd");
				if (efd_ops != 0) {
					rc = w_epoll_remove_ptr(epollfd,
								conn->efd,
								conn);
					conn->ready_dynamic = 0;
					conn->dynamic_sending = 1;
					DIE(rc < 0, "w_epoll_remove_ptr");
				}
			} else {
				if (rev.events & EPOLLIN) {
					dlog(LOG_DEBUG, "New message\n");
					handle_client_request(rev.data.ptr);
				}
				if (rev.events & EPOLLOUT) {
					dlog(LOG_DEBUG, "Ready to send message\n");
					send_message(rev.data.ptr);
				}
			}
		}
	}

	return 0;
}