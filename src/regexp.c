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

/*
 * regexp.c -- A regular expression compiler and match VM.
 *
 * UTF8 Support. The pattern and string to be matched against
 * must be strictly UTF8 encoded. Support full Unicode range.
 *
 * Compile mode: search, and tokenize.
 * match could have a callback in lexer, to submit tokens to parser.
 * MATCH callback with the submatch id.
 *
 * TODO: optimize
 *       [ ] turn splits into switch, with one byte look ahead.
 *
 * TODO: performance data, first we want the IPS of virtual machine
 *       using NOP and jumps.
 *       next we want to try its performance searching a large file
 *       for complex patterns.
 *       next we try to lex a large file.
 *       tokenizer.
 *       next we try to deal with streaming.
 *
 * REFERENCE
 *
 *  - https://swtch.com/~rsc/regexp/regexp2.html
 *
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <setjmp.h>
#include "utf8.h"
#include "regexp.h"

#define REGEXP_MAX_CCLASS   127
#define REGEXP_MAX_CODE     32767
#define REGEXP_MAX_BACKREF  15  /* at most 15 sub-matches */

#define REGEXP_VM_STACK_MAX 128
#define REGEXP_VM_CHECK_MAX 64

enum {
	REGEXP_OP_BACKREF,
	REGEXP_OP_BOL,
	REGEXP_OP_BOUNDARY,
	REGEXP_OP_CCLASS,

	/*
	 * For 16-bit unicode characters.
	 */
	REGEXP_OP_CHAR,

	/*
	 * A char32 instruction requires a following NOP, which provides
	 * the higher 16 bits of unicode char.
	 *
	 *     CHAR32  LLLL
	 *     NOP     HHHH
	 */
	REGEXP_OP_CHAR32, 

	/*
	 * Make sure input position has advanced since last time 
	 * this instruction is executed. Otherwise we are in an
	 * infinite loop and should give up.
	 */
	REGEXP_OP_PROGRESS,

	REGEXP_OP_EOL,
	REGEXP_OP_JMP,
	REGEXP_OP_MATCH,
	REGEXP_OP_NBOUNDARY,
	REGEXP_OP_NCCLASS,
	REGEXP_OP_NOP,
	REGEXP_OP_SAVE,

	/*
	 * If the operand `a' != 0, then split won't happen
	 * unless the next byte from input equals to `a'.
	 * If `a' is 0, then split will always happen.
	 */
	REGEXP_OP_SPLIT1,
	REGEXP_OP_SPLIT2,

	REGEXP_OP_SWITCH, /* Unused yet */
};

/*
 * Input action to be performed before instruction dispatch.
 */
enum {
	REGEXP_ACTION_NONE = 0,
	REGEXP_ACTION_GET,
	REGEXP_ACTION_PEEK,
	REGEXP_ACTION_PEEKB
};

struct {
	const char *name;
	int action;
} regexp_op_info[] = {
	[REGEXP_OP_BACKREF          ] = {"BACKREF",                       },
	[REGEXP_OP_BOL              ] = {"BOL",        REGEXP_ACTION_PEEK },
	[REGEXP_OP_BOUNDARY         ] = {"BOUNDARY",   REGEXP_ACTION_PEEK },
	[REGEXP_OP_CCLASS           ] = {"CCLASS",     REGEXP_ACTION_GET  },
	[REGEXP_OP_CHAR             ] = {"CHAR",       REGEXP_ACTION_GET  },
	[REGEXP_OP_CHAR32           ] = {"CHAR32",     REGEXP_ACTION_GET  },
	[REGEXP_OP_PROGRESS         ] = {"PROGRESS",                      },
	[REGEXP_OP_EOL              ] = {"EOL",        REGEXP_ACTION_PEEK },
	[REGEXP_OP_JMP              ] = {"JMP",                           },
	[REGEXP_OP_MATCH            ] = {"MATCH",                         },
	[REGEXP_OP_NBOUNDARY        ] = {"NBOUNDARY",  REGEXP_ACTION_PEEK },
	[REGEXP_OP_NCCLASS          ] = {"NCCLASS",    REGEXP_ACTION_GET  },
	[REGEXP_OP_NOP              ] = {"NOP",                           },
	[REGEXP_OP_SAVE             ] = {"SAVE",                          },
	[REGEXP_OP_SPLIT1           ] = {"SPLIT1",     REGEXP_ACTION_PEEKB},
	[REGEXP_OP_SPLIT2           ] = {"SPLIT2",     REGEXP_ACTION_PEEKB},
	[REGEXP_OP_SWITCH           ] = {"SWITCH",                        },
};

struct regexp_inst {
	uint8_t op;
	uint8_t a;
	union {
		uint16_t u16;
		int16_t  i16;
	} b;
};

/* ------------------------------------------------- */

struct regexp_crange {
	uint32_t first, last;
	uint32_t pri;
	struct regexp_crange *left, *right;
};

static bool 
regexp_crange_has(struct regexp_crange *rp, uint32_t ch) 
{
	if (rp == NULL)
		return false;
	if (ch < rp->first) {
		return regexp_crange_has(rp->left, ch);
	} else if (ch > rp->last) {
		return regexp_crange_has(rp->right, ch);
	} else {
		return true;
	}
}

static void 
regexp_crange_add(
	struct regexp_crange **node, 
	uint32_t first, 
	uint32_t last, 
	uint32_t pri)
{
	assert(first <= last);

	struct regexp_crange *rp = *node;
	if (rp == NULL) {
		rp = calloc(1, sizeof(struct regexp_crange));
		assert(rp != NULL);
		rp->first = first;
		rp->last = last;
		rp->pri = pri;
		*node = rp;
		return;
	}

	// Incoming range is on left of current range
	if (rp->first > last + 1) {
		regexp_crange_add(&rp->left, first, last, pri);
		/*
		 * Rotate
		 *
		 *      A <= rp            B
		 *     / \                / \
		 *    B   C     ===>     E  A
		 *   / \                   / \
		 *  E   F                 F  C
		 */
		if (rp->left->pri < rp->pri) {
			*node = rp->left;
			rp->left = (*node)->right;
			(*node)->right = rp;
		}
	} else if (rp->last + 1 < first) {
		regexp_crange_add(&rp->right, first, last, pri);
		if (rp->right->pri < rp->pri) {
			*node = rp->right;
			rp->right = (*node)->left;
			(*node)->left = rp;
		}
	} else {
		// Intersects or neighboring
		if (last > rp->last)
			rp->last = last;
		if (first < rp->first)
			rp->first = first;
	}
}

static void
regexp_crange_delete(struct regexp_crange *r)
{
	if (r->left) regexp_crange_delete(r->left);
	if (r->right) regexp_crange_delete(r->right);
	free(r);
}

/*
 * The first 16 bytes (128bit) is used for ASCII ranges.
 * The next 16 bytes are used for first byte of multi-byte UTF8 sequence
 * The actual non-ASCII char ranges are stored in a Treap (root).
 */
struct regexp_cclass {
	uint32_t mask[8];
	struct regexp_crange *root; // Treap structure
	uint32_t pri; // Used for crange Treap
};

