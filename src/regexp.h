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

/**
 * regexp.h
 *
 * Provides regular expression compilation and matching.
 */
#pragma once

struct regexp_program;
struct regexp_vm;

#define REGEXP_COMPOPT_UNANCHORED 1

/*
 * regexp_compile -- Compile regular expression to byte code.
 *
 * If errmsg is not NULL, the error message string will be returned in it.
 * The caller is responsible to free() the errmsg.
 */
struct regexp_program * 
regexp_compile(const char *s, int opts, char **errmsg);


void 
regexp_program_delete(struct regexp_program *prog);


/*
 * Status code for regexp_vm.
 * Returned by regexp_vm_exec().
 */
enum {
	REGEXP_VM_READY,   /* initial status or after reset */
	REGEXP_VM_WAIT,    /* need more input */
	REGEXP_VM_TIMEOUT, /* run out of cycles UNUSED */
	REGEXP_VM_UNMATCH, /* no match */
	REGEXP_VM_MATCH,   /* matched */
	REGEXP_VM_ERROR    /* errors like stack overflow */
};

/*
 * Create a virtual machine with provided program.
 * This virtual machine can take input from a string
 * or a stream and execute the program to find matches if there's any.
 */
struct regexp_vm * 
regexp_vm_create(struct regexp_program *prog);

void 
regexp_vm_delete(struct regexp_vm *vm);

void 
regexp_vm_reset(struct regexp_vm *vm);

void 
regexp_vm_set_string_input(struct regexp_vm *vm, const char *s);

void 
regexp_vm_add_stream_input(struct regexp_vm *vm, const char *buf, size_t n);

/* Where the matching should begin */
void 
regexp_vm_set_current_pos(struct regexp_vm *vm, int pos);

int 
regexp_vm_exec(struct regexp_vm *vm);

/*
 * Return the position of the matched string.
 * If index = 0, then it is the overall match.
 * Otherwise it returns the position of submatch.
 *
 * Return -1 if there is no such match.
 */
int 
regexp_vm_get_match(struct regexp_vm *vm, int index, int *len);
