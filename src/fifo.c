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
#include "fifo.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define MAXBUFSIZE 0x7fffffff

static inline size_t grow_to_power_of_2(size_t a)
{
	size_t b = 1;
	while (b < a)
		b <<= 1;
	return b;
}

struct FIFO *fifo_new(size_t size)
{
	struct FIFO *f;

	if (size > MAXBUFSIZE)
		return NULL;

	/* If size is not already power of 2, grow it. */
	if ((size & (size - 1)) != 0) 
		size = grow_to_power_of_2(size);

	f = malloc(sizeof(struct FIFO) + size);
	if (f) 
	{
		f->size = size;
		f->wpos = 0;
		f->rpos = 0;
		f->wtotal = 0;
	}
	return f;
}

struct FIFO *fifo_copy(struct FIFO *old, size_t size)
{
	assert(old->size < size);
	struct FIFO *fifo = fifo_new(size);
	assert(fifo);
	size_t n = fifo_read(old, fifo->buffer, fifo->size);
	fifo->rpos = 0;
	fifo->wpos = (int)n;
	fifo->wtotal = old->wtotal;
	return fifo;
}

size_t fifo_write(struct FIFO *f, const void *buf, size_t size)
{
	unsigned avail = (unsigned)fifo_room(f);
	if (avail == 0) 
		return 0;
	if (size > avail)
		size = avail;

	if (f->size - f->wpos >= size) 
	{
		memcpy(f->buffer+f->wpos, buf, size);
		f->wpos += size;
		f->wpos &= f->size - 1;
	} 
	else 
	{
		unsigned size1 = (unsigned)(f->size - f->wpos);
		memcpy(f->buffer+f->wpos, buf, size1);
		memcpy(f->buffer, (char*)buf + size1, size - size1);
		f->wpos = (unsigned)size - size1;
	}
	f->wtotal += size;
	return size;
}

size_t fifo_read(struct FIFO *f, void *buf, size_t size)
{
	size_t used = fifo_bytes(f);
	if (used == 0)
		return 0;
	if (size > used)
		size = used;

	if (f->size - f->rpos >= size) {
		if (buf)
			memcpy(buf, f->buffer + f->rpos, size);
		f->rpos += size;
		f->rpos &= f->size - 1;
	} else {
		unsigned size1 = (unsigned)(f->size - f->rpos);
		if (buf)
		{
			memcpy(buf, f->buffer + f->rpos, size1);
			memcpy((char*)buf + size1, f->buffer, size - size1);
		}
		f->rpos = (unsigned)size - size1;
	}
	return size;
}

void fifo_delete(struct FIFO *f)
{
	if (f)
		free(f);
}

