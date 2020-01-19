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
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <zlib.h>

#include "lisp_zstream.h"
#include "common.h"

#define ZBUF_SIZE (1024*128)

enum {
	ZSTREAM_NONE,
	ZSTREAM_COMPRESS,
	ZSTREAM_UNCOMPRESS,
	ZSTREAM_ENDED
};

struct zstream_context {
	Lisp_VM *vm;
	z_stream zs;
	Lisp_Buffer *zbuf; /* Holding source data */
	Lisp_Port *source;
	int mode;
};

static size_t zstream_read(void *stream, void *buffer, size_t cap)
{
	int zerr = Z_OK;
	struct zstream_context *zctx = stream;
	z_stream *zs = &zctx->zs;
	size_t in_size = 0;
	
	if (zctx->mode == ZSTREAM_NONE || zctx->mode == ZSTREAM_ENDED)
		return 0;

	zs->next_out = buffer;
	zs->avail_out = (int)cap;
	
	do {
		// Make sure zbuf has data
		in_size = lisp_port_fill(zctx->source);
		zs->next_in = lisp_port_pending_bytes(zctx->source);
		zs->avail_in = (int)in_size;
		if (zctx->mode == ZSTREAM_COMPRESS) {
			zerr = deflate(zs, in_size == 0 ? Z_FINISH : Z_NO_FLUSH);
			assert(zerr != Z_STREAM_ERROR);
		} else if (zctx->mode == ZSTREAM_UNCOMPRESS) {
			zerr = inflate(zs, Z_NO_FLUSH);
			switch (zerr) {
			case Z_STREAM_END:
			case Z_OK:
			case Z_BUF_ERROR:
				break;
			default:assert(0);break;
			}
		} else {
			assert(0);
		}
		lisp_port_drain(zctx->source, in_size - zs->avail_in);
	} while (zs->avail_out != 0 && in_size > 0 && zerr != Z_STREAM_END);
	return cap - zs->avail_out;
}

static void zstream_mark(void *stream)
{
	struct zstream_context *zctx = stream;
	if (zctx->source) {
		lisp_mark((Lisp_Object*)zctx->source);
	}
	if (zctx->zbuf) {
		lisp_mark((Lisp_Object*)zctx->zbuf);
	}
}

static void zstream_close(void *stream)
{
	struct zstream_context *zctx = stream;
	z_stream *zs = &zctx->zs;
	
	if (zctx->mode == ZSTREAM_UNCOMPRESS) {
		inflateEnd(zs);
	} else if (zctx->mode == ZSTREAM_COMPRESS) {
		deflateEnd(zs);
	}
	zctx->mode = ZSTREAM_NONE;
}

static struct lisp_stream_class_t zstream_class = {
	.context_size = sizeof(struct zstream_context),
	.read = zstream_read,
	.close = zstream_close,
	.mark = zstream_mark
};


/* (open-deflate <source-port>) */
static void op_open_deflate(Lisp_VM*vm, Lisp_Pair* args)
{
	if (!lisp_input_port_p(CAR(args)))
		lisp_err(vm, "deflate: source not input port");
	lisp_push_buffer(vm, NULL, 512);
	Lisp_Stream *zstream = lisp_push_stream(vm, &zstream_class, NULL);
	struct zstream_context *zctx = lisp_stream_context(zstream);
	int zerr = deflateInit(&zctx->zs, Z_DEFAULT_COMPRESSION);
	if (zerr != Z_OK) {
		lisp_err(vm, "deflateInit error: %d", zerr);
	}
	zctx->vm = vm;
	zctx->mode = ZSTREAM_COMPRESS;
	zctx->source = (Lisp_Port*)CAR(args);
	lisp_make_input_port(vm);
}

/* (open-inflate <source-port>) */
static void op_open_inflate(Lisp_VM*vm, Lisp_Pair* args)
{
	if (!lisp_input_port_p(CAR(args)))
		lisp_err(vm, "inflate: not input port");
	lisp_push_buffer(vm, NULL, 512);
	Lisp_Stream *zstream = lisp_push_stream(vm, &zstream_class, NULL);
	struct zstream_context *zctx = lisp_stream_context(zstream);
	int zerr = inflateInit(&zctx->zs);
	if (zerr != Z_OK) {
		lisp_err(vm, "inflateInit error: %d", zerr);
	}
	zctx->vm = vm;
	zctx->mode = ZSTREAM_UNCOMPRESS;
	zctx->source = (Lisp_Port*)CAR(args);
	lisp_make_input_port(vm);
}

void lisp_zstream_init(Lisp_VM* vm)
{
	lisp_defn(vm, "open-deflate", op_open_deflate);
	lisp_defn(vm, "open-inflate", op_open_inflate);
}

#if 0
int main(int argc, char *argv[])
{
	bool unz = false;
	z_stream zs;
	int zerr;
	unsigned char inbuf[512];
	unsigned char outbuf[512];
	size_t n;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--unzip")==0)
			unz = true;
	}
	
	if (unz) {
		zerr = inflateInit(&zs);
		while ((n = fread(inbuf, 1, sizeof(inbuf), stdin))) {
			zs.next_in = inbuf;
			zs.avail_in = n;
			while (true) {
				zs.next_out = outbuf;
				zs.avail_out = sizeof(outbuf);
				zerr = inflate(&zs, Z_NO_FLUSH);
				fwrite(outbuf, 1, sizeof(outbuf)-zs.avail_out, stdout);
				if (zerr == Z_STREAM_END) {
					break;
				} else if (zerr == Z_BUF_ERROR) {
					if (zs.avail_out == 0)
						continue;
					if (zs.avail_in == 0)
						break;
				} else if (zerr != Z_OK) {
					goto Error;
				}
			}
		}
	} else {
		zerr = deflateInit(&zs, Z_DEFAULT_COMPRESSION);
		while ((n = fread(inbuf, 1, sizeof(inbuf), stdin))) {
			zs.next_in = inbuf;
			zs.avail_in = n;
			int flush_flag = Z_NO_FLUSH;
			while (zs.avail_in > 0) {
				zs.next_out = outbuf;
				zs.avail_out = sizeof(outbuf);
				zerr = deflate(&zs, Z_NO_FLUSH);
				fwrite(outbuf, 1, sizeof(outbuf)-zs.avail_out, stdout);
				if (zerr == Z_BUF_ERROR) {
					assert(zs.avail_out==0);
				} else if (zerr != Z_OK) {
					goto Error;
				}
			}
		}
		
		while (true) {
			zs.next_out = outbuf;
			zs.avail_out = sizeof(outbuf);
			zerr = deflate(&zs, Z_FINISH);
			fwrite(outbuf, 1, sizeof(outbuf)-zs.avail_out, stdout);
			if (zerr == Z_STREAM_END) {
				break;
			} else if (zerr == Z_BUF_ERROR) {
				assert(zs.avail_out==0);
			} else if (zerr != Z_OK) {
				goto Error;
			}
		}
	}
	zerr = Z_OK;
	
Error:
	if (unz) {
		inflateEnd(&zs);
	} else {
		deflateEnd(&zs);
	}
	
	if (zerr != Z_OK) {
		fprintf(stderr, "%s error: %d\n", unz?"unzip":"zip", zerr);
		return -1;
	}
	
	return 0;
}
#endif
