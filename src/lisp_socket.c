/*    
 * Copyright (C) 2020, Twinkle Labs, LLC.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <fcntl.h>

#ifdef _WIN32
# include <io.h>
# include <winsock2.h>
# include <windows.h>
# include <Ws2tcpip.h>
# include <iphlpapi.h>
# include "win32/w32_compat.h"
# define close(x) closesocket(x)
#else
#include <netinet/in.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>

# include <ifaddrs.h>
# include <sys/types.h>
# include <sys/socket.h>
# include <net/if.h>
# include <netinet/in.h>
# include <arpa/inet.h>
# include <netdb.h>

#endif
#include <openssl/evp.h>

#include "lisp_socket.h"
#include "common.h"
#include "twk-internal.h"
#include "lisp_crypto.h"

#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 46
#endif

#define SOCKADDR_LEN(sa) ((sa)->sa_family == AF_INET ? \
	sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6))

#define UDP_BUFFER_SIZE 4096
#define SECURE_SOCKET_BUFFER_SIZE 4096

struct socket_stream {
	int sockfd;
	struct timeval timeout;
	EVP_CIPHER_CTX *ctx;
	uint8_t *buf;
};

/* ----------------------------------------- */

static void sa2str(struct sockaddr* sa,  char *str, size_t size, int *port)
{
	assert(size >= INET6_ADDRSTRLEN);
	if (sa->sa_family == AF_INET) {
		struct sockaddr_in *sin = (struct sockaddr_in*)sa;
		inet_ntop(AF_INET, &sin->sin_addr, str, (uint32_t)size);
		if (port)
			*port = ntohs(sin->sin_port);
	} else if (sa->sa_family == AF_INET6) {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6*)sa;
		inet_ntop(AF_INET6, &sin6->sin6_addr, str, (uint32_t)size);
		if (port)
			*port = ntohs(sin6->sin6_port);
	} else {
		assert(0);
	}
}

static void ss_decode(struct sockaddr_storage* ss,  char *str, size_t size, int *port)
{
	assert(size >= INET6_ADDRSTRLEN);
	if (ss->ss_family == AF_INET) {
		struct sockaddr_in *sa = (struct sockaddr_in*)ss;
		inet_ntop(AF_INET, &sa->sin_addr, str, (uint32_t)size);
		*port = ntohs(sa->sin_port);
	} else if (ss->ss_family == AF_INET6) {
		struct sockaddr_in6 *sa6 = (struct sockaddr_in6*)ss;
		inet_ntop(AF_INET6, &sa6->sin6_addr, str, (uint32_t)size);
		*port = ntohs(sa6->sin6_port);
	} else {
		assert(0);
	}
}

static void ss_set_port(struct sockaddr_storage *ss, uint16_t port)
{
	if (ss->ss_family == AF_INET) {
		struct sockaddr_in *sa = (struct sockaddr_in*)ss;
		sa->sin_port = htons(port);
	} else if (ss->ss_family == AF_INET6) {
		struct sockaddr_in6 *sa6 = (struct sockaddr_in6*)ss;
		sa6->sin6_port = htons(port);
	} else {
		assert(0);
	}
}

static bool ss_init(struct sockaddr_storage* ss, const char *ip, uint16_t port)
{
	struct sockaddr_in *sa = (struct sockaddr_in*)ss;
	struct sockaddr_in6 *sa6 = (struct sockaddr_in6*)ss;
	memset(ss, 0, sizeof(struct sockaddr_storage));
	if (inet_pton(AF_INET, ip, &sa->sin_addr))
	{
		sa->sin_family = AF_INET;
		sa->sin_port = htons(port);
		#if !(defined(_WIN32) || defined(__linux))
		sa->sin_len = sizeof(struct sockaddr_in);
		#endif
	}
	else if (inet_pton(AF_INET6, ip, &sa6->sin6_addr))
	{
		sa6->sin6_family = AF_INET6;
		sa6->sin6_port = htons(port);
		#if !(defined(_WIN32) || defined(__linux))
		sa6->sin6_len = sizeof(struct sockaddr_in6);
		#endif
	}
	else
	{
		return false;
	}
	return true;
}

/* --------------------------------------------------------
 * Socket Input & Output
 * --------------------------------------------------------
 */

static size_t socket_read(void *context, void *buf, size_t size)
{
	struct socket_stream *stream = context;
	fd_set rset;
	int sockfd = stream->sockfd;
	int maxfd = sockfd + 1;
	int n = 0;
	struct timeval timeout;
	FD_ZERO(&rset);
	FD_SET(sockfd, &rset);
	memcpy(&timeout, &stream->timeout, sizeof(timeout));
	select(maxfd, &rset, NULL, NULL, &timeout);
	if (FD_ISSET(sockfd, &rset)) {
		n = (int)recv(sockfd, buf, size, 0);
		//fprintf(stderr, "[%d]", n);
		if (n < 0)
			n = 0;
	}
	return n;
}

