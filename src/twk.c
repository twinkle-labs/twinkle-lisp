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
/******************************************************************
 * node:
 * - peer connections, duplex
 * - broadcast its ports and ip address
 * (hello :addr <ip> :port <port> :from <uid> :date <datetime> :
 *    device <device-id> :device-name <name>)
 *****************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdarg.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include "public/twk.h"

#ifdef _WIN32
# include <winsock2.h>
# include <windows.h>
# include <io.h>
# include <ws2tcpip.h>
# include "win32/w32_compat.h"
# define close(x) closesocket(x)

#else
//# include <ifaddrs.h>
# include <sys/types.h>
# include <sys/socket.h>
# include <net/if.h>
# include <netinet/in.h>
# include <arpa/inet.h>
# include <netdb.h>
#endif

#ifndef WIN32
#include <unistd.h>
#endif

#include <pthread.h>
#include <fcntl.h>


#include "lisp.h"
#include "lisp_crypto.h"
#include "lisp_fs.h"
#include "lisp_zstream.h"
#include "lisp_sqlite3.h"
#include "lisp_socket.h"
#include "httpd.h"

#include "common.h"
#include "twk-internal.h"

/***************************************************************/

#define MAX_PROCESS 1024
#define MAX_THREADS 8
#define DEFAULT_PROCESS_OUTPUT_SIZE (8*1024)
#define DEFAULT_PROCESS_ERROR_SIZE (4*1024)

static const char *state_names[] = {
	[TWK_PS_NONE]     = "none",
	[TWK_PS_CREATED]  = "created",
	[TWK_PS_WAITING]  = "waiting",
	[TWK_PS_RUNNING]  = "running",
	[TWK_PS_RUNNABLE] = "runnable",
	[TWK_PS_DONE]     = "done",
	[TWK_PS_PENDING]  = "pending",
	[TWK_PS_SHUTDOWN] = "shutdown"
};

static struct twk_thread {
	pthread_t thread;
	unsigned shutdown: 1;
	double run_time;
	struct twk_process *proc;
} threads[MAX_THREADS];

static pthread_mutex_t runnable_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t runnable_notify = PTHREAD_COND_INITIALIZER;

static struct twk_process processes[MAX_PROCESS];
static pthread_mutex_t pid_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t twk_log_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int sched_quit = 0;

// Notifying the scheduler which could be sleep
// so that we can let it add more fds to watch
static volatile int sched_sigfd = -1;
#ifdef WIN32
static pthread_mutex_t sched_sigfd_lock = PTHREAD_MUTEX_INITIALIZER;
#endif
//static int nproc = 0;
static int next_pid = 0;
int g_argc = 0;
const char **g_argv;
char g_dist_path[1024];
char g_var_path[1024];
char g_init_file[1024];

static twk_console_output_fn g_output;
static void *g_output_context;
static twk_receive_message_fn g_receive;
static void *g_receive_context;
Lisp_VM *g_vm = NULL;


#ifdef WIN32
static int at_tty = 0;
#else
static int at_tty = 0;
#endif

/***************************************************************/
#define MAX_LOG_PREFIX 96

static void format_log_prefix(struct twk_process *proc, char buf[MAX_LOG_PREFIX])
{
	double mt = microtime();
	time_t t = (time_t)mt;
	struct tm * tm;
	
	tm = localtime(&t);
	size_t n;
	
	if (at_tty) {
		n = strftime(buf, MAX_LOG_PREFIX, "\033[1m[%Y-%m-%d %H:%M:", tm);
	} else {
		n = strftime(buf, MAX_LOG_PREFIX, "[%Y-%m-%d %H:%M:", tm);
	}
	assert(n > 0 && n < MAX_LOG_PREFIX);
	// %09.6f: the number should be formatted with 6 digits in fraction
	// and 9 chars in total length, with 0 padding in front
	if (proc) {
	snprintf(buf+n, MAX_LOG_PREFIX-n, "%09.6f %s#%d]%s ",
	  tm->tm_sec + (mt - t),
	 proc->name, proc->pid, at_tty?"\033[22m":"");
	} else {
	snprintf(buf+n, MAX_LOG_PREFIX-n, "%09.6f]%s ",
	  tm->tm_sec + (mt - t), at_tty ? "\033[22m" : "");
	}
}



#ifdef WIN32
static volatile LONG sigfd_wakecnt;

/*
 * Waking up scheduler from the middle of select()
 * Take 0.1ms. Not a great solution, but
 * Windows pipe is not compatible with select(). 
 */
static void wake_sched(struct twk_process *proc)
{
	if (InterlockedAdd(&sigfd_wakecnt, 1) > 1)
		return;

	twk_log(proc, TWK_LOGGING_VERBOSE, "wake up sched begin"); 
	pthread_mutex_lock(&sched_sigfd_lock);
	if (sched_sigfd >= 0)
	{
		int fd = sched_sigfd;
		sched_sigfd = -1;
		// The scheduler got its copy
		close(fd);
	}
	pthread_mutex_unlock(&sched_sigfd_lock);
	twk_log(proc, TWK_LOGGING_VERBOSE, "wake up sched end");
}

// Return the sigfd usable for select().
// If return -1, scheduler should rescan process list for eligible fds
static int ensure_sched_sigfd()
{
	if (InterlockedExchange(&sigfd_wakecnt, 0) > 1)
		return -1; 
	pthread_mutex_lock(&sched_sigfd_lock);
	if (sched_sigfd < 0) {
		sched_sigfd = socket(PF_INET, SOCK_DGRAM, 0);
	}
	pthread_mutex_unlock(&sched_sigfd_lock);
	return sched_sigfd;
}

static int reset_sched_sigfd() 
{
	InterlockedExchange(&sigfd_wakecnt, 1);
	return 1;
}

#else

static int sigfd_pipe[2];


// Use pipes only takes 0.02ms for reset and wake
// It's quite simpler
static void wake_sched(struct twk_process *proc)
{
	if (sched_sigfd < 0)
		return;
	int a = 1;
	//twk_log(proc, "wake begin");
	write(sigfd_pipe[1], &a, 1);
	//twk_log(proc, "wake end");
}

static int reset_sched_sigfd() {
	static char buf[256];
	int n = 0;
	if (sched_sigfd < 0)
		return 0;
	//twk_log(NULL, "reset sigfd begin");
	n = read(sigfd_pipe[0], buf, sizeof(buf));
	//twk_log(NULL, "reset sigfd end %d", n);
	return n;
}