enum {
	REGEXP_CCLASS_DIGIT,
	REGEXP_CCLASS_SPACE,
	REGEXP_CCLASS_WORD,
	REGEXP_CCLASS_ANY,
	REGEXP_CCLASS_ALL,
	REGEXP_CCLASS_COUNT
};

static struct regexp_crange regexp_crange_not_ascii = {0x128, 0x7fffffff};

static const char* regexp_cclass_names[] = {
	[REGEXP_CCLASS_DIGIT  ] = "DIGIT",
	[REGEXP_CCLASS_SPACE  ] = "SPACE",
	[REGEXP_CCLASS_WORD   ] = "WORD",
	[REGEXP_CCLASS_ANY    ] = "ANY",
	[REGEXP_CCLASS_ALL    ] = "ALL"
};

static const struct regexp_cclass regexp_cclasses[] = {
	[REGEXP_CCLASS_DIGIT ] = {
		{
			0x00000000,0x03ff0000,0x00000000,0x00000000,
			0x00000000,0x00000000,0x00000000,0x00000000,
		}, 
		NULL
	},
	
	[REGEXP_CCLASS_SPACE ] = {
		{
			0x00003e00,0x00000001,0x00000000,0x00000000,
			0x00000000,0x00000000,0x00000000,0x00000000,
		}, 
		NULL
	},
	
	[REGEXP_CCLASS_WORD  ] = {
		{
			0x00000000,0x03ff0000,0x87fffffe,0x07fffffe,
			0x00000000,0x00000000,0x00000000,0x00000000,
		}, 
		NULL
	},
	
	[REGEXP_CCLASS_ANY   ] = {
		{
			0xffffdbff,0xffffffff,0xffffffff,0xffffffff,
			0x00000000,0x00000000,0xffffffff,0x3fffffff,
		}, 
		&regexp_crange_not_ascii
	},
	
	[REGEXP_CCLASS_ALL   ] = {
		{
			0xffffffff,0xffffffff,0xffffffff,0xffffffff,
			0x00000000,0x00000000,0xffffffff,0x3fffffff,
		}, 
		&regexp_crange_not_ascii
	},
};


static int 
isword(int ch)
{
	return isalnum(ch) || ch == '_';
}

static bool 
regexp_cclass_has(const struct regexp_cclass *a, int ch)
{
	assert(ch >= 0);
	int b = Utf8_get_first_byte((uint32_t)ch);
	assert(b >= 0);

	if (!(a->mask[b>>5] & (1 << (b&0x1f)))) {
		return false;
	}

	if (ch < 128) {
		return true;
	}
	return regexp_crange_has(a->root, (uint32_t)ch);
}

static void 
regexp_cclass_add_range(struct regexp_cclass *cc, int first, int last)
{
	assert(first >= 0);
	assert(first <= last);

	for (int i = first;  i <= last; i++) {
		int b = Utf8_get_first_byte((uint32_t)i);
		cc->mask[(b>>5)] |= (1 << (b&0x1f));
	}

	if (last < 128)
		return;

	if (first < 128)
		first = 128;

	if (first <= last) {
		cc->pri = cc->pri * 1103515245 + 12345;
		regexp_crange_add(&cc->root, 
			(uint32_t)first, 
			(uint32_t)last, 
			cc->pri);
	}
}

static void 
regexp_cclass_add_char(struct regexp_cclass *a, int ch)
{
	regexp_cclass_add_range(a, ch, ch);
}

static void
regexp_cclass_delete(struct regexp_cclass *a)
{
	if (a->root) regexp_crange_delete(a->root);
	free(a);
}

/* ---------------------------------------------- */

/*
 * regexp_program
 */

struct regexp_program {
	struct {
		struct regexp_inst *buf;
		size_t count;
		size_t cap;
	} code;
	
	// cclasses
	struct regexp_cclass *cls_list[REGEXP_MAX_CCLASS];
	int cls_count;
	int save_index;
	int check_index;
	// jump tables
};


// regexp_program methods

void 
regexp_program_delete(struct regexp_program *prog)
{
	if (prog->code.buf) {
		free(prog->code.buf);
		prog->code.buf = NULL;
	}
	for (int i = 0; i < prog->cls_count; i++) {
		regexp_cclass_delete(prog->cls_list[i]);
	}
	free(prog);
}

static void
regexp_program_grow(struct regexp_program *prog, size_t newsize)
{
	if (newsize > prog->code.cap) {
		size_t cap = prog->code.cap;
		if (cap < 128)
			cap = 128;
		while (cap < newsize)
			cap *= 2;

		if (prog->code.buf == NULL) {
			prog->code.buf = calloc(cap, 
				sizeof(struct regexp_inst));
		} else {
			prog->code.buf = realloc(prog->code.buf, 
				cap * sizeof(struct regexp_inst));
			assert(prog->code.buf != NULL);
		}
		prog->code.cap = cap;
	}
}

static struct regexp_inst *
regexp_program_add_inst(struct regexp_program *prog, int op)
{
	struct regexp_inst *ip;

	regexp_program_grow(prog, prog->code.count + 1);
	ip = prog->code.buf + prog->code.count;
	ip->op = op;
	prog->code.count++;
	return ip;
}

static int 
regexp_program_alloc_cclass(struct regexp_program *prog)
{
	assert(prog->cls_count < 100);
	struct regexp_cclass *p = calloc(1, sizeof(struct regexp_cclass));
	prog->cls_list[prog->cls_count] = p;
	return REGEXP_CCLASS_COUNT + prog->cls_count++;
}

struct regexp_cclass * 
_regexp_program_get_cclass(struct regexp_program *prog, int clsId)
{
	assert(clsId >= REGEXP_CCLASS_COUNT);
	clsId -= REGEXP_CCLASS_COUNT;
	assert(clsId < prog->cls_count);
	return prog->cls_list[clsId];
}

static const struct regexp_cclass * 
regexp_program_get_cclass(struct regexp_program *prog, int clsId)
{
	assert(clsId >= 0);
	if (clsId < REGEXP_CCLASS_COUNT)
		return regexp_cclasses + clsId;
	else
		return _regexp_program_get_cclass(prog, clsId);
}

static void 
regexp_program_add_to_cclass(
	struct regexp_program *prog, 
	int clsId, int ch)
{
	struct regexp_cclass *p = _regexp_program_get_cclass(prog, clsId);
	regexp_cclass_add_char(p, ch);
}

static void 
regexp_program_add_range_to_cclass(
	struct regexp_program *prog, 
	int clsId, int first, int last)
{
	struct regexp_cclass *p = _regexp_program_get_cclass(prog, clsId);
	regexp_cclass_add_range(p, first, last);
}


static struct regexp_inst *
regexp_program_insert_inst(struct regexp_program *prog, int loc, int op)
{
	regexp_program_grow(prog, prog->code.count + 1);

	if (loc < 0) {
		loc = (int)prog->code.count + loc;
	}
	assert(loc >= 0 && (unsigned)loc <= prog->code.count);
	struct regexp_inst *ip = prog->code.buf + loc;
	size_t n = prog->code.count - loc;
	memmove(ip + 1, ip, n * sizeof(struct regexp_inst));
	prog->code.buf[loc].op = op;
	prog->code.count++;
	return ip;
}