static size_t socket_write(void *context, const void *buf, size_t size)
{
	struct socket_stream *stream = context;
#if 0
	fprintf(stderr, "socket writer: %d: %d bytes\n", stream->sockfd, (int)size);
	if (size < 32) {
		const char *p = buf;
		for (int i = 0; i < size; i++) {
			if (isprint(p[i])) {
				printf("%c", p[i]);
			} else {
				printf("[%02x]", p[i]);
			}
		}
		printf("\n");
	}
#endif
	int n = (int)send(stream->sockfd, buf, size, MSG_NOSIGNAL);
	if (n < 0)
		n = 0;
	return n;
}

static bool socket_ready(void *context, int mode)
{
    struct socket_stream *stream = context;
    fd_set fs;
    int sockfd = stream->sockfd;
    int maxfd = sockfd + 1;
    struct timeval tv;
	
	if (sockfd < 0)
		return false;
	
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    FD_ZERO(&fs);
    FD_SET(sockfd, &fs);
    
    if (mode == 0) // read
        select(maxfd, &fs, NULL, NULL, &tv);
    else // write
        select(maxfd, NULL, &fs, NULL, &tv);
    if (FD_ISSET(sockfd, &fs))
        return true;
    return false;
}

struct lisp_stream_class_t socket_stream_class = {
	.context_size = sizeof(struct socket_stream),
	.read = socket_read,
	.write = socket_write,
    .ready = socket_ready
};

Lisp_Port* lisp_open_socket_output(Lisp_VM *vm, int sockfd, int timeout)
{
	lisp_push_buffer(vm, NULL, 512);
	Lisp_Stream *stream = lisp_push_stream(vm, &socket_stream_class, NULL);
	struct socket_stream *t = lisp_stream_context(stream);
	t->sockfd = sockfd;
	t->timeout.tv_sec = timeout;
	return lisp_make_output_port(vm);
}

Lisp_Port* lisp_open_socket_input(Lisp_VM *vm, int sockfd, int timeout)
{
	lisp_push_buffer(vm, NULL, 4096);
	Lisp_Stream *stream = lisp_push_stream(vm, &socket_stream_class, NULL);
	struct socket_stream *t = lisp_stream_context(stream);
	t->sockfd = sockfd;
	t->timeout.tv_sec = timeout;
	return lisp_make_input_port(vm);
}

/* --------------------------------------------------------
 * Secure Socket Input & Output
 * --------------------------------------------------------
 */
static size_t secure_socket_read(void *context, void *buf, size_t size)
{
	struct socket_stream *stream = context;
	
	if (stream->ctx == NULL)
		return socket_read(context, buf, size);

	if (size > SECURE_SOCKET_BUFFER_SIZE)
		size = SECURE_SOCKET_BUFFER_SIZE;
	
	int nread = (int)socket_read(context, stream->buf, size);
	if (nread == 0) return 0;
	
	int outlen = 0;
	EVP_DecryptUpdate(stream->ctx, buf, &outlen, stream->buf, nread);
	assert(outlen == nread);

	return nread;
}

static size_t secure_socket_write(void *context, const void *buf, size_t size)
{
	struct socket_stream *stream = context;
	assert(context != NULL);
	if (stream->ctx == NULL)
		return socket_write(context, buf, size);
	
	for (int i = 0; (unsigned)i < size; i+= SECURE_SOCKET_BUFFER_SIZE) {
		int outlen=0;
		int datalen = (int)(size - (unsigned)i);
		if (datalen > SECURE_SOCKET_BUFFER_SIZE)
			datalen = SECURE_SOCKET_BUFFER_SIZE;
		EVP_EncryptUpdate(stream->ctx,
			stream->buf,
			&outlen,
			(uint8_t*)buf+i,
			datalen);
		assert(outlen == datalen);
		int nwritten = (int)socket_write(context, stream->buf, outlen);
		if (nwritten == 0)
			return 0; // error
	}
	return size;
}

static void secure_socket_close(void *context)
{
	struct socket_stream *stream = context;
	assert(context != NULL);

	if (stream->ctx) {
		EVP_CIPHER_CTX_free(stream->ctx);
		stream->ctx = NULL;
	}
	if (stream->buf) {
		free(stream->buf);
		stream->buf = NULL;
	}
}