static int ensure_sched_sigfd()
{
	// Only enter during the first time in sched loop
	// No need to worry about mulitthread conflict
	if (sched_sigfd < 0) {
		pipe(sigfd_pipe);
		sched_sigfd = sigfd_pipe[0];
		int flags = fcntl(sched_sigfd, F_GETFL);
		fcntl(sched_sigfd, F_SETFL, flags | O_NONBLOCK);
	}
	return sched_sigfd;	
}

#endif
/*
 * We need to lock because there could be multiple writers
 * trying to post messages to this process.
 * All written or none.
 */
bool twk_post_message(struct twk_process *proc, const void *mbuf, size_t size)
{
	bool ok = false;
	
	assert(proc);
	if (proc->mbox == NULL
	 || proc->state == TWK_PS_DONE
	 || proc->state == TWK_PS_SHUTDOWN
	 || proc->state == TWK_PS_NONE)
		return false;
	
	pthread_mutex_lock(&proc->mbox_lock);
	if (proc->mbox && fifo_room(proc->mbox) >= size)
	{
		size_t n = fifo_write(proc->mbox, mbuf, size);
		assert(n == size);
		ok = true;
	}
	else
	{
		twk_log(proc, TWK_LOGGING_INFO, "mbox full: %d, size=%d", 
		  fifo_bytes(proc->mbox), (int)size);
	}
	pthread_mutex_unlock(&proc->mbox_lock);
	
	if (ok && proc->state == TWK_PS_WAITING)
	{
		twk_sched(proc, true);
	}
	return ok;
}

bool twk_begin_message(struct twk_process *proc)
{
	assert(proc);
	if (proc->mbox == NULL
	 || proc->state == TWK_PS_DONE
	 || proc->state == TWK_PS_SHUTDOWN
	 || proc->state == TWK_PS_NONE)
		return false;
	
	pthread_mutex_lock(&proc->mbox_lock);
	if (proc->mbox == NULL) {
		pthread_mutex_unlock(&proc->mbox_lock);
		return false;
	}
	proc->mbox_last_wpos = proc->mbox->wpos;
	proc->mbox_overflow = 0;
	return true;
}

void twk_add_message(struct twk_process *proc, const void *buf, size_t size)
{
	if (!proc->mbox_overflow && fifo_room(proc->mbox) >= size)
	{
		size_t n = fifo_write(proc->mbox, buf, size);
		assert(n == size);
	}
	else
	{
		proc->mbox_overflow = 1;
	}
}

void twk_add_message_cstr(struct twk_process *proc, const char *s)
{
	twk_add_message(proc, s, strlen(s));
}

/* Quote the buffer to lisp style string literal */
void twk_add_message_qstr(struct twk_process *proc, const char *s, size_t len)
{
	unsigned head = 0, i = 0;
	for (i = 0; i < len; i++) {
		if (s[i] == '\\' || s[i] == '\"') {
			if (i > head)
				twk_add_message(proc, s+head, i - head);
			if (s[i] == '\\')
				twk_add_message_cstr(proc, "\\\\");
			else
				twk_add_message_cstr(proc, "\\\"");
			head = i + 1;
		}
	}
	if (i > head)
		twk_add_message(proc, s+head, i - head);
}

bool twk_end_message(struct twk_process *proc)
{
	bool ok = true;
	
	// If message is not delivered in its entirety,
	// rollback to last wposition
	if (proc->mbox_overflow)
	{
		twk_log(proc, TWK_LOGGING_INFO, "mbox full: %d", fifo_bytes(proc->mbox));
		proc->mbox->wpos = proc->mbox_last_wpos;
		ok = false;
	}
	pthread_mutex_unlock(&proc->mbox_lock);
	
	if (ok && proc->state == TWK_PS_WAITING)
	{
		twk_sched(proc, true);
	}
	return ok;
}

static size_t mbox_read(void *context, void *buf, size_t size)
{
	struct twk_process *proc = context;
	if (fifo_bytes(proc->mbox) > proc->mbox->size/2
	 && proc->mbox->size < TWK_MAX_MBOX_SIZE)
	{
		// resize
		pthread_mutex_lock(&proc->mbox_lock);
		struct FIFO *fifo = fifo_new(proc->mbox->size*2);
		assert(fifo);
		size_t n = fifo_read(proc->mbox, fifo->buffer, fifo->size);
		fifo->rpos = 0;
		fifo->wpos = (int)n;
		fifo->wtotal = proc->mbox->wtotal;
		fifo_delete(proc->mbox);
		proc->mbox = fifo;
		pthread_mutex_unlock(&proc->mbox_lock);
	}
	return fifo_read(proc->mbox, buf, size);
}

static bool mbox_ready(void *context, int mode)
{
	struct twk_process *proc = context;
    assert(mode == 0);
    return fifo_bytes(proc->mbox) > 0;
}

static struct lisp_stream_class_t mbox_stream_class = {
	.read = mbox_read,
    .ready = mbox_ready
};

/* (open-mbox &optional <size>) */
static void op_open_mbox(Lisp_VM *vm, Lisp_Pair *args)
{
	struct twk_process *proc = lisp_vm_client(vm);
	unsigned size = 4096;
	if (lisp_number_p(CAR(args)))
		size = lisp_safe_int(vm, CAR(args));
	pthread_mutex_lock(&proc->mbox_lock);
	if (proc->mbox == NULL) {
		proc->mbox = fifo_new(size);
	} else if (size > proc->mbox->size){
		struct FIFO *f = fifo_copy(proc->mbox, size);
		assert(f);
		fifo_delete(proc->mbox);
		proc->mbox = f;
	}
	pthread_mutex_unlock(&proc->mbox_lock);
	lisp_push_buffer(vm, NULL, 512);
	lisp_push_stream(vm, &mbox_stream_class, proc);
	lisp_make_input_port(vm);
}

static bool process_output_ready(void *context, int mode)
{
	return true;
}


static size_t process_output_write(void *context, const void *buf, size_t size)
{
	//struct twk_process *proc = context;
	
	pthread_mutex_lock(&twk_log_lock);
	fwrite(buf, 1, size, stdout);
	pthread_mutex_unlock(&twk_log_lock);
	
	return size;
}

static void process_output_close(void *context)
{

}

