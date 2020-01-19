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
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdarg.h>
#include <time.h>
#include <assert.h>

#ifdef _WIN32
# include <io.h>
# include <winsock2.h>
# include <windows.h>
# include "win32/dirent.h"
# include "win32/w32_compat.h"
#else
#include <dirent.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#endif

#include <pthread.h>

#include "httpd.h"
#include "common.h"
#include "lisp_fs.h"
#include "lisp_socket.h"


#define MAX_HTTP_HEADER_LINE (4*1024)

static int hexval(int ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	else if (ch >='a' && ch <='f')
		return ch - 'a' + 10;
	else if (ch >='A' && ch <='F')
		return ch - 'A' + 10;
	else
		return -1;
}

static char hex_byte(char h, char l)
{
	return (char)((hexval(h)<<4) + hexval(l));
}

static char *url_parse_path(Lisp_VM *vm, char *s)
{
	char *t, *p;
	for (t = s, p = s; *p && *p != '?'; p++) {
		if (*p == '%' && isxdigit(p[1]) && isxdigit(p[2])) {
			*t++ = hex_byte(p[1], p[2]);
			p+=2;
		} else {
			*t++ = *p;
		}
	}
	if (t == s) {
		lisp_push(vm, lisp_nil);
	} else {
		PUSHX(vm, lisp_string_new(vm, s, t-s));
	}
	return p;
}

/* Url doesn't allow whitespaces, so it could be represented as `+` */
static void url_parse_query_string(Lisp_VM *vm, char *s, int url_encoded)
{
	char *t, *q;
	int n = 0;

	for (t = s, q = s; *q; q++) {
		if (*q == '%' && isxdigit(q[1]) && isxdigit(q[2])) {
			*t++ = hex_byte(q[1], q[2]);
			q+=2;
		} else if (*q == '+' && url_encoded) {
			*t++ = ' ';
		} else if (*q == '=') {
			*t = 0;
			lisp_make_symbol(vm, s);
			s = q+1;
			t = s;
			n++;
		} else if (*q == '&') {
			if (n & 1) {
				lisp_push_string(vm, s, t-s);
				lisp_cons(vm);
				s = q + 1;
				t = s;
				n++;
			}
		} else {
			*t++ = *q;
		}
	}
	if (n & 1) {
		lisp_push_string(vm, s, t-s);
		lisp_cons(vm);
		n++;
	}
	assert(n % 2 == 0);
	lisp_make_list(vm, n/2);
}



static void parse_method(Lisp_VM *vm, Lisp_Buffer *line)
{
	// method line
	char *s = (char*)lisp_buffer_bytes(line);
	int j = 0;
	int k = (int)lisp_buffer_size(line);
	while (j < k && !isspace(s[j]))
		j++;

	// METHOD field
	if (j < k) {
		s[j++] = 0;
		lisp_make_symbol(vm, s);
	}

	// URL
	while (j < k && isspace(s[j]))
		j++;

	char *url = s + j;
	while (j < k && !isspace(s[j]))
		j++;

	if (j < k) {
		s[j++] = 0;
		lisp_make_symbol(vm, "url");
		lisp_push_cstr(vm, url);
		lisp_cons(vm);
		
		lisp_make_symbol(vm, "path");
		char *t = url_parse_path(vm, url);
		lisp_cons(vm);
		
		if (*t == '?') {
			lisp_make_symbol(vm, "query");
			url_parse_query_string(vm, t+1, 1);
			lisp_cons(vm);
		}
	}

	// VERSION
	while (j < k && isspace(s[j]))
		j++;

	int v = j;
	while (j < k && !isspace(s[j]))
		j++;

	if (j > v) {
		lisp_make_symbol(vm, "version");
		lisp_push_string(vm, s+v, j-v);
		lisp_cons(vm);
	}
}

static void parse_header(Lisp_VM *vm, Lisp_Buffer *line)
{
	char *s = (char*)lisp_buffer_bytes(line);
	int j = 0;
	int k = (int)lisp_buffer_size(line);
	while (j < k && s[j] != ':')
		j++;
	
	if (j < k && s[j] == ':') {
		s[j++] = 0;
		lisp_make_symbol(vm, s);
	} else {
		lisp_err(vm, "http read: bad header field");
	}
	
	while (j < k && isspace(s[j]))
		j++;

	int v = j;
	for (j = k; isspace(s[j-1]); j--)
		;
	if (v < j) {
		lisp_push_string(vm, s+v, j-v);
		lisp_cons(vm);
	} else {
		lisp_err(vm, "http read: bad header value");
	}
}