struct lisp_stream_class_t secure_socket_stream_class = {
	.context_size = sizeof(struct socket_stream),
	.read = secure_socket_read,
	.write = secure_socket_write,
	.close = secure_socket_close,
    .ready = socket_ready
};

static Lisp_Port* safe_port(Lisp_VM *vm, Lisp_Object *o)
{
	if (lisp_output_port_p(o) || lisp_input_port_p(o))
		return (Lisp_Port*)o;
	lisp_err(vm, "Not secure port");
	return NULL;
}

static struct socket_stream *get_socket_stream(Lisp_Port* port)
{
	Lisp_Stream *s = lisp_port_get_stream(port);
	if (!s)
		return NULL;
	return lisp_stream_context(s);
}

/* (set-stream-cipher <port> <type> <key> <iv>)
 * key and salt are buffers of length >= 8.
 */
static void op_set_stream_cipher(Lisp_VM *vm, Lisp_Pair *args)
{
	Lisp_Port *p = safe_port(vm, CAR(args));
	Lisp_Stream *stream = lisp_port_get_stream(p);
	if (!stream)
		lisp_err(vm, "No stream");
	struct socket_stream *s = lisp_stream_context(stream);
	if (!s)
		lisp_err(vm, "No context");
	assert(s != NULL);
	size_t keylen=0;
	size_t ivlen=0;
	const char *cipher_name = lisp_safe_csymbol(vm, CADR(args));
	const EVP_CIPHER *cipher = NULL;
	if (strcmp(cipher_name, "aes-256-cfb8") == 0) {
		cipher = EVP_aes_256_cfb8();
	} else {
		lisp_err(vm, "Unsupported stream cipher");
	}
	Lisp_Pair *params = (Lisp_Pair*)CDDR(args);
	uint8_t *key = lisp_safe_bytes(vm, CAR(params), &keylen);
	uint8_t *iv = lisp_safe_bytes(vm, CADR(params), &ivlen);
	if (keylen < 32 || ivlen < 32)
		lisp_err(vm, "Bad key or salt");
	if (lisp_input_port_p(CAR(args))) {
		s->ctx = EVP_CIPHER_CTX_new();
		assert(s->ctx);
		EVP_DecryptInit_ex(s->ctx, cipher, NULL, key, iv);
		s->buf = calloc(1, SECURE_SOCKET_BUFFER_SIZE);
		lisp_stream_set_class(stream, &secure_socket_stream_class);
		assert(s->buf);
	} else if (lisp_output_port_p(CAR(args))) {
		struct socket_stream *s = get_socket_stream(p);
		s->ctx = EVP_CIPHER_CTX_new();
		assert(s->ctx);
		EVP_EncryptInit_ex(s->ctx, cipher, NULL, key, iv);
		s->buf = calloc(1, SECURE_SOCKET_BUFFER_SIZE);
		assert(s->buf);
		lisp_stream_set_class(stream, &secure_socket_stream_class);
	} else {
		lisp_err(vm, "Not port");
	}
	lisp_push(vm, lisp_true);
}

/* --------------------------------------------------------
 * TCP Server
 * --------------------------------------------------------
 */

int open_tcp_server_socket(uint32_t addr, uint16_t port)
{
	int sockfd, val;
	struct sockaddr_in sa;

	/* Initialize socket address */
	memset((void*) &sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(addr);
	sa.sin_port = htons(port);
	#if !(defined(_WIN32) || defined(__linux))
	sa.sin_len = sizeof(sa);
	#endif

	sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sockfd < 0)
	{
		perror("open_server_socket: socket()");
		return -1;
	}
	
	/* Allow us to reuse address after restart */
	val = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (void*)&val, sizeof(int)) < 0)
    {
        perror("open_server_socket: setsockopt(SO_REUSEADDR)");
        close(sockfd);
        return -1;
    }
	
    /* No SIGPIPE on select() */
    val = 1;
#ifdef SO_NOSIGPIPE
    if (setsockopt(sockfd, SOL_SOCKET, SO_NOSIGPIPE, (void*)&val, sizeof(int)) < 0)

    {
        perror("open_server_socket: setsockopt(SO_NOSIGPIPE)");
        close(sockfd);
        return -1;
    }
#endif
	
	if (bind(sockfd, (struct sockaddr *) &sa, sizeof(sa)) < 0)
	{
		fprintf(stderr, "open_server_socket: bind() failed at %d: %s\n",
			port, strerror(errno));
		perror("open_server_socket: bind()");
		close(sockfd);
		return -1;
	}
	
	/*
	 * Use backlog=32. When pending queue size
	 * reaches this number, new incoming connection will
	 * be refused.
	 */
	if (listen(sockfd, 32) < 0)
	{
		perror("open_server_socket: listen()");
		close(sockfd);
		return -1;
	}
	
	return sockfd;
}

