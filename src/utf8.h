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

#pragma once
#include <stdint.h>
#include <stddef.h>

/* 0 signals end, -1 signals error */

int Utf8_decode(const char *s, char **endp);
int Utf8_decode_buffer(const char *s, size_t size, char **endp);

/* 
 * Return byte count.
 * Maximal value is 6.
 */
int Utf8_encode(uint32_t codePoint, char *buf, size_t size);

/* 
 * Return first byte of UTF8 sequence for unicode `code'.
 * If out of range, then return -1.
 */
int Utf8_get_first_byte(uint32_t code);
