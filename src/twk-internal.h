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

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <stdarg.h>

#include "fifo.h"
#include "lisp_sqlite3.h"

#define TWK_DEFAULT_PORT 6767
#define TWK_DEFAULT_BROADCAST_PORT 6766
#define TWK_MAX_NAME 32
#define TWK_INI_MBOX_SIZE (4096*2)
#define TWK_MAX_MBOX_SIZE (65536*4)

enum twk_process_state
{
	TWK_PS_NONE,
	TWK_PS_CREATED,
	TWK_PS_WAITING, // wait for messages or events
	TWK_PS_RUNNABLE, // ready to run
	TWK_PS_RUNNING, // executing
	TWK_PS_PENDING, // Wait for child to resume execution
	TWK_PS_DONE,     // Wait for child to finish so we can shutdown
	TWK_PS_SHUTDOWN, // ready to be destroyed
    TWK_PS_TOTAL
};

// The higher level is, the more will be printed
enum twk_logging_level {
	TWK_LOGGING_ERROR = 0,
	TWK_LOGGING_INFO = 1,
	TWK_LOGGING_VERBOSE = 2
};

struct twk_process
{
	int pid;
	char name[TWK_MAX_NAME];
	char *instance_name;
	struct Lisp_VM *vm;
	int fd;
	int runcnt;
	double start_time;
	volatile double sched_time;
	void *data;
	void (*finalize)(struct twk_process*);
	void (*run)(struct twk_process*);
	volatile enum twk_process_state state;
	struct FIFO *mbox;
	unsigned mbox_last_wpos;
	unsigned mbox_overflow :1;
	unsigned sys: 1; // a system process, can be trusted.
	unsigned logging_level: 8;
	pthread_mutex_t mbox_lock;
	pthread_mutex_t parental_lock;
	struct twk_process *parent;   // our parent
	struct twk_process *children; // all child processes
	struct twk_process *sib_next; // for making list of children
};

struct twk_process * twk_create_process(const char *name);
struct twk_process *twk_get_process(int pid);

void twk_sched(struct twk_process *proc, bool immediate);
bool twk_post_message(struct twk_process *proc, const void *mbuf, size_t size);
typedef void (*twk_client_callback)(int sockfd, struct sockaddr* sa, uint32_t sa_len);

struct twk_process * twk_create_socket_server(const char *name, uint32_t addr,
  uint16_t port, twk_client_callback callback);

void twk_log(struct twk_process *proc, int level, const char *fmt, ...);
void twk_vlog(struct twk_process *proc, const char *fmt, va_list ap);

extern uint32_t twk_temp_id;
extern uint8_t twk_pub_key[];
extern int twk_pub_key_len;
extern uint8_t twk_pri_key[];
extern int twk_pri_key_len;
extern int opt_twk_port;
extern int opt_broadcast_port;
extern int opt_debug_port;
extern sqlite3 *g_main_db;