static int open_tcp_server(struct sockaddr *sa)
{
	int sockfd, val;

	//struct sockaddr_in in;
	//memcpy(&in, sa, sizeof(struct sockaddr_in));
	
	if (sa->sa_family != PF_INET && sa->sa_family != PF_INET6)
	{
		fprintf(stderr, "open_tcp_server: bad address family");
		return -1;
	}
	
	sockfd = socket(sa->sa_family, SOCK_STREAM, IPPROTO_TCP);
	if (sockfd < 0)
	{
		perror("open_tcp_server: socket()");
		return -1;
	}
#ifdef WIN32
	/* Windows default behavior is dangerous.
	 * We need to own the port exclusively.
	 * Ref: [MSDN] Socket Security: Using SO_REUSEADDR and SO_EXCLUSIVEADDRUSE
	 */
	val = 1;
	if (setsockopt(sockfd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (void*)&val, sizeof(int)) < 0)
	{
		perror("open_tcp_server: setsockopt(SO_EXCLUSIVEADDRUSE)");
		close(sockfd);
		return -1;
	}
#else
	/* Allow us to reuse address after restart */
	val = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (void*)&val, sizeof(int)) < 0)
    {
        perror("open_tcp_server: setsockopt(SO_REUSEADDR)");
        close(sockfd);
        return -1;
    }
#endif	
    /* No SIGPIPE on select() */
    val = 1;
#ifdef SO_NOSIGPIPE
    if (setsockopt(sockfd, SOL_SOCKET, SO_NOSIGPIPE, (void*)&val, sizeof(int)) < 0)

    {
        perror("open_tcp_server: setsockopt(SO_NOSIGPIPE)");
        close(sockfd);
        return -1;
    }
#endif
	int sa_len =  SOCKADDR_LEN(sa);
	if (bind(sockfd, (struct sockaddr *) sa, sa_len) < 0)
	{
		perror("open_tcp_server: bind()");
		close(sockfd);
		return -1;
	}
	
	/*
	 * Use backlog=32. When pending queue size
	 * reaches this number, new incoming connection will
	 * be refused.
	 */
	if (listen(sockfd, 32) < 0)
	{
		perror("open_server_socket: listen()");
		close(sockfd);
		return -1;
	}
	
	return sockfd;
}

int open_udp_server(struct sockaddr *sa)
{
	int sock;

	if (sa->sa_family != PF_INET && sa->sa_family != PF_INET6)
	{
		fprintf(stderr, "open_udp_server: bad address family");
		return -1;
	}
	
	sock = socket(sa->sa_family, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0)
	{
		perror("open_udp_server: socket()");
		return -1;
	}

#ifdef SO_REUSEPORT
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (void*)&(int){ 1 }, sizeof(int)) < 0)
		perror("open_udp_server: setsockopt(SO_REUSEPORT) failed");
#endif

	/* Bind to the broadcast port */
	if (bind(sock, sa, SOCKADDR_LEN(sa)) < 0)
	{
		perror("open_udp_server: bind() failed");
		close(sock);
		return -1;
	}
	
	return sock;
}


int open_udp_server_socket(uint32_t addr, uint16_t port)
{
	int sock;
	struct sockaddr_in sa;

	if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
	{
		perror("open_udp_server_socket: socket() failed");
		return -1;
	}

	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void*)&(int){ 1 }, sizeof(int)) < 0)
		perror("open_udp_server_socket: setsockopt(SO_REUSEADDR) failed");

#ifdef SO_REUSEPORT
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (void*)&(int){ 1 }, sizeof(int)) < 0)
		perror("open_udp_server_socket: setsockopt(SO_REUSEPORT) failed");
#endif

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(addr);
	sa.sin_port = htons(port);

	/* Bind to the broadcast port */
	if (bind(sock, (struct sockaddr *) &sa, sizeof(sa)) < 0)
	{
		perror("open_udp_server_socket: bind() failed");
		close(sock);
		return -1;
	}
	
	return sock;
}

static void op_open_socket_output(Lisp_VM *vm, Lisp_Pair *args)
{
	struct twk_process *proc = lisp_vm_client(vm);
	lisp_open_socket_output(vm, proc->fd, 30);
}

static void op_open_socket_input(Lisp_VM *vm, Lisp_Pair *args)
{
	struct twk_process *proc = lisp_vm_client(vm);
	int timeout = 30;
	if (args != (void*)lisp_nil)
		timeout = lisp_safe_int(vm, CAR(args));
	lisp_open_socket_input(vm, proc->fd, timeout);
}