static void 
regexp_program_dup(struct regexp_program *prog, int n)
{
	assert(n >= 0 && (unsigned)n <= prog->code.count);

	regexp_program_grow(prog, prog->code.count + n);
	memcpy(prog->code.buf + prog->code.count, 
		prog->code.buf + prog->code.count - n, 
		n * sizeof(struct regexp_inst));
	prog->code.count += n;
}

static void 
regexp_program_pop(struct regexp_program *prog, int n)
{
	assert(prog->code.count >= (unsigned)n);
	prog->code.count -= (unsigned)n;
}

/*
 * One byte look ahead.
 * Return 0 if we can't decide on a look ahead byte.
 */
static int
regexp_program_look_ahead(struct regexp_program *prog, int i)
{
	struct regexp_inst *ip = prog->code.buf + i;
	for (; i < (int)prog->code.count; i++, ip++) {
		switch (ip->op) {
		case REGEXP_OP_CHAR:
			return Utf8_get_first_byte(ip->b.u16);	

		case REGEXP_OP_CHAR32: {
			assert(i + 1 < (int)prog->code.count);
			int ch = ip->b.u16 + (ip[1].b.u16<<16);
			return Utf8_get_first_byte(ch);
		}

		case REGEXP_OP_JMP:
			/* forward only */
			if (ip->b.i16 >= 0) {
				i = i + 1 + ip->b.i16;
			} else {
				return 0;
			}
			break;

		/* These will not affect the look ahead byte */
		case REGEXP_OP_PROGRESS:
		case REGEXP_OP_SAVE: 
		case REGEXP_OP_BOL:
		case REGEXP_OP_BOUNDARY:
		case REGEXP_OP_NBOUNDARY:
		case REGEXP_OP_NOP:
			break;

		/* Stop looking ahead */
		default:
			return 0;
		}
	}
	return 0;
}

static void
regexp_program_optimize(struct regexp_program *prog)
{
	int i = (int)prog->code.count - 1;
	struct regexp_inst *ip = prog->code.buf + i;

	for (; i >= 0; i--, ip--) {
		switch (ip->op) {
		case REGEXP_OP_SPLIT1:
			ip->a = (uint8_t)regexp_program_look_ahead(
				prog, i + 1 + ip->b.i16);
			break;
		case REGEXP_OP_SPLIT2:
			ip->a = (uint8_t)regexp_program_look_ahead(
				prog, i + 1);
			break;
		default:
			break;
		}
	}
}

/* ------------------------------------------------------- */


struct regexp_vmstate {
	int next_ip;
	int pos;
};


/*
 * regexp_vm -- Virtual Matchine to run a regexp program.
 *
 *  
 */

struct regexp_vm {
	int status;

	struct regexp_program *prog;
	int ip; /* Instruction Pointer */

	/*
	 * Regexp Backtrack States
	 */
	int sp;
	struct regexp_vmstate stack[REGEXP_VM_STACK_MAX];
	size_t check_points[REGEXP_VM_CHECK_MAX];

	/*
	 * Saved positions for sub matches
	 *
	 *    0 - begin of match 0 (overall)
	 *    1 - end   of match 0 
	 *    2 - begin of match 1 (group 1)
	 *    3 - end   of match 1
	 *    ...
	 *
	 * At most 16 pairs.
	 */
	uint32_t save_mask;
	int save_pos[32];

	struct {
		int begin;
		int end;
	} backref;

	/*
	 * Vm works like a port, it takes input and produces matches 
	 * or unmatches.
	 * If closed, there will be no further input.
	 * If not streaming, `buf` will be an external string pointer. 
	 * count and cap is not used.
	 * If in streaming, `buf` is an internal buffer that accumulates content
	 * on the fly. 
	 */
	struct {
		/*
		 * Do we have more input coming in?
		 */
		unsigned  ended      : 1;

		unsigned  streaming  : 1;

		/*
		 * Current position, pointing to next char to be read
		 */
		int       pos; 

		/*
		 * In stream mode, `buf` points to allocated internal buffer.
		 * Otherwise it points to an external string buffer, 
		 * NUL terminated.
		 */
		char *    buf;

		/*
		 * The stream position of `buf[0]`. 
		 * Always zero for non streaming mode.
		 */
		int       base;

		/*
		 * size of buf. Used only in streaming mode.
		 */
		size_t    count; 
		size_t    cap; 
	} input;
};

struct regexp_vm * regexp_vm_create(struct regexp_program *prog)
{
	struct regexp_vm *vm = calloc(1, sizeof(struct regexp_vm));
	assert(vm != NULL);
	vm->prog = prog;
	return vm;
}

void regexp_vm_delete(struct regexp_vm *vm)
{
	if (vm->prog) 
		regexp_program_delete(vm->prog);
	if (vm->input.streaming)
		free(vm->input.buf);
	free(vm);
}

void regexp_vm_reset(struct regexp_vm *vm)
{
	vm->ip = 0;
	vm->sp = 0;
	vm->save_mask = 0;
	vm->status = REGEXP_VM_READY;
}

void regexp_vm_set_string_input(struct regexp_vm *vm, const char *s)
{
	if (vm->input.streaming) {
		if (vm->input.buf) {
			free(vm->input.buf);
		}
		vm->input.buf = 0;
		vm->input.streaming = 0;
		vm->input.count = 0;
		vm->input.cap = 0;
		vm->input.base = 0;
	}
	memset(vm->check_points, 0, sizeof(vm->check_points));
	memset(vm->save_pos, 0, sizeof(vm->save_pos));
	vm->input.buf = (char*)s;
	vm->input.pos = 0;
}

void 
regexp_vm_add_stream_input(struct regexp_vm *vm, const char *buf, size_t n)
{
	if (!vm->input.streaming) {
		assert(vm->input.cap == 0);
		assert(vm->input.count == 0);
		vm->input.streaming = 1;
		vm->input.buf = 0;
	}

	/*
	 * Initial State. No buffer is available.
	 */
	if (vm->input.cap == 0) {
		assert(vm->input.buf == NULL);
		size_t cap = 256;
		while (n > cap) {
			assert(cap * 2 > cap);
			cap *= 2;
		}
		vm->input.buf = malloc(cap);
		assert(vm->input.buf != NULL);
		vm->input.cap = cap;
		vm->input.count = n;
		strncpy(vm->input.buf, buf, n);
		return; // Done
	}

	if (vm->input.cap - vm->input.count < n) {
		int oldest_pos;

		// Can we discard some content to make room 
		if (vm->sp > 0) {
			oldest_pos = vm->stack[vm->sp-1].pos;
		} else {
			oldest_pos = vm->input.pos + vm->input.base;
		}

		assert(oldest_pos >= vm->input.base);
		oldest_pos -= vm->input.base;
		while (--oldest_pos > 0) {
			if ((vm->input.buf[oldest_pos] & 0xc0) != 0x80)
				break;
		}

		if (oldest_pos > 0) {
			memmove(vm->input.buf, 
				vm->input.buf + oldest_pos, 
				vm->input.count - oldest_pos);
			vm->input.pos -= oldest_pos;
			vm->input.count -= oldest_pos;
			vm->input.base += oldest_pos;
		}

		// Still no enough room?

		if (vm->input.cap - vm->input.count < n) {
			size_t cap = vm->input.cap;
			size_t size = vm->input.count + n;
			while (size > cap) {
				assert(cap * 2 > cap);
				cap *= 2;
			}
			vm->input.buf = realloc(vm->input.buf, cap);
			assert(vm->input.buf != NULL);
			vm->input.cap = cap;
		}
	}

	strncpy(vm->input.buf + (unsigned)vm->input.count, buf, n);
	vm->input.count+= n;
}

