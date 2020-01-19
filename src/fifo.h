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

/* 
 * FIFO -- Lock free single writer/single reader fifo 
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * struct FIFO
 * size must be power of 2 (2^n), the maximal buffer can be used is `size-1`
 * When wpos == rpos, it means buffer is empty.
 * so we can never actually fill up entire buffer.
 */
struct FIFO
{
	size_t size;
	size_t wtotal;
	volatile unsigned wpos;
	volatile unsigned rpos;
	uint8_t buffer[1];
};

/* 
 * the actual maximal size can be pushed into fifo 
 * is size - 1.  Therefore if you want to make sure that you can write
 * N bytes into the buffer in one call, then you should pass size N+1 
 */
struct FIFO *fifo_new(size_t size);

struct FIFO *fifo_copy(struct FIFO *f, size_t size);

void fifo_delete(struct FIFO *f);
size_t fifo_write(struct FIFO *f, const void *buf, size_t size);
size_t fifo_read(struct FIFO *f, void *buf, size_t size);

static inline size_t fifo_bytes(struct FIFO *f)
{
	return (f->wpos - f->rpos) & (f->size - 1);
}

static inline size_t fifo_room(struct FIFO *f)
{
	return f->size - 1 - fifo_bytes(f);
}