static size_t server_socket_read(void *context, void *buf, size_t size)
{
	bool ok = false;
	struct socket_stream *stream = context;
	int sockfd = stream->sockfd;
	int n = 0;
	int val = 1;
	uint32_t clilen;
	char ip[INET6_ADDRSTRLEN];
	int port = 0;
	struct sockaddr_storage cli_addr;
	assert(size >= 256);
	clilen = sizeof(cli_addr);
	int cli_fd = accept(sockfd, (struct sockaddr *)&cli_addr, &clilen);
	if (cli_fd == -1)
	{
		#ifndef _WIN32
		if (errno == EWOULDBLOCK)
			goto Error;
		#endif
		perror("socket server: accept()");
		goto Error;
	}

#ifdef SO_NOSIGPIPE
	if (setsockopt(cli_fd, SOL_SOCKET, SO_NOSIGPIPE, (void*)&val, sizeof(int)) < 0)
	{
		perror("client socket: setsockopt(SO_NOSIGPIPE)");
		goto Error;
	}
#endif

	if (setsockopt(cli_fd, SOL_SOCKET, SO_KEEPALIVE, (void*)&val, sizeof(int)) < 0)
	{
		perror("client socket: setsockopt(SO_KEEPALIVE)");
		goto Error;
	}
	
	ss_decode(&cli_addr, ip, sizeof(ip), &port);
	n = snprintf(buf, size, "(%d \"%s\" %d)", cli_fd, ip, port);
	assert(n > 0);
	ok = true;
Error:
	if (!ok) {
		n = 2;
		memcpy(buf, "()", n);
		if (cli_fd >= 0)
			close(cli_fd);
	}
	return n;
}

struct lisp_stream_class_t server_socket_stream_class = {
	.context_size = sizeof(struct socket_stream),
	.read = server_socket_read,
    .ready = socket_ready
};

/*
(open-tcp-server <addr> <port>)
open and start a server.
set process's fd.
*/
static void op_open_tcp_server(Lisp_VM *vm, Lisp_Pair *args)
{
	struct twk_process *proc = lisp_vm_client(vm);
	struct sockaddr_storage sa;
	const char *ip = lisp_safe_cstring(vm, CAR(args));
	int port = lisp_safe_int(vm, CADR(args));

	if (ss_init(&sa, ip, port))
	{
		proc->fd = open_tcp_server((struct sockaddr*)&sa);
		if (proc->fd < 0) {
			lisp_push(vm, lisp_false);
			return;
		}
		lisp_push_buffer(vm, NULL, 256);
		Lisp_Stream *stream = lisp_push_stream(vm, &server_socket_stream_class, NULL);
		struct socket_stream *t = lisp_stream_context(stream);
		t->sockfd = proc->fd;
		lisp_make_input_port(vm);
	}
	else
	{
		lisp_err(vm, "invalid socket addr");
	}
}

static size_t udp_read_dummy(void *context, void *buf, size_t size)
{
	// Should not read at all, use fetch-datagram instead.
	assert(0);
	return 0;
}

// UDP is not a stream, but we are reusing the data structure of
// port & stream. We treat it as a input port, but actually
// you are ok to send datagrams on it.
static struct lisp_stream_class_t udp_stream_class = {
	.context_size = sizeof(struct socket_stream),
	.read = udp_read_dummy,
    .ready = socket_ready
};

/* (open-udp-server <ip> <port>) */
static void op_open_udp_server(Lisp_VM *vm, Lisp_Pair *args)
{
	struct twk_process *proc = lisp_vm_client(vm);
	struct sockaddr_storage sa;
	const char *ip = lisp_safe_cstring(vm, CAR(args));
	int port = lisp_safe_int(vm, CADR(args));

	if (ss_init(&sa, ip, port))
	{
		proc->fd = open_udp_server((struct sockaddr*)&sa);
		if (proc->fd < 0)
			lisp_err(vm, "open-udp-server: failed");
		lisp_push_buffer(vm, NULL, UDP_BUFFER_SIZE);
		Lisp_Stream *stream = lisp_push_stream(vm, &udp_stream_class, NULL);
		struct socket_stream *t = lisp_stream_context(stream);
		t->sockfd = proc->fd;
		lisp_make_input_port(vm);
	}
	else
	{
		lisp_err(vm, "open-udp-server: invalid socket addr");
	}
}