// TODO We should shift unused content out
void regexp_vm_set_current_pos(struct regexp_vm *vm, int pos)
{
	assert(pos >= vm->input.base);
	if (vm->input.streaming) {
		assert((unsigned)pos <= vm->input.count + vm->input.base);
		vm->input.pos = pos - vm->input.base;
	} else {
		vm->input.pos = pos;
	}
}

enum {
	REGEXP_INPUT_END = -1,
	REGEXP_INPUT_WAIT = -2,
	REGEXP_INPUT_ERROR = -3
};

/*
 * Return the next char in the input buffer and advance read head.
 * Return -1 if there is no more.
 * Return -2  if input is incomplete. (partial UTF8 sequence)
 * Return -3 if malformed utf8
 * FIXME: need to add utf8
 *
 */
static int _regexp_vm_getchar(struct regexp_vm *vm, bool advance)
{
	char *begin = 0;
	size_t n;

	assert(vm->input.buf);
	if (vm->input.streaming) {
		if ((unsigned)vm->input.pos < vm->input.count) {
			begin = vm->input.buf + vm->input.pos;
			n = vm->input.count - vm->input.pos;
		} else if (vm->input.ended) {
			return REGEXP_INPUT_END;
		} else {
			return REGEXP_INPUT_WAIT;
		}
	} else {
		begin = vm->input.buf + vm->input.pos;
		n = SIZE_MAX;
		if (*begin == 0)
			return REGEXP_INPUT_END;
	}

	assert(begin);
	char *end;
	int ch = Utf8_decode_buffer(begin, n, &end);
	if (ch < 0) {
		if (ch == -1) {
			return REGEXP_INPUT_ERROR;
		} else if (ch == -2) {
			return REGEXP_INPUT_WAIT;
		}
	}
	if (advance) {
		vm->input.pos += (end - begin);
	}

	return ch;
}

int regexp_vm_getchar(struct regexp_vm *vm)
{
	return _regexp_vm_getchar(vm, true);
}

int regexp_vm_peek(struct regexp_vm *vm)
{
	return _regexp_vm_getchar(vm, false);
}

void regexp_vm_unget(struct regexp_vm *vm)
{
	assert(vm->input.pos > 0);
	while (--vm->input.pos > 0) {
		if ((vm->input.buf[vm->input.pos] & 0xC0) != 0x80)
			break;
	}
}

int regexp_vm_peek_byte(struct regexp_vm *vm)
{
	assert(vm->input.buf);
	char *begin = 0;
	if (vm->input.streaming) {
		if ((unsigned)vm->input.pos < vm->input.count) {
			begin = vm->input.buf + vm->input.pos;
		} else if (vm->input.ended) {
			return REGEXP_INPUT_END;
		} else {
			return REGEXP_INPUT_WAIT;
		}
	} else {
		begin = vm->input.buf + vm->input.pos;
		if (*begin == 0)
			return REGEXP_INPUT_END;
	}
	assert(begin);
	return (uint8_t)*begin;
}


int regexp_vm_get_byte(struct regexp_vm *vm)
{
	int b = regexp_vm_peek_byte(vm);
	if (b >= 0) {
		vm->input.pos++;
	}
	return b;
}

int regexp_vm_get_byte_at_pos(struct regexp_vm *vm, int pos)
{
	assert(vm->input.buf);
	assert(vm->input.base <= pos);
	pos -= vm->input.base;
	// streaming mode
//	assert(pos < vm->input.count);
	return (uint8_t)vm->input.buf[pos];
}

int regexp_vm_get_current_pos(struct regexp_vm *vm)
{
	return vm->input.base + vm->input.pos;
}

/*
 * Return -1 if we are at the beginning of input.
 */
int regexp_vm_get_previous_char(struct regexp_vm *vm)
{
	int pos = vm->input.pos;
	if (pos <= 0)
		return -1;
	while (--pos > 0) {
		if ((vm->input.buf[pos] & 0xC0) != 0x80)
			break;
	}
	return vm->input.buf[pos];
}


///////////////////////////////////////////////////////////////////

static void 
regexp_disasm(int base, const struct regexp_inst *code, size_t n)
{
	for (int i = 0; i < (int)n; i++) {
		int op = code[i].op;
		int curr_ip = i + base;
		const char *name = regexp_op_info[op].name;

		printf("%04d: %-10s ", curr_ip, name);
		
		switch (op) {
		case REGEXP_OP_BACKREF:
		case REGEXP_OP_PROGRESS:
			printf("%d", code[i].a);
			break;
		case REGEXP_OP_SAVE:
			printf("%d", code[i].b.u16);
			break;

		case REGEXP_OP_CHAR: {
			int ch = code[i].b.u16;
			printf(isgraph(ch)? "%c" : "U+%04x", ch);
			break;
		}

		case REGEXP_OP_CHAR32: {
			int ch = code[i].b.u16;
			ch |= ((code[i+1].b.u16)<<16);
			printf("U+%08x", ch);
			break;
		}

		case REGEXP_OP_CCLASS:	
		case REGEXP_OP_NCCLASS: {
			int cid = code[i].a;
			if (cid < REGEXP_CCLASS_COUNT) {
				printf("%s", regexp_cclass_names[cid]);
			} else {
				printf("%d", cid);
			}
			break;
		}

		case REGEXP_OP_SPLIT1:
		case REGEXP_OP_SPLIT2:
			if (code[i].a) {
				printf("\\x%02x,%04d", code[i].a, 
					curr_ip + 1 + code[i].b.i16);
				break;
			}
			/* fall thru */

		case REGEXP_OP_JMP:
			printf("%04d", curr_ip + 1 + code[i].b.i16);
			break;

		default:
			break;
		}
		printf("\n");
	}
}

/*
 * Hit on a dead end.
 * Not a match, try to fallback
 */	
void 
regexp_vm_backtrack(struct regexp_vm *vm)
{
	if (vm->sp > 0) {
		vm->sp--;
		vm->input.pos = vm->stack[vm->sp].pos - vm->input.base;
		vm->ip = vm->stack[vm->sp].next_ip;
		return;
	}
	vm->status = REGEXP_VM_UNMATCH;
}