static struct lisp_stream_class_t process_output_stream_class = {
    .write = process_output_write,
    .ready = process_output_ready,
    .close = process_output_close
};

/*
 * Names must begin with a letter, and can only contain the follow
 * characters:
 * letter, digit, /, -, _
 */
static bool validate_name(const char *name)
{
	if (!isalpha(*name))
		return false;
	while (*++name) {
		if (isalnum(*name) || *name == '/' || *name == '-' || *name == '_')
			continue;
		return false;
	}
	return true;
}

/*
 * (send-message <pid> <message>)
 *
 * Turn <message> into printable representation and append it
 * to <pid>'s mbox if there is room.
 * Return true if message is successfully added to <pid>'s mbox.
 * If destination mbox doesn't have enough space, then return false.
 *
 * FIXME: make sure it's not shutdown, lock?
 */
static void op_send_message(Lisp_VM *vm, Lisp_Pair *args)
{
	int pid = lisp_safe_int(vm, CAR(args));
	if (!lisp_pair_p(CADR(args)))
		lisp_err(vm, "Invalid message");
	lisp_stringify(vm, CADR(args));
	const char *buf = lisp_safe_cstring(vm, lisp_pop(vm, 1));
	if (pid >= 0) {
		struct twk_process *proc = &processes[pid % MAX_PROCESS];
		if (pid != proc->pid)
			lisp_push(vm, lisp_false);
		else {
			bool ok = twk_post_message(proc, buf, strlen(buf));
			lisp_push(vm, ok ? lisp_true : lisp_false);
		}
	} else {
		if (g_receive) {
			g_receive(g_receive_context, buf);
			lisp_push(vm, lisp_true);
		} else {
			lisp_push(vm, lisp_false);
		}
	}
}

static void op_process_exists(Lisp_VM*vm, Lisp_Pair *args)
{
	int pid = lisp_safe_int(vm, CAR(args));
	struct twk_process *p = twk_get_process(pid);
	lisp_push(vm, p?lisp_true:lisp_false);
}

/*
 * wildcard_match -- Match s against pattern p
 *
 * The match function M(s,p) is defined as
 *    - p[0]=0   => s[0]=0?
 *    - p[0]='?' => s[0]!=0 && M(s+1,p+1)
 *    - p[0]='*' => p[1]=0 || M(s,p+1) || M(s+1,p+1) || ....
 *    - else     => p[0]=s[0] && M(s+1,p+1)
 */
int wildcard_match(const char *s, const char *p)
{
	for (;; s++,p++) {
		if (*p == 0) {
			return *s == 0;
		} else if (*p == '?') {
			if (*s == 0)
				return 0;
		} else if (*p == '*') {
			if (p[1] == 0)
				return 1;
			for (;*s != 0;s++)
				if (wildcard_match(s, p+1))
					return 1;
			return 0;
		} else if (*p != *s) {
			return 0;
		}
	}
}

static void op_exited(Lisp_VM *vm, Lisp_Pair *args)
{
	struct twk_process *proc = lisp_vm_client(vm);
	lisp_push(vm, proc->state == TWK_PS_DONE ? lisp_true : lisp_false);
}

static void process_run(struct twk_process *proc)
{
	Lisp_VM *vm = proc->vm;
	lisp_eval_object(vm, lisp_top(vm, 0));
}

static void process_init(struct twk_process *proc)
{
	Lisp_VM *vm = proc->vm;
	Lisp_Object *o = lisp_top(vm, 0);
	Lisp_VM *origin_vm = lisp_procedure_owner(o);
	assert(origin_vm != NULL);
	struct twk_process *origin_proc = lisp_vm_client(origin_vm);
	assert(origin_proc);
	
	if (origin_proc->state != TWK_PS_PENDING
	 && origin_proc->state != TWK_PS_DONE)
	{
		lisp_err(vm, "process_init: pid#%d: init procedure is from an active "
		"process. We can not resume due to potential access conflicts.",
			proc->pid);
	}
	
	// Set up output port
	lisp_push_buffer(vm, NULL, 512);
	lisp_push_stream(vm, &process_output_stream_class, proc);
	lisp_make_output_port(vm);
	lisp_set_current_output(vm, (Lisp_Port*)lisp_pop(vm, 1));
	
	lisp_def(vm, "*init*", o);
	lisp_parse(vm, "(process-init (this))");
	lisp_eval_object(vm, lisp_top(vm, 0));
	lisp_pop(vm, 3); // clear the stack
	lisp_parse(vm, "(process-run (this))");
	proc->run = process_run;
	process_run(proc);
}


/*
 * (spawn <procedure> <arguments>)
 *
 * Create a new process and return its id.
 *
 * It's like `apply' except that the procedure will be executed
 * in a new VM. THe procedure won't actually start until the parent
 * finishes execution. When a child process is running, the parent
 * won't be able to do anything. It can do things when all of its
 * children is finished. The child process will remove itself
 * from its parent when it's done.
 *
 * The new VM will be created directly under the procedure owner VM.
 */
static void op_spawn(Lisp_VM *vm, Lisp_Pair *args)
{
	struct twk_process *curr_proc = lisp_vm_client(vm);
	assert(curr_proc != NULL);

	Lisp_VM *origin_vm = lisp_procedure_owner(CAR(args));
	if (origin_vm == NULL)
		lisp_err(vm, "spawn: Invalid procedure");
	if (!lisp_pair_p(CADR(args)))
		lisp_err(vm, "spawn: Invalid arguments");

	struct twk_process *child = twk_create_process("");
	assert(child != NULL);
	lisp_vm_set_parent(child->vm, origin_vm);
	child->run = process_init;
	child->parent = curr_proc;
	child->sys = curr_proc->sys;
	child->logging_level = curr_proc->logging_level;
	
	if (vm == origin_vm) {
		// we need to protect the procedure object from garbage collection
		lisp_keep_alive(vm, CAR(args));
	}
	
	// Move the initialization procedure into new VM's stack
	// The stack should have enough size
	// No danger here
	lisp_push(child->vm, CAR(args));
	
	// Add to parent's children list
	pthread_mutex_lock(&curr_proc->parental_lock);
	child->sib_next = curr_proc->children;
	curr_proc->children = child;
	pthread_mutex_unlock(&curr_proc->parental_lock);

	lisp_stringify(vm, CADR(args));
	const char *s = lisp_safe_cstring(vm, lisp_pop(vm, 1));
	bool ok = twk_post_message(child, s, strlen(s));
	assert(ok);
	lisp_push(vm, (Lisp_Object*)lisp_number_new(vm, child->pid));
}