/* (open-udp-client <type>) */
static void op_open_udp_client(Lisp_VM *vm, Lisp_Pair *args)
{
	struct twk_process *proc = lisp_vm_client(vm);
	
	if (lisp_nil != (void*)args) {
		const char *type = lisp_safe_cstring(vm, CAR(args));
		if (strcmp(type, "ipv6") == 0) {
			proc->fd = socket(PF_INET6, SOCK_DGRAM, IPPROTO_UDP);
		} else {
			lisp_err(vm, "Invalid type");
		}
	} else {
		proc->fd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	}
	if (proc->fd < 0)
		lisp_err(vm, "failed to open socket");
	
	lisp_push_buffer(vm, NULL, UDP_BUFFER_SIZE);
	Lisp_Stream *stream = lisp_push_stream(vm, &udp_stream_class, NULL);
	struct socket_stream *t = lisp_stream_context(stream);
	t->sockfd = proc->fd;
	lisp_make_input_port(vm);
}

/* (fetch-datagram <udp-client|udp-server>) */
static void op_fetch_datagram(Lisp_VM *vm, Lisp_Pair* args)
{
	if (!lisp_input_port_p(CAR(args)))
		lisp_err(vm, "fetch-datagram: not input port");
	Lisp_Port *p = (Lisp_Port*)CAR(args);
	Lisp_Stream *s = lisp_port_get_stream(p);
	if (!s || lisp_stream_class(s) != &udp_stream_class)
		lisp_err(vm, "fetch-databram: bad stream");
	struct socket_stream *sstream = lisp_stream_context(s);
	struct sockaddr_storage ss;
	uint32_t sa_len = sizeof(ss);
	Lisp_Buffer *b = lisp_port_get_buffer(p);
	
	int nrecv = (int)recvfrom(
		sstream->sockfd,
		lisp_buffer_bytes(b) , lisp_buffer_cap(b),
	 	0,
	 	(struct sockaddr*) &ss,
	 	&sa_len
	);
	if (nrecv > 0) {
		char buf[64] = {0};
		int port = 0;
		lisp_push_buffer(vm, lisp_buffer_bytes(b), nrecv);
		ss_decode(&ss, buf, sizeof(buf), &port);
		lisp_push_cstr(vm, buf);
		lisp_push_number(vm, port);
		lisp_make_list(vm, 3);
	} else {
		lisp_push(vm, lisp_false);
	}
}

/* (send-datagram <udp-server|udp-client> <ip> <port> <message>) */
static void op_send_datagram(Lisp_VM *vm, Lisp_Pair* args)
{
	if (!lisp_input_port_p(CAR(args)))
		lisp_err(vm, "fetch-datagram: not input port");
	Lisp_Port *p = (Lisp_Port*)CAR(args);
	Lisp_Stream *s = lisp_port_get_stream(p);
	if (!s || lisp_stream_class(s) != &udp_stream_class)
		lisp_err(vm, "fetch-databram: bad stream");
	struct socket_stream *sstream = lisp_stream_context(s);
	struct sockaddr_storage ss;
	const char *ip = lisp_safe_cstring(vm, CADR(args));
	int port = lisp_safe_int(vm, CAR(CDDR(args)));
	Lisp_Object * m = CADR(CDDR(args));
	if (!ss_init(&ss, ip, port))
		lisp_err(vm, "Bad address");
	
	const void *ptr = NULL;
	size_t n = 0;
	if (lisp_string_p(m)) {
		ptr = lisp_string_cstr((Lisp_String*)m);
		n = lisp_string_length((Lisp_String*)m);
	} else if (lisp_buffer_p(m)) {
		ptr = lisp_buffer_bytes((Lisp_Buffer *)m);
		n = lisp_buffer_size((Lisp_Buffer *)m);
	} else {
		lisp_err(vm, "Bad message type");
	}
	struct sockaddr *sa = (struct sockaddr*)&ss;
	int nsent = (int)sendto(sstream->sockfd, ptr, n, 0, sa, SOCKADDR_LEN(sa));
	lisp_push(vm, n==nsent?lisp_true:lisp_false);
}

int tcp_connect(struct sockaddr *sa)
{
	int sockfd;
	
	sockfd = socket(sa->sa_family, SOCK_STREAM, 0);
	if (sockfd < 0)
	{
		perror("open_tcp_client_socket: socket");
		return -1;
	}

#ifdef SO_NOSIGPIPE
	int val = 1;
	if (setsockopt(sockfd, SOL_SOCKET, SO_NOSIGPIPE, (void*)&val, sizeof(int)) < 0)
	{
		perror("open_tcp_client_socket: SO_NOSIGPIPE");
		close(sockfd);
		return -1;
	}
#endif

	if (connect(sockfd, sa, SOCKADDR_LEN(sa)) < 0)
	{
		perror("open_tcp_client_socket: connect");
		close(sockfd);
		return -1;
	}
	
	return sockfd;
}

