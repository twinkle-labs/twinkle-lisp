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

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <setjmp.h>

typedef struct Lisp_VM Lisp_VM;
typedef struct Lisp_Object Lisp_Object;
typedef struct Lisp_Buffer Lisp_Buffer;
typedef struct Lisp_Port Lisp_Port;
typedef struct Lisp_Pair Lisp_Pair;
typedef struct Lisp_String Lisp_String;
typedef struct Lisp_Stream Lisp_Stream;
typedef struct Lisp_Number Lisp_Number;
typedef struct Lisp_Array Lisp_Array;
typedef struct Lisp_Env Lisp_Env;
typedef struct Lisp_ObjectEx Lisp_ObjectEx;
typedef struct lisp_object_ex_class_t lisp_object_ex_class_t;

/* Extension Object Class */
struct lisp_object_ex_class_t {
	const char *name;
	size_t size;
	void (*finalize)(Lisp_VM *vm, void *ptr);
	void (*print)(void *ptr, Lisp_Port *port);
	void (*mark)(void *ptr);
};

struct lisp_stream_class_t {
    const char *name;
    size_t context_size; /* Allocate context if non zero */
    size_t (*read)(void *context, void *buf, size_t size);
    size_t (*write)(void *context, const void *buf, size_t size);
    void (*close)(void *context);
    void (*mark)(void *context);
    bool (*ready)(void *context, int mode); /* mode: 0 - read, 1 : write */
    int (*seek)(void *context, long offset); /* return 0 if ok */
};

typedef struct lisp_vm_state_t {
	Lisp_Env *env;
	size_t stack_count;
	jmp_buf *kaboom;
	Lisp_Port *input, *output, *error;
} lisp_vm_state_t;

typedef void (*lisp_func)(Lisp_VM*, Lisp_Pair* args);

extern Lisp_Object *lisp_nil;
extern Lisp_Object *lisp_true;
extern Lisp_Object *lisp_false;
extern Lisp_Object *lisp_undef;

const char *lisp_object_type_name(Lisp_Object *o);
bool lisp_string_p(Lisp_Object *o);
bool lisp_symbol_p(Lisp_Object *o);
bool lisp_number_p(Lisp_Object *o);
bool lisp_pair_p(Lisp_Object *o);
bool lisp_input_port_p(Lisp_Object *o);
bool lisp_output_port_p(Lisp_Object *o);
bool lisp_integer_p(Lisp_Object *o);
bool lisp_buffer_p(Lisp_Object *o);
bool lisp_stream_p(Lisp_Object *o);
size_t lisp_stream_write(Lisp_Stream *stream, const void *buf, size_t size);
size_t lisp_stream_read(Lisp_Stream *stream, void *buf, size_t size);
void lisp_stream_close(Lisp_Stream *stream);

void *lisp_alloc(Lisp_VM *vm, size_t size);
void lisp_free(Lisp_VM*vm, void *ptr, size_t size);
void lisp_mark(Lisp_Object *obj);
void lisp_err(Lisp_VM *vm, const char *fmt, ...);
Lisp_Object* lisp_pop(Lisp_VM *vm, int n);
void lisp_push(Lisp_VM *vm, Lisp_Object *obj);
Lisp_Number *lisp_number_new(Lisp_VM *vm, double value);
Lisp_Number *lisp_push_number(Lisp_VM *vm, double value);
double lisp_number_value(Lisp_Number* n);

Lisp_String *lisp_string_new(Lisp_VM *vm, const char *buf, size_t length);