static void list_children(Lisp_VM *vm, void *data)
{
	struct twk_process *proc = data;
	int n = 0;
	for (struct twk_process *p = proc->children;p;p=p->sib_next) {
		lisp_push_number(vm, p->pid);
		if (p->name[0]) {
			lisp_make_symbol(vm, p->name);
		} else {
			lisp_make_symbol(vm, "*noname*");
		}
		lisp_make_symbol(vm, state_names[p->state]);
		lisp_make_list(vm, 3);
		n++;
	}
	lisp_make_list(vm, n);
}

static void op_list_processes(Lisp_VM *vm, Lisp_Pair *args)
{
	struct twk_process *proc = lisp_vm_client(vm);
	Lisp_Object *ret;
	pthread_mutex_lock(&proc->parental_lock);
	ret = lisp_try(vm, list_children, proc);
	if (!ret) {
		pthread_mutex_unlock(&proc->parental_lock);
		lisp_err(vm, "list-processes: fatal");
	}
	pthread_mutex_unlock(&proc->parental_lock);
	lisp_push(vm, ret);
}

// Lock Protected
static struct twk_process * find_runnable()
{
	static volatile int last = 0;
	for (int i = 0, j = last + 1; i < MAX_PROCESS; i++, j++)
	{
		if (j >= MAX_PROCESS)
			j = 0;
		if (processes[j].state == TWK_PS_RUNNABLE) {
			last = j;
			return &processes[j];
		}
	}
	return NULL;
}

/*
 * Could be running in any thread.
 */
struct twk_process *twk_create_process(const char *name)
{
	struct twk_process *proc = NULL;
	int pid = -1;
	
	pthread_mutex_lock(&pid_lock);
	for (int i = 0; i < MAX_PROCESS; i++, next_pid++)
	{
		pid = next_pid % MAX_PROCESS;
		if (processes[pid].state == TWK_PS_NONE)
		{
			proc = &processes[pid];
			pid = next_pid++;
			proc->state = TWK_PS_CREATED;
			if (next_pid == 1000000) {
				// We should not have 1 million processes here.
				// we won't have enough RAM any way.
				// Average process should require about 64KB.
				next_pid = 1;
			}
			break;
		}
	}
	pthread_mutex_unlock(&pid_lock);
	
	if (proc)
	{
		assert(pid >= 0);
		memset(proc, 0, sizeof(struct twk_process));
		proc->vm = lisp_vm_new();
		lisp_vm_set_client(proc->vm, proc);
		proc->pid = pid;
		proc->fd = -1;
        proc->start_time = microtime();
        proc->mbox = fifo_new(TWK_INI_MBOX_SIZE);
		pthread_mutex_init(&proc->mbox_lock, NULL);
		pthread_mutex_init(&proc->parental_lock, NULL);
		if (name)
			strncpy(proc->name, name, TWK_MAX_NAME-1);
		proc->state = TWK_PS_CREATED;
	}
	return proc;
}

struct twk_process *twk_get_process(int pid)
{
	int i = pid % MAX_PROCESS;
	if (processes[i].state != TWK_PS_NONE && processes[i].pid == pid) {
		return &processes[i];
	} else {
		return NULL;
	}
}


// Many threads may want to sched same process at the same time
// we do not use a lock here, because it seems harmless
void twk_sched(struct twk_process *proc, bool immediate)
{
	if (proc->state != TWK_PS_CREATED && proc->state != TWK_PS_WAITING)
	{
		// Already running or pending or done
		// We should not try to do anything
		return;
	}
	
	if (immediate)
	{
		proc->state = TWK_PS_RUNNABLE;
		pthread_cond_signal(&runnable_notify);
	}
	else
	{
		proc->state = TWK_PS_WAITING;
	}
}

static void remove_child(struct twk_process *parent, struct twk_process *child)
{
	assert(parent == child->parent);
	
	pthread_mutex_lock(&parent->parental_lock);
	
	struct twk_process **pp = &parent->children;
	for (; *pp != NULL; pp = &(*pp)->sib_next)
	{
		if (*pp == child)
		{
			*pp = child->sib_next;
			child->sib_next = NULL;
			break;
		}
	}
	
	if (parent->children == NULL)
	{
		if (parent->state == TWK_PS_PENDING)
		{
			// Remove keep alive objects which is used by children
			// but now all children's gone
			// lisp_vm_gc(parent->vm, true);
			
			// Give the parent a chance to run
			// however, the parent needs to have input messages
			// or timeout set
			parent->state = TWK_PS_WAITING;
		}
		else if (parent->state == TWK_PS_DONE)
		{
			parent->state = TWK_PS_SHUTDOWN;
		}
	}
	pthread_mutex_unlock(&parent->parental_lock);
}

static void shutdown_process(struct twk_process *proc)
{
	twk_log(proc, TWK_LOGGING_VERBOSE, "shutdown");

	assert(proc->children == NULL);
	assert(proc->state != TWK_PS_NONE);

	if (proc->parent) {
		remove_child(proc->parent, proc);
		if (proc->parent->state == TWK_PS_SHUTDOWN || proc->parent->state==TWK_PS_WAITING) {
			wake_sched(proc); // So our parent may be able to run as soon as possible
		}
	}

	if (proc->data && proc->finalize)
	{
		proc->finalize(proc);
		proc->data = NULL;
		proc->finalize = NULL;
	}
	if (proc->fd >= 0)
	{
		close(proc->fd);
		proc->fd = -1;
	}
	proc->name[0] = 0;
	if (proc->instance_name)
	{
		free(proc->instance_name);
		proc->instance_name = NULL;
	}

	pthread_mutex_lock(&proc->mbox_lock);
	if (proc->mbox)
	{
		fifo_delete(proc->mbox);
		proc->mbox = NULL;
	}
	pthread_mutex_unlock(&proc->mbox_lock);

	pthread_mutex_destroy(&proc->mbox_lock);
	pthread_mutex_destroy(&proc->parental_lock);
	if (proc->vm)
	{
		lisp_vm_delete(proc->vm);
		if (proc->vm == g_vm) {
			g_vm = NULL;
		}
		proc->vm = NULL;
	}
	
	proc->parent = NULL;
	proc->sib_next = NULL;
	proc->children = NULL;
	proc->state = TWK_PS_NONE;
}