/*
 * (connect <ip> <port>)
 *
 * Connect to remote tcp server.
 * Return true if success, otherwise false.
 */
static void op_connect(Lisp_VM *vm, Lisp_Pair *args)
{
	struct twk_process *proc = lisp_vm_client(vm);
	const char *ip = lisp_safe_cstring(vm, CAR(args));
	int port = lisp_safe_int(vm, CADR(args));
	struct sockaddr_storage ss;
	
	if (proc->fd >= 0)
			lisp_err(vm, "process socket already used");
	
	if (!ss_init(&ss, ip, port))
		lisp_err(vm, "Bad address");

	proc->fd = tcp_connect((struct sockaddr*)&ss);
	if (proc->fd < 0) {
		lisp_push(vm, lisp_false);
	} else {
		lisp_push(vm, lisp_true);
	}
}

/* (set-process-socket <pid> <sockfd>)
FIXME: needs permission manauagement.
only sibling can do that.
*/
static void op_set_process_socket(Lisp_VM *vm, Lisp_Pair *args)
{
	int pid = lisp_safe_int(vm, CAR(args));
	int sockfd = lisp_safe_int(vm, CADR(args));
	struct twk_process *proc = twk_get_process(pid);
	if (proc) {
		if (proc->fd >= 0) {
			lisp_err(vm, "process %d has socket already", pid);
		}
		proc->fd = sockfd;
		lisp_push(vm, lisp_true);
	} else {
		lisp_err(vm, "Process %d not found", pid);
	}
}

// https://stackoverflow.com/questions/683624/udp-broadcast-on-all-interfaces
#ifdef _WIN32
static const char *get_broadcast_addr(char *buf, size_t size)
{
	MIB_IPADDRTABLE * ipTable = NULL;
	ULONG bufLen = 0;
	uint32_t baddr = 0;
	
	for (int i = 0; i < 5; i++)
	{
		DWORD ipRet = GetIpAddrTable(ipTable, &bufLen, false);
		if (ipRet == ERROR_INSUFFICIENT_BUFFER)
		{
			free(ipTable);  // in case we had previously allocated it
			ipTable = (MIB_IPADDRTABLE *) malloc(bufLen);
		}
		else if (ipRet == ERROR_SUCCESS)
		{
			break;
		}
		else
		{
			free(ipTable);
			ipTable = NULL;
			break;
		}
	}
	
	if (ipTable == NULL)
		return NULL;
	
	for (DWORD i=0; i < ipTable->dwNumEntries; i++)
	{
		const MIB_IPADDRROW * row = &ipTable->table[i];
		uint32_t ipAddr  = ntohl(row->dwAddr);
		uint32_t netmask = ntohl(row->dwMask);
		if (row->dwBCastAddr)
		{
			baddr   = ipAddr & netmask;
			baddr |= ~netmask;
			break;
		}
	}
	
	free(ipTable);
	
	if (baddr > 0)
	{
		struct in_addr a;
		a.s_addr = htonl(baddr);
		const char *s = inet_ntoa(a);
		if (s) {
			assert(strlen(s) < size);
			strcpy(buf, s);
			return buf;
		}
	}
	return NULL;
}
#else
static const char *get_broadcast_addr(char*buf, size_t size)
{
	const char *ip = NULL;
	struct ifaddrs *ifa, *ifaddr = NULL;;
	
	if (getifaddrs(&ifaddr) < 0) {
		perror("getifaddrs");
		return NULL;
	}
	
	/* Walk through linked list, maintaining head pointer so we
	 can free list later */
	for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
		if (ifa->ifa_flags & IFF_BROADCAST) {
			if (ifa->ifa_broadaddr) {
				if (ifa->ifa_broadaddr->sa_family == AF_INET) {
					struct sockaddr_in *addr = (struct sockaddr_in*)ifa->ifa_broadaddr;
					ip = inet_ntop(AF_INET, &addr->sin_addr, buf, (uint32_t)size);
				} else if (ifa->ifa_broadaddr->sa_family == AF_INET6) {
					struct sockaddr_in6 *addr = (struct sockaddr_in6*)ifa->ifa_broadaddr;
					ip = inet_ntop(AF_INET6, &addr->sin6_addr, buf, (uint32_t)size);
				}
				break;
			}
		}
	}
	if (ifaddr)
		freeifaddrs(ifaddr);
	return ip;
}
#endif

static void op_broadcast_address(Lisp_VM *vm, Lisp_Pair* args)
{
	char buf[64];
	if (!get_broadcast_addr(buf, sizeof(buf))) {
		lisp_push(vm, lisp_false);
	} else {
		lisp_push_cstr(vm, buf);
	}
}