void 
regexp_vm_save_state(struct regexp_vm *vm, int curr_pos, int next_ip)
{
	const size_t max_stack_depth = sizeof(vm->stack)/sizeof(vm->stack[0]);
	if ((unsigned)vm->sp >= max_stack_depth) {
		fprintf(stderr, "regexp vm error: stack overflow\n");
		vm->status = REGEXP_VM_ERROR;
		return;
	}
	vm->stack[vm->sp].next_ip = next_ip;
	vm->stack[vm->sp].pos = curr_pos;
	vm->sp++;
}

/*
 * this function is designed to be able to call repeatedly.
 * it works with partial input, and can resume its work
 * when more data is available.
 * because we may fallback to previous positions,
 * the text should be pointing to the same string in a same session.
 * it's allowed to be reallocated.
 *
 * accepting inputs from s starting at `pos`,
 * and eat up to `len` bytes.
 *
 * if len == 0, then we should take it as an EOF.
 * EOF is acceptable for the branch table, `a*` can take EOF as a possible.
 * because EOF is 255, and that is not invalid in UTF8 string. 
 * so we use it as EOF.
 *
 * return  vm status.
 */
int 
regexp_vm_exec(struct regexp_vm *vm)
{
	struct regexp_program *prog = vm->prog;
	struct regexp_inst *code = prog->code.buf;

	while (vm->status == REGEXP_VM_READY) {
		//regexp_disasm(vm->ip, &prog->code.buf[vm->ip], 1);
		// should we use assert instead?
		if (vm->ip < 0 || (unsigned)vm->ip >= prog->code.count) {
			return (vm->status = REGEXP_VM_ERROR);
		}

		struct regexp_inst *inst = code + (vm->ip++);
		int ch;
		int op = inst->op;
		int action = regexp_op_info[op].action;
		switch (action) {
		case REGEXP_ACTION_GET:
			ch = regexp_vm_getchar(vm);
			break;
		case REGEXP_ACTION_PEEK:
			ch = regexp_vm_peek(vm);
			break;
		case REGEXP_ACTION_PEEKB:
			ch = regexp_vm_peek_byte(vm);
			break;
		default:
			ch = 0;
			break;
		}

		if (ch < 0) {
			if (ch == REGEXP_INPUT_WAIT) {
				vm->ip--;
				return (vm->status == REGEXP_VM_WAIT);
			} else if (ch == REGEXP_INPUT_ERROR) {
				vm->ip--;
				return (vm->status == REGEXP_VM_ERROR);
			}
		}
		
		switch (op) {
		/*
		 * we may not consumed all text
		 * The matched position was written in 
		 * save_pos[0..1]. the caller should examine 
		 * the match data and 
		 * make decision on should we continue to try 
		 * to match the rest
		 */
		case REGEXP_OP_MATCH: {
			int pos = regexp_vm_get_current_pos(vm);
			vm->save_pos[1] = pos;
			vm->save_mask |= 2;
			return (vm->status = REGEXP_VM_MATCH);
		}
			
		case REGEXP_OP_SAVE: {
			int i = inst->b.u16;
			int pos = regexp_vm_get_current_pos(vm);
			vm->save_mask |= (1 << i);
			vm->save_pos[i] = pos;
			break;
		}
		
		/*
		 * There are three types of beginning of line:
		 *
		 * \r\n\r\n
		 *     ^
		 * \r\r
		 *   ^
		 * \n\n
		 *   ^
		 *
		 */
		case REGEXP_OP_BOL: {
			int prev_ch = regexp_vm_get_previous_char(vm);
			if (prev_ch == -1 || prev_ch == '\n' || 
			    (prev_ch == '\r' && ch != '\n')) 
			{
				// Nothing
			} else {
				regexp_vm_backtrack(vm);
			}
			break;
		}
		
		case REGEXP_OP_EOL: {
			int prev_ch = regexp_vm_get_previous_char(vm);
			if (ch == -1 || ch == '\r' || 
			    (ch == '\n' && prev_ch != '\r'))
			{
				// Do nothing
			} else {
				regexp_vm_backtrack(vm);
			}
			break;
		}
		
		case REGEXP_OP_BOUNDARY: {
			int prev_ch = regexp_vm_get_previous_char(vm);
			int w0 = isword(prev_ch);
			int w1 = isword(ch);
			if (w0 == w1) {
				regexp_vm_backtrack(vm);
			}
			break;
		}
		
		case REGEXP_OP_NBOUNDARY: {
			int prev_ch = regexp_vm_get_previous_char(vm);
			int w0 = isword(prev_ch);
			int w1 = isword(ch);
			if (w0 != w1)
				regexp_vm_backtrack(vm);
			break;
		}
		
		case REGEXP_OP_CCLASS: {
			const struct regexp_cclass *p;
			int clsId = inst->a;

			p = regexp_program_get_cclass(prog, clsId);
			if (ch == -1 || !regexp_cclass_has(p, ch)) {
				regexp_vm_backtrack(vm);
			}
			break;
		}

		case REGEXP_OP_NCCLASS: {
			const struct regexp_cclass *p;
			int clsId = inst->a;

			p = regexp_program_get_cclass(prog, clsId);
			if (ch == -1 || regexp_cclass_has(p, ch)) {
				regexp_vm_backtrack(vm);
			}
			break;
		}

		case REGEXP_OP_CHAR: {
			if (ch != inst->b.u16) {
				regexp_vm_backtrack(vm);
			}
			break;
		}

		case REGEXP_OP_CHAR32: {
			int code_ch = inst->b.u16 + (inst[1].b.u16<<16);
			assert(inst[1].op == REGEXP_OP_NOP);
			vm->ip++;
			if (ch != code_ch) {
				regexp_vm_backtrack(vm);
			}
			break;
		}
			
		case REGEXP_OP_JMP:
			vm->ip += inst->b.i16;
			break;
			
		case REGEXP_OP_SPLIT1: {
			if (inst->a != 0 && inst->a != ch) 
				break;
			int curr_pos = regexp_vm_get_current_pos(vm);
			int split_ip = vm->ip + inst->b.i16;
			regexp_vm_save_state(vm, curr_pos, split_ip);
			break;
		}
		
		case REGEXP_OP_SPLIT2: {
			if (inst->a == 0 || inst->a == ch) {
				int curr_pos = regexp_vm_get_current_pos(vm);
				regexp_vm_save_state(vm, curr_pos, vm->ip);
			}
			vm->ip += inst->b.i16;
			break;
		}

		case REGEXP_OP_PROGRESS: {
			size_t curr_pos = regexp_vm_get_current_pos(vm);
			size_t last_pos = vm->check_points[inst->a];
			if (last_pos == curr_pos) {
				fprintf(stderr, 
					"%04d: infinite loop: %d\n", 
					vm->ip - 1, 
					(int)curr_pos);
				return (vm->status = REGEXP_VM_ERROR);
			}
			vm->check_points[inst->a] = curr_pos;
			break;
		}
		
		/*
		 * If active backref is not empty, consume
		 * incoming bytes.
		 */
		case REGEXP_OP_BACKREF: {
			// If this is a new backref
			if (vm->backref.begin == vm->backref.end) {
				int i = inst->a;
				vm->backref.begin = vm->save_pos[i*2];
				vm->backref.end = vm->save_pos[i*2+1];
				if (vm->backref.begin==vm->backref.end) {
					break;
				}
			}

			assert(vm->backref.begin < vm->backref.end);
			int b0 = regexp_vm_get_byte_at_pos(
				vm, vm->backref.begin
			);
			int b1 = regexp_vm_get_byte(vm);
			if (b1 == -2) {
				vm->ip--;
				return (vm->status = REGEXP_VM_WAIT);
			}
			if (b0 == b1) {
				vm->backref.begin++;
				if (vm->backref.begin!=vm->backref.end) 
				{
					vm->ip--;
				}
			} else {
				regexp_vm_backtrack(vm);
			}
			break;
		}
		
		default:
			assert(0);
			break;
		}
	}
	return vm->status;
}

