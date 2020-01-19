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
#include "common.h"
#include "regexp.h"


struct regexp_object {
	Lisp_VM *vm;
	struct regexp_program *re_prog;
	struct regexp_vm *re_vm;
};

static void regexp_object_finalize(Lisp_VM *vm, void *ctx)
{
	struct regexp_object *o = ctx;
	if (o) {
		if (o->re_vm) {
			regexp_vm_delete(o->re_vm);
			o->re_vm = NULL;
			o->re_prog = NULL; // Auto deleted
		}
	}	
}

struct lisp_object_ex_class_t regexp_class = {
	.name = "regexp",
	.size = sizeof(struct regexp_object),
	.finalize = regexp_object_finalize
};

static struct regexp_object* re_obj(Lisp_Object *o)
{
	if (lisp_object_ex_class(o) == &regexp_class)
		return (struct regexp_object*)lisp_object_ex_ptr(o);
	else
		return NULL;
}


static void op_regexp_compile(Lisp_VM *vm, Lisp_Pair *args)
{
	const char *s = lisp_safe_cstring(vm, CAR(args));
	Lisp_Object *o = lisp_make_object_ex(vm, &regexp_class);
	struct regexp_object *x = re_obj(o);
	char *errmsg = NULL;
	x->vm = vm;
	x->re_prog = regexp_compile(s, REGEXP_COMPOPT_UNANCHORED, &errmsg);
	if (x->re_prog == NULL) {
		lisp_err(vm, "Bad regexp: %s", errmsg?errmsg:"Unkown error");
	}
	x->re_vm = regexp_vm_create(x->re_prog);
	if (x->re_vm == NULL) {
		lisp_err(vm, "Can not create regexp vm");
	}
}

// (regexp-match <regexp-object|string> input-string &optional start-pos)
static void op_regexp_match(Lisp_VM *vm, Lisp_Pair *args)
{
	struct regexp_object *x = NULL;
	int start_pos = 0;
	if (lisp_string_p(CAR(args))) {
		op_regexp_compile(vm, args);
		x = re_obj(lisp_top(vm, 0));
	} else if ((x=re_obj(CAR(args)))) {
		if (x->vm != vm)
			lisp_err(vm, "Not in same vm");
		lisp_push(vm, CAR(args));
	} else {
		lisp_err(vm, "Bad argument");
	}
	args = (Lisp_Pair*)CDR(args);
	const char *s = lisp_safe_cstring(vm, CAR(args));
	if (lisp_nil != CDR(args)) {
		start_pos = lisp_safe_int(vm, CADR(args));
	}
	assert(x != NULL);
	regexp_vm_set_string_input(x->re_vm, s);
	regexp_vm_reset(x->re_vm);
	regexp_vm_set_current_pos(x->re_vm, start_pos);
	int ret = regexp_vm_exec(x->re_vm);
	if (ret == REGEXP_VM_MATCH) {
		int i;
		for (i = 0; true; i++) {
			int len;
			int pos = regexp_vm_get_match(x->re_vm, i, &len);
			if (pos < 0) 
				break;
			lisp_push_number(vm, pos);
			lisp_push_number(vm, len);
			lisp_cons(vm);
		}
		if (i > 0) {
			lisp_make_list(vm, i);
		} else {
			lisp_push(vm, lisp_false);
		}
	} else if (ret == REGEXP_VM_ERROR) {
		lisp_err(vm, "Fatal regexp vm error");
	} else {
		lisp_push(vm, lisp_false);
	}
	lisp_exch(vm);
	lisp_pop(vm, 1);
}

static void op_regexp_p(Lisp_VM *vm, Lisp_Pair *args)
{
	lisp_push(vm, re_obj(CAR(args))? lisp_true: lisp_false);
}


bool lisp_regexp_init(Lisp_VM *vm)
{
	lisp_defn(vm, "regexp?", op_regexp_p);
	lisp_defn(vm, "regexp-compile", op_regexp_compile);
	lisp_defn(vm, "regexp-match", op_regexp_match);
	return true;
}