static bool broadcast(struct sockaddr* sa, const char *message)
{
	int sock = -1;                         /* Socket */
	bool ok = false;
	
	/* Create socket for sending/receiving datagrams */
	if ((sock = socket(sa->sa_family==AF_INET ? PF_INET : PF_INET6, SOCK_DGRAM, IPPROTO_UDP)) < 0)
	{
		perror("socket() failed");
		goto Error;
	}
	
	/* Set socket to allow broadcast */
	int val = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (void *)&val, sizeof(val)) < 0)
	{
		perror("setsockopt() failed");
		goto Error;
	}
	
	int len = (int)strlen(message);  /* Find length of sendString */
	size_t nsent = sendto(sock, message, len, 0,
		sa, SOCKADDR_LEN(sa));
	
	if (nsent != len) {
		perror("broadcast: sendto() sent a different number of bytes than expected");
		goto Error;
	}

	ok = true;
Error:
	if (sock >= 0)
		close(sock);
	return ok;
}

// (broadcast <ip> <port> <message>)
static void op_broadcast(Lisp_VM *vm, Lisp_Pair *args)
{
	const char *ip = lisp_safe_cstring(vm, CAR(args));
	int port = lisp_safe_int(vm, CADR(args));
	const char *message = lisp_safe_cstring(vm, CAR(CDDR(args)));
	struct sockaddr_storage ss;
	ss_init(&ss, ip, port);
	if (broadcast((struct sockaddr*)&ss, message))
		lisp_push(vm, lisp_true);
	else
		lisp_push(vm, lisp_false);
}

static void extract_addrinfo(Lisp_VM *vm, void *data)
{
	struct addrinfo *ai, *res = data;
	lisp_begin_list(vm);
	for (ai = res; ai; ai = ai->ai_next)
	{
		struct sockaddr *sa = ai->ai_addr;
		char buf[INET6_ADDRSTRLEN];
		lisp_begin_list(vm);
	
		lisp_make_symbol(vm, "ip");
		sa2str(sa, buf, sizeof(buf), NULL);
		lisp_push_cstr(vm, buf);
		lisp_cons(vm);
		
		lisp_make_symbol(vm, "family");
		switch (ai->ai_family) {
			case AF_INET:
				lisp_make_symbol(vm, "inet");
				break;
			case AF_INET6:
				lisp_make_symbol(vm, "inet6");
				break;
			default:
				lisp_push(vm, lisp_undef);
				break;
		}
		lisp_cons(vm);

		lisp_make_symbol(vm, "protocol");
		switch (ai->ai_protocol) {
		case IPPROTO_TCP:
			lisp_make_symbol(vm, "tcp");
			break;
		case IPPROTO_UDP:
			lisp_make_symbol(vm, "udp");
			break;
		default:
			lisp_push(vm, lisp_undef);
			break;
		}
		lisp_cons(vm);
		
		lisp_end_list(vm);
	}
	lisp_end_list(vm);
}

static void op_getaddrinfo(Lisp_VM *vm, Lisp_Pair *args)
{
	const char *name = lisp_safe_cstring(vm, CAR(args));
	struct addrinfo hints = {0};
	struct addrinfo *res = NULL;

	hints.ai_flags = AI_CANONNAME;
	if (0 != getaddrinfo(name, NULL, &hints, &res)) {
		lisp_push(vm, lisp_nil);
		return;
	}
	
	Lisp_Object* o = lisp_try(vm, extract_addrinfo, res);
	if (res)
		freeaddrinfo(res);
	if (!o)
		lisp_err(vm, "getaddrinfo: error");
	lisp_push(vm, o);
}

bool lisp_socket_init(Lisp_VM *vm)
{
	lisp_defn(vm, "open-tcp-server", op_open_tcp_server);
	lisp_defn(vm, "open-udp-server", op_open_udp_server);
	lisp_defn(vm, "open-udp-client", op_open_udp_client);
	lisp_defn(vm, "open-socket-input", op_open_socket_input);
	lisp_defn(vm, "open-socket-output", op_open_socket_output);
	lisp_defn(vm, "set-process-socket", op_set_process_socket);
	lisp_defn(vm, "connect", op_connect);
	lisp_defn(vm, "get-broadcast-address", op_broadcast_address);
	lisp_defn(vm, "broadcast", op_broadcast);
	lisp_defn(vm, "fetch-datagram", op_fetch_datagram);
	lisp_defn(vm, "send-datagram", op_send_datagram);
	lisp_defn(vm, "getaddrinfo", op_getaddrinfo);
	lisp_defn(vm, "get-address-info", op_getaddrinfo);
	lisp_defn(vm, "set-stream-cipher", op_set_stream_cipher);
	return true;
}