int 
regexp_vm_get_match(struct regexp_vm *vm, int index, int *len)
{
	if (index > REGEXP_MAX_BACKREF)
		return -1;

	if (((vm->save_mask >> (index*2)) & 3) != 3)
		return -1;

	int begin = vm->save_pos[index*2  ];
	int end   = vm->save_pos[index*2+1];
	if (len)
		*len = end - begin;
	return begin;
}


////////////////////////////////////////////////////////////////////////

/*
 * Regular expression compiling context.
 */
struct regexp_comp {
	int                    subexpr_level;
	int                    opts;
	jmp_buf                kaboom;
	char                 **errmsg;
	const char            *origin;
	struct regexp_program *prog;
};

static const char* 
regexp_compile_alt(struct regexp_comp *ctx, const char *s);

static void 
regexp_compile_err(
	struct regexp_comp *ctx, 
	const char *src, 
	const char *fmt, 
	...)
{
	char buf[512] = "regexp compile error: ";
	char *p = buf;

	va_list args;

	va_start(args, fmt);
	p += strlen(buf);
	p += vsnprintf(p, sizeof(buf), fmt, args);
	assert(p - buf < 256);
	p += sprintf(p, "\n");
	va_end(args);

	if (src != NULL) {
		int pos = (int)(src - ctx->origin);
		p += sprintf(p, "> ");
		int i = 0;
		if (pos > 60) {
			i = pos - 57;
			p += sprintf(p, "...");
		}
		strncat(p, ctx->origin+i, 72);
		p += strlen(p);
		if (strlen(ctx->origin+i) > 72) {
			p += sprintf(p, "...\n> ");
		} else {
			p += sprintf(p, "\n> ");
		}
		for (int n = (pos > 60 ? 60 : pos); n > 0; n--) {
			*p++ = ' ';
		}
		sprintf(p, "^ %d\n", pos); 
	}
	
	fputs(buf, stderr);
	if (ctx->errmsg) {
		*ctx->errmsg = strdup(buf);
	}
		
	longjmp(ctx->kaboom, 1);
}

static struct regexp_inst*
regexp_emit(struct regexp_comp *ctx, int op)
{
	if (ctx->prog->code.count >= REGEXP_MAX_CODE) {
		regexp_compile_err(ctx, NULL, "too many instructions");
	}
	return regexp_program_add_inst(ctx->prog, op);
}

static struct regexp_inst*
regexp_emit_at(struct regexp_comp *ctx, int loc, int op)
{
	if (ctx->prog->code.count >= REGEXP_MAX_CODE) {
		regexp_compile_err(ctx, NULL, "too many instructions");
	}
	return regexp_program_insert_inst(ctx->prog, loc, op);
}

static void
regexp_emit_dup(struct regexp_comp *ctx, int n)
{
	if (ctx->prog->code.count + n >= REGEXP_MAX_CODE) {
		regexp_compile_err(ctx, NULL, "too many instructions");
	}
	regexp_program_dup(ctx->prog, n);
}

static void 
regexp_emit_cclass(struct regexp_comp *ctx, int classId)
{
	assert(classId >= 0 && classId < 256);
	regexp_emit(ctx, REGEXP_OP_CCLASS)->a = (uint8_t)classId;
}

static void 
regexp_emit_ncclass(struct regexp_comp *ctx, int classId)
{
	assert(classId >= 0 && classId < 256);
	regexp_emit(ctx, REGEXP_OP_NCCLASS)->a = (uint8_t)classId;
}

static void 
regexp_emit_char(struct regexp_comp *ctx, int ch)
{
	assert(ch >= 0);
	if (ch <= 0xffff) {
		regexp_emit(ctx, REGEXP_OP_CHAR)->b.u16 = (uint16_t)ch;
	} else {
		regexp_emit(ctx, REGEXP_OP_CHAR32)->b.u16 = (uint16_t)ch;
		regexp_emit(ctx, REGEXP_OP_NOP   )->b.u16 = (uint16_t)(ch>>16);
	}
}


static void 
regexp_emit_save(struct regexp_comp *ctx, int index)
{
	if (index > REGEXP_MAX_BACKREF*2+1)
		return;
	regexp_emit(ctx, REGEXP_OP_SAVE)->b.u16 = (uint8_t)index;
}

/*
 * Parse and compile a char class.
 * generate a CCLASS or NCCLASS op
 *
 * \B is the negated version of \b. \B matches at every position where \b
 * does not. Effectively, \B matches at any position between two word
 * characters as well as at any position between two non-word characters.
 */
static const char* 
regexp_compile_cclass(struct regexp_comp *ctx, const char *s)
{
	struct regexp_program *prog = ctx->prog;
	bool negated = false;
	bool ended = false;
	const char *begin = s;
	int clsId = regexp_program_alloc_cclass(prog);
	if (clsId > REGEXP_MAX_CCLASS) {
		regexp_compile_err(ctx, s, "too many character classes");
	}

	for (;!ended;s++) {
		switch (*s) {
		case '^':
			if (s == begin) {
				negated = true;
			} else {
				regexp_program_add_to_cclass(
					prog, clsId, '^');
			}
			break;
			
		case ']':
			if (s == begin) {
				regexp_program_add_to_cclass(
					prog, clsId, ']');
			} else {
				ended = true;
			}
			break;
			
		case '\\':
			switch (*++s) {
			case '/': case '\\': case '^': 
			case ']': case '-':
				regexp_program_add_to_cclass(
					prog, clsId, *s);
				break;
			case 'b':
				regexp_program_add_to_cclass(
					prog, clsId, '\b');
				break;
			case 'r':
				regexp_program_add_to_cclass(
					prog, clsId, '\r');
				break;
			case 'n':
				regexp_program_add_to_cclass(
					prog, clsId, '\n');
				break;
			case 't':
				regexp_program_add_to_cclass(
					prog, clsId, '\t');
				break;
			default:
				regexp_compile_err(ctx, s, 
					"unknown escaped char"
					" in c class");
				break;
			}
			break;
			
		case '\0':
			regexp_compile_err(ctx, s, "missing `]'");
			break;

		default: {
			char *end;
			int c1, c2;

			c1 = Utf8_decode(s, &end);
			if (*end == '-') {
				c2 = Utf8_decode(end + 1, &end);
				regexp_program_add_range_to_cclass(
					prog, clsId, c1, c2);
			} else {
				regexp_program_add_to_cclass(
					prog, clsId, c1);
			}
			s = end-1;
			break;
		}
		}
	}

	int op = negated ? REGEXP_OP_NCCLASS : REGEXP_OP_CCLASS;
	regexp_emit(ctx, op)->a = (uint8_t)clsId;
	return s;
}


