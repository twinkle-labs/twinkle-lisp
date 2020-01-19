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
#include "utf8.h"
#include <assert.h>

static const unsigned char leading_ones[256] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 
	3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 
	4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 7, 8
};

/* Utf8_decode -- Fetch a unicode char from UTF8 string.

The UTF-8 translation table is:

 Bits Last code point  Byte 1    Byte 2    Byte 3   Byte 4   Byte 5   Byte 6
 7    U+007F           0xxxxxxx
 11   U+07FF           110xxxxx  10xxxxxx
 16   U+FFFF           1110xxxx	10xxxxxx  10xxxxxx
 21   U+1FFFFF         11110xxx	10xxxxxx  10xxxxxx 10xxxxxx
 26   U+3FFFFFF        111110xx	10xxxxxx  10xxxxxx 10xxxxxx 10xxxxxx
 31   U+7FFFFFFF       1111110x	10xxxxxx  10xxxxxx 10xxxxxx 10xxxxxx 10xxxxxx

Most significant bit is on the left, such that:

 11100010  10000010  10101100
     ----    ****--    --****
 U+    2      0     A      C

 Return 0 if reaches end of string, endp points to right after the NUL byte.
 Return -1 if there is invalid code sequence, endp is undefined.
 Otherwise return the unicode value, and endp points to 
 the next byte to decode.
 
 UTF8 can encode up to 31-bit integer, so return type of `int` is enough.
*/
int Utf8_decode(const char *s, char **endp)
{
	return Utf8_decode_buffer(s, SIZE_MAX, endp);
}

/*
 * Return -1 if error occurs.
 * Return -2 if buffer is incomplete
 */
int Utf8_decode_buffer(const char *s, size_t size, char **endp)
{
	int n, ch;
	unsigned char b;
	
	if (size == 0)
		return -2;

	assert(s != NULL);
	b = (unsigned char)*s;
	if (b < 128) {
		if (b != 0) s++;
		if (endp) *endp = (char*)s;
		return b;
	}
	s++;
	n = leading_ones[b];
	if (n >= 2 && n <= 6) {
		if ((size_t)n > size) {
			return -2;
		}
		b <<= n+1;
		b >>= n+1;
		for (ch = b, n--; n > 0; n--) {
			b = (unsigned char)*s++;
			if ((b & 0xc0) != 0x80) {
				return -1;
			}
			ch <<= 6;
			ch |= (b & 0x3f);
		}
		if (endp) *endp = (char*)s;
		return ch;
	} else {
		return -1;
	}
}
/* 
 * Return first byte of UTF8 sequence for unicode `code'.
 * If out of range, then return -1.
 */
int Utf8_get_first_byte(uint32_t code)
{
	if (code <= 0x7F) {
		return (int)code;
	} else if (code <= 0x7FF) {
		return (int)(code >>  6) | 0xC0;
	} else if (code <= 0xFFFF) {
		return (int)(code >> 12) | 0xE0;
	} else if (code <= 0x1FFFFF) {
		return (int)(code >> 18) | 0xF0;
	} else if (code <= 0x3FFFFFF) {
		return (int)(code >> 24) | 0xF8;
	} else if (code <= 0x7FFFFFFF) {
		return (int)(code >> 30) | 0xFC;
	} else {
		return -1;
	}
}

int Utf8_get_length(uint32_t code)
{
	if (code <= 0x7F) {
		return 1;
	} else if (code <= 0x7FF) {
		return 2;
	} else if (code <= 0xFFFF) {
		return 3;
	} else if (code <= 0x1FFFFF) {
		return 4;
	} else if (code <= 0x3FFFFFF) {
		return 5;
	} else if (code <= 0x7FFFFFFF) {
		return 6;
	} else {
		return -1;
	}
}

/*
 * Return the encoded UTF8 byte count.
 * buf is not zero terminated.
 * If buf is NULL, then return the required byte count
 */
int Utf8_encode(uint32_t code, char *buf, size_t size)
{
	int mark; /* first byte */
	int len;

	if (code <= 0x7F) {
		len = 1;
		mark = 0;
	} else if (code <= 0x7FF) {
		len = 2;
		mark = 0xC0;
	} else if (code <= 0xFFFF) {
		len = 3;
		mark = 0xE0;
	} else if (code <= 0x1FFFFF) {
		len = 4;
		mark = 0xF0;
	} else if (code <= 0x3FFFFFF) {
		len = 5;
		mark = 0xF8;
	} else if (code <= 0x7FFFFFFF) {
		len = 6;
		mark = 0xFC;
	} else {
		return 0;
	}
	
	if (buf == NULL)
		return len;

	if ((size_t)len > size)
		return 0;

	for (int i = len - 1; i > 0; i--, code>>=6)
		buf[i] = (0x80 | (code & 0x3F));
	buf[0] = (mark | code);
	return len;
}
