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
#include <stdlib.h>

#ifdef _WIN32
# include <winsock2.h>
# include <windows.h>
#endif

#include "public/twk.h"

extern void* twk_start_threads(void*);
extern int g_argc;
extern char **g_argv;

int main(int argc, char *argv[])
{
#ifdef _WIN32
	{
		WSADATA wsaData;
		WSAStartup(MAKEWORD(2,2), &wsaData);
	}
#endif
	g_argc = argc;
	g_argv = argv;
	
	const char *s = getenv("TWK_DIST");
	twk_set_dist_path(s?s:".");
	s = getenv("TWK_VAR");
	twk_set_var_path(s?s:"./var");
	
	twk_start_threads(NULL);
	return 0;
}