/*
 * Save position if we are at the top level.
 */
static const char *
regexp_compile_subexpr(
	struct regexp_comp *ctx, 
	const char *s)
{
	struct regexp_program *prog = ctx->prog;
	
	if (ctx->subexpr_level++ == 0) {
		regexp_emit_save(ctx, prog->save_index++);
	}
				
	s = regexp_compile_alt(ctx, s);
	if (*s != ')') {
		regexp_compile_err(ctx, s, "missing `)'");
	}

	if (--ctx->subexpr_level == 0) {
		regexp_emit_save(ctx, prog->save_index++);
	}

	return ++s;
}


/*
 * Compile a single char or character class.
 * A group ( ... ) is also regarded as atom.
 * 
 * Outputs
 *   - min_length is the minimal string length that can be 
 *     matched by this primitive pattern.
 *   - bv specifies the leading bytes that can start this pattern.
 */
static const char *
regexp_compile_atom(
	struct regexp_comp *ctx, 
	const char *s)
{
	switch (*s) {
	case '\0': case '|': case ')': 
		return s;

	case '*': case '+': case '?':
	case '{': case '}': 
		regexp_compile_err(ctx, s, "unescaped char");
		break;

	case '^': 
		regexp_emit(ctx, REGEXP_OP_BOL);
		s++;
		break;
			
	case '$': 
		regexp_emit(ctx, REGEXP_OP_EOL);
		s++;
		break;
	
	case '(':
		s = regexp_compile_subexpr(ctx, ++s);
		break;

	case '[':
		s = regexp_compile_cclass(ctx, ++s);
		break;

	/*
	 * Could be escaped chars, or predefined character class
	 */
	case '\\': {
		switch (*++s) {
		case 'w': 
			regexp_emit_cclass(ctx, REGEXP_CCLASS_WORD);
			break;
				
		case 'W': 
			regexp_emit_ncclass(ctx, REGEXP_CCLASS_WORD);
			break;
			
		case 'd': 
			regexp_emit_cclass(ctx, REGEXP_CCLASS_DIGIT);
			break;
				
		case 'D': 
			regexp_emit_ncclass(ctx, REGEXP_CCLASS_DIGIT);
			break;
			
		case 's': 
			regexp_emit_cclass(ctx, REGEXP_CCLASS_SPACE);
			break;
				
		case 'S': 
			regexp_emit_ncclass(ctx, REGEXP_CCLASS_SPACE);
			break;
			
		case 'b': 
			regexp_emit(ctx, REGEXP_OP_BOUNDARY);
			break;
				
		case 'B': 
			regexp_emit(ctx, REGEXP_OP_NBOUNDARY);
			break;

		case 'n': 
			regexp_emit_char(ctx, '\n');
			break;
				
		case 't':
			regexp_emit_char(ctx, '\t');
			break;
				
		case 'r':
			regexp_emit_char(ctx, '\r');
			break;

		case '|': case '[': case ']': case '(': case ')':
		case '{': case '}': case '*': case '^': case '$':
		case '/': case '\\': case '.': case '?': case '+':
			regexp_emit_char(ctx, *s);
			break;

		case '1': case '2': case '3': case '4': case '5': 
		case '6': case '7': case '8': case '9': {
			char *endp = 0;
			int index = strtol(s, &endp, 10);
			if (index > REGEXP_MAX_BACKREF)
				regexp_compile_err(ctx, s, "backref too large");
			regexp_emit(ctx, REGEXP_OP_BACKREF)->a = (uint8_t)index;
			s = endp - 1;
			break;
		}

		case '\0':
			regexp_compile_err(ctx, s, "missing escaped char");
			break;

		default:
			regexp_compile_err(ctx, s, "unknown escaped char");
			break;
		}
		s++;
		break;
	}
		
	case '.': 
		regexp_emit_cclass(ctx, REGEXP_CCLASS_ANY);
		s++;
		break;

	default: {
		char *end;
		int ch = Utf8_decode(s, &end);
		if (ch >= 0) {
			regexp_emit_char(ctx, ch);
			s = end;
		} else {
			regexp_compile_err(ctx, s, "invalid utf8: %d", ch);
		}
		break;
	}}
	return s;
}

/*
 * Determine if the codes pointed by `ip` will at least consume
 * one character from input.
 */
static bool regexp_will_advance_pos(
	struct regexp_inst *ip,
	int len)
{
	for (int i = 0; i < len; i++) {
		switch (ip[i].op) {
		case REGEXP_OP_CHAR:
		case REGEXP_OP_CHAR32:
		case REGEXP_OP_CCLASS:
		case REGEXP_OP_NCCLASS:
			return true;

		// FIXME: A lazy implementation
		case REGEXP_OP_SPLIT1:
		case REGEXP_OP_SPLIT2:
		case REGEXP_OP_JMP:
			return false;
		default:
			break;
		}
	}
	return false;
}

static const char *
regexp_compile_rep(
	struct regexp_comp *ctx, 
	const char *s)
{
	struct regexp_program *prog = ctx->prog;
	int m, n;
	int start = (int)(prog->code.count);
	s = regexp_compile_atom(ctx, s);
	switch (*s) {
	case '*':
		m = 0;
		n = INT_MAX;
		s++;
		break;

	case '+':
		m = 1;
		n = INT_MAX;
		s++;
		break;

	case '?':
		m = 0;
		n = 1;
		s++;
		break;

	case '{': {
		char *endp = NULL;
		m = strtol(s+1, &endp, 10);
		if (*endp != ',') {
			regexp_compile_err(ctx, s, "invalid rep: m");
		}
		endp++;
		n = strtol(endp, &endp, 10);
		if (*endp != '}') {
			regexp_compile_err(ctx, s, "invalid rep: n");
		}
		if (m < 0 || n < m) {
			regexp_compile_err(ctx, s, "invalid rep: n < m");
		}
		s = endp + 1;
		break;
	}
	default:
		return s;
	}

	int greedy = true;
	if (*s == '?') {
		greedy = false;
		s++;
	}

	int len = (int)(prog->code.count) - start;
	struct regexp_inst *ip;
		
	if (n == INT_MAX) {
		// Infinite
		if (len == 0) {
			regexp_compile_err(ctx, s, "invalid infinite rep");
		}
		ip = &prog->code.buf[prog->code.count-len];
		int shouldCheck = !regexp_will_advance_pos(ip, len);

		if (m == 0) {
			/* 
			 * e* =>    L1: split L2
			 *              e
			 *              check
			 *              jmp L1
			 *          L2: 
			 */		
			regexp_emit_at(
				ctx, -len, greedy?REGEXP_OP_SPLIT1:REGEXP_OP_SPLIT2
			)->b.i16 = len + 1 + shouldCheck;
			if (shouldCheck) {
				regexp_emit(
					ctx, REGEXP_OP_PROGRESS
				)->a = prog->check_index++;
			}
			regexp_emit(
				ctx, REGEXP_OP_JMP
			)->b.i16 = -(len + 2 + shouldCheck);
		} else {
			/*    
			 * e+ =>    L1: <e>
			 *              check
			 *              split2  L1
			 *          L2:			
			 */
			assert(m == 1);
			if (shouldCheck) {
				regexp_emit(
					ctx, REGEXP_OP_PROGRESS
				)->a = prog->check_index++;
			}
			regexp_emit(
				ctx, greedy?REGEXP_OP_SPLIT2:REGEXP_OP_SPLIT1
			)->b.i16 = -(len + 1 + shouldCheck);
		}
	} else {
		// m is the minimal rep count
		// we need to have at least m instances
		// including the previous one
		for (int i = 1; i < m; i++) {
			regexp_emit_dup(ctx, len);
		}

		if (m == n) {
			if (m == 0) {
				regexp_program_pop(prog, len);
			}
		} else {
			if (m > 0) {
				n -= m;
				regexp_emit_dup(ctx, len);
			}
			regexp_emit_at(
				ctx, -len, greedy?REGEXP_OP_SPLIT1:REGEXP_OP_SPLIT2
			)->b.i16 = len;
			while (--n > 0) {
				regexp_emit_dup(ctx, len + 1);
			}
		}
	}
	
	return s;
}