Lisp_Buffer *lisp_buffer_new(Lisp_VM *vm, size_t cap);
Lisp_Buffer *lisp_buffer_copy(Lisp_VM *vm, const void *data, size_t size);
void *lisp_buffer_bytes(Lisp_Buffer *b);
#define lisp_buffer_data lisp_buffer_bytes
size_t lisp_buffer_cap(Lisp_Buffer *b);
size_t lisp_buffer_size(Lisp_Buffer *b);
void lisp_buffer_grow(Lisp_Buffer *sb, size_t size);
void lisp_buffer_set_size(Lisp_Buffer *b, size_t size);
void lisp_buffer_add_bytes(Lisp_Buffer *b, const void *data, size_t size);
void lisp_buffer_add_byte(Lisp_Buffer *b, unsigned char value);
void lisp_buffer_shift(Lisp_Buffer *b, size_t n);
void lisp_buffer_clear(Lisp_Buffer *b);
Lisp_Buffer* lisp_push_buffer(Lisp_VM *vm, const void *data, size_t size);

#define lisp_vm_exec(vm,s) lisp_vm_load_string(vm,s,NULL)
void lisp_parse(Lisp_VM *vm, const char *s);
void lisp_eval_object(Lisp_VM *vm, Lisp_Object *obj);
void lisp_load_file(Lisp_VM*, const char *path);
bool lisp_vm_load_string(Lisp_VM *vm, const char *s, const char *name);
void lisp_vm_set_verbose(Lisp_VM*vm, bool verbose);
Lisp_Object *lisp_vm_last_eval(Lisp_VM*vm);
Lisp_Object *lisp_car(Lisp_Pair *p);
Lisp_Object *lisp_cdr(Lisp_Pair *p);
Lisp_Object *lisp_nth(Lisp_Pair *p, int index);
Lisp_Pair *lisp_cons(Lisp_VM *vm);
void lisp_make_list(Lisp_VM *vm, int n);
const char *lisp_string_cstr(Lisp_String *s);
size_t lisp_string_length(Lisp_String *s);
Lisp_String *lisp_push_string(Lisp_VM *vm, const char *buf, size_t length);
Lisp_String *lisp_push_cstr(Lisp_VM *vm, const char *buf);
Lisp_String *lisp_push_string_from_buffer(Lisp_VM *vm, Lisp_Buffer *b);

Lisp_Object* lisp_top(Lisp_VM*, int i);
int lisp_safe_int(Lisp_VM *vm, Lisp_Object* o);
Lisp_Pair* lisp_safe_list(Lisp_VM *vm, Lisp_Object* o);
double lisp_safe_number(Lisp_VM *vm, Lisp_Object* o);
void *lisp_safe_bytes(Lisp_VM *vm, Lisp_Object *o, size_t *len);
const char* lisp_safe_cstring(Lisp_VM *vm, Lisp_Object* o);
const char* lisp_safe_csymbol(Lisp_VM *vm, Lisp_Object* o);
Lisp_Object *lisp_vm_get(Lisp_VM *vm, const char *name);
void lisp_def(Lisp_VM *vm, const char *name, Lisp_Object *o);
void lisp_defn(Lisp_VM *vm, const char *name, lisp_func fn);
//bool lisp_vm_defn(Lisp_VM *vm, const char *name, lisp_func fn);
bool lisp_vm_load(Lisp_VM *vm, const char *filename);
bool lisp_vm_run(Lisp_VM *vm);
void lisp_vm_enable_debug(Lisp_VM *vm, bool enabled);
bool lisp_vm_set_error_stream(Lisp_VM *vm, Lisp_Stream*stream);
bool lisp_vm_set_output_stream(Lisp_VM *vm, Lisp_Stream*stream);
bool lisp_vm_set_input_stream(Lisp_VM *vm, Lisp_Stream*stream);
bool lisp_vm_set_input_file(Lisp_VM *vm, const char *filename);
void lisp_vm_save_state(Lisp_VM *vm, lisp_vm_state_t *state);
void lisp_vm_restore_state(Lisp_VM *vm, lisp_vm_state_t *state);
jmp_buf* lisp_vm_set_error_trap(Lisp_VM *vm, jmp_buf *jbuf);
void lisp_vm_resume_error(Lisp_VM *vm, jmp_buf *old);
void lisp_exch(Lisp_VM *vm);
Lisp_Env *lisp_vm_get_root_env(Lisp_VM *vm);