static void run_vm(Lisp_VM *vm, void *data)
{
	assert(data != NULL);
	struct twk_process* p = data;
	p->run(p);
}

/*
 * Running on worker thread.
 */
static void *thread_main(void *obj)
{
	struct twk_thread *thread = obj;
	struct twk_process *proc;
	while (!thread->shutdown)
	{
		proc = NULL;
		
		/*
		 * Fetch a runnable process from process table.
		 */
		pthread_mutex_lock(&runnable_lock);
		while (!thread->shutdown)
		{
			proc = find_runnable();
			if (proc)
			{
				proc->state = TWK_PS_RUNNING;
				break;
			}
			pthread_cond_wait(&runnable_notify, &runnable_lock);
		}
		pthread_mutex_unlock(&runnable_lock);

		if (thread->shutdown)
		{
			/*
			 * If we are going to shutdown but already
			 * acquired a runnable, then we must restore its state.
			 */
			if (proc)
			{
				proc->state = TWK_PS_RUNNABLE;
				pthread_cond_signal(&runnable_notify);
			}
			break;
		}
		
		assert(proc != NULL);
		thread->run_time = (double)time(0);
		thread->proc = proc;
		assert(proc->run);
		Lisp_Object* ret = lisp_try(proc->vm, run_vm, proc);
		proc->runcnt++;
		thread->proc = NULL;
		thread->run_time = 0.0;
		
		if (proc->children)
		{
			// this process has children,
			// therefore make all its children running
			// new child is added to the front.
			int new_child = 0;
			pthread_mutex_lock(&proc->parental_lock);
			for (struct twk_process *p = proc->children; p ; p = p->sib_next)
			{
				if (p->state == TWK_PS_CREATED) {
					p->state = TWK_PS_RUNNABLE;
					new_child++;
				} else {
					break;
				}
			}
			pthread_mutex_unlock(&proc->parental_lock);
			if (new_child > 0)
				pthread_cond_signal(&runnable_notify);
		}

		// Modifying process state should be the last thing we
		// do here, because the schedule loop thread is also
		// checking the state.
		if (!ret)
		{
			twk_log(proc, TWK_LOGGING_ERROR, "finished due to run time error");
			if (proc->children == NULL) {
				proc->state = TWK_PS_SHUTDOWN;
				wake_sched(proc);
			} else {
				proc->state = TWK_PS_DONE;
			}
		}
		else if (proc->state == TWK_PS_DONE && proc->children == NULL)
		{
			proc->state = TWK_PS_SHUTDOWN;
			wake_sched(proc);
		}
		else if (proc->state == TWK_PS_RUNNING)
		{
			proc->state = TWK_PS_WAITING;
			// The scheduler may be in the middle of select()
			// we should wake it so that it can add our fd to watch list
			if (proc->fd >= 0 || proc->sched_time > 0) 
				wake_sched(proc);
		}
	}
	
	return NULL;
}

static void set_timeval(struct timeval *tv, double secs)
{
	tv->tv_sec = (int)floor(secs);
	tv->tv_usec = (int)((secs - tv->tv_sec) * 1000000);
}

/*
 * server process will return to wait if there is a 5 seconds of inactivity.
 * Therefore we will not occupy a thread instance doing nothing.
 */
static void socket_server_run(struct twk_process *proc)
{
	uint32_t clilen;
	struct sockaddr_in cli_addr;
	fd_set rset;
	struct timeval timeout;

	while (true)
	{
	    int val = 1;

		timeout.tv_sec = 5;
		timeout.tv_usec = 0;
		FD_ZERO(&rset);
		FD_SET(proc->fd, &rset);
		select(proc->fd+1, &rset, NULL, NULL, &timeout);
		if (FD_ISSET(proc->fd, &rset))
		{
			clilen = sizeof(cli_addr);
			int cli_fd = accept(proc->fd, (struct sockaddr *)&cli_addr, &clilen);
			if (cli_fd == -1)
			{
				#ifndef _WIN32
				if (errno == EWOULDBLOCK)
					break;
				#endif
				perror("socket server: accept()");
				return;
			}

#ifdef SO_NOSIGPIPE			
            if (setsockopt(cli_fd, SOL_SOCKET, SO_NOSIGPIPE, (void*)&val, sizeof(int)) < 0)
            {
                perror("client socket: setsockopt(SO_NOSIGPIPE)");
                close(cli_fd);
                return;
            }
#endif
            if (setsockopt(cli_fd, SOL_SOCKET, SO_KEEPALIVE, (void*)&val, sizeof(int)) < 0)
            {
                perror("client socket: setsockopt(SO_KEEPALIVE)");
                close(cli_fd);
                return;
            }
			char adrbuf[64] = { 0 };
			inet_ntop(cli_addr.sin_family, &cli_addr.sin_addr, adrbuf, sizeof(adrbuf));
			twk_log(proc, TWK_LOGGING_VERBOSE, "client fd: %d %s len=%d\n", cli_fd, 
				adrbuf,
					clilen);
			twk_client_callback fn = proc->data;
			fn(cli_fd, (struct sockaddr *)&cli_addr, clilen);
		}
		else
			break;
	}
}

struct twk_process * twk_create_socket_server(
	const char *name,
	uint32_t addr,
	uint16_t port,
	twk_client_callback fn)
{
	int fd = open_tcp_server_socket(addr, port);
	if (fd < 0)
		return NULL;
	struct twk_process *proc = twk_create_process(name);
	if (proc)
	{
		// check fd
		proc->fd = fd;
		proc->run = socket_server_run;
		proc->data = fn;
		twk_sched(proc, false);
		printf("Created socket server listening at port %d\n", port);
	}
	return proc;
}

void sleep_for_seconds(double secs)
{
#ifdef WIN32
	Sleep((unsigned)(secs * 1000));
#else
	struct timespec t;
	t.tv_sec = (time_t)secs;
	t.tv_nsec = (long)((secs - t.tv_sec) * 1000000000L);
	nanosleep(&t, NULL);
#endif
}