/*
  (http-read <port>)
*/
static void op_http_read(Lisp_VM *vm, Lisp_Pair *args)
{
	Lisp_Port *port = (Lisp_Port*)CAR(args);
	size_t total_bytes = 0;
	int ln = 0;
	
	if (!lisp_input_port_p(CAR(args))) {
		lisp_err(vm, "http-read: not input port");
	}
	Lisp_Buffer *line = lisp_push_buffer(vm, NULL, 512);
	
	lisp_begin_list(vm);
	while (true) {
		// Fill input port buffer
		size_t n = lisp_port_fill(port);
		if (n == 0) {
			if (total_bytes == 0)
				goto Done;
			lisp_err(vm,"http-read: incomplete request");
		}
		uint8_t *p = (uint8_t*)lisp_port_pending_bytes(port);
		unsigned i = 0;
		for (i = 0; i < n; i++) {
			if (p[i] == '\n') {
				if (lisp_buffer_size(line) == 0) {
					if (ln == 0) {
						lisp_err(vm, "http-read: missing first line");
					}
					lisp_make_list(vm, ln-1);
					lisp_make_symbol(vm, "headers");
					lisp_exch(vm);
					lisp_cons(vm);
					total_bytes += i+1;
					lisp_port_drain(port, i+1);
					goto Done;
				}
				if (ln == 0) {
					parse_method(vm, line);
				} else {
					parse_header(vm, line);
				}
				ln++;
				lisp_buffer_set_size(line, 0);
			} else if (p[i] != '\r'){
				lisp_buffer_add_byte(line, p[i]);
			}
		}
		
		if (lisp_buffer_size(line) > MAX_HTTP_HEADER_LINE)
			lisp_err(vm, "Header line too big");
		
		total_bytes += n;
		lisp_port_drain(port, n);
	}

Done:
	lisp_end_list(vm);
	if (total_bytes == 0) {
		lisp_pop(vm, 2);
		lisp_push(vm, lisp_false);
	} else {
		lisp_push(vm, lisp_pop(vm, 2));
	}
}

/* on entering, offset should be zero, and size should be total size */
static int parse_range(const char *range, off_t *offset, size_t *size)
{
	long a = 0, b = 0;
	assert(*offset == 0);
	if (strncmp(range, "bytes=", 6) == 0) {
		const char *p = range + 6;
		char *endp = NULL;
		if (*p == '-') {
			b = strtol(p+1, &endp, 10);
			if (*endp != 0) return 0;
			a = (long)(*size) - b;
			b = a + b - 1;
		} else {
			a = strtol(p, &endp, 10);
			if (*endp == '-') {
				if (endp[1] == 0) {
					if (*size > 0) b = *size - 1;
				} else {
					p = endp+1;
					b = strtol(p, &endp, 10);
					if (*endp != 0) return 0;
				}
			} else {
				return 0;
			}
		}
	}
	
	if (a >= 0 && b >= a && *size > (size_t)b) {
		*offset = a;
		*size = b - a + 1;
		return 1;
	}
	return 0;
}

static void op_http_parse_range(Lisp_VM *vm, Lisp_Pair *args)
{
	const char *range = lisp_safe_cstring(vm, CAR(args));
	off_t offset = 0;
	size_t size = lisp_safe_int(vm, CADR(args));
	if (parse_range(range, &offset, &size)) {
		lisp_push_number(vm, (double)offset);
		lisp_push_number(vm, (double)size);
		lisp_cons(vm);
	} else {
		lisp_push(vm, lisp_false);
	}
}

static int read_byte(Lisp_VM *vm, Lisp_Port *port)
{
	int b = lisp_port_getc(port);
	if (b == EOF)
		lisp_err(vm, "websocket-read: missing bytes");
	return b;
}

#define WEBSOCKET_OP_CONTINUATION 0
#define WEBSOCKET_OP_TEXT 1
#define WEBSOCKET_OP_BINARY 2
#define WEBSOCKET_OP_CLOSE 8
#define WEBSOCKET_OP_PING 9
#define WEBSOCKET_OP_PONG 10

static const char *websocket_op_table[16] = {
	[ 0] = "continuation",
	[ 1] = "text",
	[ 2] = "binary",
	[ 8] = "close",
	[ 9] = "ping",
	[10] = "pong"
};

/* (websocket-read <port>)
 * return a websocket frame.
 *
 */