Lisp_String *lisp_make_symbol(Lisp_VM *vm, const char *name);
void lisp_begin_list(Lisp_VM *vm);
void lisp_end_list(Lisp_VM *vm);
Lisp_Stream *lisp_stream_new(Lisp_VM *vm, struct lisp_stream_class_t *cls, void *context);
Lisp_Stream *lisp_push_stream(Lisp_VM *vm, struct lisp_stream_class_t *cls, void *context);
void lisp_stream_close(Lisp_Stream *stream);
void *lisp_stream_context(Lisp_Stream *stream);
struct lisp_stream_class_t* lisp_stream_class(Lisp_Stream *stream);
//bool lisp_stream_set_context(Lisp_Stream *stream, void *context);
void lisp_stream_set_class(Lisp_Stream *stream, struct lisp_stream_class_t *cls);

Lisp_Port *lisp_make_input_port(Lisp_VM *vm);
Lisp_Port *lisp_make_output_port(Lisp_VM *vm);
Lisp_Port *lisp_open_output_buffer(Lisp_VM *vm, Lisp_Buffer *buffer);
size_t lisp_port_fill(Lisp_Port*port);
Lisp_Buffer *lisp_port_get_buffer(Lisp_Port*port);
void *lisp_port_pending_bytes(Lisp_Port*);
void lisp_port_drain(Lisp_Port *p, size_t nbytes);
void lisp_port_print(Lisp_Port *p, Lisp_Object *obj);
#define lisp_port_write(p,obj)  lisp_port_print(p,obj)
bool lisp_port_set_output_stream(Lisp_Port *port, Lisp_Stream *stream);
bool lisp_port_set_input_stream(Lisp_Port *port, Lisp_Stream *stream);
Lisp_Stream *lisp_port_get_stream(Lisp_Port*port);

int lisp_port_getc(Lisp_Port *port);
void lisp_port_putc(Lisp_Port *port, int c);
void lisp_port_put_bytes(Lisp_Port *port, const void *data, size_t size);
void lisp_port_flush(Lisp_Port *port);
void lisp_port_printf(Lisp_Port *port, const char *fmt, ...);
void lisp_port_puts(Lisp_Port *port, const char *s);

Lisp_Port *lisp_current_output(Lisp_VM* vm);
Lisp_Port *lisp_current_input(Lisp_VM* vm);
void lisp_set_current_output(Lisp_VM* vm, Lisp_Port *port);

void *lisp_object_ex_ptr(Lisp_Object* obj);
const lisp_object_ex_class_t *lisp_object_ex_class(Lisp_Object *obj);
Lisp_Object* lisp_make_object_ex(Lisp_VM *vm, const lisp_object_ex_class_t *cls);
void lisp_object_ex_finalize(Lisp_VM *vm, Lisp_Object* obj);
void lisp_object_ex_set_ptr(Lisp_Object* obj, void *ptr);

Lisp_VM *lisp_vm_new(void);
void lisp_vm_delete(Lisp_VM *vm);
void lisp_vm_set_client(Lisp_VM* vm, void *client);
void* lisp_vm_client(Lisp_VM* vm);
void lisp_vm_set_parent(Lisp_VM *vm, Lisp_VM *parent);
void lisp_vm_gc(Lisp_VM *vm, bool clear_keep_alive);
Lisp_Object* lisp_try(Lisp_VM *vm, void (*func)(Lisp_VM*, void *), void *data);
Lisp_Env *lisp_vm_root_env(Lisp_VM *vm);
Lisp_VM *lisp_procedure_owner(Lisp_Object *obj);
void lisp_keep_alive(Lisp_VM *vm, Lisp_Object* obj);
void lisp_stringify(Lisp_VM *vm, Lisp_Object *obj);

