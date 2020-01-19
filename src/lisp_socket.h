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

#include "lisp.h"

Lisp_Port* lisp_open_socket_output(Lisp_VM *vm, int sockfd, int timeout);
Lisp_Port* lisp_open_socket_input(Lisp_VM *vm, int sockfd, int timeout);

int open_tcp_server_socket(uint32_t addr, uint16_t port);
int open_udp_server_socket(uint32_t addr, uint16_t port);

bool lisp_socket_init(Lisp_VM *vm);