static void op_sleep(Lisp_VM *vm, Lisp_Pair *args)
{
	double secs = lisp_safe_number(vm, CAR(args));
#ifdef WIN32
	Sleep((unsigned)(secs * 1000));
#else
	struct timespec t;
	t.tv_sec = (time_t)secs;
	t.tv_nsec = (long)((secs - t.tv_sec) * 1000000000L);
	nanosleep(&t, NULL);
#endif
	lisp_push(vm, lisp_undef);
}

static void op_exit(Lisp_VM *vm, Lisp_Pair *args)
{
	struct twk_process *proc = lisp_vm_client(vm);
	proc->state = TWK_PS_DONE;
	lisp_push(vm, lisp_undef);
}

static void op_wait(Lisp_VM *vm, Lisp_Pair *args)
{
	struct twk_process *proc = lisp_vm_client(vm);
	proc->state = TWK_PS_PENDING;
	lisp_push(vm, lisp_undef);
}

static void op_set_timeout(Lisp_VM *vm, Lisp_Pair *args)
{
	double secs = lisp_safe_number(vm, CAR(args));
	struct twk_process *proc = lisp_vm_client(vm);
	if (secs <= 0)
		proc->sched_time = 0;
	else
		proc->sched_time = microtime() + secs;
	lisp_push(vm, lisp_undef);
}

static void op_get_timeout(Lisp_VM *vm, Lisp_Pair *args)
{
	struct twk_process *proc = lisp_vm_client(vm);
	lisp_push(vm, (Lisp_Object*)lisp_number_new(vm, proc->sched_time));
}

/* (get-pid [<name>])
 *
 * Return process id. If <name> is not given, then
 * return current process id.
 *
 */
static void op_get_pid(Lisp_VM *vm, Lisp_Pair *args)
{
	struct twk_process *proc = lisp_vm_client(vm);
	lisp_push(vm, (Lisp_Object*)lisp_number_new(vm, proc->pid));
}

static void op_get_parent_pid(Lisp_VM *vm, Lisp_Pair *args)
{
	struct twk_process *proc = lisp_vm_client(vm);
	if (proc->parent)
		lisp_push(vm, (Lisp_Object*)lisp_number_new(vm, proc->parent->pid));
	else
		lisp_push(vm, lisp_false);
}


static void push_arg_value(Lisp_VM *vm, const char *s)
{
	if (strcmp(s, "yes")==0) {
		PUSHX(vm, lisp_true);
	} else if (strcmp(s, "no")==0) {
		PUSHX(vm, lisp_false);
	} else if (strcmp(s, "nil") == 0) {
		PUSHX(vm, lisp_nil);
	} else if (s[0] == ',') {
		lisp_push_number(vm, strtod(s+1, NULL));
	} else {
		PUSHX(vm, lisp_string_new(vm, s, strlen(s)));
	}
}

/*
 * (get-args)
 *
 * Return parsed command line arguments.
 *
 * Arguments started with one or more '-' are options and translated to
 * symbols, otherwise they are treated as strings, even if they are
 * composed of a string of digits.
 *
 * Options with prefix '-' will be converted to symbol, without taking
 * any value.
 * Options with prefix '--' takes one value, and prefix '---' takes
 * the rest of argument list as its value. Values are all treated as strings.
 * even if they has '-' prefixes. An option with value will be converted
 * to a cons pair.
 *
 * The first argument will always be converted to symbol. By convention,
 * it indicates the command we want to execute.
 *
 * A literal string 'yes' will be converted to true, 'no' to false, and 'nil' to ().
 * String began with ',' will be converted to number.
 *
 * Example:
 *  <program-name> startup MyServer -daemon --root 123 ---files a b c
 * Parsed:
 *  (startup "MyServer" daemon (root . "123") (files "a" "b" "c"))
 */
static void op_get_args(Lisp_VM *vm, Lisp_Pair *args)
{
	if (g_argc <= 1){
		lisp_push(vm, lisp_nil);
		return;
	}
	lisp_make_symbol(vm, g_argv[1]);
	int n = 1;
	for (int i = 2; i < g_argc; i++) {
		const char *s = g_argv[i];
		if (s[0] == '-' && !isdigit(s[1])) {
			const char *t = s;
			while (*t == '-')
				t++;
			if (*t == 0) {
				push_arg_value(vm, s);
				continue;
			}
			lisp_make_symbol(vm, t);
			switch (t-s) {
			case 1:
				break;
			case 2:
				if (++i < g_argc) {
					s = g_argv[i];
					push_arg_value(vm, s);
				} else {
					lisp_push(vm, lisp_nil);
				}
				lisp_cons(vm);
				break;
			case 3:
			{
				int n = 0;
				while (++i < g_argc) {
					push_arg_value(vm, g_argv[i]);
					n++;
				}
				lisp_make_list(vm, n);
				lisp_cons(vm);
				break;
			}
			default:
				lisp_err(vm, "Bad option: %s", s);
				break;
			}
		} else {
			push_arg_value(vm, s);
		}
		n++;
	}
	lisp_make_list(vm, n);
}

static void op_get_process_env(Lisp_VM *vm, Lisp_Pair *args)
{
	lisp_push(vm, (Lisp_Object*)lisp_vm_get_root_env(vm));
}

static void op_set_process_name(Lisp_VM *vm, Lisp_Pair *args)
{
	const char *s = lisp_safe_cstring(vm, CAR(args));
	struct twk_process *proc = lisp_vm_client(vm);
	if (!validate_name(s)) {
		lisp_err(vm, "Bad name");
	}
	strncpy(proc->name, s, sizeof(proc->name)-1);
	lisp_push(vm, lisp_undef);
}

static void op_set_logging_level(Lisp_VM *vm, Lisp_Pair *args)
{
	struct twk_process *proc = lisp_vm_client(vm);
	proc->logging_level = lisp_safe_int(vm, CAR(args));
	lisp_push(vm, lisp_undef);
}

static void op_get_logging_level(Lisp_VM *vm, Lisp_Pair *args)
{
	struct twk_process *proc = lisp_vm_client(vm);
	lisp_push_number(vm, proc->logging_level);
}

/* (set-sys-mode! <true|false>)
 * System processes can use priviledged procedures.
 * A process created by system process automatically inherits
 * sys-mode, however, we can turn off sys-mode before user code
 * is run, so that the new process will be running with user level
 * priviledge from then on.
 */
