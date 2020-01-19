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
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "base58.h"

/** All alphanumeric characters except for "0", "I", "O", and "l" */
static const char* b58charset = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

/*
 * Allow leading and trailing space in b58src, but not in middle.
 *
 * If dstlen is insufficient, return a size required (may be bigger than actually needed).
 */
size_t base58_decode(const char* b58src, uint8_t *b256, size_t dstlen)
{
	const char *p = (const char*)b58src;
	size_t n, zeroes=0;
	int i;
	
	// Skip leading spaces.
	while (*p && isspace(*p))
		p++;
	
	// Skip and count leading '1's.
	while (*p == '1') {
		zeroes++;
		p++;
	}
	
	/*
	 * Calculate required space:
	 * log(58)/log(256) + rounded up + zeros
	 */
	n = strlen(p) * 733 / 1000 + 1 + zeroes;
	if (dstlen < n) {
		return n;
	}
	
	// Process the characters.
	while (*p && !isspace(*p)) {
		int carry;
		// Decode base58 character
		const char* ch = strchr(b58charset, *p);
		if (ch == NULL)
			return 0;
		// Apply "b256 = b256 * 58 + ch".
		carry = (int)(ch - b58charset);
		for (i = (int)n-1; i >= 0; i--) {
			carry += 58 * b256[i];
			b256[i] = carry % 256;
			carry /= 256;
		}
		assert(carry == 0);
		p++;
	}
	
	// Skip trailing spaces.
	while (isspace(*p))
		p++;
	
	if (*p != 0) /* Invalid */
		return 0;
	
	// Skip leading zeroes in b256.
	for (i = 0; (unsigned)i < n; i++)
		if (b256[i] != 0)
			break;
	
	i -= zeroes;
	memmove(b256, b256+i, n - i);
	return n - i;
}

size_t base58_encode(const uint8_t* src, size_t srclen, char *b58, size_t dstlen)
{
	const uint8_t *p = src;
	const uint8_t *end = p + srclen;

	// Skip & count leading zeroes.
	int zeroes = 0;
	int i, j;
	size_t n;
	
	while (p != end && *p == 0) {
		p++;
		zeroes++;
	}
	
	/*
	 * Calculate required space:
	 * log(256) / log(58) + rounded up + leading zeroes + padding NUL
	 */
	n = (end - p) * 138 / 100 + 1 + zeroes + 1;
	if (dstlen < n)
		return n;
	
	memset(b58, 0, dstlen);
	
	// Process the bytes.
	for (;p != end; p++) {
		int carry = *p;
		// Apply "b58 = b58 * 256 + ch".
		for (i = (int)n-1; i >= 0; i--) {
			carry += 256 * b58[i];
			b58[i] = carry % 58;
			carry /= 58;
		}
		assert(carry == 0);
	}
	
	for (i = 0; (unsigned)i < n; i++)
		if (b58[i])
			break;
	
	/* Each leading zero byte is represented by digit '1' */
	for (j = 0; j < zeroes; j++) {
		b58[j] = '1';
	}
	
	for (; (unsigned)i < n; i++, j++) {
		b58[j] = b58charset[(int)b58[i]];
	}
	b58[j] = 0;
	return j;
}