static void op_websocket_read(Lisp_VM *vm, Lisp_Pair *args)
{
	if (!lisp_input_port_p(CAR(args))) {
		lisp_err(vm, "http-read: not input port");
	}
	Lisp_Port *port = (Lisp_Port*)CAR(args);
	int b0 = lisp_port_getc(port);
	if (b0 == EOF)
	{
		lisp_push(vm, lisp_false);
		return;
	}
	int b1 = read_byte(vm, port);
	int fin = !!(b0 & 0x80);
	int opcode = (b0 & 0xf);
	int mask = !!(b1 & 0x80);
	uint8_t masking_key[4];
	uint64_t payload_len = (b1 & 0x7f);
	
	if (!mask && payload_len > 0) {
		lisp_err(vm, "websocket-read: frame not masked");
	}
	
	if (payload_len == 126) {
		payload_len = (
			((uint64_t)read_byte(vm, port) << 8) |
			((uint64_t)read_byte(vm, port) << 0)
		);
	} else if (payload_len == 127) {
		payload_len = (
			((uint64_t)read_byte(vm, port) <<56) |
			((uint64_t)read_byte(vm, port) <<48) |
			((uint64_t)read_byte(vm, port) <<40) |
			((uint64_t)read_byte(vm, port) <<32) |
			((uint64_t)read_byte(vm, port) <<24) |
			((uint64_t)read_byte(vm, port) <<16) |
			((uint64_t)read_byte(vm, port) << 8) |
			((uint64_t)read_byte(vm, port) << 0)
		);
	}
	
	if (payload_len >= 16*1024*1024)
		lisp_err(vm, "websocket-read: payload too large, maximum=16MiB");
	
	if (mask) {
		masking_key[0] = read_byte(vm, port);
		masking_key[1] = read_byte(vm, port);
		masking_key[2] = read_byte(vm, port);
		masking_key[3] = read_byte(vm, port);
	}
	
	lisp_begin_list(vm);

	if (websocket_op_table[opcode]) {
		lisp_make_symbol(vm, websocket_op_table[opcode]);
	} else {
		lisp_push_number(vm, opcode);
	}

	Lisp_Buffer *b = lisp_push_buffer(vm, NULL, (size_t)payload_len);
	int n = 0;
	while (n < payload_len)
	{
		size_t x = lisp_port_fill(port);
		if (x == 0)
			lisp_err(vm, "bad frame: missing payload");
		x = MIN(x, (size_t)(payload_len-n));
		lisp_buffer_add_bytes(b, lisp_port_pending_bytes(port), x);
		// Remove added bytes from input buffer
		lisp_port_drain(port, x);
		n += x;
	}
	assert(lisp_buffer_size(b) == payload_len);

	if (mask) {
		uint8_t *p = lisp_buffer_bytes(b);
		for (int i = 0; i < payload_len; i++) {
			p[i] ^= masking_key[i%4];
		}
	}
	
	if (opcode == WEBSOCKET_OP_TEXT) {
		lisp_push_string_from_buffer(vm, b);
		lisp_exch(vm);
		lisp_pop(vm, 1);
	}
	
	if (fin) {
		lisp_make_symbol(vm, "fin");
		lisp_push(vm, lisp_true);
		lisp_cons(vm);
	}
	
	lisp_end_list(vm);
}

/* (websocket-write <data> <port>) */
static void op_websocket_write(Lisp_VM *vm, Lisp_Pair *args)
{
	Lisp_Object *o = CAR(args);
	Lisp_Port *output = (Lisp_Port*)CADR(args);
	int opcode = -1;
	const uint8_t *payload = NULL;
	size_t len = 0;
	int fin = 1;
	
	if (!lisp_output_port_p((Lisp_Object*)output)) {
		lisp_err(vm, "not output port");
	}
	
	if (lisp_buffer_p(o)) {
		opcode = WEBSOCKET_OP_BINARY;
		payload = lisp_buffer_bytes((Lisp_Buffer*)o);
		len = lisp_buffer_size((Lisp_Buffer*)o);
	} else if (lisp_string_p(o)) {
		opcode = WEBSOCKET_OP_TEXT;
		payload = (const uint8_t*)lisp_string_cstr((Lisp_String*)o);
		len = lisp_string_length((Lisp_String*)o);
	} else if (lisp_symbol_p(o)) {
		const char* s = lisp_string_cstr((Lisp_String*)o);
		if (strcmp(s, "ping") == 0) {
			opcode = WEBSOCKET_OP_PING;
		} else if (strcmp(s, "close") == 0) {
			opcode = WEBSOCKET_OP_CLOSE;
			fin = 1;
		} else if (strcmp(s, "pong") == 0) {
			opcode = WEBSOCKET_OP_PONG;
		} else {
			lisp_err(vm, "unrecognized op: %s", s);
		}
	}
	
	if (len >= 16*1024*1024) {
		lisp_err(vm, "payload too large");
	}
	
	lisp_port_putc(output, (fin << 7) | opcode);
	if (len < 126) {
		lisp_port_putc(output, (int)len);
	} else if (len < 65535) {
		lisp_port_putc(output, 126);
		lisp_port_putc(output, (len >> 8) & 0xff);
		lisp_port_putc(output, (len >> 0) & 0xff);
	} else {
		lisp_port_putc(output, 127);
		lisp_port_putc(output, 0);
		lisp_port_putc(output, 0);
		lisp_port_putc(output, 0);
		lisp_port_putc(output, 0);
		lisp_port_putc(output, (len >>24) & 0xff);
		lisp_port_putc(output, (len >>16) & 0xff);
		lisp_port_putc(output, (len >> 8) & 0xff);
		lisp_port_putc(output, (len >> 0) & 0xff);
	}
	
	if (len > 0) {
		lisp_port_put_bytes(output, payload, len);
	}
	lisp_push(vm, lisp_undef);
}

bool lisp_http_init(Lisp_VM *vm)
{
	lisp_defn(vm, "http-read", op_http_read);
	lisp_defn(vm, "websocket-read", op_websocket_read);
	lisp_defn(vm, "websocket-write", op_websocket_write);
	lisp_defn(vm, "http-parse-range", op_http_parse_range);
	return true;
}