static void op_set_sys_mode(Lisp_VM *vm, Lisp_Pair *args)
{
	struct twk_process *proc = lisp_vm_client(vm);
	if (!proc->sys)
		lisp_err(vm, "set-sys-mode: for system processes only");
	if (CAR(args) == lisp_true)
		proc->sys = 1;
	else
		proc->sys = 0;
	lisp_push(vm, lisp_undef);
}

static struct twk_process *dispatch_proc = NULL;

static void op_wakeup_dispatch(Lisp_VM *vm, Lisp_Pair *args)
{
		if (dispatch_proc)
		{
			if (dispatch_proc->state == TWK_PS_WAITING)
				twk_sched(dispatch_proc, true);
		}
		lisp_push(vm, lisp_undef);
}

static void op_microtime(Lisp_VM *vm, Lisp_Pair *args)
{
	PUSHX(vm, lisp_number_new(vm, microtime()));
}

static void verbose(Lisp_VM *vm, Lisp_Pair *args, int level)
{
	struct twk_process *proc = lisp_vm_client(vm);
	if (proc->logging_level < level)
		return;
	char prefix[MAX_LOG_PREFIX];
	format_log_prefix(proc, prefix);
	Lisp_Port *out = lisp_current_output(vm);
	lisp_port_puts(out, prefix);
	for (; args != (void*)lisp_nil; args = (Lisp_Pair*)CDR(args)) {
		Lisp_Object *o = CAR(args);
		if (lisp_string_p(o)) {
			lisp_port_puts(out, lisp_string_cstr((Lisp_String*)o));
		} else {
			lisp_port_write(out, o);
		}
	}
}

static void op_verbose(Lisp_VM *vm, Lisp_Pair *args)
{
	verbose(vm, args, TWK_LOGGING_VERBOSE);
	lisp_push(vm, lisp_undef);
}

static void op_vverbose(Lisp_VM *vm, Lisp_Pair *args)
{
	verbose(vm, args, TWK_LOGGING_VERBOSE+1);
	lisp_push(vm, lisp_undef);
}

static void print_threads(void)
{
	double curr_time = microtime();
	printf("*** BEGIN Thread Listing %f\n", curr_time);
	int nrun = 0;
	for (int i = 0; i < MAX_THREADS; i++)
	{
		if (threads[i].run_time > 0)
		{
			printf("Thread #%d: PID%d %s: started %f seconds ago\n",
			  i,
			  threads[i].proc->pid,
			  threads[i].proc->name,
			  curr_time - threads[i].run_time);
			nrun++;
		}
	}
	printf("%d/%d running\n", nrun, MAX_THREADS);
}

static void print_processes(void)
{
    double curr_time = microtime();
    printf("*** BEGIN Process Listing %f\n", curr_time);
    int nrun = 0;
    for (int i = 0; i < MAX_PROCESS; i++)
    {
        struct twk_process *proc = &processes[i];
        if (proc->state != TWK_PS_NONE)
        {
        	// We are accessing from another thread
        	pthread_mutex_lock(&proc->mbox_lock);
        	int n = (int)fifo_bytes(proc->mbox);
        	int total = (int)proc->mbox->wtotal;
        	pthread_mutex_unlock(&proc->mbox_lock);
			
            printf("NODE PROCESS PID %2d: %12s[MBOX:%d/%d/%d]: %8s, %f secs, %d\n",
              proc->pid,
              proc->name,
			  n,(int)proc->mbox->size, total,
              state_names[proc->state],
              curr_time - proc->start_time,
              proc->runcnt);
            nrun++;
        }
    }
}

void twk_log(struct twk_process *proc, int level, const char *fmt, ...)
{
	va_list ap;

	int currlevel = proc ? proc->logging_level : processes[0].logging_level;
	if (currlevel < level)
			return;
	va_start(ap, fmt);
	twk_vlog(proc, fmt, ap);
	va_end(ap);
}

void twk_vlog(struct twk_process *proc, const char *fmt, va_list ap)
{
	char buf[MAX_LOG_PREFIX];
	//assert(proc);
	format_log_prefix(proc, buf);
	pthread_mutex_lock(&twk_log_lock);
	fputs(buf, stderr);
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	pthread_mutex_unlock(&twk_log_lock);
}

static void sched_loop(void)
{
	fd_set rset;
	struct timeval timeout;
	struct twk_process *proc;
	double last_check_time;
	last_check_time = microtime();

	while (!sched_quit)
	{
		FD_ZERO(&rset);
		int maxfd = 0;
		double curr_time = microtime();
		// Affects performance?
		double max_wait_secs = 10;


		if (curr_time - last_check_time > 15.0)
		{
			print_threads();
			print_processes();
			last_check_time = curr_time;
		}
Retry:
		reset_sched_sigfd();
		/*
		 * Checking if there is scheduled process
		 */
		for (int i = 0; i < MAX_PROCESS; i++)
		{
			proc = &processes[i];
			
			if (proc->state == TWK_PS_SHUTDOWN)
			{
				shutdown_process(proc);
				if (i == 0) { // Root process
					// There is no more active processes
					goto Done;
				}
				continue;
			}
			
			if (proc->state != TWK_PS_WAITING)
				continue;
			
			int fd = proc->fd;
			double sched_time = proc->sched_time;

			if (proc->mbox && proc->mbox->rpos != proc->mbox->wpos)
			{
				twk_sched(proc, true);
			}
			else if (sched_time > 0)
			{
				if (sched_time <= curr_time)
				{
					twk_sched(proc, true);
				}
				else
				{
					max_wait_secs = MIN(max_wait_secs,
					  sched_time - curr_time);
				}
			}

			if (fd > 0) 
			{
				if (fd > maxfd)
					maxfd = fd;
				FD_SET(fd, &rset);
			}
		}

		// Potential bug
		// what if we are waiting for timeout , a long timeout, and then
		// a new socket is not in the set, how can we responds to that?
		// perhaps we should use a way to interrupt the select.
		int rc;
	
		int sigfd = ensure_sched_sigfd();
		if (sigfd < 0)
			goto Retry;
		if (sigfd > maxfd)
			maxfd = sigfd;
		FD_SET(sigfd, &rset);
		
		set_timeval(&timeout, max_wait_secs);
		rc = select(maxfd + 1, &rset, NULL, NULL, &timeout);
		if (rc < 0) {
			// It's because the sigfd
			perror("select");
		}
		else if (rc == 0) {
			// timeout
		}
		else {

		}
		
		// TODO rc optimize
		for (int i = 0; rc > 0 && i < MAX_PROCESS; i++)
		{
			proc = &processes[i];
			if (proc->fd < 0 || proc->state != TWK_PS_WAITING)
				continue;
			if (FD_ISSET(proc->fd, &rset))
			{
				twk_log(proc, TWK_LOGGING_VERBOSE, "fd ready");
				twk_sched(proc, true);

			}
		}
	}
Done:
	twk_log(NULL, TWK_LOGGING_VERBOSE, "Stopping workers");
	// Wait for all worker threads to quit
	while (true) {
		int nrun = 0;
		for (int i = 0; i < MAX_THREADS; i++) {
			threads[i].shutdown = 1;
			if (threads[i].proc) {
				nrun++;
			}
		}
		if (nrun > 0) {
			sleep_for_seconds(0.3);
		} else {
			break;
		}
	}
	twk_log(NULL, TWK_LOGGING_VERBOSE, "Scheduler finished");
	return;
}