/*
 * Compile simple concatenation.
 *
 * Compile up to `)` or `|` or end of file ('\0').
 * 
 * alt => cat => rep => atom => char,bol,eol,boundary,nboundary
 *                              cclass, ncclass
 *                              subexp ( alt )
 * rep has {m, n}.
 *   star : 0, INF
 *   plus : 1, INF
 *   quest: 0, 1
 */
static const char *
regexp_compile_cat(
	struct regexp_comp *ctx, 
	const char *s)
{
	while (*s && *s != ')' && *s != '|') {
		s = regexp_compile_rep(ctx, s);
	}
	return s;
}


/*
 * Compile alternations.
 *
 *   e1 | e2  ==>
 *     start:   
 *          SPLIT1 L1
 *          e1
 *          JMP L2
 *     L1:  e2
 *     L2:
 *
 *  L2 = codelen(e1) + 3
 *  L1 is zero, because the jump is offset
 * 
 * stops at `\0' or `)'.
 * return the position that match ends at,
 * pointing to the next char after the matched.
 *
 */
static const char* regexp_compile_alt(
	struct regexp_comp *ctx, 
	const char *s)
{
	struct regexp_program *prog = ctx->prog;
	// starting index of generated code for this expression
	int start = (int)prog->code.count; 
	
	const char *next = regexp_compile_cat(ctx, s);
	if (*next == '|') {
		int len = (int)(prog->code.count) - start;
		// inst is not persistent, it can go invalid
		regexp_emit_at(ctx, start, REGEXP_OP_SPLIT1)->b.i16 = len + 1;
		regexp_emit(ctx, REGEXP_OP_JMP);

		// Compile the right handside
		start = (int)prog->code.count;
		next = regexp_compile_alt(ctx, next+1);
		len = (int)(prog->code.count) - start;

		// Set the jmp offset, we can reserve the previous 
		// inst pointer, because the code buf could be reallocated
		prog->code.buf[start-1].b.i16 = len;
	} else if (*next && *next != ')') {
		regexp_compile_err(ctx, next, "unexpected char");
	}
	return next;
}

static void
regexp_compile_start(struct regexp_comp *ctx)
{
	/*
	 *  SPLIT2     +2
	 *  CCLASS     ALL
	 *  JMP        -3
	 */
	if (ctx->opts & REGEXP_COMPOPT_UNANCHORED) {
		regexp_emit(ctx, REGEXP_OP_SPLIT2)->b.i16 = 2;
		regexp_emit_cclass(ctx, REGEXP_CCLASS_ALL);
		regexp_emit(ctx, REGEXP_OP_JMP)->b.i16 = -3;
	}

	/* save index 0 and 1 is reserved for overall match */
	ctx->prog->save_index = 2;
	regexp_emit_save(ctx, 0);
	const char *s = regexp_compile_alt(ctx, ctx->origin);
	if (*s != 0) {
		regexp_compile_err(ctx, s, "unexpected char");
	}
	regexp_emit(ctx, REGEXP_OP_MATCH);
	regexp_program_optimize(ctx->prog);
}


struct regexp_program * 
regexp_compile(const char *s, int opts, char **errmsg)
{
	struct regexp_comp ctx = {0};
	struct regexp_program *prog;
	
	prog = calloc(1, sizeof(struct regexp_program));
	assert(prog);

	if (!setjmp(ctx.kaboom)) {
		ctx.prog = prog;
		ctx.opts = opts;
		ctx.origin = s;
		ctx.errmsg = errmsg;
		regexp_compile_start(&ctx);
	} else {
		/* Error occurs */
		regexp_program_delete(prog);
		prog = NULL;
	}
	return prog;
}

#ifdef REGEXP_MAIN
int main(int argc, char *argv[])
{
	bool is_grep = false;
	char buf[512];
	char patbuf[512];
	const char *pattern = NULL;
	const char *file = NULL;
	struct regexp_program *prog = 0;
	
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-grep") == 0) {
			is_grep = true;
		} else if (strcmp(argv[i], "-f") == 0) {
			file = argv[++i];
		} else {
			pattern = argv[i];
		}
	}
	
	if (!pattern) {
		printf("/");
		if (fgets(patbuf, sizeof(patbuf), stdin)) {
			char *p = strrchr(patbuf,'/');
			if (p) {
				pattern = patbuf;
				*p = 0;
			}
		}
	}

	if (pattern) {
		printf("/%s/\n", pattern);
		prog = regexp_compile(pattern, REGEXP_COMPOPT_UNANCHORED, NULL);
		if (prog == NULL) {
			exit(EXIT_FAILURE);
		}
		regexp_disasm(0, prog->code.buf, prog->code.count);
	} else {
		fprintf(stderr, "Usage: regcomp [-grep] pattern\n");
		exit(EXIT_FAILURE);
	}

	if (is_grep) {
		struct regexp_vm *vm = regexp_vm_create(prog);
		FILE *fp = stdin;
		
		if (file) {
			fp = fopen(file, "rb");
		}
		
		while (fgets(buf, sizeof(buf), fp)) {
			int len = (int)strlen(buf);
			regexp_vm_set_string_input(vm, buf);
			regexp_vm_reset(vm);
			regexp_vm_set_current_pos(vm, 0);
			int ret = regexp_vm_exec(vm);
			if (ret == REGEXP_VM_MATCH) {
				int len;
				int pos = regexp_vm_get_match(vm, 0, &len);
				printf("matched at %d, length is %d:", 
					pos, len);
				printf("%s", buf);
				break;
			} else if (ret == REGEXP_VM_ERROR) {
				exit(-1);
			}
		}
		
		if (fp != stdin && fp) {
			fclose(fp);
		}
		regexp_vm_delete(vm);
	}
	
	return 0;
}
#endif