static void init(struct twk_process *root)
{
	extern bool lisp_regexp_init(Lisp_VM *);
	g_vm = root->vm;
	lisp_fs_init(g_vm);
	lisp_crypto_init(g_vm);
	lisp_zstream_init(g_vm);
	lisp_sqlite3_init(g_vm);
	lisp_socket_init(g_vm);
	lisp_http_init(g_vm);
	lisp_regexp_init(g_vm);
	
	lisp_defn(g_vm, "spawn",           op_spawn);
	lisp_defn(g_vm, "sleep",           op_sleep);
	lisp_defn(g_vm, "microtime",       op_microtime);
	lisp_defn(g_vm, "send-message",    op_send_message);
	lisp_defn(g_vm, "exit",            op_exit);
	lisp_defn(g_vm, "wait",            op_wait);
	lisp_defn(g_vm, "exited?",         op_exited);
	lisp_defn(g_vm, "get-pid",         op_get_pid);
	lisp_defn(g_vm, "get-parent-pid",         op_get_parent_pid);
	lisp_defn(g_vm, "open-mbox",       op_open_mbox);
//	lisp_defn(g_vm, "mbox-ready?",     op_mbox_ready_p);
	lisp_defn(g_vm, "list-processes",  op_list_processes);
	lisp_defn(g_vm, "set-timeout",     op_set_timeout);
	lisp_defn(g_vm, "get-timeout",     op_get_timeout);
	lisp_defn(g_vm, "wakeup-dispatch", op_wakeup_dispatch);
	lisp_defn(g_vm, "get-args",        op_get_args);
	lisp_defn(g_vm, "set-process-name", op_set_process_name);
	lisp_defn(g_vm, "set-logging-level", op_set_logging_level);
	lisp_defn(g_vm, "get-logging-level", op_get_logging_level);
	lisp_defn(g_vm, "process-exists?",  op_process_exists);
	lisp_defn(g_vm, "set-sys-mode!",     op_set_sys_mode);
	lisp_defn(g_vm, "get-process-environment", op_get_process_env);
	lisp_defn(g_vm, "verbose", op_verbose);
	lisp_defn(g_vm, "vverbose", op_vverbose);

	Lisp_String* s = lisp_push_cstr(g_vm, g_dist_path);
	lisp_def(g_vm, "*dist-path*", (Lisp_Object*)s);
	lisp_pop(g_vm, 1);

	s = lisp_push_cstr(g_vm, g_var_path);
	lisp_def(g_vm, "*var-path*", (Lisp_Object*)s);
	lisp_pop(g_vm, 1);

	// Set up output port
	lisp_push_buffer(g_vm, NULL, 512);
	lisp_push_stream(g_vm, &process_output_stream_class, root);
	lisp_make_output_port(g_vm);
	lisp_set_current_output(g_vm, (Lisp_Port*)lisp_pop(g_vm, 1));

	snprintf(g_init_file, sizeof(g_init_file), "%s/lisp/init.l",
		g_dist_path);
	
	if (!file_exists(g_init_file))
		lisp_err(g_vm, "Missing init.l\n");
	
	lisp_load_file(g_vm, g_init_file);
}

static void sigterm_handler(int sig)
{
	fprintf(stderr, "Received SIGTERM\n");
	sched_quit = 1;
}


void* twk_start_threads(void *_ptr)
{
	srand((unsigned int)time(NULL));
	#ifndef _WIN32
	// Ignore SIGPIPE which could arise during select.
	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, sigterm_handler);
	#endif
	
	for (int i = 0; i < MAX_THREADS; i++)
	{
		pthread_create(&threads[i].thread, NULL, thread_main,
		  &threads[i]);
	}

	struct twk_process *proc = twk_create_process("init");
	if (proc)
	{
		proc->run = init;
		proc->sys = 1;
		proc->data = NULL;
		twk_sched(proc, true);
	}

	/* Enter scheduler main loop */
	sched_loop();
	return 0;
}

void twk_set_dist_path(const char *path)
{
	strncpy(g_dist_path, path, sizeof(g_dist_path)-1);
}

void twk_set_var_path(const char *path)
{
	strncpy(g_var_path, path, sizeof(g_var_path)-1);
}

const char *twk_get_dist_path(void)
{
	return g_dist_path;
}

const char *twk_get_var_path(void)
{
	return g_var_path;
}

int twk_start(int n, const char *argv[])
{
	pthread_t tid;
	g_argc = n;
	g_argv = argv;
	
	const char *s = getenv("TWK_DIST");
	if (s) twk_set_dist_path(s);
	
	s = getenv("TWK_VAR");
	if (s) twk_set_var_path(s);
	
	if (!g_dist_path[0]) g_dist_path[0] = '.';
	if (!g_var_path[0]) g_var_path[0] = '.';

	if (!dir_exists(g_dist_path)) {
		fprintf(stderr, "Directory not found -- *dist*\n");
		return -1;
	}

	if (!dir_exists(g_var_path)) {
		fprintf(stderr, "Directory not found -- *var*\n");
		return -1;
	}

	printf("dist=%s var=%s\n", g_dist_path, g_var_path);
	
	return pthread_create(&tid, NULL, twk_start_threads, NULL);
}


void twk_set_console_output(twk_console_output_fn output, void *ctx)
{
	g_output = output;
	g_output_context = ctx;
}

void twk_set_receive_message(twk_receive_message_fn recv, void *ctx)
{
	g_receive = recv;
	g_receive_context = ctx;
}
