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
#include <math.h>
#include <float.h>
#include <ctype.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef WIN32
#include <io.h>
#define fileno(x) _fileno(x)
#define isatty(x) _isatty(x)
#define stat _stat
#else
#include <unistd.h>
#endif
#include "./lisp.h"

#define PROGNAME "lisp"
#define IOBUFSIZE 256 /* Port buffer size */
#define FILEIOBUFSIZE 1024 /* File Port buffer size */
#define TOKENBUFSIZE 256 /* Tokenizer buffer size */
#define INISTACKSIZE 512 /* Initial stack size */
#define INIPOOLSIZE 1024 /* Initial object pool size */
#define INISYMLISTSIZE 512 /* Initial symbols dictionary size */
#define INIFILELISTSIZE 64 /* Initial source files dictionary size  */
#define MAX_DEPTH    1000 /* Max nested levels for expression eval */
#define BLKSIZE 16  /* Min memory block size */
#define DTOA_BUFSIZE 32 /* dtoa() buffer size */
#define MAX_CACHED_OBJECT_SIZE 128 /* Max cachable memory block */
#define MAX_SYMBOL_LENGTH 127 /* Limit for parsing symbols in source */
#define DEBUG_TOKENIZER 0

#ifdef HAVE_COLORS
#define COLOR_HL(s)  ("\033[1m" s "\033[22m")
#define COLOR_RED(s) ("\33[31m" s "\33[0m")
#define COLOR_GREEN(s) ("\33[32m" s "\33[0m")
#else
#define COLOR_HL(s)  s
#define COLOR_RED(s) s
#define COLOR_GREEN(s) s
#endif
#ifdef _WIN32
#define snprintf _snprintf
#endif
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)>(b)?(b):(a))
// Windows always use little endian.
#ifdef _WIN32
#  define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__
#endif

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define memcpy_r(x,y,z) memcpy(x,y,z)
#elif __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
static void memcpy_r(uint8_t *dest, const uint8_t *src, size_t n)
{
	for (src += n; n > 0; n--)
		*dest++ = *--src;
}
#else
#  error "Unknown endianness"
#endif

typedef struct Lisp_SourceFile Lisp_SourceFile;
typedef struct Lisp_SourceMapping Lisp_SourceMapping;
typedef struct lisp_memblock_t lisp_memblock_t;

typedef enum {
	T_INVALID = 0, T_EOF,
	T_STRING, T_SYMBOL, T_NUMBER,
	T_DOT, T_LPAREN, T_RPAREN,
	T_LBRACKET, T_RBRACKET, T_LBRACE, T_RBRACE,
	T_QUOTE, T_QUASIQUOTE, T_UNQUOTE, T_UNQUOTE_SPLICING,
	T_AT, T_DOLLAR, T_CIRCUMFLEX, T_BUFFER, T_ARRAY_BEGIN, T_DICT_BEGIN,
	T_COLON, T_STRING_PART, T_COLON_COMPONENT
} Token_Type;

typedef enum {
  O_INVALID, O_BUFFER, O_PORT, O_SYMBOL, O_STRING, O_NUMBER, O_PAIR,
	O_ARRAY, O_DICT, O_ENV, O_PROC, O_NATIVE_PROC, O_MACRO,
	O_OBJECT_EX, O_STREAM,
	O_SOURCE_FILE, O_SOURCE_MAPPING, O_MAX
} Object_Type;

struct Lisp_Object {
	unsigned type        : 5;  
	unsigned marked      : 1; /* used in gc */
	unsigned is_const    : 1; /* evals to self. immutable. const symbol can't be redefined */
	unsigned is_primitive: 1; /* symbol is built-in primitive */
	unsigned is_special  : 1; /* symbol or procedure is a special form */
	unsigned tracing     : 1; /* procedure is being traced */
	unsigned is_method   : 1; /* procedure is callable method */
	unsigned is_list     : 1; /* pair is a list */
	unsigned tail_call   : 1; /* pair is a tail call */
	unsigned is_return   : 1; /* pair is a returning result */
	unsigned no_def      : 1; /* prohibit new definition in env */
};

struct Lisp_Buffer {
	Lisp_Object obj;
	unsigned char *buf;
	size_t length;
	size_t cap;
	Lisp_VM *vm;
};

struct Lisp_Stream {
    Lisp_Object obj;
    Lisp_VM *vm;
    struct lisp_stream_class_t *cls;
    void *context;
};

/*
  Lisp_Port is a bufferred stream reader or writer.
  LISP parser/printer reads and writes to ports.
 
  A port object contains a buffer to hold incoming or outgoing data.
  OUTPUT PORT Writes data to iobuf and then flush out to the stream
  using the stream interface write.
  INPUT PORT Reads data to iobuf from the stream using stream interface read.
 
  Line tracking is enabled for input port, so that we can generate
  informatic error messages. However, only input ports for
  scripts needs to track line and columns.
*/
struct Lisp_Port {
	Lisp_Object obj;
	Lisp_VM *vm; /* All mutable data type contains VM ptr */
	Lisp_String *name;
	Lisp_Buffer *iobuf;
	Lisp_Stream *stream; /* could be a filter */
	Lisp_SourceFile *src_file;
	size_t input_pos;  // getc pointer to iobuf. Input port only
	uint32_t line; // 1-based. Defined only in input port
	uint32_t src_pos; // 0-based. Only in input port.
	size_t byte_count; // Input/Output Bytes
	size_t max_output;
	unsigned isatty: 1; // file port only
	unsigned no_buf: 1; // for error output purpose
	unsigned out: 1; // is a output port.
	unsigned closed: 1; // port is closed
};

struct Lisp_Number {
	Lisp_Object obj;
	double value;
};

struct Lisp_Pair {
	Lisp_Object obj;
	Lisp_SourceMapping *mapping;
	Lisp_Object *car, *cdr;
};

struct Lisp_Array {
	Lisp_Object obj;
	Lisp_VM *vm;
	Lisp_Object **items;
	size_t cap, count;
};

struct Lisp_String {
	Lisp_Object obj;
	uint32_t hash;
	const char *buf;
	size_t length;
};

struct Lisp_Env {
	Lisp_Object obj;
	Lisp_Array *bindings; /* of type dict */
	struct Lisp_Env *parent;
};

typedef struct { // Procedure
	Lisp_Object obj;
	Lisp_Env *env;
	Lisp_Pair *lambda;
} Lisp_Proc;

typedef struct {
	Lisp_Object obj;
	Lisp_Env *env;
	Lisp_String *name;
	lisp_func fn;
} Lisp_Native_Proc;

struct Lisp_ObjectEx {
	Lisp_Object obj;
	Lisp_VM *vm;
	const lisp_object_ex_class_t *cls;
	void *ptr;
};

struct Lisp_SourceFile {
	Lisp_Object obj;
	Lisp_String *path;
	Lisp_Array *mappings;
};

struct Lisp_SourceMapping {
	Lisp_Object obj;
	Lisp_SourceFile *file;
	Lisp_Pair *expr;
	uint32_t begin, end;
	uint32_t line;
	uint32_t cnt;
};

// Lisp_VM -- Virtual Machine State
struct Lisp_VM {
	int eval_level;
	Lisp_VM *parent;
	Lisp_Env *env, *root_env;
	Lisp_Array *stack;
	Lisp_Array *pool;
	Lisp_Array *symbols; // dictionary of all dynamic symbols; reduce sym check to ptr comp
	Lisp_Array *source_files; // dictionary of all loaded files
	Lisp_Array *keep_alive_pool;
	Lisp_Port *input;
	Lisp_Port *output;
	Lisp_Port *error;
	Lisp_Object *last_eval;
	size_t memsize; // total allocated, excluding context itself
	uint32_t rand_next;
	Lisp_Buffer* token;
	Token_Type token_type;
	lisp_memblock_t *freelist[MAX_CACHED_OBJECT_SIZE/BLKSIZE];
	struct {
		uint32_t first_line, first_pos;
		uint32_t last_line, last_pos;
	} token_pos;
	int utf8_remain; /* For validating utf8 string/symbol */
	jmp_buf *catch;
	void *client;
	unsigned reading:1;/* Reading sexp from input */
	unsigned verbose: 1;
	unsigned debugging: 1;
	unsigned debug_enabled: 1;
	unsigned cov_trace: 1; /* Code Coverage */
};

/* A simple type checking to make sure x is an extended lisp object */
#define pushx(vm, x) lisp_push(vm, &((x)->obj))
#define push_num(vm, val) pushx(vm, lisp_number_new(vm,val))

/**
 ** Constants
 */

/* _SYM() -- Defining a symbol object */
#define _SYM(name, c, p, s) {.obj = { \
 .type = O_SYMBOL, \
 .marked = 1, \
 .is_const = c, \
 .is_primitive = p,\
 .is_special = s \
}, .buf = name, .length = sizeof(name)-1}

static Lisp_String _symtab[] = {
	/*** NAME ********************* C P S ** SYMBOL ID */
	_SYM("&key",                    1,0,0), // S_ARG_KEY
	_SYM("&label",                  1,0,0), // S_ARG_LABEL
	_SYM("&optional",               1,0,0), // S_ARG_OPTIONAL
	_SYM("&rest",                   1,0,0), // S_ARG_REST
	_SYM("*",                       0,1,0), // S_MUL
	_SYM("*dot*",                   1,0,0), // S_DOT
	_SYM("*eof*",                   1,0,0), // S_EOF
	_SYM("*expr-mark*",             1,0,0), // S_EXPR_MARK
	_SYM("*frame-mark*",            1,0,0), // S_FRAME_MARK
	_SYM("*mark*",                  1,0,0), // S_MARK
	_SYM("*stderr*",                1,0,0), // S_STDERR
	_SYM("*stdin*",                 1,0,0), // S_STDIN
	_SYM("*stdout*",                1,0,0), // S_STDOUT
	_SYM("+",                       0,1,0), // S_ADD
	_SYM("-",                       0,1,0), // S_SUB
	_SYM("/",                       0,1,0), // S_DIV
	_SYM("<",                       0,1,0), // S_NUMBER_LT
	_SYM("<=",                      0,1,0), // S_NUMBER_LE
	_SYM("=",                       0,1,0), // S_NUMBER_EQ
	_SYM(">",                       0,1,0), // S_NUMBER_GT
	_SYM(">=",                      0,1,0), // S_NUMBER_GE
	_SYM("abs",                     0,1,0), // S_ABS
	_SYM("acos",                    0,1,0), // S_ACOS
	_SYM("and",                     0,1,1), // S_AND
	_SYM("append",                  0,1,0), // S_APPEND
	_SYM("apply",                   0,1,0), // S_APPLY
	_SYM("array",                   0,1,0), // S_ARRAY
	_SYM("array->list",             0,1,0), // S_ARRAY_TO_LIST
	_SYM("array-count",             0,1,0), // S_ARRAY_COUNT
	_SYM("array-get",               0,1,0), // S_ARRAY_GET
	_SYM("array-pop!",              0,1,0), // S_ARRAY_POP
	_SYM("array-push!",             0,1,0), // S_ARRAY_PUSH
	_SYM("array-set!",              0,1,0), // S_ARRAY_SET
	_SYM("array?",                  0,1,0), // S_ARRAYP
	_SYM("asin",                    0,1,0), // S_ASIN
	_SYM("assoc",                   0,1,0), // S_ASSOC
	_SYM("atan",                    0,1,0), // S_ATAN
	_SYM("atom?",                   0,1,0), // S_ATOMP
	_SYM("begin",                   0,1,1), // S_BEGIN
	_SYM("boolean?",                0,1,0), // S_BOOLEANP
	_SYM("buffer->string",          0,1,0), // S_BUFFER_TO_STRING
	_SYM("buffer-append!",          0,1,0), // S_BUFFER_APPEND
	_SYM("buffer-getd",             0,1,0), // S_BUFFER_GETD
	_SYM("buffer-getf",             0,1,0), // S_BUFFER_GETF
	_SYM("buffer-geti16",           0,1,0), // S_BUFFER_GETI16
	_SYM("buffer-geti32",           0,1,0), // S_BUFFER_GETI32
	_SYM("buffer-geti8",            0,1,0), // S_BUFFER_GETI8
	_SYM("buffer-getu16",           0,1,0), // S_BUFFER_GETU16
	_SYM("buffer-getu32",           0,1,0), // S_BUFFER_GETU32
	_SYM("buffer-getu8",            0,1,0), // S_BUFFER_GETU8
	_SYM("buffer-set!",             0,1,0), // S_BUFFER_SET
	_SYM("buffer-setd!",            0,1,0), // S_BUFFER_SETD
	_SYM("buffer-setf!",            0,1,0), // S_BUFFER_SETF
	_SYM("buffer-seti16!",          0,1,0), // S_BUFFER_SETI16
	_SYM("buffer-seti32!",          0,1,0), // S_BUFFER_SETI32
	_SYM("buffer-seti8!",           0,1,0), // S_BUFFER_SETI8
	_SYM("buffer-setu16!",          0,1,0), // S_BUFFER_SETU16
	_SYM("buffer-setu32!",          0,1,0), // S_BUFFER_SETU32
	_SYM("buffer-setu8!",           0,1,0), // S_BUFFER_SETU8
	_SYM("car",                     0,1,0), // S_CAR
	_SYM("case",                    0,1,1), // S_CASE
	_SYM("catch",                   0,1,1), // S_CATCH
	_SYM("cdr",                     0,1,0), // S_CDR
	_SYM("ceil",                    0,1,0), // S_CEIL
	_SYM("char-at",                 0,1,0), // S_CHAR_AT
	_SYM("clear!",                  0,1,0), // S_CLEAR
	_SYM("clone",                   0,1,0), // S_CLONE
	_SYM("close",                   0,1,0), // S_CLOSE
	_SYM("concat",                  0,1,0), // S_CONCAT
	_SYM("cond",                    0,1,1), // S_COND
	_SYM("cons",                    0,1,0), // S_CONS
	_SYM("consq",                   0,1,1), // S_CONSQ
	_SYM("cos",                     0,1,0), // S_COS
	_SYM("current-input",           0,1,0), // S_CURRENT_INPUT
	_SYM("current-output",          0,1,0), // S_CURRENT_OUTPUT
	_SYM("date",                    0,1,0), // S_DATE
	_SYM("debug",                   0,1,1), // S_DEBUG
	_SYM("defconst",                0,1,1), // S_DEFCONST
	_SYM("define",                  0,1,1), // S_DEFINE
	_SYM("defmacro",                0,1,1), // S_DEFMACRO
	_SYM("defmethod",               0,1,1), // S_DEFMETHOD
	_SYM("dict",                    0,1,0), // S_DICT
	_SYM("dict->list",              0,1,0), // S_DICT_TO_LIST
	_SYM("dict-get",                0,1,0), // S_DICT_GET
	_SYM("dict-set!",               0,1,0), // S_DICT_SET
	_SYM("dict-unset!",             0,1,0), // S_DICT_UNSET
	_SYM("dict?",                   0,1,0), // S_DICTP
	_SYM("display",                 0,1,0), // S_DISPLAY
	_SYM("else",                    1,0,0), // S_ELSE
	_SYM("env?",                    0,1,0), // S_ENVP
	_SYM("eq?",                     0,1,0), // S_EQP
	_SYM("error",                   0,1,0), // S_ERROR
	_SYM("eval",                    0,1,0), // S_EVAL
	_SYM("evalq",                   0,1,1), // S_EVALQ
	_SYM("exists?",                 0,1,0), // S_EXISTS
	_SYM("exp",                     0,1,0), // S_EXP
	_SYM("false",                   1,0,0), // S_FALSE
	_SYM("find-file",               0,1,0), // S_FIND_FILE
	_SYM("floor",                   0,1,0), // S_FLOOR
	_SYM("flush",                   0,1,0), // S_FLUSH
	_SYM("format",                  0,1,0), // S_FORMAT
	_SYM("get",                     0,1,0), // S_GET
	_SYM("get-byte-count",          0,1,0), // S_GET_BYTE_COUNT
	_SYM("get-output-buffer",       0,1,0), // S_GET_OUTPUT_BUFFER
	_SYM("if",                      0,1,1), // S_IF
	_SYM("input-port?",             0,1,0), // S_INPUT_PORTP
	_SYM("integer?",                0,1,0), // S_INTEGERP
	_SYM("join",                    0,1,0), // S_JOIN
	_SYM("lambda",                  0,1,1), // S_LAMBDA
	_SYM("length",                  0,1,0), // S_LENGTH
	_SYM("let",                     0,1,1), // S_LET
	_SYM("list",                    0,1,0), // S_LIST
	_SYM("list?",                   0,1,0), // S_LISTP
	_SYM("load",                    0,1,0), // S_LOAD
	_SYM("load-path",               0,0,0), // S_LOAD_PATH
	_SYM("log",                     0,1,0), // S_LOG
	_SYM("make-buffer",             0,1,0), // S_MAKE_BUFFER
	_SYM("match",                   0,1,1), // S_MATCH
	_SYM("method?",                 0,1,0), // S_METHODP
	_SYM("mod",                     0,1,0), // S_MOD
	_SYM("new",                     0,1,0), // S_NEW
	_SYM("newline",                 0,1,0), // S_NEWLINE
	_SYM("not",                     0,1,0), // S_NOT
	_SYM("nth",                     0,1,0), // S_NTH
	_SYM("null?",                   0,1,0), // S_NULLP
	_SYM("number->string",          0,1,0), // S_NUMBER_TO_STRING
	_SYM("number?",                 0,1,0), // S_NUMBERP
	_SYM("open-input-buffer",       0,1,0), // S_OPEN_INPUT_BUFFER
	_SYM("open-input-file",         0,1,0), // S_OPEN_INPUT_FILE
	_SYM("open-output-buffer",      0,1,0), // S_OPEN_OUTPUT_BUFFER
	_SYM("open-output-file",        0,1,0), // S_OPEN_OUTPUT_FILE
	_SYM("or",                      0,1,1), // S_OR
	_SYM("output-port?",            0,1,0), // S_OUTPUT_PORTP
	_SYM("pair?",                   0,1,0), // S_PAIRP
	_SYM("print",                   0,1,0), // S_PRINT
	_SYM("println",                 0,1,0), // S_PRINTLN
	_SYM("procedure?",              0,1,0), // S_PROCEDUREP
	_SYM("pump",                    0,1,0), // S_PUMP
	_SYM("quasiquote",              0,1,1), // S_QUASIQUOTE
	_SYM("quote",                   0,1,1), // S_QUOTE
	_SYM("random",                  0,1,0), // S_RANDOM
	_SYM("random-seed",             0,1,0), // S_RANDOM_SEED
	_SYM("read",                    0,1,0), // S_READ
	_SYM("ready?",                  0,1,0), // S_READY
	_SYM("return",                  0,1,1), // S_RETURN
	_SYM("round",                   0,1,0), // S_ROUND
	_SYM("seek",                    0,1,0), // S_SEEK
	_SYM("set!",                    0,1,1), // S_SET
	_SYM("set-current-error!",      0,1,0), // S_SET_CURRENT_ERROR
	_SYM("set-current-input!",      0,1,0), // S_SET_CURRENT_INPUT
	_SYM("set-current-output!",     0,1,0), // S_SET_CURRENT_OUTPUT
	_SYM("sin",                     0,1,0), // S_SIN
	_SYM("slice",                   0,1,0), // S_SLICE
	_SYM("sort",                    0,1,0), // S_SORT
	_SYM("split",                   0,1,0), // S_SPLIT
	_SYM("sqrt",                    0,1,0), // S_SQRT
	_SYM("string->buffer",          0,1,0), // S_STRING_TO_BUFFER
	_SYM("string->number",          0,1,0), // S_STRING_TO_NUMBER
	_SYM("string->symbol",          0,1,0), // S_STRING_TO_SYMBOL
	_SYM("string-compare",          0,1,0), // S_STRING_COMPARE
	_SYM("string-find",             0,1,0), // S_STRING_FIND
	_SYM("string-find-backward",    0,1,0), // S_STRING_FIND_BACKWARD
	_SYM("string-length",           0,1,0), // S_STRING_LENGTH
	_SYM("string-quote",            0,1,0), // S_STRING_QUOTE
	_SYM("string?",                 0,1,0), // S_STRINGP
	_SYM("substring",               0,1,0), // S_SUBSTRING
	_SYM("symbol->string",          0,1,0), // S_SYMBOL_TO_STRING
	_SYM("symbol?",                 0,1,0), // S_SYMBOLP
	_SYM("system",                  0,1,0), // S_SYSTEM
	_SYM("tan",                     0,1,0), // S_TAN
	_SYM("this",                    0,1,0), // S_THIS
	_SYM("throw",                   0,1,0), // S_THROW
	_SYM("time",                    0,1,0), // S_TIME
	_SYM("trace",                   0,1,0), // S_TRACE
	_SYM("true",                    1,0,0), // S_TRUE
	_SYM("truncate",                0,1,0), // S_TRUNCATE
	_SYM("undefined",               1,0,0), // S_UNDEF
	_SYM("unquote",                 1,0,0), // S_UNQUOTE
	_SYM("unquote-splicing",        1,0,0), // S_UNQUOTE_SPLICING
	_SYM("untrace",                 0,1,0), // S_UNTRACE
	_SYM("with-input",              0,1,1), // S_WITH_INPUT
	_SYM("with-output",             0,1,1), // S_WITH_OUTPUT
	_SYM("write",                   0,1,0), // S_WRITE
	_SYM("write-buffer",            0,1,0), // S_WRITE_BUFFER
	_SYM("write-string",            0,1,0), // S_WRITE_STRING
};

enum SymID { /* Must follow the order in `_symtab' */
	S_ARG_KEY, S_ARG_LABEL, S_ARG_OPTIONAL, S_ARG_REST,
	S_MUL, S_DOT, S_EOF, S_EXPR_MARK, S_FRAME_MARK,
	S_MARK, S_STDERR, S_STDIN, S_STDOUT,
	S_ADD, S_SUB, S_DIV,
	S_NUMBER_LT, S_NUMBER_LE, S_NUMBER_EQ, S_NUMBER_GT,
	S_NUMBER_GE, S_ABS, S_ACOS, S_AND,
	S_APPEND, S_APPLY, S_ARRAY, S_ARRAY_TO_LIST,
	S_ARRAY_COUNT, S_ARRAY_GET, S_ARRAY_POP, S_ARRAY_PUSH,
	S_ARRAY_SET, S_ARRAYP, S_ASIN, S_ASSOC,
	S_ATAN, S_ATOMP, S_BEGIN, S_BOOLEANP, S_BUFFER_TO_STRING,
	S_BUFFER_APPEND, S_BUFFER_GETD, S_BUFFER_GETF, S_BUFFER_GETI16,
	S_BUFFER_GETI32, S_BUFFER_GETI8, S_BUFFER_GETU16, S_BUFFER_GETU32,
	S_BUFFER_GETU8, S_BUFFER_SET, S_BUFFER_SETD, S_BUFFER_SETF,
	S_BUFFER_SETI16, S_BUFFER_SETI32, S_BUFFER_SETI8, S_BUFFER_SETU16,
	S_BUFFER_SETU32, S_BUFFER_SETU8, S_CAR, S_CASE, S_CATCH, S_CDR,
	S_CEIL, S_CHAR_AT, S_CLEAR, S_CLONE, S_CLOSE,
	S_CONCAT, S_COND, S_CONS, S_CONSQ, S_COS,
	S_CURRENT_INPUT, S_CURRENT_OUTPUT, S_DATE, S_DEBUG, S_DEFCONST, S_DEFINE,
	S_DEFMACRO, S_DEFMETHOD, S_DICT, S_DICT_TO_LIST,
	S_DICT_GET, S_DICT_SET, S_DICT_UNSET, S_DICTP,
	S_DISPLAY, S_ELSE, S_ENVP, S_EQP, S_ERROR,
	S_EVAL, S_EVALQ, S_EXISTS, S_EXP, S_FALSE, S_FIND_FILE, S_FLOOR, S_FLUSH,
	S_FORMAT, S_GET, S_GET_BYTE_COUNT, S_GET_OUTPUT_BUFFER, S_IF, S_INPUT_PORTP,
	S_INTEGERP, S_JOIN, S_LAMBDA, S_LENGTH, S_LET,
	S_LIST, S_LISTP, S_LOAD, S_LOAD_PATH, S_LOG,
	S_MAKE_BUFFER, S_MATCH, S_METHODP, S_MOD, S_NEW, S_NEWLINE, S_NOT,
	S_NTH, S_NULLP, S_NUMBER_TO_STRING, S_NUMBERP,
	S_OPEN_INPUT_BUFFER, S_OPEN_INPUT_FILE, S_OPEN_OUTPUT_BUFFER, S_OPEN_OUTPUT_FILE,
	S_OR, S_OUTPUT_PORTP, S_PAIRP,
	S_PRINT, S_PRINTLN, S_PROCEDUREP, S_PUMP,
	S_QUASIQUOTE, S_QUOTE, S_RANDOM, S_RANDOM_SEED, S_READ, S_READYP, S_RETURN, S_ROUND,
	S_SEEK,S_SET, S_SET_CURRENT_ERROR, S_SET_CURRENT_INPUT, S_SET_CURRENT_OUTPUT, S_SIN,
	S_SLICE, S_SORT, S_SPLIT, S_SQRT, S_STRING_TO_BUFFER, S_STRING_TO_NUMBER,
	S_STRING_TO_SYMBOL, S_STRING_COMPARE, S_STRING_FIND, S_STRING_FIND_BACKWARD,
	S_STRING_LENGTH, S_STRING_QUOTE, S_STRINGP,
	S_SUBSTRING, S_SYMBOL_TO_STRING, S_SYMBOLP, S_SYSTEM, S_TAN,
	S_THIS, S_THROW, S_TIME, S_TRACE, S_TRUE,
	S_TRUNCATE, S_UNDEF, S_UNQUOTE,
	S_UNQUOTE_SPLICING, S_UNTRACE, S_WITH_INPUT, S_WITH_OUTPUT, S_WRITE,
	S_WRITE_BUFFER, S_WRITE_STRING,
	S_TOTAL
};

/* References to constant symbols */
#define LISP_QUOTE            (&_symtab[S_QUOTE].obj)
#define LISP_QUASIQUOTE       (&_symtab[S_QUASIQUOTE].obj)
#define LISP_UNQUOTE          (&_symtab[S_UNQUOTE].obj)
#define LISP_UNQUOTE_SPLICING (&_symtab[S_UNQUOTE_SPLICING].obj)
#define LISP_UNDEF            (&_symtab[S_UNDEF].obj)
#define LISP_TRUE             (&_symtab[S_TRUE].obj)
#define LISP_FALSE            (&_symtab[S_FALSE].obj)
#define LISP_EOF              (&_symtab[S_EOF].obj)
#define LISP_MARK             (&_symtab[S_MARK].obj)
#define LISP_EXPR_MARK        (&_symtab[S_EXPR_MARK].obj)
#define LISP_FRAME_MARK       (&_symtab[S_FRAME_MARK].obj)
#define LISP_DOT              (&_symtab[S_DOT].obj)

#define LISP_NIL              ((void*)(&_lisp_nil.obj)) 

#define SYMID(s) ((int)((s)-_symtab))
#define SYM(i)  (&_symtab[i])

/* NIL is a pair whose cdr points to itself
 * convenient for arguments checking
 * borrowed from pico lisp except our car is undefined
 */
static Lisp_Pair _lisp_nil = {
	{
		.type = O_PAIR,
		.marked = 1,
		.is_const = 1,
		.is_list = 1
	},
	0, LISP_UNDEF, LISP_NIL
};

Lisp_Object *lisp_nil = &_lisp_nil.obj;
Lisp_Object *lisp_true = LISP_TRUE;
Lisp_Object *lisp_false = LISP_FALSE;
Lisp_Object *lisp_undef = LISP_UNDEF;

static struct {
	const char *name;
	size_t size;
} objtypes[] = {
	[O_BUFFER] = {"BUFFER", sizeof(Lisp_Buffer)},
	[O_PORT]   = {"PORT", sizeof(Lisp_Port)},
	[O_SYMBOL] = {"SYMBOL", sizeof(Lisp_String)},
	[O_STRING] = {"STRING", sizeof(Lisp_String)},
	[O_NUMBER] = {"NUMBER", sizeof(Lisp_Number)},
	[O_PAIR]   = {"PAIR", sizeof(Lisp_Pair)},
	[O_ARRAY]  = {"ARRAY", sizeof(Lisp_Array)},
	[O_DICT]   = {"DICTIONARY", sizeof(Lisp_Array)},
	[O_ENV]    = {"ENVIRONMENT", sizeof(Lisp_Env)},
	[O_PROC]   = {"PROCEDURE", sizeof(Lisp_Proc)},
	[O_MACRO]  = {"MACRO", sizeof(Lisp_Proc)},
	[O_NATIVE_PROC] = {"NATIVE-PROCEDURE",sizeof(Lisp_Native_Proc)},
	[O_OBJECT_EX]   = {"OBJECT-EX",sizeof(Lisp_ObjectEx)},
	[O_STREAM]      = {"STREAM", sizeof(Lisp_Stream)},
	[O_SOURCE_FILE]      = {"SOURCE-FILE", sizeof(Lisp_SourceFile)},
	[O_SOURCE_MAPPING]   = {"SOURCE-MAPPING", sizeof(Lisp_SourceMapping)},
};

static void load(Lisp_VM *vm);

const char *lisp_object_type_name(Lisp_Object *o)
{
	return objtypes[o->type].name;
}

/**
 ** Memory Allocation
 */

/* Used for caching memory blocks */
struct lisp_memblock_t {
	struct lisp_memblock_t *next;
};

#define ROUND_BLOCK_SIZE(sz) (((sz) + (BLKSIZE-1)) & ~(BLKSIZE-1))

/* lisp_alloc -- Allocate a memory block
 * Just a thin wrapper over c lib call. 
 * Callers must lisp_free() manually. 
 * vm->memsize is updated to track memory usage.
 */
void *lisp_alloc(Lisp_VM *vm, size_t size)
{
	assert(size > 0);
	size = ROUND_BLOCK_SIZE(size);

	/* Small blocks are fetched from freelist.
	 * Making cons and env really cheap.
	 * Huge performance boost.
	 */
	if (size <= MAX_CACHED_OBJECT_SIZE) {
		size_t i = size / BLKSIZE - 1;
		if (vm->freelist[i]) {
			void *p = vm->freelist[i];
			vm->freelist[i] = vm->freelist[i]->next;
			memset(p, 0, size);
			return p;
		}
	}

	void *ptr = calloc(1, size);
	if (!ptr) {
		lisp_err(vm, "memory allocation failure");
		return NULL;
	}
	vm->memsize += size;
	return ptr;
}

void *lisp_realloc(Lisp_VM *vm, void *buf,
    size_t oldsize, size_t newsize)
{
	oldsize = ROUND_BLOCK_SIZE(oldsize);
	newsize = ROUND_BLOCK_SIZE(newsize);
	
	void *ptr = realloc(buf, newsize);
	if (!ptr) {
		lisp_err(vm, "memory realloc failure");
		return NULL;
	}
	vm->memsize -= oldsize;
	vm->memsize += newsize;
	return ptr;
}

/* Caller must provide size of memory to be free'd */
void lisp_free(Lisp_VM*vm, void *ptr, size_t size)
{
	size = ROUND_BLOCK_SIZE(size);
	/* Put the block on freelist if it's cachable */
	if (size <= MAX_CACHED_OBJECT_SIZE) {
		size_t i = size / BLKSIZE - 1;
		((lisp_memblock_t*)ptr)->next = vm->freelist[i];
		vm->freelist[i] = ptr;
	} else {
		assert(vm->memsize >= size);
		vm->memsize -= size;
		free(ptr);
	}
}

static void lisp_port_close(Lisp_Port*);
static void delete_obj(Lisp_VM *vm, Lisp_Object *obj)
{
	switch (obj->type) {
	case O_BUFFER: {
		Lisp_Buffer *b = (Lisp_Buffer*)obj;
		lisp_free(vm, b->buf, b->cap);
		break;
	}
	case O_ARRAY: case O_DICT: {
		Lisp_Array *a = (Lisp_Array*)obj;
		lisp_free(vm, a->items, sizeof(Lisp_Object*)*a->cap);
		break;
	}
	case O_STRING: case O_SYMBOL: {
		Lisp_String *s = (Lisp_String*)obj;
		lisp_free(vm, (void*)s->buf, s->length+1);
		break;
	}
	case O_PORT: {
		/* need to close stream if necessary.
		 * the IO buf is deleted later than port
		 */
		lisp_port_close((Lisp_Port*)obj);
		break;
	}
	case O_OBJECT_EX: {
		lisp_object_ex_finalize(vm, obj);
		break;
	}
    case O_STREAM: {
        lisp_stream_close((Lisp_Stream*)obj);
        break;
    }
	default: break;
	}
	lisp_free(vm, obj, objtypes[obj->type].size);
}

static void clone(Lisp_VM *vm, Lisp_Object *obj)
{
	switch (obj->type) {
	case O_NUMBER:
	{
		Lisp_Number *n = (Lisp_Number*)obj;
		lisp_push(vm, (Lisp_Object*)lisp_number_new(vm, n->value));
		break;
	}
	case O_STRING:
	{
		Lisp_String *s = (Lisp_String*)obj;
		lisp_push(vm, (Lisp_Object*)lisp_string_new(vm, s->buf, s->length));
		break;
	}
	case O_SYMBOL:
	{
		Lisp_String *s = (Lisp_String*)obj;
		if (SYMID(s) >= 0 && SYMID(s) < S_TOTAL)
			lisp_push(vm, obj);
		else
			lisp_make_symbol(vm, s->buf);
		break;
	}
	case O_BUFFER:
	{
		Lisp_Buffer *b = (Lisp_Buffer*)obj;
		lisp_push(vm, (Lisp_Object*)lisp_buffer_copy(vm, b->buf, b->length));
		break;
	}
	case O_PAIR:
	{
		if (obj == LISP_NIL) {
			lisp_push(vm, obj);
		} else {
			Lisp_Pair *p = (Lisp_Pair*)obj;
			int n = 0;
			do {
				clone(vm, p->car);
				p = (Lisp_Pair*)p->cdr;
				n++;
			} while (p->obj.type == O_PAIR && p != LISP_NIL);
			if (p == LISP_NIL)
				lisp_push(vm, LISP_NIL);
			else
				clone(vm, (Lisp_Object*)p);
			for (; n > 0; n--)
				lisp_cons(vm);
			break;
		}
		break;
	}
	default:
		lisp_err(vm, "Can not clone object of type %s", objtypes[obj->type].name);
		break;
	}
}


static void mark(void *_obj)
{
	assert(_obj); // catch bugs
	Lisp_Object *obj = _obj;
Loop:
	if (obj == NULL || obj->marked)
		return;
	obj->marked = 1;
	switch(obj->type) {
		case O_DICT: {
			Lisp_Array* a = (Lisp_Array*)obj;
			assert(a->count > 0);
			// Special treatment. First item is lookup array.
			// All of its inside will be marked later
			// so we don't want to go into it.
			if (a->items[0])
				a->items[0]->marked = 1;
			for (unsigned i = 1; i < a->count; i++)
				mark(a->items[i]);
			break;
		}
		case O_ARRAY: {
			Lisp_Array* a = (Lisp_Array*)obj;
			for (unsigned i = 0; i < a->count; i++)
				mark(a->items[i]);
			break;
		}
		case O_NATIVE_PROC:
			mark(((Lisp_Native_Proc*)obj)->env);
			mark(((Lisp_Native_Proc*)obj)->name);
			break;
		case O_PROC: case O_MACRO:
			mark(((Lisp_Proc*)obj)->env);
			mark(((Lisp_Proc*)obj)->lambda);
			break;
		case O_PAIR:
		{
			Lisp_Pair *p = (Lisp_Pair*)obj;
			if (p->mapping) mark(p->mapping);
			mark(p->car);
			obj = p->cdr;
			goto Loop;
		}
		case O_ENV: {
			Lisp_Env *env = (Lisp_Env*)obj;
			if (env->bindings) mark(env->bindings);
			if (env->parent) mark(env->parent);
			break;
		}
		case O_OBJECT_EX: {
			Lisp_ObjectEx *ex = (Lisp_ObjectEx*)obj;
			if (ex->ptr && ex->cls && ex->cls->mark) {
				ex->cls->mark(ex->ptr);
			}
			break;
		}
		case O_PORT: {
			Lisp_Port *p = (Lisp_Port*)obj;
			if (p->iobuf) mark(p->iobuf);
			if (p->name) mark(p->name);
			if (p->stream) mark(p->stream);
			break;
		}
		case O_STREAM: {
			Lisp_Stream *s = (Lisp_Stream*)obj;
			assert(s->cls);
			if (s->context && s->cls->mark)
				s->cls->mark(s->context);
			break;
		}
		case O_SOURCE_FILE:
		{
			Lisp_SourceFile *f = (Lisp_SourceFile*)obj;
			if (f->mappings) mark(f->mappings);
			if (f->path) mark(f->path);
			break;
		}
		case O_SOURCE_MAPPING:
		{
			Lisp_SourceMapping *m = (Lisp_SourceMapping*)obj;
			mark(m->file);
			if (m->expr) mark(m->expr);
			break;
		}
		default:
			break;
	}
}

void lisp_mark(Lisp_Object *o)
{
	mark(o);
}

static void gc(Lisp_VM *vm)
{
	unsigned i;
	for (i = 0; i < vm->pool->count; i++)
		vm->pool->items[i]->marked = 0;

	mark(vm->stack);
	mark(vm->env);
	mark(vm->root_env);
	mark(vm->input);
	mark(vm->output);
	mark(vm->error);
	mark(vm->token);
	mark(vm->last_eval);

	// TODO Optimize symbol table
	// Mark symbols last because we can know if some of them
	// are unsued. we should get rid of those
	mark(vm->symbols);
	mark(vm->source_files);
	mark(vm->keep_alive_pool);

	Lisp_Object **p = vm->pool->items;
	for (i = 0; i < vm->pool->count; i++) {
		Lisp_Object *obj = vm->pool->items[i];
		if (!obj->marked) {
			delete_obj(vm, obj);
		} else {
			*p++ = obj;
		}
	}
	vm->pool->count = (size_t)(p - vm->pool->items);
#if 0
	fprintf(stderr, "GC: %zd of %zd objects are deleted\n",
		vm->pool->cap - vm->pool->count, vm->pool->cap);
#endif
}



void lisp_array_push(Lisp_Array*, Lisp_Object*);
static void lisp_array_grow(Lisp_Array*);
static void *new_obj(Lisp_VM*vm, Object_Type type)
{
	Lisp_Object *o = lisp_alloc(vm, objtypes[type].size);
	o->type = type;
	if (vm->pool->count == vm->pool->cap) {
	  gc(vm);
	  if (vm->pool->count > vm->pool->cap / 2)
	    lisp_array_grow(vm->pool);
	}
	lisp_array_push(vm->pool, o);
	return o;
}

/*** Lisp_Buffer ***/

/*
 * Buffer has a minimal size of 64 bytes(512 bits), which is still cacheable,
 * and large enough to hold key information.
 */
Lisp_Buffer* lisp_buffer_new(Lisp_VM *vm, size_t cap)
{
	Lisp_Buffer *b = new_obj(vm, O_BUFFER);
	if (cap < 64) cap = 64;
	b->buf = lisp_alloc(vm, cap);
	b->cap = cap;
	b->vm = vm;
	return b;
}

Lisp_Buffer* lisp_push_buffer(Lisp_VM *vm, const void *data, size_t size)
{
	Lisp_Buffer *b = lisp_buffer_new(vm, size);
	lisp_push(vm, (Lisp_Object*)b);
	if (data) {
		lisp_buffer_add_bytes(b, data, size);
	}
	return b;
}

Lisp_Buffer *lisp_buffer_copy(Lisp_VM *vm, const void *data, size_t size)
{
	Lisp_Buffer *b = lisp_buffer_new(vm, size);
	if (data) {
		lisp_buffer_add_bytes(b, data, size);
	}
	return b;	
}

void lisp_buffer_set_size(Lisp_Buffer *b, size_t size)
{
	assert(size <= b->cap);
	b->length = size;
}

bool lisp_buffer_p(Lisp_Object* o)
{
	return o->type == O_BUFFER;
}

void lisp_buffer_grow(Lisp_Buffer *sb, size_t size)
{
	if (size > sb->cap) {
		size_t oldcap = sb->cap;
		while (size > sb->cap) {
		    sb->cap *= 2;
		}
		sb->buf = lisp_realloc(sb->vm, sb->buf, oldcap, sb->cap);
	}
}

void* lisp_buffer_bytes(Lisp_Buffer*b)
{
	return b->buf;
}

size_t lisp_buffer_cap(Lisp_Buffer*b)
{
	return b->cap;
}

size_t lisp_buffer_size(Lisp_Buffer*b)
{
	return b->length;
}

/* behavior is undefined if data is pointing to buffer bytes */
void lisp_buffer_add_bytes(Lisp_Buffer *b, const void *data, size_t size)
{
	assert(!b->obj.is_const);

	size_t newcap = b->cap;
	while (newcap < b->length + size) {
		newcap *= 2;
	}
	if (b->cap != newcap) {
		lisp_buffer_grow(b, newcap);
	}
	memcpy((char*)b->buf + b->length, data, size);
	b->length += size;
}

void lisp_buffer_add(Lisp_Buffer *b, int c)
{
	assert(!b->obj.is_const);
	if (b->length + 1>= b->cap) {
		lisp_buffer_grow(b, b->cap * 2);
	}
	b->buf[b->length++] = (char)c;
}

void lisp_buffer_add_byte(Lisp_Buffer *b, uint8_t value)
{
	assert(!b->obj.is_const);
	lisp_buffer_add(b, value);
}

void lisp_buffer_adds(Lisp_Buffer *b, const char *s)
{
	assert(!b->obj.is_const);
	for (; *s; s++)
		lisp_buffer_add(b, *s);
}

void lisp_buffer_shift(Lisp_Buffer *b, size_t n)
{
	assert(!b->obj.is_const);
	if (b->length <= n) {
		b->length = 0;
	} else {
		memmove(b->buf, b->buf+b->length-n, n);
		b->length-=n;
	}
}

/* Force memory cleared to zero. Necessary for security reasons. */
void lisp_buffer_clear(Lisp_Buffer *b)
{
	assert(!b->obj.is_const);
	memset(b->buf, 0, b->length);
	b->length = 0;
}

/*** Lisp_Stream ***/

Lisp_Stream *lisp_stream_new(Lisp_VM *vm, struct lisp_stream_class_t *cls, void *context)
{
    Lisp_Stream *s = new_obj(vm, O_STREAM);
    s->vm = vm;
    s->cls = cls;
    if (context) {
        s->context = context;
    } else if (cls->context_size>0) {
        s->context = lisp_alloc(vm, cls->context_size);
    }
    return s;
}

Lisp_Stream *lisp_push_stream(Lisp_VM *vm, struct lisp_stream_class_t *cls, void *context)
{
	Lisp_Stream *s = lisp_stream_new(vm, cls, context);
	pushx(vm, s);
	return s;
}


bool lisp_stream_p(Lisp_Object *o) { return o->type == O_STREAM; }

void *lisp_stream_context(Lisp_Stream*stream)
{
	return stream->context;
}

struct lisp_stream_class_t* lisp_stream_class(Lisp_Stream *stream)
{
	return stream->cls;
}

/*
 * Stream class can be updated, but you must make sure that the
 * new class is compatible with old class. It's usually used for modifying
 * the stream behavior on the fly.
 */
void lisp_stream_set_class(Lisp_Stream *stream, struct lisp_stream_class_t *cls)
{
	assert(stream != NULL);
	stream->cls = cls;
}

size_t lisp_stream_write(Lisp_Stream *stream, const void *buf, size_t size)
{
	assert(stream->cls && stream->cls->write);
	if (stream->context == NULL) // stream is closed (Useful check in GC)
		return 0;
	else
		return stream->cls->write(stream->context, buf, size);
}

size_t lisp_stream_read(Lisp_Stream *stream, void *buf, size_t size)
{
	assert(stream->cls && stream->cls->read && stream->context);
	return stream->cls->read(stream->context, buf, size);
}

/*  Shutdown stream and used memory */
void lisp_stream_close(Lisp_Stream *stream)
{
    assert(stream->cls);
    if (stream->context) {
        if (stream->cls->close)
            stream->cls->close(stream->context);
        if (stream->cls->context_size > 0)
            lisp_free(stream->vm, stream->context, stream->cls->context_size);
        stream->context = NULL;
    }
}

static size_t file_stream_read(void *_fp, void *buf, size_t size)
{
    return fread(buf, 1, size, (FILE*)_fp);
}

static size_t file_stream_gets(void *_fp, void *buf, size_t size)
{
    FILE *fp = _fp;
    if (!fgets((char*)buf, (int)size, fp)) {
        return 0;
    }
    return strlen(buf);
}

static size_t file_stream_write(void *_fp, const void *buf, size_t size)
{
    return fwrite(buf, 1, size, (FILE*)_fp);
}

static int file_stream_seek(void *_fp, long offset)
{
	return fseek((FILE*)_fp, offset, SEEK_SET);
}

static void file_stream_close(void *_fp)
{
    FILE *fp = _fp;
    if (fp != stdin && fp != stdout && fp != stderr)
        fclose(fp);
}

struct lisp_stream_class_t lisp_file_stream = {
    .read = file_stream_read,
    .write = file_stream_write,
    .close = file_stream_close,
    .seek = file_stream_seek
};

struct lisp_stream_class_t lisp_tty_input_stream = {
    .read = file_stream_gets
};

Lisp_Stream *lisp_fstream_new(Lisp_VM *vm, const char *path, const char *mode)
{
    if (path[0] == '*') {
        if (strcmp(path, "*stdin*") == 0) {
            if (isatty(fileno(stdin))) {
                return lisp_stream_new(vm, &lisp_tty_input_stream, stdin);
            } else {
                return lisp_stream_new(vm, &lisp_file_stream, stdin);
            }
        } else if (strcmp(path, "*stdout*") == 0) {
            return lisp_stream_new(vm, &lisp_file_stream, stdout);
        } else if (strcmp(path, "*stderr*") == 0) {
            return lisp_stream_new(vm, &lisp_file_stream, stderr);
        } else {
            lisp_err(vm, "invalid filename '%s'", path);
            return NULL;
        }
    }
	
    Lisp_Stream *stream = lisp_stream_new(vm, &lisp_file_stream, NULL);
    FILE *fp = fopen(path, mode);
    if (!fp) {
        lisp_err(vm, "can not open file '%s' with mode '%s'", path, mode);
        return NULL;
    }
    stream->context = fp;
    return stream;
}

////////////////////////////////////////
/// Lisp_Port
////////////////////////////////////////

Lisp_Port *lisp_make_port(Lisp_VM *vm, bool output)
{
	Lisp_Port *p = new_obj(vm, O_PORT);
	p->vm = vm;

	Lisp_Object *a = lisp_pop(vm, 1);
	assert(a->type == O_STREAM);
	p->stream = (Lisp_Stream*)a;
	if (output)
		assert(p->stream->cls && p->stream->cls->write);
	else
		assert(p->stream->cls && p->stream->cls->read);

	Lisp_Object *b = lisp_pop(vm, 1);
	assert(b->type == O_BUFFER);
	p->iobuf = (Lisp_Buffer*)b;

	pushx(vm, p);
	p->out = output;
	return p;
}

Lisp_Port *lisp_make_input_port(Lisp_VM *vm)
{
	return lisp_make_port(vm, false);
}

Lisp_Port *lisp_make_output_port(Lisp_VM *vm)
{
	return lisp_make_port(vm, true);
}

bool lisp_input_port_p(Lisp_Object *o)
{
    return o->type == O_PORT && !((Lisp_Port*)o)->out;
}

bool lisp_output_port_p(Lisp_Object *o)
{
    return o->type == O_PORT && ((Lisp_Port*)o)->out;
}

bool lisp_port_set_output_stream(Lisp_Port *port, Lisp_Stream *stream)
{
    port->stream = stream;
    port->out = 1;
    return true;
}

Lisp_Stream *lisp_port_get_stream(Lisp_Port *port)
{
	return port->stream;
}

bool lisp_port_set_input_stream(Lisp_Port *port, Lisp_Stream *stream)
{
    port->stream = stream;
    port->out = 0;
    return true;
}

Lisp_Buffer *lisp_port_get_buffer(Lisp_Port*port)
{
	return port->iobuf;
}

void *lisp_port_pending_bytes(Lisp_Port*p)
{
	assert(p->out == 0);
	return p->iobuf->buf + p->input_pos;
}

Lisp_Port* lisp_open_input_file(Lisp_VM *vm, Lisp_String* path)
{
	Lisp_Port *p = new_obj(vm, O_PORT);
	pushx(vm, p);
	p->stream = lisp_fstream_new(vm, path->buf, "rb");
	FILE *fp = p->stream->context;
	p->name = path;
	p->vm = vm;
	p->isatty = isatty(fileno(fp));
	p->iobuf = lisp_buffer_new(vm, FILEIOBUFSIZE);
	p->line = 1;
	lisp_pop(vm, 1);
	return p;
}

typedef enum {
	FILE_OUTPUT_TRUNCATE = 0,
	FILE_OUTPUT_APPEND = 1
} File_Output_Mode;

Lisp_Port* lisp_open_output_file(Lisp_VM *vm, Lisp_String*path, File_Output_Mode mode)
{
	Lisp_Port *p = new_obj(vm, O_PORT);
	pushx(vm, p);
	switch (mode) {
	case FILE_OUTPUT_TRUNCATE:
		p->stream = lisp_fstream_new(vm, path->buf, "wb");
		break;
	case FILE_OUTPUT_APPEND:
		p->stream = lisp_fstream_new(vm, path->buf, "ab");
		break;
	default:
		lisp_err(vm, "Invalid file output mode");
		break;
	}
	FILE *fp = p->stream->context;
	if (fp == stderr)
		p->no_buf = 1;
	p->vm = vm;
	p->isatty = isatty(fileno(fp));
	p->iobuf = lisp_buffer_new(vm, FILEIOBUFSIZE);
	p->out = 1;
	lisp_pop(vm, 1);
	return p;
}

Lisp_Port *lisp_open_output_buffer(Lisp_VM *vm, Lisp_Buffer *buffer)
{
	Lisp_Port *p = new_obj(vm, O_PORT);
	pushx(vm, p);
	p->vm = vm;
	p->iobuf = buffer?buffer:lisp_buffer_new(vm, IOBUFSIZE);
	p->out = 1;
	lisp_pop(vm, 1);
	return p;
}

Lisp_Port *lisp_open_input_buffer(Lisp_VM *vm, Lisp_Buffer *buffer, Lisp_String *name)
{
    Lisp_Port *p = new_obj(vm, O_PORT);
    p->name = name;
    p->vm = vm;
    p->iobuf = buffer;
    assert(buffer);
    return p;
}

Lisp_Port *lisp_open_input_string(Lisp_VM *vm, Lisp_String *data, Lisp_String *name)
{
    Lisp_Buffer *iobuf = lisp_buffer_new(vm, data->length);
    memcpy(iobuf->buf, data->buf, data->length);
    iobuf->length = data->length;
    pushx(vm, iobuf);
    Lisp_Port *p = lisp_open_input_buffer(vm, iobuf, name);
    lisp_pop(vm, 1);
    return p;
}

// Empty iobuf
// Note that we always assume that stream write works
// Because we don't want to throw an error in the middle of GC
// TODO what we can do is to mark it as error
void lisp_port_flush(Lisp_Port *port)
{
	assert(port->out);
	if (!port->stream || !port->iobuf || port->iobuf->length == 0)
		return;
	size_t n = 0;
	if (port->max_output > 0)
	{
		if (port->byte_count < port->max_output)
		{
			size_t m = port->max_output - port->byte_count;
			if (port->iobuf->length < m)
				m = port->iobuf->length;
			n = lisp_stream_write(port->stream,
				port->iobuf->buf, m);
		}
	}
	else
	{
		n = lisp_stream_write(port->stream,
			port->iobuf->buf, port->iobuf->length);
	}
	port->byte_count += n;
	port->iobuf->length = 0;
}

/* Empty input buffer. Useful for error recovering in tty mode */
void lisp_port_drain(Lisp_Port *port, size_t n)
{
	assert(!port->out);
	size_t avail = port->iobuf->length - port->input_pos;
	assert(avail <= port->iobuf->length);
	if (n < avail) {
		port->input_pos += n;
	} else if (port->stream) {
		port->iobuf->length = 0;
		port->input_pos = 0;
	} else {
		port->input_pos = port->iobuf->length;
	}
}

size_t lisp_port_fill(Lisp_Port *port)
{
	assert(!port->out && port->iobuf);
	if (port->stream) {
		if (port->input_pos >= port->iobuf->length) {
			size_t n = lisp_stream_read(port->stream,
				port->iobuf->buf, port->iobuf->cap);
			port->byte_count += n;
			port->iobuf->length = n;
			port->input_pos = 0;
			if (n == 0) {
				/* Do not close it, could reseek */
				//lisp_stream_close(port->stream);
				//port->stream = NULL;
				return 0;
			}
		}
	}
	return port->iobuf->length - port->input_pos;
}

/* Close port. Flush output and close stream. */
void lisp_port_close(Lisp_Port *port)
{
	if (port->closed)
		return;
	if (port->out) {
		lisp_port_flush(port);
	}
	if (port->stream) {
		lisp_stream_close(port->stream);
		port->stream = 0;
	}
	port->closed = 1;
}

int lisp_port_getc(Lisp_Port *port)
{
	assert(!port->out);
	if (port->input_pos >= port->iobuf->length) {
		if (lisp_port_fill(port) == 0)
			return EOF;
	}
	int c = (unsigned)port->iobuf->buf[port->input_pos++];
	if (c == '\n')
		port->line++;
	port->src_pos++;
	return c;
}

/* unget() must be paired with getc().
   `c' must be exactly what is returned by last getc(). */
void lisp_port_unget(Lisp_Port *port, int c)
{
	if (port->closed || c == EOF)
		return;
	assert(port->input_pos > 0);
	port->iobuf->buf[--port->input_pos] = c;
	if (c == '\n')
		port->line--;
	port->src_pos--;
}

int lisp_port_peekc(Lisp_Port *port)
{
	int c = lisp_port_getc(port);
	if (c != EOF) lisp_port_unget(port, c);
	return c;
}

void lisp_port_putc(Lisp_Port *port, int c)
{
	assert(port->out);
	if (!port->out || port->closed)
        return;
    if (port->iobuf->length >= port->iobuf->cap)
        lisp_port_flush(port);
	assert(port->iobuf->length < port->iobuf->cap);
	lisp_buffer_add_byte(port->iobuf, c);
	if (port->no_buf || c == '\n' || port->iobuf->length == port->iobuf->cap)
		lisp_port_flush(port);
}

// Buffered, unless there is a newline character.
// or the port has `no_buf` set.
void lisp_port_puts(Lisp_Port *port, const char *s)
{
	size_t n = 0;
	bool should_flush = false;
	for (; *s; s++, n++) {
		if (port->iobuf->cap == port->iobuf->length) {
			lisp_port_flush(port);
			should_flush = false;
		}
		lisp_buffer_add_byte(port->iobuf, (uint8_t)*s);
		if (*s == '\n')
			should_flush = true;
	}
	if (port->no_buf)
		should_flush = true;
	if (should_flush)
		lisp_port_flush(port);
}

void lisp_port_put_bytes(Lisp_Port *port, const void *data, size_t size)
{
	assert(port->out);
	if (port->stream) {
		if (port->iobuf->cap - port->iobuf->length >= size) {
			lisp_buffer_add_bytes(port->iobuf, data, size);
		} else {
			lisp_port_flush(port);
			size_t n = lisp_stream_write(port->stream, (const uint8_t*)data, size);
			port->byte_count += n;
		}
	} else {
		lisp_buffer_add_bytes(port->iobuf, (const uint8_t*)data, size);
	}
}

size_t lisp_buffer_vprintf(Lisp_Buffer *buf, const char *fmt, va_list _ap)
{
    char *p;
    size_t n, avail;
    va_list ap;
Retry:
    avail = buf->cap - buf->length;
    p = (char*)buf->buf + buf->length;
    va_copy(ap, _ap);
    n = vsnprintf(p, avail, fmt, ap);
    va_end(ap);
    if (n >= avail) {
        lisp_buffer_grow(buf, buf->cap + n - avail + 1);
        goto Retry;
    }
    buf->length += n;
    return buf->length;
}

size_t lisp_buffer_printf(Lisp_Buffer *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t size = lisp_buffer_vprintf(buf, fmt, ap);
    va_end(ap);
    return size;
}

static void lisp_port_vprintf(Lisp_Port *port, const char *fmt, va_list ap)
{
    lisp_buffer_vprintf(port->iobuf, fmt, ap);
    if (port->no_buf)
        lisp_port_flush(port);
}

void lisp_port_printf(Lisp_Port *port, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
    lisp_port_vprintf(port, fmt, ap);
	va_end(ap);
}

void lisp_port_print(Lisp_Port*,Lisp_Object*);

static void show_expr(Lisp_VM *vm, int i, Lisp_Pair *expr)
{
	lisp_port_printf(vm->error, "#%zu: ", i);
	if (expr->mapping && expr->mapping->file) {
		Lisp_String *name = (Lisp_String*)expr->mapping->file->path;
		FILE *fp = fopen(name->buf, "rb");
		if (fp) {
			char buf[128];
			long nleft = expr->mapping->begin;
			if (nleft > 40)
				nleft = 40;
			fseek(fp, expr->mapping->begin-nleft, SEEK_SET);
			int n = (int)fread(buf, 1, sizeof(buf)-1, fp);
			if (n > nleft) {
				buf[n] = 0;
				char *p = buf + nleft;
				while (p > buf && *p != '\n')
					p--;
				while (isspace(*p))
					p++;
				char *endp = p;
				while (*endp && *endp != '\n')
					endp++;
				*endp = 0;
				lisp_port_printf(vm->error, "%s:%d: %s\n",
					name->buf, expr->mapping->line, p);
			}
			fclose(fp);
			return;
		}
	}
	vm->error->max_output = vm->error->byte_count + 60;
	lisp_port_print(vm->error, (Lisp_Object*)expr);
	vm->error->max_output = 0;
	lisp_port_putc(vm->error, '\n');
}

/*
 * 1. Set the current position at the stack top.
 * 2. Start from current position of stack, run to the first *EXPR-MARK*,
 *    print the expression it's pointing to;
 * 3. Scan the stack until we find *FRAME-MARK*. Goto 2.
 * If no more *FRAME-MARK* is found, we are done.
 */
static void show_callstack(Lisp_VM *vm)
{
	size_t n = vm->stack->count;
	int i = 0;
	bool skipped = false;
	
	while (n > 0) {
		Lisp_Object *obj = vm->stack->items[--n];
		if (obj == LISP_EXPR_MARK) {
			i++;
			if (i < 20 || n < 500 || (vm->stack->count - n) < 500)
			{
				assert(n > 0);
				show_expr(vm, i, (Lisp_Pair*)vm->stack->items[n-1]);
			}
			else if (!skipped) {
				lisp_port_printf(vm->error, "...\n");
				skipped = true;
			}
			// Back track to FRAME MARK
			while (n > 0) {
				obj = vm->stack->items[--n];
				if (obj == LISP_FRAME_MARK)
					break;
			}
		}
	}
}

static void throw_error(Lisp_VM *vm, Lisp_Object *obj)
{
	// Display callstack even if this is going to be
	// caught by a catch handler.
	show_callstack(vm);
	
	if (vm->debug_enabled
	 && !vm->debugging
	 && !vm->reading) {
	 	vm->debugging = 1;
		Lisp_String *s = lisp_string_new(vm, "*stdin*", 7);
		pushx(vm, s);
		vm->input = lisp_open_input_file(vm, s);
		load(vm);
		vm->debugging = 0;
	}
	
	if (vm->debugging)
	{
		lisp_port_puts(vm->error, "Quiting debug mode due to new error\n");
		vm->debugging = 0;
	}
	
	assert(vm->catch);
	// FIXME could cause further errors
	// also we should include callstack and  error messages
	// in the thrown object.
	// unless we already has error
	// then we should throw FATAL.
	lisp_push(vm, (Lisp_Object*)SYM(S_ERROR));
	lisp_push(vm, obj? obj : lisp_nil);
	lisp_cons(vm);
	longjmp(*vm->catch, 1);
}

void lisp_err(Lisp_VM *vm, const char *fmt, ...)
{
	va_list ap;
	
	if (vm->error->isatty) {
		lisp_port_puts(vm->error, COLOR_RED("error: "));
	} else {
		lisp_port_puts(vm->error, "error: ");
	}
	
	// Show error location in batch reading mode
	// Perhaps we should show column number here
	if (vm->reading && !vm->input->isatty)
	{
		if (vm->input->src_file) {
			lisp_port_printf(vm->error, "file '%s': ",
				vm->input->src_file->path->buf);
		}
		lisp_port_printf(vm->error, "position %d: line %d: ",
			vm->token_pos.first_pos, vm->token_pos.first_line);
	}
	
	va_start(ap, fmt);
	lisp_port_vprintf(vm->error, fmt, ap);
	va_end(ap);
	
	lisp_port_putc(vm->error, '\n');
	throw_error(vm, LISP_NIL);
}

void lisp_puts(Lisp_VM *vm, const char *s)
{
	lisp_port_puts(vm->output, s);
}

void lisp_putc(Lisp_VM *vm, int c)
{
	lisp_port_putc(vm->output, c);
}

void lisp_printf(Lisp_VM *vm, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	lisp_port_vprintf(vm->output, fmt, ap);
	va_end(ap);
}

bool lisp_eq(Lisp_Object *a, Lisp_Object *b)
{
	if (a == b)
		return true;
	if (a->type != b->type) 
		return false;
	switch (a->type) {
		case O_NUMBER:
			return ((Lisp_Number*)a)->value == ((Lisp_Number*)b)->value;
		case O_STRING:
			return strcmp(((Lisp_String*)a)->buf, ((Lisp_String*)b)->buf)==0;
		case O_BUFFER:
			return ((Lisp_Buffer*)a)->length == ((Lisp_Buffer*)b)->length &&
			  memcmp(((Lisp_Buffer*)a)->buf, ((Lisp_Buffer*)b)->buf, ((Lisp_Buffer*)a)->length) == 0;
		default:
			return false;
	}
}

///////////////////////////////////////////////////
/// ATOMS -- Lisp_Number/Lisp_String/Lisp_Symbol
//  These are all immutable.
///////////////////////////////////////////////////

#define NUMVAL(o) (((Lisp_Number*)o)->value)

/*
 * 17 digits is all we need to recover exact bits in a double number.
 * However if we start with 17 digits, then (/ 5)
 * will be printed as 0.20000000000000001 instead of 0.2
 */
static void dtoa(double d, char s[DTOA_BUFSIZE])
{
	int n = snprintf(s, DTOA_BUFSIZE, "%.*g", DBL_DIG /* =15 */, d);
	assert(n > 0 && n < DTOA_BUFSIZE);
	volatile double t = strtod(s, NULL);
	if (t != d)
	{
		n = snprintf(s, DTOA_BUFSIZE, "%.*g", DBL_DIG+2, d);
		assert(n > 0 && n < DTOA_BUFSIZE);
	}
}

static bool str2dbl(const char *s, double *d)
{
	char *endp = NULL;
	*d = strtod(s, &endp);
	if (endp == NULL || *endp != 0) {
	   return false;
	}
	return true;
}

Lisp_Number *lisp_number_new(Lisp_VM *vm, double val)
{
	Lisp_Number *n = new_obj(vm, O_NUMBER);
	n->obj.is_const = 1;
	n->value = val;
	return n;
}

Lisp_Number *lisp_push_number(Lisp_VM *vm, double value)
{
	Lisp_Number *n = lisp_number_new(vm, value);
	lisp_push(vm, (Lisp_Object*)n);
	return n;
}

double lisp_number_value(Lisp_Number*n) { return n->value; }

static bool is_integer(double d)
{
	return d == round(d);
}

bool lisp_integer_p(Lisp_Object *o)
{
	return o->type == O_NUMBER && is_integer(NUMVAL(o));
}

bool lisp_number_p(Lisp_Object *o) { return o->type == O_NUMBER; }
bool lisp_string_p(Lisp_Object *o) { return o->type == O_STRING; }
bool lisp_symbol_p(Lisp_Object *o) { return o->type == O_SYMBOL; }
bool lisp_pair_p(Lisp_Object *o) { return o->type == O_PAIR; }

/* Return the position where encoding error is found.
   Return -1 if no error is found. */
static int check_utf8(const char *s, size_t n)
{
	int remain = 0;
	int i = 0;
	for (; (unsigned)i < n; i++) {
		int c = (unsigned char)s[i];
		if (remain > 0) {
			if ((c & ~0x3f) != 0x80)
				return i;
			remain--;
		} else if ((c >> 7) == 0) { // ASCII
			assert(remain==0);
		} else if ((c >> 6) == 2) { // 0x10xxxxxx
			return i;
		} else if ((c >> 5) == 6) { // 0x110xxxxx
			remain = 1;
		} else if ((c >> 4) == 0xe) { // 0x1110xxxx
			remain = 2;
		} else if ((c >> 3) == 0x1e) { // 0x11110xxx
			remain = 3;
		} else {
			return i;
		}
	}
	return remain == 0 ? -1 : i;
}

Lisp_String *lisp_string_new(Lisp_VM *vm, const char *buf, size_t length)
{
	Lisp_String *s = new_obj(vm, O_STRING);
	s->obj.is_const = 1;
	char *t = lisp_alloc(vm, length+1);
	if (buf) {
		strncpy(t, buf, length);
		t[length] = 0;
	}
	s->buf = t;
	s->length = length;
	return s;
}

Lisp_String *lisp_push_string(Lisp_VM *vm, const char *buf, size_t length)
{
	Lisp_String *s = lisp_string_new(vm, buf, length);
	lisp_push(vm, (Lisp_Object*)s);
	return s;
}

Lisp_String *lisp_push_string_from_buffer(Lisp_VM *vm, Lisp_Buffer *b)
{
	return lisp_push_string(vm, (char*)b->buf, b->length);
}

Lisp_String *lisp_push_cstr(Lisp_VM *vm, const char *buf)
{
	return lisp_push_string(vm, buf, strlen(buf));
}

const char *lisp_string_cstr(Lisp_String*s) { return s->buf; }

bool lisp_string_equal(Lisp_String* a, Lisp_String *b)
{
	if (a == b) return true;
	if (a->length == b->length)
		if (strcmp(a->buf, b->buf) == 0)
			return true;
	return false;
}

int lisp_string_compare(Lisp_String* a, Lisp_String *b)
{
	return strcmp(a->buf, b->buf);
}

size_t lisp_string_length(Lisp_String *s)
{
	return s->length;
}

// Reference: http://www.cse.yorku.ca/~oz/hash.html
static uint32_t hash_cstr(const char *s)
{
	uint32_t hash = 0;
	int c;

	while ((c = *s++))
		hash = c + (hash << 6) + (hash << 16) - hash;

	return hash;
}

static uint32_t lisp_string_hash(Lisp_String *a)
{
	if (a->hash == 0)
		a->hash = hash_cstr(a->buf);
	return a->hash;
}

static Lisp_String *find_sym(Lisp_String table[], size_t n, const char *name)
{
	size_t low = 0, high = n;
	while (low < high) {
 		size_t mid = low + (high-low)/2;
 		int cmp = strcmp(table[mid].buf, name);
 		if (cmp == 0) return table + mid;
 		else if (cmp < 0) low = mid + 1;
 		else high = mid;
 	}
	return NULL;
}

Lisp_String* lisp_symbol_new(Lisp_VM *vm, const char *name, size_t length)
{
	Lisp_String *t = lisp_string_new(vm, name, length);
	t->obj.type = O_SYMBOL;
	t->obj.is_const = 0;
	return t;
}

Lisp_Object* lisp_make_object_ex(Lisp_VM *vm, const lisp_object_ex_class_t *cls)
{
	Lisp_ObjectEx *ex = new_obj(vm, O_OBJECT_EX);
	pushx(vm, ex);
	ex->cls = cls;
	ex->vm = vm;
	if (cls->size > 0)
	{
		ex->ptr = lisp_alloc(vm, cls->size);
	}
	return (Lisp_Object*)ex;
}

const lisp_object_ex_class_t *lisp_object_ex_class(Lisp_Object *obj)
{
	if (obj->type != O_OBJECT_EX) return NULL;
	return ((Lisp_ObjectEx*)obj)->cls;
}

void *lisp_object_ex_ptr(Lisp_Object* obj)
{
	if (obj->type != O_OBJECT_EX) return NULL;
	return ((Lisp_ObjectEx*)obj)->ptr;
}

void lisp_object_ex_set_ptr(Lisp_Object* obj, void *ptr)
{
	if (obj->type != O_OBJECT_EX) return;
	Lisp_ObjectEx *ex = (Lisp_ObjectEx*)obj;
	assert(ex->cls && ex->cls->size == 0);
	ex->ptr = ptr;
}

void lisp_object_ex_finalize(Lisp_VM *vm, Lisp_Object* obj)
{
	if (obj->type != O_OBJECT_EX)
		return;
	Lisp_ObjectEx *ex = (Lisp_ObjectEx*)obj;
	assert(vm == ex->vm);
	if (ex->ptr && ex->cls)
	{
		if (ex->cls->finalize)
			ex->cls->finalize(vm, ex->ptr);
		if (ex->cls->size > 0)
			lisp_free(vm, ex->ptr, ex->cls->size);
		ex->ptr = NULL;
	}
}

//////////////////////////////////////
/// Lisp_Array
//////////////////////////////////////

// Treat the object pool differently, since it's
// the first object we have ever built
Lisp_Array *lisp_pool_new(Lisp_VM *vm, size_t cap)
{
	Lisp_Array *a = lisp_alloc(vm, sizeof(Lisp_Array));
	a->obj.type = O_ARRAY;
	a->items = lisp_alloc(vm, sizeof(Lisp_Object*)*cap);
	a->cap = cap;
	a->vm = vm;
	return a;
}

Lisp_Array *lisp_array_new(Lisp_VM *vm, size_t cap)
{
	Lisp_Array *a = new_obj(vm, O_ARRAY);
	a->items = lisp_alloc(vm, sizeof(Lisp_Object*)*cap);
	a->cap = (cap > 4 ? cap : 4);
	a->vm = vm;
	return a;
}

Lisp_Array *lisp_array_copy(Lisp_VM *vm, Lisp_Array*a, int begin, size_t count)
{
	if (begin < 0 || begin + count > a->count)
		lisp_err(vm, "copy array: invalid range");
	Lisp_Array *t = new_obj(vm, O_ARRAY);
	size_t cap = count;
	if (cap < 4) cap = 4;
	t->items = lisp_alloc(vm, sizeof(Lisp_Object*)*cap);
	memcpy(t->items, a->items + begin, sizeof(Lisp_Object*)*count);
	t->count = count;
	t->cap = cap;
	t->vm = vm;
	return t;
}

static void lisp_array_grow(Lisp_Array *a)
{
	size_t sz = a->cap * sizeof(Lisp_Object*);
	a->items = lisp_realloc(a->vm, a->items, sz, sz*2);
	a->cap *= 2;
}

void lisp_array_push(Lisp_Array *a, Lisp_Object *obj)
{
	assert(obj && obj->type > 0 && obj->type < O_MAX);
	if (a->count == a->cap)
		lisp_array_grow(a);
	a->items[a->count++] = obj;
}

Lisp_Object* lisp_array_pop(Lisp_Array *a, int n)
{
	assert(a->count >= (unsigned)n && a->count > 0);
	Lisp_Object *obj = a->items[a->count - 1];
	a->count -= n;
	return obj;
}

void lisp_push(Lisp_VM *vm, Lisp_Object* obj)
{
	lisp_array_push(vm->stack, obj);
}

Lisp_Object* lisp_pop(Lisp_VM *vm, int n)
{
	return lisp_array_pop(vm->stack, n);
}

Lisp_Object* lisp_top(Lisp_VM *vm, int n)
{
	assert(vm->stack->count >= (unsigned)n+1 && vm->stack->count > 0);
	return vm->stack->items[vm->stack->count-n-1];
}

/* Exchange the top 2 items */
void lisp_exch(Lisp_VM *vm)
{
	assert(vm->stack->count > 1);
	Lisp_Object** top = vm->stack->items + vm->stack->count - 2;
	Lisp_Object *t = top[0];
	top[0] = top[1];
	top[1] = t;
}

/* shift(n) equals to push(pop(n))
 * Removing the top N items but then put back the top most item.
 * Then the bottom object before shifting is returned.
 */
Lisp_Object* lisp_shift(Lisp_VM *vm, int n)
{
	assert(vm->stack->count >= (unsigned)n);
	assert(n > 0);
	size_t pos = vm->stack->count - n;
	Lisp_Object *o = vm->stack->items[pos];
	vm->stack->items[pos] = vm->stack->items[vm->stack->count-1];
	vm->stack->count = pos + 1;
	return o;
}

static int compare_object(const void *_a, const void *_b)
{
	Lisp_Object *a = *(Lisp_Object**)_a, *b = *(Lisp_Object**)_b;
	if (a->type == b->type) {
		if (a->type == O_STRING || a->type == O_SYMBOL) {
			return lisp_string_compare((Lisp_String*)a, (Lisp_String*)b);
		} else if (a->type == O_NUMBER) {
			if (NUMVAL(a) < NUMVAL(b)) return -1;
			else if (NUMVAL(a) > NUMVAL(b)) return 1;
			else return 0;
		} else {
			if (a < b) return -1;
			else if (a > b) return 1;
			else return 0;
		}
	} else {
		return (int)a->type - (int)b->type;
	}
}

void lisp_array_sort(Lisp_Array *a, size_t startIndex, size_t count)
{
	assert(startIndex < a->count && count <= a->count && startIndex+count <= a->count);
	qsort(a->items+startIndex, count, sizeof(Lisp_Object*), compare_object);
}

//////////////////////////////////////
/// Lisp_Pair
//////////////////////////////////////

#define cons(vm, a,b) ((Lisp_Object*)lisp_pair_new(vm,(a),(b)))

// Type checking should be done in caller
// Arguments is guareented to be a list
// so these macros are same on the arguments
#define CAR(p)   (((Lisp_Pair*)(p))->car)
#define CDR(p)   (((Lisp_Pair*)(p))->cdr)
#define CADR(p)  (((Lisp_Pair*)CDR(p))->car)
#define CDDR(p)  (((Lisp_Pair*)CDR(p))->cdr)
#define REST(p)  ((Lisp_Pair*)CDR(p))

Lisp_Pair *lisp_pair_new(Lisp_VM *vm, Lisp_Object *car, Lisp_Object *cdr)
{
	Lisp_Pair *p = new_obj(vm, O_PAIR);
	p->car = car;
	p->cdr = cdr;
	return p;
}

// LISP_NIL always has is_list set.
// Lisp_Pair is not entirely immutable data structure, since
// we use it in dict bindings. so we can't cache is_list bit here.
// unless we can guarantee that bindings won't be tested for is_list
static bool is_list(Lisp_Object* o)
{
	while (o->type == O_PAIR && !o->is_list)
		o = ((Lisp_Pair*)o)->cdr;
	return o->is_list;
}

Lisp_Object *lisp_car(Lisp_Pair*p) { return p->car; }
Lisp_Object *lisp_cdr(Lisp_Pair*p) { return p->cdr; }

Lisp_Object *lisp_nth(Lisp_Pair *p, int index)
{
	for (; p != LISP_NIL; p = (Lisp_Pair*)p->cdr, index--) {
		if (index == 0)
			return p->car;
		if (p->cdr->type != O_PAIR)
			break;
	}
	return LISP_UNDEF;
}

#define make_pair(vm) lisp_cons(vm)
Lisp_Pair *lisp_cons(Lisp_VM *vm)
{
	assert(vm->stack->count>=2);
	Lisp_Object **t = vm->stack->items + vm->stack->count - 2;
	t[0] = (Lisp_Object*)lisp_pair_new(vm, t[0], t[1]);
	vm->stack->count--;
	return (Lisp_Pair*)t[0];
}

void lisp_make_list(Lisp_VM *vm, int n)
{
	lisp_push(vm, LISP_NIL);
	for (; n > 0; n--)
		make_pair(vm);
}

static int push_list(Lisp_VM *vm, Lisp_Pair *l)
{
	int n = 0;
	for (; l != LISP_NIL; l = (Lisp_Pair*)l->cdr, n++) {
		if (l->obj.type == O_PAIR) {
			lisp_push(vm, l->car);
		} else {
			lisp_err(vm, "not a list");
		}
	}
	return n;
}

static void append(Lisp_VM *vm, Lisp_Pair *l, Lisp_Object *o)
{
	int n = 0;
	for (; l != LISP_NIL; l = (Lisp_Pair*)l->cdr, n++) {
		assert(l->obj.type == O_PAIR);
		lisp_push(vm, l->car);
	}
	lisp_push(vm, o);
	for (; n > 0; n--)
		make_pair(vm);
}

Lisp_Pair *lisp_assoc(Lisp_Pair *l, Lisp_Object *k)
{
	while (l != LISP_NIL) {
		if (CAR(l)->type == O_PAIR) {
			if (lisp_eq(k, CAR(CAR(l)))) {
				return (Lisp_Pair*)CAR(l);
			}
		}
		if (CDR(l)->type == O_PAIR)
			l = REST(l);
		else
			break;
	}
	return NULL;
}

/////////////////////////////////////////
/// Dictionary
//
//  The first element is reserved for lookup array.
//  variable binding pairs start from 1th location.
/////////////////////////////////////////

// If dictionary contains less than this number of items,
// do brute force searching instead of hash.
// Since our key comparison is plain pointer comparsion,
// it's really fast.
#define DICT_LOOKUP_COUNT 8

Lisp_Array *lisp_dict_new(Lisp_VM *vm, uint32_t n)
{
	Lisp_Array *dict = lisp_array_new(vm, n);
	dict->obj.type = O_DICT;
	dict->count = 1; // The first element is lookup array. Created when needed.
	return dict;
}

static Lisp_Pair* lookup(Lisp_Array *a, Lisp_String *name)
{
	uint32_t h = lisp_string_hash(name);
	for (unsigned i = h % (a->cap-1), n = 0; n < a->cap; n++) {
		Lisp_Pair *p = (Lisp_Pair*)a->items[i];
		if (!p)
			break;
		if (p->car == (Lisp_Object*)name) {
			//fprintf(stderr, "%s %d h=%d\n", name->buf, n+1,h);
			return p;
		}
		if (++i >= a->cap)
			i = 0;
	}
	return NULL;
}

static Lisp_Pair* lookup_cstr(Lisp_Array *a, const char *name)
{
	uint32_t h = hash_cstr(name);
	for (unsigned i = h % (a->cap-1), n = 0; n < a->cap; n++) {
		Lisp_Pair *p = (Lisp_Pair*)a->items[i];
		if (!p)
			break;
		Lisp_String *s = (Lisp_String*)p->car;
		if (strcmp(s->buf, name) == 0)
			return p;
		if (++i >= a->cap)
			i = 0;
	}
	return NULL;
}

// When items are few, look up brute force
Lisp_Pair *lisp_dict_assoc(Lisp_Array* dict, Lisp_String*name)
{
	if (dict->count > DICT_LOOKUP_COUNT) {
		assert(dict->items[0]);
		return lookup((Lisp_Array*)dict->items[0], name);
	} else {
		for (unsigned i = 1; i < dict->count; i++) {
			Lisp_Pair *p = (Lisp_Pair*)dict->items[i];
			if (name == (Lisp_String*)p->car)
				return p;
		}
	}
	return NULL;
}

// When items are few, look up brute force
Lisp_Pair *lisp_dict_assoc_cstr(Lisp_Array* dict, const char *name)
{
	if (dict->count > DICT_LOOKUP_COUNT) {
		assert(dict->items[0]);
		return lookup_cstr((Lisp_Array*)dict->items[0], name);
	} else {
		for (unsigned i = 1; i < dict->count; i++) {
			Lisp_Pair *p = (Lisp_Pair*)dict->items[i];
			if (strcmp(name, ((Lisp_String*)p->car)->buf)==0)
				return p;
		}
	}
	return NULL;
}

static void add_to_lookup_table(Lisp_Array *a, Lisp_Pair *p)
{
	Lisp_String *s = (Lisp_String*)p->car;
	uint32_t h = lisp_string_hash(s);
	for (unsigned i = h % (a->cap-1), n = 0; n < a->cap; n++) {
		if (!a->items[i]) {
			a->items[i] = (Lisp_Object*)p;
			return;
		}
		if (++i >= a->cap)
			i = 0;
	}
	assert(0);
}

void lisp_dict_add_item(Lisp_Array *dict, Lisp_Pair *p)
{
	lisp_array_push(dict, (Lisp_Object*)p);
	if (dict->count > DICT_LOOKUP_COUNT) {
		Lisp_Array *table = (Lisp_Array*)dict->items[0];
		if (!table || table->cap < dict->cap * 2) {
			table = lisp_array_new(dict->vm, dict->cap*2);
			dict->items[0] = (Lisp_Object*)table;
			for (unsigned i = 1; i < dict->count; i++)
				add_to_lookup_table(table, (Lisp_Pair*)dict->items[i]);
		} else {
			add_to_lookup_table(table, p);
		}
	}
}

// No checking for existing variables. TODO _set
Lisp_Pair* lisp_dict_add(Lisp_Array *dict, Lisp_String*name, Lisp_Object*val)
{
	Lisp_Pair *p = lisp_pair_new(dict->vm, (Lisp_Object*)name, val);
	lisp_dict_add_item(dict, p);
	return p;
}

void lisp_dict_remove(Lisp_Array *dict, Lisp_String*name)
{
	Lisp_Pair *p = lisp_dict_assoc(dict, name);
	if (p != NULL) {
		p->cdr = LISP_UNDEF;
	}
}

void lisp_dict_clear(Lisp_Array* dict)
{
	dict->items[0] = NULL;
	dict->count = 1;
}

////////////////////////////////////////////////
/// Environment
////////////////////////////////////////////////

Lisp_Env *lisp_env_new(Lisp_VM* vm, Lisp_Env *parent)
{
	Lisp_Env *env = new_obj(vm, O_ENV);
	lisp_push(vm, (Lisp_Object*)env);
	env->bindings = lisp_dict_new(vm, 8);
	env->parent = parent;
	lisp_pop(vm, 1);
	return env;
}

void lisp_begin_env(Lisp_VM *vm, Lisp_Env *parent)
{
	lisp_push(vm, LISP_FRAME_MARK);
	lisp_push(vm, (Lisp_Object*)vm->env);
	vm->env = lisp_env_new(vm, parent);
}

/* STACK: FRAME-MARK <PREV-ENV> <RESULT> */
void lisp_end_env(Lisp_VM* vm)
{
	assert(vm->stack->count >= 3);
	Lisp_Object **p = vm->stack->items + vm->stack->count - 3;
	assert(p[0] == LISP_FRAME_MARK);
	assert(p[1]->type == O_ENV);
	vm->env = (Lisp_Env*)p[1];
	p[0] = p[2];
	vm->stack->count -= 2;
}

static void clear_env(Lisp_VM *vm)
{
	lisp_dict_clear(vm->env->bindings);
}

Lisp_Pair* lisp_env_assoc(Lisp_Env *env, Lisp_String *name)
{
	for (; env; env = env->parent) {
		Lisp_Pair *p = lisp_dict_assoc(env->bindings, name);
		if (p) return p;
	}
	return NULL;
}

/* Public only. accessing global variables */
Lisp_Object *lisp_vm_get(Lisp_VM *vm, const char *name)
{
	Lisp_Pair* p = lisp_dict_assoc_cstr(vm->env->bindings, name);
	if (p) return p->cdr;
	else return NULL;
}

Lisp_Object* lisp_getvar(Lisp_VM *vm, Lisp_String *name)
{
	assert(!name->obj.is_const);
	Lisp_Pair *p = lisp_env_assoc(vm->env, name);
	if (p == NULL) {
		if (name->obj.is_primitive)
			return (Lisp_Object*)name;
		else
			lisp_err(vm, "Undefined variable '%s'", name->buf);
	}
	assert(p != NULL);
	return p->cdr;
}

Lisp_Pair* lisp_defvar(Lisp_VM *vm, Lisp_String* name, Lisp_Object *value)
{
	assert(!name->obj.is_const);
	Lisp_Pair *t = lisp_dict_assoc(vm->env->bindings, name);
	if (t == NULL) {
		t = lisp_dict_add(vm->env->bindings, name, value);
	} else {
		if (t->obj.is_const)
			lisp_err(vm, "Can not redefine constant: %s", name->buf);
		t->cdr = value;
	}
	return t;
}

void lisp_setvar(Lisp_VM *vm, Lisp_String* name, Lisp_Object *value)
{
	assert(!name->obj.is_const);
	
	Lisp_Env *env = vm->env;
	Lisp_Pair *p = NULL;
	for (; env; env = env->parent) {
		p = lisp_dict_assoc(env->bindings, name);
		if (p) {
			if (env->bindings->vm != vm) {
				lisp_err(vm, "Can not modify variable in foreign environment -- %s", name->buf);
			}
			break;
		}
	}
	if (p) {
		if (p->obj.is_const)
			lisp_err(vm, "Can not modify constant: %s", name->buf);
		p->cdr = value;
	} else {
		lisp_err(vm, "Undefined variable: '%s'", name->buf);
	}
}

/////////////////////////////////////
// Printing
/////////////////////////////////////

void lisp_port_print(Lisp_Port *port, Lisp_Object *obj);

static void print_string(Lisp_Port *port, Lisp_String *s)
{
	const char *p;
	lisp_port_putc(port, '\"');
	for (p = s->buf;*p;p++) {
		switch (*p) {
			case '\"': lisp_port_puts(port, "\\\""); break;
			case '\\': lisp_port_puts(port, "\\\\"); break;
			case '\n': lisp_port_puts(port, "\\n"); break;
			case '\r': lisp_port_puts(port, "\\r"); break;
			case '\t': lisp_port_puts(port, "\\t"); break;
			default: lisp_port_putc(port, *p); break;
		}
	}
	lisp_port_putc(port, '\"');
}

static void print_quoted(Lisp_Port *port, const char *prefix, Lisp_Object *o)
{
	lisp_port_puts(port, prefix);
	lisp_port_print(port, o);
}

static void print_pair(Lisp_Port *port, Lisp_Pair *p)
{
	if (p->car->type == O_SYMBOL) {
		if (p->cdr->type == O_PAIR && REST(p)->cdr == LISP_NIL) {
			Lisp_String *s = (Lisp_String*)p->car;
			Lisp_Object *o = REST(p)->car;
			switch (SYMID(s)) {
			case S_QUOTE: print_quoted(port, "'", o); return;
			case S_UNQUOTE: print_quoted(port, ",", o); return;
			case S_QUASIQUOTE: print_quoted(port, "`", o); return;
			case S_UNQUOTE_SPLICING: print_quoted(port, ",@", o); return;
			default: break;
			}
		}
	}
	lisp_port_putc(port, '(');
	while (p != LISP_NIL) {
		lisp_port_print(port, p->car);
		if (p->cdr->type != O_PAIR) {
			lisp_port_puts(port, " . ");
			lisp_port_print(port, p->cdr);
			break;
		}
		if (p->cdr != LISP_NIL)
			lisp_port_putc(port, ' ');
		p = (Lisp_Pair*)p->cdr;
	}
	lisp_port_putc(port, ')');
}

static void print_array_item(Lisp_Port *port, Lisp_Object *o)
{
	if (!o) {
		lisp_port_puts(port, "undefined");
	} else {
		lisp_port_print(port, o);
	}
}

static void print_array(Lisp_Port *port, Lisp_Array *a)
{
	lisp_port_puts(port, "#(");
	for (unsigned i = 0; i < a->count; i++) {
		if (i > 0) lisp_port_putc(port, ' ');
		print_array_item(port, a->items[i]);
	}
	lisp_port_putc(port, ')');
}

static void print_dict(Lisp_Port *port, Lisp_Array *a)
{
	lisp_port_puts(port, "##[");
	for (unsigned i = 1; i < a->count; i++) {
		Lisp_Pair *p = (Lisp_Pair*)a->items[i];
		if (i > 1) lisp_port_putc(port, ' ');
		if (p) {
			lisp_port_print(port, a->items[i]);
		}
	}
	lisp_port_putc(port, ']');
}

static void print_symbol(Lisp_Port *port, Lisp_String *s)
{
	if (port->isatty) {
		if (s->obj.is_const) {
			lisp_port_printf(port, COLOR_HL("%s"), s->buf);
		} else {
			lisp_port_puts(port, s->buf);
		}
	} else {
		lisp_port_puts(port, s->buf);
	}
}

static int hex_digit(int i)
{
	return i < 10? '0'+i : 'a'+(i-10);
}

static int hex_value(unsigned char c)
{
	if ('0' <= c && c <= '9') {
		return c - '0';
	} else if ('A' <= c && c <= 'F') {
		return c - 'A' + 10;
	} else if ('a' <= c && c <= 'f') {
		return c - 'a' + 10;
	} else {
		assert(0);
		return -1;
	}
}

static void print_buffer(Lisp_Port*port, Lisp_Buffer *buf)
{
	lisp_port_puts(port, "#x");
	for (size_t i = 0; i < buf->length; i++) {
		lisp_port_putc(port, hex_digit(buf->buf[i] >> 4));
		lisp_port_putc(port, hex_digit(buf->buf[i] & 0xf));
	}
}

static void print_env(Lisp_Port *port, Lisp_Env *env)
{
	lisp_port_puts(port, "#(ENVIRONMENT ");
	Lisp_Array *a = env->bindings;
	for (unsigned i = 1; i < a->count; i++) {
		lisp_port_puts(port, "\n  ");
		if (a->items[i]) {
			lisp_port_print(port, a->items[i]);
		}
	}
	lisp_port_putc(port, ')');
}

void lisp_port_print(Lisp_Port *port, Lisp_Object *obj)
{
	switch (obj->type) {
	case O_STRING: print_string(port, (Lisp_String*)obj); break;
	case O_SYMBOL: print_symbol(port, (Lisp_String*)obj); break;
	case O_PAIR: print_pair(port, (Lisp_Pair*)obj); break;
	case O_ARRAY: print_array(port, (Lisp_Array*)obj); break;
	case O_DICT: print_dict(port, (Lisp_Array*)obj); break;
	case O_BUFFER: print_buffer(port, (Lisp_Buffer*)obj); break;
	case O_NUMBER:
	{
		char buf[DTOA_BUFSIZE];
		dtoa(((Lisp_Number*)obj)->value, buf);
		lisp_port_puts(port, buf);
		break;
	}
	case O_PROC: case O_MACRO:
		lisp_port_printf(port, "#(%s ", objtypes[obj->type].name);
		lisp_port_print(port, ((Lisp_Proc*)obj)->lambda->car);
		lisp_port_putc(port, ')');
		break;
	case O_NATIVE_PROC:
		lisp_port_printf(port, "#(NATIVE-PROCEDURE %s)",
			((Lisp_Native_Proc*)obj)->name->buf);
		break;
	case O_OBJECT_EX: {
		Lisp_ObjectEx *ex = (Lisp_ObjectEx*)obj;
		lisp_port_puts(port, "#(OBJECT-EX ");
		if (ex->cls) {
			if (ex->cls->name) {
				lisp_port_printf(port, "%s ", ex->cls->name);
			}
			if (ex->ptr && ex->cls->print) {
				ex->cls->print(ex->ptr, port);
			}
		}
		lisp_port_putc(port, ')');
		break;
	}
	case O_ENV: {
		print_env(port, (Lisp_Env*)obj);
		break;
	}
	default:
		lisp_port_printf(port, "#(NON-PRINTABLE %s)",
			objtypes[obj->type].name);
		break;
	}
}

void lisp_print(Lisp_VM *vm, Lisp_Object* obj)
{
	lisp_port_print(vm->output, obj);
}

void lisp_println(Lisp_VM *vm, Lisp_Object* obj)
{
	lisp_port_print(vm->output, obj);
	lisp_port_putc(vm->output, '\n');
}

void lisp_stringify(Lisp_VM *vm, Lisp_Object *o)
{
	Lisp_Buffer *buf = lisp_buffer_new(vm, 128);
	pushx(vm, buf);
	Lisp_Port *port = lisp_open_output_buffer(vm, buf);
	pushx(vm, port);
	lisp_port_print(port, o);
	pushx(vm, lisp_string_new(vm, (char*)buf->buf, buf->length));
	lisp_push(vm, lisp_pop(vm, 3));
}

/**
 ** Tokenizer
 **/

static const char *token_names[] = {
	[T_EOF] = "EOF", [T_INVALID]="INVALID",
	[T_QUOTE]="QUOTE", [T_QUASIQUOTE] = "QQ", [T_UNQUOTE] = "UNQ",
	[T_UNQUOTE_SPLICING] = "UNQS", [T_DOT] = "DOT",
	[T_LPAREN] = "LPAREN", [T_RPAREN] = "RPAREN",
	[T_LBRACKET] = "LBRAKET", [T_RBRACKET] = "RBRACKET",
	[T_LBRACE] = "LBRACE", [T_RBRACE] = "RBRACE",
	[T_STRING] = "STRING", [T_SYMBOL] = "SYMBOL",
	[T_NUMBER] = "NUMBER",
	[T_DOLLAR] = "DOLLAR", [T_AT] = "AT",
	[T_CIRCUMFLEX] = "CIRCUMFLEX", [T_BUFFER] = "BUFFER",
	[T_ARRAY_BEGIN] = "ABEGIN", [T_DICT_BEGIN] = "DBEGIN",
	[T_COLON] = "COLON", [T_STRING_PART]="STRINGPART",
	[T_COLON_COMPONENT] = "COLONCOMPONENT"
};

#if DEBUG_TOKENIZER
static void print_token(Lisp_VM *vm)
{
	lisp_puts(vm, token_names[vm->token_type]);
	switch (vm->token_type) {
	case T_NUMBER:	case T_SYMBOL: case T_STRING: {
		lisp_puts(vm, " `");
		for (unsigned i = 0; i < vm->token->length; i++)
			lisp_putc(vm, vm->token->buf[i]);
		lisp_putc(vm, '\'');
		break;
	}
	default: break;
	}
	lisp_printf(vm, " L%d-L%d %d-%d\n",
		vm->token_pos.first_line,
		vm->token_pos.last_line,
		vm->token_pos.first_pos,
		vm->token_pos.last_pos);
	lisp_port_flush(vm->output);
}
#endif

static bool isdelim(int c) /* symbol delimiter */
{
	switch (c) {
	case ' ': case '\t': case '\n': case '\r': case '(':
	case ')': case '{': case '}': case '[': case ']': case ';':
	case EOF:
		return true;
	default: return false;
	}
}

static bool issym(int c)
{
	switch (c) {
	case '+': case '-': case '*': case '/': case '?': case '!':
	case '<': case '=': case '>': case '&': case '_':
		return true;
	default: return isalnum(c) || c > 127;
	}
}

static bool isnum(int c)
{
	if (isdigit(c))
		return true;
	switch (toupper(c)) {
	case 'X': case '.': case '+': case '-':
	case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
		return true;
	default:
		return false;
	}
}

static void append_utf8_byte(Lisp_VM *vm, int c)
{
	if (vm->utf8_remain > 0) {
		if ((c & ~0x3f) != 0x80)
			lisp_err(vm, "invalid utf8 sequence");
		vm->utf8_remain--;
	} else if ((c >> 7) == 0) { // ASCII
		vm->utf8_remain = 0;
	} else if ((c >> 6) == 2) { // 0x10xxxxxx
		lisp_err(vm, "invalid utf8 first byte: %02x", c);
	} else if ((c >> 5) == 6) { // 0x110xxxxx
		vm->utf8_remain = 1;
	} else if ((c >> 4) == 0xe) { // 0x1110xxxx
		vm->utf8_remain = 2;
	} else if ((c >> 3) == 0x1e) { // 0x11110xxx
		vm->utf8_remain = 3;
	} else {
		lisp_err(vm, "invalid utf8 first byte: %02x", c);
	}
	lisp_buffer_add(vm->token, c);
}

/* Fetch a symbol token from current input */
static void symtok(Lisp_VM *vm)
{
	while (vm->token->length <= MAX_SYMBOL_LENGTH) {
		int c = lisp_port_getc(vm->input);
		if (issym(c)) {
			append_utf8_byte(vm, c);
		} else if (c == ':') {
			append_utf8_byte(vm, 0);
			vm->token_type = T_COLON_COMPONENT;
			break;
		} else if (isdelim(c)) {
			append_utf8_byte(vm, 0);
			vm->token_type = T_SYMBOL;
			lisp_port_unget(vm->input, c);
			break;
		} else {
			lisp_err(vm, "invalid symbol char `%c'", c);
		}
	}
	if (vm->token->length > MAX_SYMBOL_LENGTH)
		lisp_err(vm, "Invalid symbol: too long");
}

/* Fetch a number token from current input */
static void numtok(Lisp_VM *vm)
{
	while (true) {
		int c = lisp_port_getc(vm->input);
		if (isnum(c))
		{
			lisp_buffer_add(vm->token, c);
		}
		else if (isdelim(c))
		{
			lisp_buffer_add(vm->token, 0);
			vm->token_type = T_NUMBER;
			lisp_port_unget(vm->input, c);
			break;
		}
		else
		{
			lisp_err(vm, "invalid symbol char `%c'", c);
		}
	}
}

static int xval(int c) /* c is a hex digit */
{
	return c <= '9' ? c - '0' : toupper(c) - 'A' + 10;
}

static int xchar(Lisp_VM *vm)
{
	int a = lisp_port_getc(vm->input);
	int b = lisp_port_getc(vm->input);
	if (!isxdigit(a) || !isxdigit(b))
		lisp_err(vm, "invalid hex digit in string");
	return (xval(a) << 4) | xval(b);
}

static void skip_trailing_spaces(Lisp_VM *vm, int c)
{
	/* Expecting whitespaces until end of line */
	while (isspace(c) && c != '\n') {
		c = lisp_port_getc(vm->input);
	}
	if (c == '\n')
		return;
	lisp_err(vm, "invalid escaped char `%c'", c);
}

static void strtoken(Lisp_VM *vm)
{
	while (true) {
		int c = lisp_port_getc(vm->input);
		if (c == '"') {
			append_utf8_byte(vm, 0);
			vm->token_type = T_STRING;
			return;
		} else if (c == '\\') {
			if (vm->utf8_remain > 0)
				lisp_err(vm, "not terminated utf8 sequence before \\");
			c = lisp_port_getc(vm->input);
			switch (c) {
			case '{': case '(': case '[':
				vm->token_type = T_STRING_PART;
				append_utf8_byte(vm, 0);
				lisp_port_unget(vm->input, c);
				return;
			case 'n': c = '\n'; break;
			case 'r': c = '\r'; break;
			case 't': c = '\t'; break;
			case '"': c = '"';  break;
			case '\\':c = '\\'; break;
			case 'x': c = xchar(vm); break;
			default:
				skip_trailing_spaces(vm, c);
				break;
			}
		}
		append_utf8_byte(vm, c);
	}
}

static void hextok(Lisp_VM *vm)
{
	int n = 8;
	int bits = 0;
	while (true) {
		int c = lisp_port_getc(vm->input);
		if (isxdigit(c)) {
			n -= 4;
			bits |= (hex_value(c) << n);
			if (n == 0) {
				lisp_buffer_add_byte(vm->token, bits);
				n = 8;
				bits = 0;
			}
		} else if (c == '\\') {
			skip_trailing_spaces(vm, ' ');
		} else if (isdelim(c)) {
			lisp_port_unget(vm->input, c);
			break;
		} else {
			lisp_err(vm, "bad hex");
		}
	}
	if (n != 8)
		lisp_err(vm, "unexpected end of hex string");
	vm->token_type = T_BUFFER;
}

static void bintok(Lisp_VM *vm)
{
	int n = 8;
	int bits = 0;
	while (true) {
		int c = lisp_port_getc(vm->input);
		if (c == '0' || c == '1') {
			bits |= ((c - '0') << (--n));
			if (n == 0) {
				lisp_buffer_add_byte(vm->token, bits);
				n = 8;
				bits = 0;
			}
		} else if (c == '\\') {
			skip_trailing_spaces(vm, ' ');
		} else if (isdelim(c)) {
			lisp_port_unget(vm->input, c);
			break;
		} else {
			lisp_err(vm, "bad bin");
		}
	}
	if (n != 8)
		lisp_err(vm, "unexpected end of binary string");
	vm->token_type = T_BUFFER;
}

static void badchar(Lisp_VM *vm, int c)
{
	lisp_err(vm, "Invalid character: %d,0x%02x,[%c]", c, c, c);
}

static void next_token(Lisp_VM *vm)
{
	int c;
	vm->token->buf[0] = 0;
	vm->token->length = 0;
	vm->token_type = T_INVALID;
Retry:
	/* Get the first non-space char */
	do {
		c = lisp_port_getc(vm->input);
	} while (isspace(c));
	vm->token_pos.first_line = vm->input->line;
	vm->token_pos.first_pos = vm->input->src_pos;
	switch (c) {
	case '(': vm->token_type = T_LPAREN; break;
	case ')': vm->token_type = T_RPAREN; break;
	case '[': vm->token_type = T_LBRACKET; break;
	case ']': vm->token_type = T_RBRACKET; break;
	case '{': vm->token_type = T_LBRACE; break;
	case '}': vm->token_type = T_RBRACE; break;
	//case '@': vm->token_type = T_AT; break;
	case '^': vm->token_type = T_CIRCUMFLEX; break;
	case '\'': vm->token_type = T_QUOTE; break;
	case '`': vm->token_type = T_QUASIQUOTE; break;
	case ':': vm->token_type = T_COLON; break;
	case EOF: vm->token_type = T_EOF; break;
	case ';': /* Ignore comments */
		do {
			c = lisp_port_getc(vm->input);
		} while (c != '\n' && c != '\r' && c != EOF);
		goto Retry;
	case '.':
		c = lisp_port_getc(vm->input);
		if (isdelim(c)) {
			lisp_port_unget(vm->input, c);
			vm->token_type = T_DOT;
		} else {
			lisp_err(vm, "do not begin %s with `.'",
				isdigit(c) ? "number" : "symbol");
		}
		break;
	case ',':
		c = lisp_port_getc(vm->input);
		if (c == '@') {
			vm->token_type = T_UNQUOTE_SPLICING;
		} else {
			lisp_port_unget(vm->input, c);
			vm->token_type = T_UNQUOTE;
		}
		break;
	case '"':
		strtoken(vm);
		break;
	case '#':
		c = lisp_port_getc(vm->input);
		switch (c) {
		case 'b':
			bintok(vm);
			break;
		case 'x':
			hextok(vm);
			break;
		case '(': case '[': case '{':
			vm->token_type = T_ARRAY_BEGIN;
			lisp_buffer_add(vm->token, c);
			break;
		case '#':
			c = lisp_port_getc(vm->input);
			if (c == '('|| c == '[' || c == '{') {
				vm->token_type = T_DICT_BEGIN;
				lisp_buffer_add(vm->token, c);
			} else {
				lisp_err(vm, "invalid dict: ##%c", c);
			}
			break;
		default:
			lisp_err(vm, "invalid char: #%c", c);
			break;
		}
		break;
	default:
		append_utf8_byte(vm, c);
		if (isdigit(c)) {
			numtok(vm);
		} else if (c == '+' || c == '-') {
			int nextc = lisp_port_peekc(vm->input);
			if (isdigit(nextc) || nextc == '.') {
				numtok(vm);
			} else {
				symtok(vm);
			}
		} else if (issym(c)) {
			symtok(vm);
		} else {
			badchar(vm, c);
		}
		break;
	}
	vm->token_pos.last_line = vm->input->line;
	vm->token_pos.last_pos = vm->input->src_pos;
#if DEBUG_TOKENIZER
	print_token(vm);
#endif
}

/*******************************************************************
 ** Parser
 *******************************************************************/

/* Construct a symbol object on stack */
Lisp_String *lisp_make_symbol(Lisp_VM *vm, const char *name)
{
	Lisp_String *t = find_sym(&_symtab[0], S_TOTAL, name);
	if (t) {
		pushx(vm, t);
	} else {
		Lisp_Pair *p = lisp_dict_assoc_cstr(vm->symbols, name);
		if (!p && vm->parent) {
			p = lisp_dict_assoc_cstr(vm->parent->symbols, name);
		}
		if (p) {
			t = (Lisp_String*)p->car;
			pushx(vm, t);
		} else {
			t = lisp_symbol_new(vm, name, strlen(name));
			pushx(vm, t);
			lisp_dict_add(vm->symbols, t, LISP_NIL);
		}
	}
	return t;
}

static void badtok(Lisp_VM *vm)
{
	lisp_err(vm, "unexpected token: %s %s",
		token_names[vm->token_type], vm->token->buf);
}

static void expect_token(Lisp_VM *vm, Token_Type type)
{
	if (vm->token_type != type) {
		lisp_err(vm, "expect token %s but got %s %s",
			token_names[type], token_names[vm->token_type],
			vm->token->buf);
	}
}

static void begin_expr_mapping(Lisp_VM *vm)
{
	if (vm->input->src_file == NULL)
		return;
	Lisp_SourceMapping *m = new_obj(vm, O_SOURCE_MAPPING);
	m->file = vm->input->src_file;
	m->begin = vm->token_pos.first_pos;
	m->line = vm->token_pos.first_line;
	pushx(vm, m);
}

static void end_expr_mapping(Lisp_VM *vm)
{
	if (vm->input->src_file == NULL)
		return;
	Lisp_Pair *p = (Lisp_Pair*)lisp_top(vm, 0);
	Lisp_SourceMapping *m = (Lisp_SourceMapping*)lisp_top(vm, 1);
	assert(p->obj.type == O_PAIR);
	assert(m->obj.type == O_SOURCE_MAPPING);
	if (p != LISP_NIL)
	{
		m->end = vm->token_pos.last_pos;
		m->expr = p;
		p->mapping = m;
		lisp_array_push(vm->input->src_file->mappings, (Lisp_Object*)m);
	}
	lisp_exch(vm);
	lisp_pop(vm, 1);
}

static void sexps(Lisp_VM *vm);
static void sexps_without_dots(Lisp_VM *vm);
static void mklist(Lisp_VM *vm);
static void mkdict(Lisp_VM *vm, int n);
static void mkarray(Lisp_VM *vm, int n);
static int sexp(Lisp_VM *vm);

static void quoted(Lisp_VM*vm, Lisp_Object* q)
{
	begin_expr_mapping(vm);
	lisp_push(vm, q);
	next_token(vm);
	if (!sexp(vm)) {
		lisp_err(vm, "expect valid sexp for %s",
			((Lisp_String*)q)->buf);
	}
	lisp_make_list(vm, 2);
	end_expr_mapping(vm);
}

Lisp_Object* lisp_read(Lisp_VM *vm);

static void concated(Lisp_VM *vm)
{
	begin_expr_mapping(vm);
	lisp_push(vm, LISP_MARK);
	lisp_push(vm, (Lisp_Object*)SYM(S_CONCAT));
	do {
		if (vm->token->length > 1)
			lisp_push_string(vm, (char*)vm->token->buf, vm->token->length-1);
		if (vm->token_type == T_STRING)
			break;
		// Wrapping EVALQ allows better error reporting
		// for symbol references
		lisp_push(vm, (Lisp_Object*)SYM(S_EVALQ));
		next_token(vm);
		sexp(vm);
		Lisp_Pair *p = (Lisp_Pair*)lisp_top(vm, 0);
		assert(p->obj.type == O_PAIR);
		lisp_cons(vm);
		// Move source mapping to head of expression
		Lisp_Pair *t = (Lisp_Pair*)lisp_top(vm, 0);
		t->mapping = p->mapping;
		p->mapping = NULL;
		vm->token->length = 0;
		strtoken(vm);
	} while (1);
	mklist(vm);
	end_expr_mapping(vm);
}

static void colon_path(Lisp_VM *vm)
{
	begin_expr_mapping(vm);
	lisp_push(vm, LISP_MARK);
	pushx(vm, SYM(S_GET));
	lisp_make_symbol(vm, (char*)vm->token->buf);
	do {
		next_token(vm);
		if (vm->token_type == T_SYMBOL||vm->token_type==T_COLON_COMPONENT) {
			pushx(vm, SYM(S_QUOTE));
			lisp_make_symbol(vm, (char*)vm->token->buf);
			lisp_make_list(vm, 2);
			if (vm->token_type == T_SYMBOL)
				break;
		} else {
			lisp_err(vm, "Expect symbol after colon");
		}
	} while (1);
	mklist(vm);
	end_expr_mapping(vm);
}

static void lambda_expr(Lisp_VM *vm)
{
	begin_expr_mapping(vm);
	pushx(vm, SYM(S_LAMBDA));
	next_token(vm);
	if (!sexp(vm))
		lisp_err(vm, "expect valid sexp for lambda");
	lisp_cons(vm);
	end_expr_mapping(vm);
}

static void consq(Lisp_VM *vm)
{
	begin_expr_mapping(vm);
	pushx(vm, SYM(S_CONSQ));
	next_token(vm);
	if (!sexp(vm)) lisp_err(vm, "Missing argument for :");
	next_token(vm);
	if (!sexp(vm)) lisp_err(vm, "Missing argument for :");
	lisp_make_list(vm, 3);
	end_expr_mapping(vm);
}

static int sexp(Lisp_VM *vm)
{
	Lisp_String *s;
	double d;
	int tt = vm->token_type;
	switch (tt) {
	case T_LPAREN:
	case T_LBRACKET:
	case T_LBRACE:
		begin_expr_mapping(vm);
		lisp_push(vm, LISP_MARK);
		sexps(vm);
		expect_token(vm, (tt == T_LPAREN   ? T_RPAREN :
		                 (tt == T_LBRACKET ? T_RBRACKET :
		                 T_RBRACE)));
		mklist(vm);
		end_expr_mapping(vm);
		break;
	case T_ARRAY_BEGIN:
	case T_DICT_BEGIN:
	{
		int c = vm->token->buf[0];
		size_t cnt = vm->stack->count;
		sexps_without_dots(vm);
		cnt = vm->stack->count - cnt;
		if (c == '(')
			expect_token(vm, T_RPAREN);
		else if (c == '[')
			expect_token(vm, T_RBRACKET);
		else {
			assert(c == '{');
			expect_token(vm, T_RBRACE);
		}
		if (tt == T_DICT_BEGIN) {
			mkdict(vm, (int)cnt);
		} else {
			mkarray(vm, (int)cnt);
		}
		break;
	}
	case T_BUFFER: {
		Lisp_Buffer *b = lisp_buffer_copy(vm, vm->token->buf, vm->token->length);
		b->obj.is_const = 1;
		lisp_push(vm, (Lisp_Object*)b);
		break;
	}
	case T_SYMBOL:
		lisp_make_symbol(vm, (char*)vm->token->buf);
		break;
	case T_COLON_COMPONENT:
		colon_path(vm);
		break;
	case T_STRING:
		s = lisp_string_new(vm, (char*)vm->token->buf, vm->token->length-1);
		lisp_push(vm, (Lisp_Object*)s);
		break;
	case T_STRING_PART:
		concated(vm);
		break;
	case T_NUMBER:
		if (str2dbl((char*)vm->token->buf, &d)) {
			Lisp_Number *n = lisp_number_new(vm, d);
			lisp_push(vm, (Lisp_Object*)n);
		} else {
			lisp_err(vm, "invalid number");
		}
		break;
	case T_CIRCUMFLEX: lambda_expr(vm);break;
	case T_QUOTE: quoted(vm, LISP_QUOTE); break;
	case T_QUASIQUOTE: quoted(vm, LISP_QUASIQUOTE); break;
	case T_UNQUOTE: quoted(vm, LISP_UNQUOTE); break;
	case T_UNQUOTE_SPLICING: quoted(vm,LISP_UNQUOTE_SPLICING);break;
	case T_COLON: consq(vm);break;
	default: return false;
	}
#if 0
	lisp_println(vm, lisp_top(vm,0));
#endif
	return true;
}

static void sexps_without_dots(Lisp_VM *vm)
{
	while (true) {
		next_token(vm);
		if (vm->token_type == T_DOT)
			lisp_err(vm, "DOTS not allowed");
		if (!sexp(vm))
			return;
	}
}

static void sexps(Lisp_VM *vm)
{
	next_token(vm);
	if (!sexp(vm))
		return;
	while (true) {
		next_token(vm);
		if (vm->token_type == T_DOT) {
			lisp_push(vm, LISP_DOT);
			next_token(vm);
			if (!sexp(vm))
				lisp_err(vm, "invalid sexps");
		} else if (!sexp(vm)) {
				break;
		}
	}
}

/* We set is_list flag if possible.
 * this speeds up run time checking.
 * Since pairs contructed here will never be modified,
 * so the flag is valid through out.
 */
static void mklist(Lisp_VM *vm)
{
	Lisp_Object **top = vm->stack->items + vm->stack->count - 1;
	if (top[0] == LISP_MARK) {
		top[0] = LISP_NIL;
		return;
	}
	bool is_list = false;
	if (top[-1] != LISP_DOT) {
		is_list = true;
		top[0] = cons(vm, top[0], LISP_NIL);
	}
	
	for (;true;top--) {
		if (top[-1] == LISP_DOT) {
			top[-1] = top[0];
		} else if (top[-1] == LISP_MARK) {
			top[-1] = top[0];
			break;
		} else {
			top[-1] = cons(vm, top[-1], top[0]);
			top[-1]->is_list = is_list;
		}
	}
	vm->stack->count = top - vm->stack->items;
}

static void mkarray(Lisp_VM *vm, int n)
{
	assert((unsigned)n <= vm->stack->count);
	Lisp_Array *a = lisp_array_copy(vm, vm->stack, (int)vm->stack->count-n, n);
	a->obj.is_const = 1;
	vm->stack->count-=n;
	pushx(vm, a);
}

static void mkdict(Lisp_VM *vm, int n)
{
	assert((unsigned)n <= vm->stack->count);
	int i = (int)vm->stack->count-n;
	Lisp_Array *a = lisp_dict_new(vm, n);
	pushx(vm, a);
	for (; (unsigned)i < vm->stack->count-1; i++) {
		Lisp_Object *o = vm->stack->items[i];
		if (o->type != O_PAIR || CAR(o)->type != O_SYMBOL)
			lisp_err(vm, "bad dict: must be a symbol binding pair");
		lisp_dict_add_item(a, (Lisp_Pair*)o);
	}
	a->obj.is_const = 1;
	vm->stack->count -= n+1;
	pushx(vm, a);
}

void lisp_begin_list(Lisp_VM *vm)
{
	lisp_push(vm, LISP_MARK);
}

void lisp_end_list(Lisp_VM *vm)
{
	mklist(vm);
}

/* lisp_read -- Read a lisp object from input
 * On success, returns the object and also leaves it at the stack top.
 * Otherwise, long jump to current error handler.
 */
Lisp_Object* lisp_read(Lisp_VM *vm)
{
	next_token(vm);
	if (vm->token_type == T_EOF)
		lisp_push(vm, LISP_EOF);
	else if (!sexp(vm))
		badtok(vm);
	return vm->stack->items[vm->stack->count-1];
}


/*******************************************************************
 ** Eval & Apply
 *******************************************************************/

static Lisp_Object *lisp_eval_core(Lisp_VM*, int at_tail);
#define lisp_eval(vm) lisp_eval_core(vm, 0)
#define lisp_eval_tail(vm) lisp_eval_core(vm, 1)

void lisp_apply(Lisp_VM *vm, Lisp_Object *proc, Lisp_Pair *args);
static void apply_primitive(Lisp_VM*vm, int sid, Lisp_Pair* args);

/* `args' is a list.
 * Reuse constant pairs.
 */
static void eval_args(Lisp_VM *vm, Lisp_Pair *args)
{
	int n = 0;
	Lisp_Pair *p = args;
	for (; p != LISP_NIL; p = REST(p), n++)
		pushx(vm, p);
	lisp_push(vm, LISP_NIL);
	for (; n > 0; n--) {
		Lisp_Object *t = lisp_top(vm, 0);
		Lisp_Pair* p = (Lisp_Pair*)lisp_top(vm, 1);
		if (p->cdr == t) {
			if (p->car->is_const) {
				lisp_pop(vm, 1);
				continue;
			}
		}
		lisp_push(vm, p->car);
		lisp_eval(vm);
		p = lisp_pair_new(vm, lisp_top(vm, 0), t);
		lisp_pop(vm, 3);
		pushx(vm, p);
	}
}

/* `l' must be a list. Checked by caller
 * If at_tail is false, then no tail recursion is allowed.
 * execution will not be delayed.
 */
static void eval_list(Lisp_VM *vm, Lisp_Pair *l, bool at_tail)
{
	for (; l->cdr != LISP_NIL; l = (Lisp_Pair*)l->cdr) {
		lisp_push(vm, l->car);
		Lisp_Object *r = lisp_eval(vm);
		if (r->is_return) return;
		lisp_pop(vm, 1);
	}
	lisp_push(vm, l->car);
	lisp_eval_core(vm, at_tail);
}

/* Evaluate procedure body */
static void eval_body(Lisp_VM *vm, Lisp_Pair *l)
{
	eval_list(vm, l, 1);
}

/* p is also at stack top
 *
 * Stack Layout during evaluation:
 *
 * 0  p
 * 1  EXPR-MARK
 * 2  op
 * 3  args
 * 4  returned value
 */
static Lisp_Object *eval_expr(Lisp_VM *vm, Lisp_Pair* p, int at_tail)
{
	if (++vm->eval_level > MAX_DEPTH)
		lisp_err(vm, "exceeding max depth: %d", MAX_DEPTH);
	
	if (vm->cov_trace && p->mapping)
		p->mapping->cnt++;
	
	Lisp_Object *op = p->car;
	lisp_push(vm, LISP_EXPR_MARK); /* mark callstack */
	lisp_push(vm, op);
	op = lisp_eval(vm);

	if (!is_list((Lisp_Object*)p))
		lisp_err(vm, "bad sexp: not a list");
	if (op->is_special) {
		lisp_push(vm, p->cdr);
	} else {
		eval_args(vm, (Lisp_Pair*)p->cdr);
	}

	if (at_tail) {
		if (op->type == O_PROC) {
			/* Tail Call Object: (expr . (op . args))
			 * EXPR is required to track source location
			 * for error reporting
			 */
			Lisp_Object *args = lisp_top(vm, 0);
			lisp_push(vm, (Lisp_Object*)p);
			lisp_push(vm, op);
			lisp_push(vm, args);
			lisp_cons(vm);
			Lisp_Pair *t = lisp_cons(vm);
			t->obj.tail_call = 1;
		} else {
			lisp_apply(vm, op, (Lisp_Pair*)lisp_top(vm, 0));
		}
	} else {
		while (true) {
			lisp_apply(vm, op, (Lisp_Pair*)lisp_top(vm, 0));
			assert(vm->stack->count > 3);
			Lisp_Object **t = vm->stack->items + vm->stack->count - 3;
			if (!t[2]->tail_call) break;
			assert(t[-1] == LISP_EXPR_MARK);
			t[-2] = CAR(t[2]);          /* update expression */
			op = t[0] = CAR(CDR(t[2])); /* new op */
			t[1] = CDR(CDR(t[2]));      /* new args */
			vm->stack->count--;
		}
	}
	
	Lisp_Object *ret = lisp_pop(vm, 5);
	lisp_push(vm, ret);
	vm->eval_level--;
	return ret;
}

/* Eval the stack top object and replace it with result. */
Lisp_Object* lisp_eval_core(Lisp_VM *vm, int at_tail)
{
	size_t count = vm->stack->count;
	Lisp_Object *obj = lisp_top(vm, 0);
	if (obj->is_const) return obj;
	switch (obj->type) {
	case O_SYMBOL:
		obj = lisp_getvar(vm, (Lisp_String*)obj);
		lisp_pop(vm, 1);
		lisp_push(vm, obj);
		break;
	case O_PAIR:
		obj = eval_expr(vm, (Lisp_Pair*)obj, at_tail);
		break;
	default:
		lisp_err(vm, "Can not eval type: '%s'", objtypes[obj->type].name);
		break;
	}
	assert(count == vm->stack->count);
	return obj;
}

/* values is a list
 * When we build procedure, arguments is checked to be a list
 * and there is no const symbols except modifiers 
 */
static void bind_args(Lisp_VM *vm, Lisp_Proc *p, Lisp_Pair *values)
{
	Lisp_String *modifier = NULL;
	clear_env(vm);
	Lisp_Pair *args = (Lisp_Pair*)p->lambda->car;
	const char *procedure_name = "<unknown-procedure>";
	for (; args != LISP_NIL; args = (Lisp_Pair*)args->cdr) {
		Lisp_String *name = (Lisp_String*)args->car;
		if (name->buf[0] == '&') {
			modifier = name;
		} else if (!modifier) {
			if (values == LISP_NIL)
				lisp_err(vm, "%s: missing arguments", procedure_name);
			lisp_dict_add(vm->env->bindings, name, values->car);
			values = (Lisp_Pair*)values->cdr;
		} else if (modifier == SYM(S_ARG_LABEL)) {
			procedure_name = name->buf;
			lisp_dict_add(vm->env->bindings, name, (Lisp_Object*)p);
			modifier = NULL;
		} else if (modifier == SYM(S_ARG_OPTIONAL)) {
			lisp_dict_add(vm->env->bindings, name, values==LISP_NIL?LISP_FALSE:values->car);
			values = (Lisp_Pair*)values->cdr;
		} else if (modifier == SYM(S_ARG_REST)) {
			lisp_dict_add(vm->env->bindings, name, (Lisp_Object*)values);
		} else if (modifier == SYM(S_ARG_KEY)) {
			Lisp_Pair *p = lisp_assoc(values, (Lisp_Object*)name);
			lisp_dict_add(vm->env->bindings, name, p ? p->cdr : LISP_FALSE);
		} else {
			lisp_err(vm, "%s: invalid argument modifier '%s'",
				procedure_name,
				modifier->buf);
		}
	}
	if (values != LISP_NIL && !modifier)
		lisp_err(vm, "%s: too many arguments", procedure_name);
}

static void apply_native(Lisp_VM *vm, Lisp_Native_Proc *c, Lisp_Pair *args)
{
	lisp_push(vm, (Lisp_Object*)vm->env);
	vm->env = c->env;
	c->fn(vm, args);
	lisp_exch(vm);
	vm->env = (Lisp_Env*)lisp_pop(vm, 1);
	assert(vm->env->obj.type == O_ENV);
}

static void eval_procedure_body(Lisp_VM *vm, Lisp_Proc *c, Lisp_Pair *args)
{
	Lisp_Pair *lbody = (Lisp_Pair*)(c->lambda->cdr);
	bind_args(vm, c, args);
	while (true) {
		eval_body(vm, lbody);
		Lisp_Object *t = lisp_top(vm, 0);
		if (t->is_return) {
			lisp_pop(vm, 1);
			t = CAR(t);
			lisp_push(vm, t);
		}
		if (t->tail_call && CADR(t) == (Lisp_Object*)c) {
			args = (Lisp_Pair*)CDDR(t);
			/* Need to start in a new environment.
			 * It's possible that the body execution exports
			 * some closures, so that we can't overwrite their
			 * enclosed environment.
			 * TODO optimize if their is no closures generated
			 *      in the body.
			 */
			vm->env = lisp_env_new(vm, c->env);
			bind_args(vm, c, args);
			lisp_pop(vm, 1);
		} else {
			break;
		}
	}
}

static void apply_procedure(Lisp_VM *vm, Lisp_Proc *c, Lisp_Pair *args)
{
	lisp_begin_env(vm, c->env);
	eval_procedure_body(vm, c, args);
	lisp_end_env(vm);
}

static void apply_constructor(Lisp_VM *vm, Lisp_Proc *c, Lisp_Pair *args)
{
	lisp_begin_env(vm, c->env);
	eval_procedure_body(vm, c, args);
	lisp_pop(vm, 1);
	lisp_push(vm, (Lisp_Object*)vm->env);
	lisp_end_env(vm);
}

static void print_trace(Lisp_VM *vm, Lisp_Object*c,
	const char* prompt, Lisp_Object *obj)
{
	int level = vm->eval_level - 1;
	if (level > 60) level = 60;
	while (level-- > 0) lisp_putc(vm, ' ');
	lisp_printf(vm, "[%d] ", vm->eval_level);
	lisp_print(vm, c);
	lisp_printf(vm, " %s = ", prompt);
	lisp_print(vm, obj);
	lisp_putc(vm, '\n');
}

static void call_method(Lisp_VM *vm, Lisp_Env *env, Lisp_String *method, Lisp_Pair *args)
{
	Lisp_Pair *p = lisp_env_assoc(env, method);
	if (p == NULL)
		lisp_err(vm, "Method undefined: '%s'", method->buf);
	assert(p != NULL);
	if (p->cdr->type != O_PROC || !p->obj.is_method)
		lisp_err(vm, "Not a method: '%s'", method->buf);
	lisp_push(vm, p->cdr); /* DO WE REALLY NEED THIS PROTECTION? */
	apply_procedure(vm, (Lisp_Proc*)p->cdr, args);
	lisp_push(vm, lisp_pop(vm, 2));
}

void lisp_apply(Lisp_VM *vm, Lisp_Object *proc, Lisp_Pair *args)
{
	if (proc->tracing)
		print_trace(vm, proc, ":args", (Lisp_Object*)args);
	switch (proc->type) {
		case O_SYMBOL:
			if (proc->is_primitive)
				apply_primitive(vm, SYMID((Lisp_String*)proc), args);
			else
				lisp_err(vm, "apply: Not a primitive: '%s'",
					((Lisp_String*)proc)->buf);
			break;
		case O_PROC:
			apply_procedure(vm, (Lisp_Proc*)proc, args);
			break;
		case O_MACRO:
			apply_procedure(vm, (Lisp_Proc*)proc, args);
			lisp_eval(vm);
			break;
		case O_NATIVE_PROC:
			apply_native(vm, (Lisp_Native_Proc*)proc, args);
			break;
		case O_ENV:
			if (CAR(args)->type != O_SYMBOL)
				lisp_err(vm, "Invalid method: expecting symbol");
			call_method(vm, (Lisp_Env*)proc, (Lisp_String*)CAR(args), (Lisp_Pair*)CDR(args));
			break;
		default:
			lisp_err(vm, "Invalid type for apply: %s",
				objtypes[proc->type].name);
			break;
	}
	if (proc->tracing)
		print_trace(vm, proc, ":value", lisp_top(vm,0));
}

/**
 ** Primitives
 **/

static void* safe_ptr(Lisp_VM*vm, Lisp_Object*o, Object_Type type)
{
	if (o->type == type)
		return o;
	lisp_err(vm, "expect %s but got %s",
		objtypes[type].name, objtypes[o->type].name);
	return NULL;
}

int lisp_safe_int(Lisp_VM*vm, Lisp_Object*o)
{
	if (o->type != O_NUMBER || !is_integer(NUMVAL(o)))
		lisp_err(vm, "not an integer");
	double d = NUMVAL(o);
	if (d > INT_MAX || d < INT_MIN)
		lisp_err(vm, "integer out of range");
	return (int)d;
}

double lisp_safe_number(Lisp_VM*vm, Lisp_Object*o)
{
	if (o->type != O_NUMBER)
		lisp_err(vm, "not a number");
	return ((Lisp_Number*)o)->value;
}

Lisp_Pair* lisp_safe_list(Lisp_VM*vm, Lisp_Object*o)
{
	if (!is_list(o))
		lisp_err(vm, "not a list");
	return (Lisp_Pair*)o;
}

void *lisp_safe_bytes(Lisp_VM *vm, Lisp_Object *o, size_t *len)
{
	if (o->type == O_BUFFER) {
		*len = ((Lisp_Buffer*)o)->length;
		return ((Lisp_Buffer*)o)->buf;
	} else {
		lisp_err(vm, "object has no bytes");
		return NULL;
	}
}

const char *lisp_safe_cstring(Lisp_VM *vm, Lisp_Object*o)
{
	if (o->type != O_STRING) {
		lisp_err(vm, "not a string");
	}
	return ((Lisp_String*)o)->buf;
}

const char *lisp_safe_csymbol(Lisp_VM *vm, Lisp_Object*o)
{
	if (o->type != O_SYMBOL) {
		lisp_err(vm, "not a symbol");
	}
	return ((Lisp_String*)o)->buf;
}

#define safe_int lisp_safe_int
#define safe_num lisp_safe_number
#define list_ptr lisp_safe_list

static void op_p(Lisp_VM*vm, bool val)
{
	lisp_push(vm, val ? LISP_TRUE : LISP_FALSE);
}

static void op_add(Lisp_VM*vm,Lisp_Pair*args)
{
	double t = 0;
	for (; args != LISP_NIL; args = REST(args)) {
		Lisp_Number *a = safe_ptr(vm, args->car, O_NUMBER);
		t += a->value;
	}
	lisp_push(vm, (Lisp_Object*)lisp_number_new(vm, t));
}

static void op_mul(Lisp_VM*vm,Lisp_Pair*args)
{
	double t = 1;
	for (; args != LISP_NIL; args = REST(args)) {
		Lisp_Number *a = safe_ptr(vm, args->car, O_NUMBER);
		t *= a->value;
	}
	lisp_push(vm, (Lisp_Object*)lisp_number_new(vm, t));
}

static void op_sub(Lisp_VM*vm,Lisp_Pair*args)
{
	double t = 0;
	Lisp_Number *a;
	
	if (args->cdr != LISP_NIL) {
		a = safe_ptr(vm,args->car,O_NUMBER);
		t = a->value;
		args = REST(args);
	}
	for (; args != LISP_NIL; args = REST(args)) {
		Lisp_Number *a = safe_ptr(vm, args->car, O_NUMBER);
		t -= a->value;
	}
	lisp_push(vm, (Lisp_Object*)lisp_number_new(vm, t));
}

static void op_div(Lisp_VM*vm,Lisp_Pair*args)
{
	double t = 1;
	Lisp_Number *a;

	if (args->cdr != LISP_NIL) {
		a = safe_ptr(vm,args->car,O_NUMBER);
		t = a->value;
		args = REST(args);
	}
	for (; args != LISP_NIL; args = REST(args)) {
		Lisp_Number *a = safe_ptr(vm, args->car, O_NUMBER);
		t /= a->value;
	}
	lisp_push(vm, (Lisp_Object*)lisp_number_new(vm, t));
}

static bool numeric_lt(Lisp_VM *vm, double a, double b)
{
	return a < b;
}

static bool numeric_eq(Lisp_VM *vm, double a, double b)
{
	return a == b;
}

static bool numeric_le(Lisp_VM *vm, double a, double b)
{
	return a <= b;
}

static bool numeric_gt(Lisp_VM *vm, double a, double b)
{
	return a > b;
}

static bool numeric_ge(Lisp_VM *vm, double a, double b)
{
	return a >= b;
}

static void op_numeric_test(Lisp_VM*vm,Lisp_Pair*args,
	bool (test)(Lisp_VM*,double,double))
{
	while (args != LISP_NIL && args->cdr != LISP_NIL) {
		Lisp_Number *a = safe_ptr(vm, CAR(args), O_NUMBER);
		Lisp_Number *b = safe_ptr(vm, CADR(args), O_NUMBER);
		if (!test(vm, a->value, b->value)) {
			lisp_push(vm, LISP_FALSE);
			return;
		}
		args = REST(args);
    }
    lisp_push(vm, LISP_TRUE);
}

static void op_display(Lisp_VM*vm, Lisp_Pair* args)
{
	Lisp_Port *output = vm->output;
	if (CADR(args)->type == O_PORT) {
		output = (Lisp_Port*)CADR(args);
	}
	Lisp_Object *a = args->car;
	if (a->type == O_STRING)
		lisp_port_printf(output, "%s", ((Lisp_String*)a)->buf);
	else
		lisp_port_print(output, a);
	lisp_push(vm, LISP_UNDEF);
}

static void op_print(Lisp_VM*vm, Lisp_Pair* args)
{
    Lisp_Port *output = vm->output;

    for (; args != LISP_NIL; args = REST(args)) {
        Lisp_Object *a = args->car;
        if (a->type == O_STRING) {
			Lisp_String *s = (Lisp_String*)a;
			lisp_port_puts(output, s->buf);
        } else {
            lisp_port_print(output, a);
		}
    }
    lisp_push(vm, LISP_UNDEF);
}

static void op_newline(Lisp_VM*vm, Lisp_Pair* args)
{
	Lisp_Port *output = vm->output;
	if (CAR(args)->type == O_PORT)
		output = (Lisp_Port*)CAR(args);
	lisp_port_putc(output, '\n');
	lisp_push(vm, LISP_UNDEF);
}

/* (if <test> <true-expr> <false-expr>) */
static void op_if(Lisp_VM*vm, Lisp_Pair* args)
{
	lisp_push(vm, args->car);
	lisp_eval(vm);
	if (lisp_pop(vm,1) == LISP_FALSE) {
		Lisp_Pair *p = REST(REST(args)); /* else clause */
		lisp_push(vm, p->car);
		lisp_eval_tail(vm);
	} else { /* Any other value will be taken as true, including nil */
		lisp_push(vm, REST(args)->car);
		lisp_eval_tail(vm);
	}
}

/* (<test> <expression1> ...) */
static bool exec_cond_clause(Lisp_VM *vm, Lisp_Pair *p)
{
	lisp_push(vm, p->car);
	lisp_eval(vm);
	if (lisp_pop(vm,1) != LISP_FALSE) {
		if (!is_list(p->cdr))
			lisp_err(vm, "cond: invalid expression");
		eval_list(vm, (Lisp_Pair*)p->cdr, 1);
		return true;
	}
	return false;
}

/* (cond clause1 ... ) */
static void op_cond(Lisp_VM*vm, Lisp_Pair* args)
{
	for (;args != LISP_NIL;args = REST(args)) {
		Lisp_Pair *p = safe_ptr(vm, args->car, O_PAIR);
		if (exec_cond_clause(vm, p))
			return;
	}
	lisp_push(vm, LISP_UNDEF);
}

/* (case <key>
      [a <expr>]
      [(b c) <body>]
      [else ...])
 */
static void op_case(Lisp_VM *vm, Lisp_Pair *args)
{
	lisp_push(vm, CAR(args));
	Lisp_Object *k = lisp_eval(vm);
	for (args = REST(args); args != LISP_NIL; args = REST(args))
	{
		bool hit = false;
		Lisp_Object *t = CAR(args);
		if (!is_list(t))
			lisp_err(vm, "case: bad clause");
		if (lisp_eq(k, CAR(t)) || CAR(t) == (Lisp_Object*)SYM(S_ELSE))
			hit = true;
		else if (is_list(CAR(t))) {
			Lisp_Pair *l = (Lisp_Pair*)CAR(t);
			for (; l != LISP_NIL; l = REST(l))
			{
				if (lisp_eq(k, l->car)) {
					hit = true;
					break;
				}
			}
		}
		if (hit)
		{
			eval_list(vm, REST(t), 1);
			lisp_push(vm, lisp_pop(vm, 2));
			break;
		}
	}
	if (args == LISP_NIL) {
		lisp_pop(vm, 1);
		lisp_push(vm, LISP_UNDEF);
	}
}

static void op_and(Lisp_VM*vm, Lisp_Pair* args)
{
	if (args == LISP_NIL) {
		lisp_push(vm, LISP_TRUE);
		return;
	}
	while (true) {
		lisp_push(vm, CAR(args));
		if (lisp_eval(vm) == LISP_FALSE)
			return;
		args = REST(args);
		if (args == LISP_NIL)
			return;
		lisp_pop(vm, 1);
	}
}

static void op_or(Lisp_VM*vm, Lisp_Pair* args)
{
	if (args == LISP_NIL) {
		lisp_push(vm, LISP_FALSE);
		return;
	}
	while (true) {
		lisp_push(vm, CAR(args));
		if (lisp_eval(vm) != LISP_FALSE)
			return;
		args = REST(args);
		if (args == LISP_NIL)
			return;
		lisp_pop(vm, 1);
	}
}


static void op_set(Lisp_VM*vm, Lisp_Pair*args)
{
	if (args->car->type != O_SYMBOL)
		lisp_err(vm, "set!: bad variable name");
	Lisp_String *name = (Lisp_String*)args->car;
	if (args->car->is_const)
		lisp_err(vm, "set!: constant symbol `%s'", name->buf);
	lisp_push(vm, ((Lisp_Pair*)args->cdr)->car);
	Lisp_Object *o = lisp_eval(vm);
	lisp_setvar(vm, name, o);
	lisp_pop(vm, 1);
	lisp_push(vm, LISP_UNDEF);
}

static void op_trace(Lisp_VM *vm, Lisp_Pair *args, int enabled)
{
	for (;args != LISP_NIL; args = REST(args)) {
		(CAR(args))->tracing = enabled;
	}
	lisp_push(vm, LISP_UNDEF);
}

static Lisp_Proc* op_lambda(Lisp_VM *vm, Lisp_Env *env, Lisp_Pair *args)
{
	if (!is_list(args->car))
		lisp_err(vm, "Bad arguments: not a list");
	for (Lisp_Pair *p=(Lisp_Pair*)args->car; p!=LISP_NIL; p=REST(p)) {
		if (p->car->type != O_SYMBOL)
			lisp_err(vm, "Bad argument: %s",
				objtypes[p->car->type].name);
		Lisp_String *s = (Lisp_String*)p->car;
		if (p->car->is_const && s->buf[0]!='&') {
			lisp_err(vm, "Bad argument: `%s' is const", s->buf);
		}
	}
	Lisp_Proc *proc = new_obj(vm, O_PROC);
	proc->env = env;
	proc->lambda = args;
	lisp_push(vm, (Lisp_Object*)proc);
	return proc;
}

static Lisp_Pair* defproc(Lisp_VM*vm, Lisp_Pair*args, Lisp_Pair* body)
{
	if (args->car->type != O_SYMBOL) {
		lisp_err(vm, "procedure name must be a symbol");
	}
	lisp_push(vm, (Lisp_Object*)SYM(S_ARG_LABEL));/* &label marker */
	lisp_push(vm, (Lisp_Object*)args);
	make_pair(vm);
	lisp_push(vm, (Lisp_Object*)body);
	make_pair(vm); /* since body is a list, the final to op_lambda is also a list */
	op_lambda(vm, vm->env, (Lisp_Pair*)lisp_top(vm, 0));
	Lisp_Pair *p = lisp_defvar(vm, (Lisp_String*)args->car, lisp_top(vm, 0));
	lisp_pop(vm, 2);
	lisp_push(vm, args->car);
	return p;
}

/*
 * (define <name> <val>)
 * (define (<name> ...) ...)
 */
static Lisp_Pair* op_define(Lisp_VM *vm, Lisp_Pair *args)
{
	if (args == LISP_NIL)
		lisp_err(vm, "define: missing arguments");
	if (vm->env->obj.no_def)
		lisp_err(vm, "define: prohibited");
	if (args->car->type == O_SYMBOL) {
		Lisp_String *name = (Lisp_String*)(CAR(args));
		if (name->obj.is_const)
			lisp_err(vm, "define: const %s", name->buf);
		lisp_push(vm, (Lisp_Object*)name);
		lisp_push(vm, CADR(args)); /* push EXPR */
		vm->env->obj.no_def = 1;
		Lisp_Object *val = lisp_eval(vm); /* evaluate EXPR to value */
		vm->env->obj.no_def = 0;
		Lisp_Pair *p = lisp_defvar(vm, name, val);
		lisp_pop(vm, 1); /* leaves name on the stack */
		return p;
	} else if (args->car->type == O_PAIR) {
		return defproc(vm, (Lisp_Pair*)args->car, REST(args));
	} else {
		lisp_err(vm, "define: bad arguments");
	}
	assert(0);
	return NULL;
}

static void op_defconst(Lisp_VM *vm, Lisp_Pair *args)
{
	Lisp_Pair *p = op_define(vm, args);
	p->obj.is_const = 1;
}

static void op_defmethod(Lisp_VM *vm, Lisp_Pair* args)
{
	Lisp_Pair *p = op_define(vm, args);
	if (p->cdr->type != O_PROC)
		lisp_err(vm, "defmethod: not procedure");
	p->obj.is_method = 1;
}

static void op_defmacro(Lisp_VM *vm, Lisp_Pair *args)
{
	if (vm->env->obj.no_def)
		lisp_err(vm, "defmacro: prohibited");
	if (args->car->type != O_PAIR)
		lisp_err(vm, "defmacro: bad arguments");
	Lisp_Pair *p = defproc(vm, (Lisp_Pair*)args->car, REST(args));
	p->cdr->is_special = 1;
	p->cdr->type = O_MACRO;
}

/*
   (match <key>
      [<pattern> <body>]
      [else <body> ])
	The difference from case is that the body is executed
	in new closure. it's like let.
 
 */
static void op_match(Lisp_VM *vm, Lisp_Pair *args)
{
	lisp_push(vm, CAR(args));
	Lisp_Object *k = lisp_eval(vm);
	bool k_is_list = is_list(k);
	Lisp_Pair *vars = NULL;
	Lisp_Pair *vals = NULL;
	for (args = REST(args); args != LISP_NIL; args = REST(args))
	{
		Lisp_Object *t = CAR(args);
		
		if (!is_list(t))
			lisp_err(vm, "match: bad clause");
		
		if (lisp_eq(k, CAR(t)) || CAR(t)==(Lisp_Object*)SYM(S_ELSE))
		{
			vars = LISP_NIL;
			vals = LISP_NIL;
		}
		else if (k_is_list && is_list(CAR(t)))
		{
			Lisp_Pair *l = (Lisp_Pair*)CAR(t);
			if (l->car->type != O_SYMBOL)
				lisp_err(vm, "match: first element must be a symbol");
			if (l->car == CAR(k))
			{
				vars = (Lisp_Pair*)l->cdr;
				vals = REST(k);
			}
		}

		if (vars) {
			lisp_push(vm, (Lisp_Object*)vars);
			lisp_push(vm, CDR(t)); // body
			lisp_cons(vm);
			op_lambda(vm, vm->env, (Lisp_Pair*)lisp_top(vm, 0));
			Lisp_Proc *proc = (Lisp_Proc*)lisp_top(vm, 0);
			apply_procedure(vm, proc, vals);
			lisp_push(vm, lisp_pop(vm, 4));
			return;
		}
	}
	lisp_pop(vm, 1);
	lisp_push(vm, LISP_UNDEF);
}

/* (let optional-label (bindings) body ) */
static void op_let(Lisp_VM *vm, Lisp_Pair *args)
{
	int n = (int)vm->stack->count;
	if (CAR(args)->type == O_SYMBOL) {
		pushx(vm, SYM(S_ARG_LABEL));
		lisp_push(vm, CAR(args));
		args = REST(args);
	}
	Lisp_Pair *p = list_ptr(vm, CAR(args));
	for(;p!=LISP_NIL;p=REST(p)) {
		if (!is_list(p->car))
			lisp_err(vm, "let parameter: not a list");
		lisp_push(vm, CAR(p->car));
	}
	lisp_make_list(vm, (int)vm->stack->count - n);
	lisp_push(vm, CDR(args)); /* body */
	make_pair(vm);
	op_lambda(vm, vm->env, (Lisp_Pair*)lisp_top(vm, 0));
	Lisp_Proc*proc = (Lisp_Proc*)lisp_top(vm, 0);
	
	p = (Lisp_Pair*)CAR(args);
	n = (int)vm->stack->count;
	for(;p!=LISP_NIL;p=REST(p)) {
		/* `p->car' is a list by previous check */
		lisp_push(vm, CADR(p->car));
		lisp_eval(vm);
	}
	lisp_make_list(vm, (int)vm->stack->count - n);
	p = (Lisp_Pair*)lisp_top(vm, 0);
	apply_procedure(vm, proc, p);
	lisp_push(vm, lisp_pop(vm, 4));
}

static Lisp_Object* quasiquote(Lisp_VM *vm, Lisp_Object *o)
{
	if (o->type != O_PAIR || o == LISP_NIL) {
		lisp_push(vm, o);
		return o;
	}
	Lisp_Pair *p = (Lisp_Pair*)o;
	
	/* Is an unquote? */
	if (p->car == (Lisp_Object*)SYM(S_UNQUOTE_SPLICING)
	 || p->car == (Lisp_Object*)SYM(S_UNQUOTE)) {
		if (!is_list(p->cdr))
			lisp_err(vm, "unquote: not a list");
		lisp_push(vm, CAR(p->cdr));
		return lisp_eval(vm);
	}

	Lisp_Object *qqcar = quasiquote(vm, p->car);
	Lisp_Object *qqcdr = quasiquote(vm, p->cdr);
	if (qqcdr == p->cdr && qqcar == p->car) {
		lisp_pop(vm, 2);
		lisp_push(vm, o);
		return o;
	} else if (p->car->type == O_PAIR
	  && CAR(p->car) == (Lisp_Object*)SYM(S_UNQUOTE_SPLICING)) {
		if (!is_list(qqcar))
			lisp_err(vm, "unquote-splicing: not a list");
		append(vm, (Lisp_Pair*)qqcar, qqcdr);
		o = lisp_pop(vm, 3);
		lisp_push(vm, o);
		return o;
	} else {
		return (Lisp_Object*)make_pair(vm);
	}
}

static Lisp_SourceFile *ensure_source_file(Lisp_VM* vm, Lisp_String* path)
{
	Lisp_Pair *p = lisp_dict_assoc_cstr(vm->source_files, path->buf);
	if (p) {
		assert(p->cdr->type == O_SOURCE_FILE);
		return (Lisp_SourceFile*)p->cdr;
	}
	Lisp_SourceFile *f = new_obj(vm, O_SOURCE_FILE);
	pushx(vm, f);
	f->path = path;
	f->mappings = lisp_array_new(vm, 64);
	lisp_make_symbol(vm, path->buf);
	lisp_dict_add(vm->source_files, (Lisp_String*)lisp_top(vm, 0), (Lisp_Object*)f);
	lisp_pop(vm, 2);
	return f;
}


static bool is_standard_path(const char *s)
{
	// FIXME windows
	return s[0]
		&& !strstr(s, "/./")
		&& !strstr(s, "/../")
		&& !strstr(s, "//");
}

static bool is_absolute_path(const char *s)
{
	return s[0] == '/' || s[0] == '\\' || (isalpha(s[0]) && s[1] == ':');
}

static bool file_exists(const char *path)
{
	struct stat sb;
	if (stat(path, &sb) == 0)
		if (sb.st_mode & S_IFREG)
			return true;
	return false;	
}

#define is_path_sep(c) ((c) == '/' || ((c) == '\\'))

static void remove_last_path_component(Lisp_VM *vm, Lisp_Buffer* buf)
{
	if (buf->length == 0 || (buf->length==1 && buf->buf[0] == '/'))
		lisp_err(vm, "Invalid path");
	int i = (int)buf->length - 1;
	for (; i > 0; i--)
	{
		if (is_path_sep(buf->buf[i-1]))
			break;
	}
	buf->length = i;
}

static Lisp_String* resolve_path(Lisp_VM *vm, Lisp_String *path, Lisp_String *base, bool isBaseDir, bool allowUpwards)
{
	if (base == NULL || base->length == 0 || base->buf[0] == '*')
		goto Done;
	
	Lisp_Buffer *buf = lisp_buffer_new(vm, 256);
	pushx(vm, buf);
	lisp_buffer_adds(buf, base->buf);
	if (!isBaseDir)
		remove_last_path_component(vm, buf);
	if (buf->length > 0)
	{
		if (!is_path_sep(buf->buf[buf->length-1]))
			lisp_buffer_add(buf, '/');
	}
	const char *p = path->buf;
	while (*p == '.') {
		if (p[1] == '/' || p[1] == '\\') {
			p+=2;
		} else if (p[1] == '.') {
			if (p[2] == '/' || p[2] == '\\') {
				if (!allowUpwards)
					lisp_err(vm, "Invalid path");
				remove_last_path_component(vm, buf);
				p += 3;
			} else {
				lisp_err(vm, "Invalid path");
			}
		} else {
			break;
		}
	}
	lisp_buffer_adds(buf, p);
	path = lisp_string_new(vm, (const char*)buf->buf, buf->length);
	lisp_pop(vm, 1);
Done:
	if (!file_exists(path->buf))
		return NULL;
	pushx(vm, path);
	return path;
}

static bool find_file(Lisp_VM *vm, Lisp_String *path)
{
	if (!is_standard_path(path->buf))
		lisp_err(vm, "find file: bad path: `%s`", path->buf);

	/* if path is absolute, return it */
	if (is_absolute_path(path->buf) || file_exists(path->buf)) {
		pushx(vm, path);
	} else if (!resolve_path(vm, path, vm->input->name, false, true)) {
		// Look in list `load_path'
		Lisp_Pair *t = lisp_env_assoc(vm->env, SYM(S_LOAD_PATH));
		if (t) {
			if (!is_list(t->cdr))
				lisp_err(vm, "load-path is not a list");
			
			for (t = REST(t); t != LISP_NIL; t = REST(t)) {
				if (t->car->type != O_STRING)
					lisp_err(vm, "load-path must be a list of strings");
				if (resolve_path(vm, path, (Lisp_String*)t->car, true, false))
					break;
			}
		
			if (t == LISP_NIL)
				return false;
		} else {
			return false;
		}
	}
	return true;
}

static void op_find_file(Lisp_VM *vm, Lisp_Pair *args)
{
	Lisp_String *path = safe_ptr(vm, CAR(args), O_STRING);
	if (!find_file(vm, path))
		lisp_push(vm, lisp_false);
}

static void op_load(Lisp_VM *vm, Lisp_Pair *args)
{
	Lisp_String *path = safe_ptr(vm, CAR(args), O_STRING);
	if (strcmp(path->buf, "*stdin*") != 0)
	{
		if (!find_file(vm, path))
			lisp_err(vm, "load: file not found: `%s`", path->buf);
		path = (Lisp_String*)lisp_top(vm, 0);
		assert(path->obj.type == O_STRING);
	}
	lisp_push(vm, (Lisp_Object*)vm->input);
	vm->input = lisp_open_input_file(vm, path);
	vm->input->src_file = ensure_source_file(vm, path);
	load(vm);
	lisp_exch(vm);
	lisp_port_close(vm->input);
	vm->input = (Lisp_Port*)lisp_pop(vm, 1);
	lisp_exch(vm);
	lisp_pop(vm, 1);
}

/*
 * (catch <body>...)
 * CATCH is a special form like begin, except that
 * errors and objects thrown during execution will be caught and returned.
 */
static void op_catch(Lisp_VM *vm, Lisp_Pair *args)
{
	jmp_buf *prev = vm->catch;
	jmp_buf jbuf;
	unsigned old_level = vm->eval_level;
	size_t cnt = vm->stack->count;
	Lisp_Env *old_env = vm->env;
	vm->catch = &jbuf;
	if (setjmp(jbuf) == 0)
		eval_list(vm, args, 0);
	assert(vm->stack->count > cnt);
	vm->eval_level = old_level;
	vm->catch = prev;
	vm->env = old_env;
	vm->stack->items[cnt] = lisp_top(vm, 0);
	vm->stack->count = cnt+1;
}

/*
 * (throw <object>)
 */
static void op_throw(Lisp_VM *vm, Lisp_Pair *args)
{
	if (CDR(args) != LISP_NIL)
		lisp_err(vm, "throw: too many arguments");
	lisp_push(vm, CAR(args));
	longjmp(*vm->catch, 2);
}

static void op_error(Lisp_VM*vm, Lisp_Pair*args)
{
	Lisp_Pair *p = args;
	lisp_port_puts(vm->error, "ERROR:");
    Lisp_Object *o = CAR(args);
    if (o->type == O_STRING || o->type == O_SYMBOL) {
        lisp_port_putc(vm->error, ' ');
        lisp_port_puts(vm->error, ((Lisp_String*)o)->buf);
        args = REST(args);
        if (args != LISP_NIL)
            lisp_port_puts(vm->error, " --");
    }
	for (;args!=LISP_NIL; args=REST(args)) {
		Lisp_Object *a = CAR(args);
		lisp_port_putc(vm->error, ' ');
        lisp_port_print(vm->error, a);
	}
	lisp_port_putc(vm->error, '\n');
	throw_error(vm, (Lisp_Object*)p);
}

static void math_fn(Lisp_VM *vm, Lisp_Pair *args,
	double (fn)(double))
{
	if (args->car->type != O_NUMBER) {
		lisp_err(vm, "not a number");
	}
	Lisp_Number *num = lisp_number_new(vm, fn(NUMVAL(CAR(args))));
	lisp_push(vm, (Lisp_Object*)num);
}

static void math_fn2(Lisp_VM *vm, Lisp_Pair *args,
	double (fn)(double, double))
{
	if (CAR(args)->type != O_NUMBER || CADR(args)->type != O_NUMBER) {
		lisp_err(vm, "not a number");
	}
	Lisp_Number *num = lisp_number_new(vm,
		fn(NUMVAL(CAR(args)), NUMVAL(CADR(args))));
	lisp_push(vm, (Lisp_Object*)num);
}

/* (NUMBER-TO-STRING n &optional base) */
static void op_num2str(Lisp_VM*vm, Lisp_Pair* args)
{
	char buf[64];
	int n = 0;
	int base = (CDR(args) == LISP_NIL ? 10 : safe_int(vm, CADR(args)));
	double value = safe_num(vm, CAR(args));
	switch (base) {
		case 10:
			n = snprintf(buf, sizeof(buf), "%.16g", value);
			break;
		case 16: case 8:
			if (!is_integer(value))
				lisp_err(vm, "number->string: not an integer for base %d", base);
			if (value < 0)
				lisp_err(vm, "number->string: base %d: integer not postive", base);
			n = snprintf(buf, sizeof(buf), base == 8 ? "%llo" : "%llx",
				(long long)round(value));
			break;
		default:
			lisp_err(vm, "number->string: unsupported base %d", base);
			break;
	}
	lisp_push(vm, (Lisp_Object*)lisp_string_new(vm, buf, n));
}

/* (format obj &rest params) */
static void op_format(Lisp_VM *vm, Lisp_Pair *args)
{
	Lisp_Object *o = CAR(args);
	Lisp_Object *w = LISP_UNDEF;
	Lisp_Object *p = LISP_UNDEF;
	int width = -1, precision = -1;
	if (w != LISP_UNDEF) width = safe_int(vm, w);
	if (p != LISP_UNDEF) precision = safe_int(vm, p);
	if (o->type == O_NUMBER) {
		char buf[DTOA_BUFSIZE];
		if (precision < 0) {
			dtoa(NUMVAL(o), buf);
		} else {
			if (precision > DBL_DIG+2)
				precision = DBL_DIG+2;
			snprintf(buf, sizeof(buf), "%.*f", precision, NUMVAL(o));
		}
		size_t len = strlen(buf);
		if (width < 0 || (unsigned)width <= len) {
			pushx(vm, lisp_string_new(vm, buf, len));
		} else {
			// Number padding happens on the left
			Lisp_String *s = lisp_push_string(vm, NULL, width);
			memset((char*)s->buf, ' ', width - (int)len);
			strcpy((char*)(s->buf+width-len), buf);
		}
	} else if (o->type == O_STRING || o->type == O_SYMBOL) {
		Lisp_String *s = (Lisp_String*)o;
		if (width <= 0 || width < (int)s->length)
			lisp_push_string(vm, s->buf, s->length);
		else {
			// string padding happens on the right
			Lisp_String *s2 = lisp_push_string(vm, NULL, width);
			strcpy((char*)s2->buf, s->buf);
			memset((char*)s2->buf + s->length, ' ', width - s->length);
		}
	} else {
		lisp_err(vm, "Bad arguments");
	}
}

/*
 * (pump <source> <sink> <size>)
 */
static void op_pump(Lisp_VM *vm, Lisp_Pair *args)
{
	Lisp_Port *source = (Lisp_Port*)CAR(args);
	Lisp_Port *sink = (Lisp_Port*)CADR(args);
	Lisp_Object *oSize = CADR(CDR(args));
	size_t size = SIZE_MAX;
	if (oSize != LISP_UNDEF) {
		size = (size_t)lisp_safe_number(vm, oSize);
	}
	if (sink == (void*)LISP_NIL) {
		sink = vm->output;
	}
	if (source->obj.type != O_PORT || source->out) {
		lisp_err(vm, "bad source");
	}
	if (sink->obj.type != O_PORT || !sink->out) {
		lisp_err(vm, "bad sink");
	}
	
	size_t n = 0;
	while (n < size && lisp_port_fill(source) > 0) {
		size_t len = source->iobuf->length - source->input_pos;
		uint8_t *bytes = source->iobuf->buf + source->input_pos;
		len = MIN(len, (size-n));
		lisp_port_put_bytes(sink, bytes, len);
		source->input_pos += len;
		n += len;
	}
	lisp_port_flush(sink);
	lisp_push_number(vm, n);
}

/* (exists? <var> &optional <env>) */
static void op_exists(Lisp_VM *vm, Lisp_Pair *args, bool method)
{
	Lisp_String *name = safe_ptr(vm, CAR(args), O_SYMBOL);
	Lisp_Env *env = vm->env;
	if (CADR(args)->type == O_ENV)
		env = (Lisp_Env*)CADR(args);
	else if (CADR(args)->type == O_DICT && !method) {
		Lisp_Array *d = (Lisp_Array*)CADR(args);
		if (lisp_dict_assoc(d, name)) {
			lisp_push(vm, lisp_true);
		} else {
			lisp_push(vm, lisp_false);
		}
		return;
	} else if (CDR(args) != LISP_NIL)
		lisp_err(vm, "Invalid env");
	Lisp_Pair *p = lisp_env_assoc(env, name);
	if (!method)
		op_p(vm, p || name->obj.is_const || name->obj.is_primitive);
	else
		op_p(vm, p && p->obj.is_method);
}

static void op_concat(Lisp_VM*vm, Lisp_Pair *args)
{
	bool all_strings = true;
	size_t total_length = 0;
	for (Lisp_Pair *p = args; p != LISP_NIL; p = REST(p))
	{
		if (CAR(p)->type != O_STRING && CAR(p)->type != O_SYMBOL)
		{
			all_strings = false;
		}
		else
		{
			Lisp_String *s = (Lisp_String *)CAR(p);
			total_length += s->length;
		}
	}
	
	if (all_strings)
	{
		Lisp_String *s = lisp_push_string(vm, NULL, total_length);
		char *t = (char*)s->buf;
		for (Lisp_Pair *p = args; p != LISP_NIL; p = REST(p))
		{
			Lisp_String *s =  (Lisp_String*)CAR(p);
			strcpy(t, s->buf);
			t += s->length;
		}
		return;
	}

	Lisp_Buffer *buf = lisp_buffer_new(vm, total_length+32);
	pushx(vm, buf);
	Lisp_Port *port = lisp_open_output_buffer(vm, buf);
	pushx(vm, port);
	for (Lisp_Pair *p = args; p != LISP_NIL; p = REST(p))
	{
		Lisp_Object *o = CAR(p);
		if (o->type == O_STRING || o->type == O_SYMBOL)
			lisp_port_puts(port, ((Lisp_String*)o)->buf);
		else
			lisp_port_print(port, o);
	}
	pushx(vm, lisp_string_new(vm, (char*)buf->buf, buf->length));
	lisp_push(vm, lisp_pop(vm, 3));
}

#define op_buffer_set(T, N) do { \
		union { T val; uint8_t bytes[N]; } u; \
		Lisp_Buffer *b = safe_ptr(vm, CAR(args), O_BUFFER); \
		vm_check(b->vm); \
		int i = lisp_safe_int(vm, CADR(args)); \
		if (i < 0 || (unsigned)(i + N) > b->length) \
			lisp_err(vm, "Bad offset: %d", i); \
		u.val = (T)lisp_safe_number(vm, CAR(CDDR(args))); \
		memcpy_r(b->buf + i, u.bytes, N); \
		lisp_push(vm, LISP_UNDEF); \
} while (0)

#define op_buffer_get(T, N) do { \
		union { T val; uint8_t bytes[N]; } u; \
		Lisp_Buffer *b = safe_ptr(vm, CAR(args), O_BUFFER); \
		int i = lisp_safe_int(vm, CADR(args)); \
		if (i < 0 || (unsigned)(i + N) > b->length) \
			lisp_err(vm, "Bad offset: %d", i); \
		memcpy_r(u.bytes, b->buf + i, N); \
		lisp_push_number(vm, (double)u.val); \
} while (0)

#define vm_check(x) do {\
 if (vm != (x)) { \
   lisp_err(vm, "Can not modify foreign object"); \
 } \
} while (0)

/*
 * apply_primitive -- The dispatch center
 * Push the result onto stack.
 * - sid: The primitive symbol id
 */
static void apply_primitive(Lisp_VM*vm, int sid, Lisp_Pair* args)
{
	switch (sid) {
	case S_ADD: op_add(vm, args); break;
	case S_MUL: op_mul(vm, args); break;
	case S_SUB: op_sub(vm, args); break;
	case S_DIV: op_div(vm, args); break;
	case S_NUMBER_LT:
		op_numeric_test(vm, args, numeric_lt);
		break;
	case S_NUMBER_LE:
		op_numeric_test(vm, args, numeric_le);
		break;
	case S_NUMBER_EQ:
		op_numeric_test(vm, args, numeric_eq);
		break;
	case S_NUMBER_GT:
		op_numeric_test(vm, args, numeric_gt);
		break;
	case S_NUMBER_GE:
		op_numeric_test(vm, args, numeric_ge);
		break;
		
	case S_QUOTE: lisp_push(vm, args->car); break;
	case S_QUASIQUOTE: quasiquote(vm, args->car); break;
	case S_LIST:  lisp_push(vm, (Lisp_Object*)args); break;
	case S_NOT: op_p(vm, args->car==LISP_FALSE); break;
	case S_LISTP: op_p(vm, is_list(args->car)); break;
	case S_NULLP: op_p(vm, args->car==LISP_NIL); break;
	case S_PAIRP:
		op_p(vm, args->car->type == O_PAIR);
		break;
	case S_CONS:
		lisp_push(vm, (Lisp_Object*)lisp_pair_new(vm, CAR(args), CADR(args)));
		break;
	case S_CONSQ:
		lisp_push(vm, CAR(args));
		lisp_push(vm, CADR(args));
		lisp_eval(vm);
		lisp_cons(vm);
		break;
	case S_CLONE: clone(vm, CAR(args)); break;
	case S_CAR:
		switch (args->car->type) {
		case O_PAIR:
			lisp_push(vm, ((Lisp_Pair*)args->car)->car);
			break;
		case O_MACRO: case O_PROC:
			lisp_push(vm, (Lisp_Object*)(((Lisp_Proc*)args->car)->env));
			break;
		default:
			lisp_err(vm, "car: invalid operand");
		}
		break;
	case S_CDR:
		switch (args->car->type) {
		case O_PAIR:
			lisp_push(vm, ((Lisp_Pair*)args->car)->cdr);
			break;
		case O_MACRO: case O_PROC:
			lisp_push(vm, (Lisp_Object*)((Lisp_Proc*)args->car)->lambda);
			break;
		default:
			lisp_err(vm, "cdr: invalid operand");
			break;
		}
		break;
	case S_APPEND: {
		Lisp_Pair *l1 = list_ptr(vm, CAR(args));
		Lisp_Pair *l2 = list_ptr(vm, CADR(args));
		append(vm, l1, (Lisp_Object*)l2);
		break;
	}
	case S_THIS: pushx(vm, vm->env); break;
	case S_RETURN:
		lisp_push(vm, CAR(args));
		lisp_eval_tail(vm);
		lisp_push(vm, lisp_nil);
		lisp_cons(vm)->obj.is_return = 1;
		break;
	case S_DEBUG:
		lisp_push(vm, (Lisp_Object*)args);
		lisp_eval(vm);
		break;
	case S_BEGIN: eval_list(vm, args, 1); break;
	case S_COND: op_cond(vm, args); break;
	case S_CASE: op_case(vm, args); break;
	case S_MATCH: op_match(vm, args); break;
	case S_IF: op_if(vm, args); break;
	case S_AND: op_and(vm, args); break;
	case S_OR: op_or(vm, args); break;
	case S_TRACE: op_trace(vm, args, 1); break;
	case S_UNTRACE: op_trace(vm, args, 0); break;
	case S_LAMBDA: op_lambda(vm, vm->env, args); break;
	case S_DEFMACRO: op_defmacro(vm, args); break;
	case S_DEFMETHOD: op_defmethod(vm, args); break;
	case S_DEFINE: op_define(vm, args); break;
	case S_DEFCONST: op_defconst(vm, args); break;
	case S_LET: op_let(vm, args); break;
	case S_SET: op_set(vm, args); break;
	case S_ERROR: op_error(vm, args); break;
	case S_DISPLAY: op_display(vm, args); break;
	case S_PRINT: op_print(vm, args); break;
	case S_PRINTLN: op_print(vm, args); lisp_putc(vm, '\n'); break;
	case S_NEWLINE: op_newline(vm, args); break;
	case S_FIND_FILE: op_find_file(vm, args); break;
	case S_LOAD: op_load(vm, args); break;
	case S_CATCH: op_catch(vm, args); break;
	case S_THROW: op_throw(vm, args); break;
	case S_NUMBERP: op_p(vm, CAR(args)->type == O_NUMBER); break;
	case S_INTEGERP:
		op_p(vm, CAR(args)->type == O_NUMBER &&
		    is_integer(((Lisp_Number*)CAR(args))->value));
		break;
	case S_SYSTEM: {
    #ifndef LISP_ENABLE_SYSTEM
        lisp_err(vm, "(system) not available");
    #else
        const char *cs = lisp_safe_cstring(vm, CAR(args));
        lisp_push_number(vm, system(cs));
    #endif
		break;
	}
	// Math Functions
	case S_ABS:        math_fn(vm, args, fabs);  break;
	case S_ACOS:       math_fn(vm, args, acos);  break;
	case S_ASIN:       math_fn(vm, args, asin);  break;
	case S_COS:        math_fn(vm, args, cos);   break;
	case S_CEIL:       math_fn(vm, args, ceil);  break;
	case S_FLOOR:      math_fn(vm, args, floor); break;
	case S_ROUND:      math_fn(vm, args, round); break;
	case S_SIN:        math_fn(vm, args, sin);   break;
	case S_SQRT:       math_fn(vm, args, sqrt);  break;
	case S_TAN:        math_fn(vm, args, tan);   break;
	case S_EXP:        math_fn(vm, args, exp);   break;
	case S_LOG:        math_fn(vm, args, log);   break;
	case S_TRUNCATE:   math_fn(vm, args, trunc); break;
	case S_MOD:        math_fn2(vm, args, fmod);  break;
	case S_ATAN:
		if (args->cdr == LISP_NIL) {
			math_fn(vm, args, atan);
		} else {
			math_fn2(vm, args, atan2);
		}
		break;
	case S_NEW:
		if (args->car->type != O_PROC)
			lisp_err(vm, "new: not a procedure");
		apply_constructor(vm, (Lisp_Proc*)CAR(args), REST(args));
		break;
	case S_APPLY:
		if (!is_list(CADR(args)))
			lisp_err(vm, "apply: arguments not a list");
		lisp_apply(vm, CAR(args), (Lisp_Pair*)CADR(args));
		break;
	case S_EVALQ:
		lisp_push(vm, CAR(args));
		lisp_eval(vm);
		break;
	case S_EVAL:
		if (CDR(args) == LISP_NIL) {
			lisp_push(vm, CAR(args));
			lisp_eval(vm);
		} else if (CADR(args)->type == O_ENV) {
			Lisp_Env *env = (Lisp_Env*)CADR(args);
			if (env->bindings->vm != vm)
				lisp_err(vm, "foreign vm");
			pushx(vm, vm->env);
			vm->env = env;
			lisp_push(vm, CAR(args));
			lisp_eval(vm);
			lisp_exch(vm);
			vm->env = (Lisp_Env*)lisp_pop(vm, 1);
			assert(vm->env->obj.type == O_ENV);
		} else {
			lisp_err(vm, "eval: not environment");
		}
		break;
	case S_ARRAYP: op_p(vm, CAR(args)->type == O_ARRAY); break;
	case S_DICTP: op_p(vm, CAR(args)->type == O_DICT); break;
	case S_ENVP: op_p(vm, CAR(args)->type == O_ENV); break;
	case S_STRINGP: op_p(vm, CAR(args)->type == O_STRING); break;
	case S_SYMBOLP: op_p(vm, CAR(args)->type == O_SYMBOL); break;
	case S_EXISTS: op_exists(vm, args, false); break;
	case S_METHODP: op_exists(vm, args, true); break;
	case S_ATOMP:
		op_p(vm, CAR(args) == LISP_NIL ||
			CAR(args)->type == O_STRING ||
			CAR(args)->type == O_NUMBER ||
			CAR(args)->type == O_SYMBOL ||
			CAR(args)->type == O_BUFFER);
		break;
			
	case S_PROCEDUREP:
		if (CAR(args)->is_primitive ||
		    CAR(args)->type == O_PROC ||
		    CAR(args)->type == O_MACRO ||
		    CAR(args)->type == O_NATIVE_PROC)
		{
			lisp_push(vm, LISP_TRUE);
		} else {
			lisp_push(vm, LISP_FALSE);
		}
		break;
	case S_BOOLEANP: op_p(vm, CAR(args)==LISP_TRUE||CAR(args)==LISP_FALSE); break;
	case S_SORT: {
		if (CAR(args)->type != O_PAIR)
			lisp_err(vm, "sort: not a list");
		int n = push_list(vm, (Lisp_Pair*)CAR(args));
		lisp_array_sort(vm->stack, vm->stack->count-n, n);
		lisp_make_list(vm, n);
		break;
	}
	case S_ARRAY: {
		Lisp_Array *a = lisp_array_new(vm, 8);
		lisp_push(vm, (Lisp_Object*)a);
		for (Lisp_Pair *p = args; p != LISP_NIL; p = REST(p))
			lisp_array_push(a, CAR(p));
		break;
	}
	case S_MAKE_BUFFER: {
		int cap = 0;
		if (args != LISP_NIL)
			cap = safe_int(vm, CAR(args));
		if (CDR(args) == LISP_NIL) {
			pushx(vm, lisp_buffer_new(vm, cap));
		} else {
			int i = safe_int(vm, CADR(args));
			Lisp_Buffer *b = lisp_push_buffer(vm, NULL, cap);
			memset(b->buf, i, cap);
			b->length = cap;
		}
		break;
	}
	case S_STRING_TO_BUFFER: {
		Lisp_String *s = safe_ptr(vm, CAR(args), O_STRING);
		pushx(vm, lisp_buffer_copy(vm, s->buf, s->length));
		break;
	}
	case S_DICT: {
		Lisp_Array *a = lisp_dict_new(vm, 8);
		lisp_push(vm, (Lisp_Object*)a);
		for (Lisp_Pair *p = args; p != LISP_NIL; p = REST(p)) {
			Lisp_Pair *x = (Lisp_Pair*)p->car;
			if (x->obj.type != O_PAIR)
				lisp_err(vm, "dict: item not a pair");
			if (x->car->type != O_SYMBOL)
				lisp_err(vm, "dict: key not a symbol");
			lisp_dict_add(a, (Lisp_String*)x->car, x->cdr);
		}
		break;
	}
	case S_NTH: { /* (nth <l> <index>) */
		Lisp_Pair *l = safe_ptr(vm, CAR(args), O_PAIR);
		int index = safe_int(vm, CADR(args));
		if (index < 0) lisp_err(vm, "negative index");
		for (int i = 0; i < index; i++) {
			if (l->cdr->type != O_PAIR)
				lisp_err(vm, "not a list");
			l = REST(l);
		}
		if (l == LISP_NIL)
			lisp_err(vm, "out of bound: %d", index);
		lisp_push(vm, l->car);
		break;
	}
	case S_ARRAY_GET: {
		Lisp_Array *a = safe_ptr(vm, CAR(args), O_ARRAY);
		int index = safe_int(vm, CADR(args));
		if (index < 0 || (unsigned)index >= a->count)
			lisp_err(vm, "array-get: out of bound");
		lisp_push(vm, a->items[index]);
		break;
	}
	case S_ARRAY_SET: {
		Lisp_Array *a = safe_ptr(vm, CAR(args), O_ARRAY);
		vm_check(a->vm);
		int index = safe_int(vm, CADR(args));
		if (index < 0 || (unsigned)index >= a->count)
			lisp_err(vm, "array-set: out of bound");
		a->items[index] = CAR(CDR(CDR(args)));
		lisp_push(vm, LISP_UNDEF);
		break;
	}
	case S_ARRAY_COUNT: {
		Lisp_Array *a = safe_ptr(vm,CAR(args),O_ARRAY);
		lisp_push(vm, (Lisp_Object*)lisp_number_new(vm, a->count));
		break;
	}
	case S_ARRAY_POP:{
		Lisp_Array *a = safe_ptr(vm,CAR(args),O_ARRAY);
		vm_check(a->vm);
		Lisp_Object*o = lisp_array_pop(a, 1);
		lisp_push(vm, o);
		break;
	}
	case S_ARRAY_PUSH:{
		Lisp_Array *a = safe_ptr(vm,CAR(args),O_ARRAY);
		vm_check(a->vm);
		lisp_array_push(a, CADR(args));
		lisp_push(vm, LISP_UNDEF);
		break;
	}
	case S_DICT_SET: {
		Lisp_Array *a = safe_ptr(vm,CAR(args),O_DICT);
		vm_check(a->vm);
		Lisp_String*k = safe_ptr(vm,CADR(args),O_SYMBOL);
		Lisp_Pair *p = lisp_dict_assoc(a, k);
		if (p) {
			p->cdr = CAR(CDR(CDR(args)));
		} else {
			lisp_dict_add(a, k, CAR(CDR(CDR(args))));
		}
		lisp_push(vm, LISP_UNDEF);
		break;
	}
	case S_DICT_UNSET: {
		Lisp_Array *a = safe_ptr(vm,CAR(args),O_DICT);
		vm_check(a->vm);
		Lisp_String *k = safe_ptr(vm,CADR(args),O_SYMBOL);
		lisp_dict_remove(a, k);
		lisp_push(vm, LISP_UNDEF);
		break;
	}
	case S_DICT_GET: {
		Lisp_Array *a = safe_ptr(vm,CAR(args),O_DICT);
		Lisp_String*k = safe_ptr(vm,CADR(args),O_SYMBOL);
		Lisp_Pair *p = lisp_dict_assoc(a, k);
		if (p) {
			lisp_push(vm, p->cdr);
		} else {
			lisp_push(vm, LISP_UNDEF);
		}
		break;
	}
	case S_DICT_TO_LIST: {
		Lisp_Array *a = safe_ptr(vm,CAR(args),O_DICT);
		for (unsigned i = 1; i < a->count; i++)
			lisp_push(vm, a->items[i]);
		lisp_make_list(vm, (int)a->count-1);
		break;
	}
	case S_ARRAY_TO_LIST: {
		Lisp_Array *a = safe_ptr(vm,CAR(args),O_ARRAY);
		for (unsigned i = 0; i < a->count; i++)
			lisp_push(vm, a->items[i]);
		lisp_make_list(vm, (int)a->count);
		break;
	}
	case S_GET: {
		Lisp_Object *o = CAR(args);
		Lisp_Pair *p = REST(args);
		for (; p != LISP_NIL; p = REST(p)) {
			Lisp_Object *k = CAR(p);
			switch (o->type) {
			case O_PAIR: {
				Lisp_Pair *t = lisp_assoc((Lisp_Pair*)o, k);
				if (t) {
					o = t->cdr;
				} else {
					lisp_err(vm, "Bad key");
				}
				break;
			}
			case O_DICT: {
				Lisp_String *name = safe_ptr(vm, k, O_SYMBOL);
				Lisp_Pair *t = lisp_dict_assoc((Lisp_Array*)o, name);
				if (t) {
					o = t->cdr;
				} else {
					lisp_err(vm, "Bad key");
				}
				break;
			}
			case O_ENV: {
				Lisp_String *name = safe_ptr(vm, k, O_SYMBOL);
				Lisp_Pair *t = lisp_env_assoc((Lisp_Env*)o, name);
				if (t) {
					if (t->obj.is_const || t->obj.is_method)
						o = t->cdr;
					else
						lisp_err(vm, "Can not access: %s", name->buf);
				} else {
					lisp_err(vm, "Bad key");
				}
				break;
			}
			case O_ARRAY: {
				int i = safe_int(vm, k);
				if (i >= 0 && (size_t)i < ((Lisp_Array*)o)->count) {
					o = ((Lisp_Array*)o)->items[i];
				} else {
					lisp_err(vm, "Bad index");
				}
				break;
			}
			default:
				lisp_err(vm, "bad type: %s", objtypes[o->type].name);
			}
		}
		lisp_push(vm, o);
		break;
	}
	case S_EQP: op_p(vm, lisp_eq(CAR(args), CADR(args))); break;
	case S_WRITE_BUFFER: case S_WRITE_STRING: case S_WRITE:
	{
		Lisp_Port *p = vm->output;
		Lisp_Object *o = CADR(args);
		if (o->type == O_PORT) {
			p = (Lisp_Port*)o;
			if (!p->out)
				lisp_err(vm, "Not output port");
		} else if (o != LISP_UNDEF) {
			lisp_err(vm, "Not port");
		}
		if (sid == S_WRITE) {
			lisp_port_print(p, CAR(args));
		} else if (sid == S_WRITE_STRING) {
			Lisp_String *s = safe_ptr(vm, CAR(args), O_STRING);
			lisp_port_puts(p, s->buf);
		} else if (sid == S_WRITE_BUFFER) {
			Lisp_Buffer *b = safe_ptr(vm, CAR(args), O_BUFFER);
			lisp_port_put_bytes(p, b->buf, b->length);
		}
		lisp_push(vm, LISP_UNDEF);
		break;
	}
	case S_FLUSH:
	{
		Lisp_Port *port = safe_ptr(vm, CAR(args), O_PORT);
		vm_check(port->vm);
		lisp_port_flush(port);
		lisp_push(vm, LISP_UNDEF);
		break;
	}
	case S_RANDOM:
	{
		/* (random) returns a floating point number in range [0,1],
		   inclusively */
		vm->rand_next = (vm->rand_next * 1103515245 + 12345) & 0x7fffffff;
		pushx(vm, lisp_number_new(vm, (vm->rand_next>>16)/(double)0x7fff));
		break;
	}
	case S_RANDOM_SEED:
	{
		/* (random-seed <integer>) */
		vm->rand_next = lisp_safe_int(vm, CAR(args)) & 0x7fffffff;
		lisp_push(vm, LISP_UNDEF);
		break;
	}
	case S_READYP:
	{
		Lisp_Port *p = safe_ptr(vm, CAR(args), O_PORT);
		if (p->out)
			op_p(vm, p->iobuf->length < p->iobuf->cap);
		else
        {
            if (p->iobuf->length > p->input_pos)
                lisp_push(vm, LISP_TRUE);
            else if (p->stream
                  && p->stream->cls
                  && p->stream->context
                  && p->stream->cls->ready
                  && p->stream->cls->ready(p->stream->context, 0))
            {
                lisp_push(vm, LISP_TRUE);
            }
            else
                lisp_push(vm, LISP_FALSE);
		}
        break;
	}
	case S_READ:
	{
		Lisp_Port *old = vm->input;
		if (old)
			pushx(vm, old);
		Lisp_Object *o = CAR(args);
		if (o->type == O_PORT) {
			if (((Lisp_Port*)o)->out) {
				lisp_err(vm, "not input port");
			}
			vm_check(((Lisp_Port*)o)->vm);
			vm->input = (Lisp_Port*)o;
		} else if (o != LISP_UNDEF) {
			lisp_err(vm, "not port");
		}
		lisp_read(vm);
		if (old) {
			lisp_exch(vm);
			vm->input = (Lisp_Port*)lisp_pop(vm, 1);
			assert(vm->input == old);
		} else {
			vm->input = NULL;
		}
		break;
	}
	case S_INPUT_PORTP: {
		if (CAR(args)->type == O_PORT &&
		  !((Lisp_Port*)CAR(args))->out) {
			lisp_push(vm, LISP_FALSE);
		} else {
			lisp_push(vm, LISP_FALSE);
		}
		break;
	}
	case S_WITH_INPUT: {
		pushx(vm, vm->input);
		lisp_push(vm, CAR(args));
		lisp_eval(vm);
		if (!lisp_input_port_p(lisp_top(vm, 0)))
			lisp_err(vm, "with-input: not input port");
		vm->input = (Lisp_Port*)lisp_pop(vm, 1);
		eval_list(vm, REST(args), 0);
		vm->input = (Lisp_Port*)lisp_shift(vm, 2);
		break;
	}
	case S_WITH_OUTPUT: {
		pushx(vm, vm->output);
		lisp_push(vm, CAR(args));
		lisp_eval(vm);
		if (!lisp_output_port_p(lisp_top(vm, 0)))
			lisp_err(vm, "with-input: not input port");
		vm->output = (Lisp_Port*)lisp_pop(vm, 1);
		eval_list(vm, REST(args), 0);
		vm->output = (Lisp_Port*)lisp_shift(vm, 2);
		break;
	}
	case S_CLOSE: {
		switch (CAR(args)->type) {
		case O_PORT:
		{
			Lisp_Port *p = (Lisp_Port*)CAR(args);
			vm_check(p->vm);
			lisp_port_close(p);
			break;
		}
		case O_OBJECT_EX:
		{
			Lisp_ObjectEx *ex = (Lisp_ObjectEx*)CAR(args);
			vm_check(ex->vm);
			lisp_object_ex_finalize(vm, (Lisp_Object*)ex);
			break;
		}
		default:
			lisp_err(vm, "close: invalid object");
			break;
		}
		lisp_push(vm, LISP_UNDEF);
		break;
	}
	case S_OUTPUT_PORTP: {
		if (CAR(args)->type == O_PORT &&
		  ((Lisp_Port*)CAR(args))->out) {
			lisp_push(vm, LISP_TRUE);
		} else {
			lisp_push(vm, LISP_FALSE);
		}
		break;
	}
 
    case S_CLEAR:
    {
    	Lisp_Object *o = CAR(args);
        if (o->type == O_BUFFER) {
        	vm_check(((Lisp_Buffer*)o)->vm);
            lisp_buffer_clear((Lisp_Buffer*)o);
        } else if (o->type == O_ARRAY) {
        	vm_check(((Lisp_Array*)o)->vm);
            ((Lisp_Array*)CAR(args))->count = 0;
        } else if (o->type == O_DICT) {
        	vm_check(((Lisp_Array*)o)->vm);
            lisp_dict_clear((Lisp_Array*)o);
        }
        lisp_push(vm, o);
        break;
    }
	case S_CHAR_AT: {
		Lisp_String *s = safe_ptr(vm, CAR(args), O_STRING);
		int i = safe_int(vm, CADR(args));
		if (i < 0 || (unsigned)i >= s->length)
			lisp_err(vm, "char-at: index out of bound");
		push_num(vm, (unsigned)s->buf[i]);
		break;
	}
	/* Find the position of the first occurrence of a substring in a string.
	 * (string-find s sub &optional offset)
	 * default offset is 0.
	 * If no such substring is found, return false, otherwise return
	 * the index of substring.
	 */
	case S_STRING_FIND: {
		Lisp_String *s = safe_ptr(vm, CAR(args), O_STRING);
		Lisp_String *sub = safe_ptr(vm, CADR(args), O_STRING);
		Lisp_Object *o = CAR((Lisp_Pair*)CDDR(args));
		int offset = 0;
		const char *p = NULL;
		if (o != LISP_UNDEF) {
			offset = lisp_safe_int(vm, o);
		}
		if (offset >= 0 && (unsigned)offset < s->length)
			p = strstr(s->buf+offset, sub->buf);
		if (p == NULL)
			lisp_push(vm, LISP_FALSE);
		else
			pushx(vm, lisp_number_new(vm, p-s->buf));
		break;
	}
	/* Find the last occurrence of substring.
	 * search begin at (offset-1), then (offset-2), ... 0
	 */
	case S_STRING_FIND_BACKWARD: {
		Lisp_String *s = safe_ptr(vm, CAR(args), O_STRING);
		Lisp_String *sub = safe_ptr(vm, CADR(args), O_STRING);
		Lisp_Object *o = CAR((Lisp_Pair*)CDDR(args));
		int offset = (int)s->length;
		const char *p = NULL;
		if (o != LISP_UNDEF) {
			offset = lisp_safe_int(vm, o);
		}
		offset--;
		if (offset >= 0 && (unsigned)offset < s->length && sub->length <= s->length) {
			if ((unsigned)offset > s->length - sub->length)
				offset = (int)(s->length - sub->length);
			for (;offset >= 0;offset--) {
				if (strncmp(s->buf+offset, sub->buf, sub->length)==0) {
					p = s->buf+offset;
					break;
				}
			}
		}
		if (p == NULL)
			lisp_push(vm, LISP_FALSE);
		else
			pushx(vm, lisp_number_new(vm, p-s->buf));
		break;
	}
	case S_STRING_LENGTH: {
		Lisp_Object *o = CAR(args);
		if (o->type == O_SYMBOL || o->type == O_STRING)
			push_num(vm, ((Lisp_String*)o)->length);
		else
			lisp_err(vm, "string-length: invalid argument");
		break;
	}
	case S_LENGTH: {
		Lisp_Object *o = CAR(args);
		int n = 0;
		if (o->type == O_PAIR) {
			Lisp_Pair *p = list_ptr(vm, CAR(args));
			for (; p != LISP_NIL; p = REST(p))
				n++;
		} else if (o->type == O_BUFFER) {
			n = (int)((Lisp_Buffer*)o)->length;
		} else if (o->type == O_STRING || o->type == O_SYMBOL) {
			n = (int)((Lisp_String*)o)->length;
		} else if (o->type == O_ARRAY) {
			n = (int)((Lisp_Array*)o)->count;
		} else {
			lisp_err(vm, "no length");
		}
		push_num(vm, n);
		break;
	}
	case S_SPLIT: { /* (split <str> <delim>) */
		Lisp_String *s = safe_ptr(vm, CAR(args), O_STRING);
		Lisp_String *delim = safe_ptr(vm, CADR(args), O_STRING);
		int n = 0;
		if (delim->length == 0)
			lisp_err(vm, "split: delim must not be empty");
		const char *t = s->buf;
		const char *p;
		
		while ((p = strstr(t, delim->buf))) {
			pushx(vm, lisp_string_new(vm, t, p-t));
			n++;
			t = p + delim->length;
		}
		
		if (t == s->buf) {
			pushx(vm, s);
		} else {
			pushx(vm, lisp_string_new(vm, t, s->length-(t-s->buf)));
		}
		lisp_make_list(vm, n+1);
		break;
	}
	
	case S_SUBSTRING: { /* (substring str start &optional end) */
		Lisp_String *s = safe_ptr(vm, CAR(args), O_STRING);
		int start = safe_int(vm, CADR(args));
		int end = (int)s->length;
		if (CDDR(args) != LISP_NIL)
			end = safe_int(vm, CAR(CDDR(args)));
		if (start >= 0 && start <= end) {
			if ((unsigned)end > s->length)
				end = (int)s->length;
			// If end points to the middle of a multi-byte unicode char
			// advance until we reach the end of that char
			// So that we will always return a valid utf8 string
			// This allows us to iterate through a string char by char
			// by using (substring s pos (+ pos 1))
			while ((s->buf[end] & 0xc0) == 0x80)
				end++;
			if ((unsigned)start < s->length && (s->buf[start] & 0xc0) == 0x80)
				lisp_err(vm, "bad first byte");
			lisp_push(vm, (Lisp_Object*)lisp_string_new(vm, s->buf+start, end-start));
		} else {
			lisp_err(vm, "bad range");
		}
		break;
	}
	/* (slice <string|buffer|array> &optional begin end)
	 * end is excluded.
	 */
	case S_SLICE: {
		Lisp_Object *o = CAR(args);
		int begin = 0, end = -1;
		if (CDR(args) != LISP_NIL) {
			begin = safe_int(vm, CADR(args));
			if (CDDR(args) != LISP_NIL)
				end = safe_int(vm, CAR(CDDR(args)));
		}
		
		switch (o->type) {
		case O_BUFFER: {
			Lisp_Buffer *b = (Lisp_Buffer*)o;
			if (end == -1)
				end = (int)b->length;
			if (begin >= 0 && begin <= end && end <= (int)b->length) {
				pushx(vm, lisp_buffer_copy(vm, b->buf+begin, end-begin));
			} else {
				lisp_err(vm, "slice: invalid range");
			}
			break;
		}
		case O_STRING: {
			Lisp_String *s = (Lisp_String*)o;
			if (end == -1)
				end = (int)s->length;
			if (begin >= 0 && begin <= end && end <= (int)s->length) {
				pushx(vm, lisp_string_new(vm, s->buf+begin, end-begin));
			} else {
				lisp_err(vm, "slice: invalid range");
			}
			break;
		}
		case O_ARRAY: {
			Lisp_Array *a = (Lisp_Array*)o;
			if (end == -1)
				end = (int)a->count;
			if (begin >= 0 && begin <= end && end <= (int)a->count) {
				pushx(vm, lisp_array_copy(vm, a, begin, end-begin));
			} else {
				lisp_err(vm, "slice: invalid range");
			}
			break;
		}
		default:
			lisp_err(vm, "slice: invalid object");
			break;
		}
		break;
	}
	case S_ASSOC: { /* (assoc key alist) */
		Lisp_Pair *l = safe_ptr(vm, CADR(args), O_PAIR);
		Lisp_Pair *p = lisp_assoc(l, CAR(args));
		lisp_push(vm, p ? (Lisp_Object*)p : LISP_FALSE);
		break;
	}
	case S_CONCAT: op_concat(vm, args); break;
	case S_JOIN: {
		// TODO Optimize: the destination buffer size can be determined 
		// by going through the list. We don't need the buffer
		// directly write to an empty string.
		Lisp_Buffer *b = lisp_push_buffer(vm, NULL, 64);
		Lisp_Pair *l = lisp_safe_list(vm, CAR(args));
		const char *sep = lisp_safe_cstring(vm, CADR(args));
		for (int n = 0; l != LISP_NIL; l = REST(l), n++) {
			if (n > 0)
				lisp_buffer_adds(b, sep);
			Lisp_Object *o = CAR(l);
			if (o->type == O_STRING || o->type == O_SYMBOL)
				lisp_buffer_adds(b, ((Lisp_String*)o)->buf);
			else 
				lisp_err(vm, "not symbol or string");
		}
		lisp_push_string(vm, (char*)b->buf, b->length);
		lisp_shift(vm, 2);
		break;
	}
	case S_STRING_TO_NUMBER: {
		double t;
		Lisp_String *s = safe_ptr(vm, CAR(args), O_STRING);
		if (!str2dbl(s->buf, &t))
			lisp_push(vm, LISP_UNDEF);
		else
			push_num(vm, t);
		break;
	}
	case S_STRING_QUOTE: {
		Lisp_String *s = safe_ptr(vm, CAR(args), O_STRING);
		size_t n = 0;
		for (const char *p = s->buf; *p; p++, n++) {
			switch (*p) {
			case '\"': case '\\': case '\n':
			case '\r': case '\t':
				n++; break;
			default: break;
			}
		}
		Lisp_String *t = lisp_push_string(vm, NULL, n+2);
		char *q = (char*)t->buf;
		*q++ = '\"';
		for (const char *p = s->buf; *p; p++, n++) {
			switch (*p) {
			case '\"': *q++ = '\\'; *q++ = '\"'; break;
			case '\\': *q++ = '\\'; *q++ = '\\'; break;
			case '\n': *q++ = '\\'; *q++ = 'n'; break;
			case '\r': *q++ = '\\'; *q++ = 'r'; break;
			case '\t': *q++ = '\\'; *q++ = 't'; break;
			default:   *q++ = *p; break;
			}
		}
		*q++ = '\"';
		break;
	}
	case S_NUMBER_TO_STRING: op_num2str(vm, args);	break;
	case S_STRING_TO_SYMBOL: {
		Lisp_String *s = safe_ptr(vm, CAR(args), O_STRING);
		lisp_make_symbol(vm, s->buf);
		break;
	}
	case S_SYMBOL_TO_STRING: {
		Lisp_String *s = safe_ptr(vm, CAR(args), O_SYMBOL);
		pushx(vm, lisp_string_new(vm, s->buf, s->length));
		break;
	}
	case S_STRING_COMPARE: {
		Lisp_String *a = safe_ptr(vm, CAR(args), O_STRING);
		Lisp_String *b = safe_ptr(vm, CADR(args), O_STRING);
		push_num(vm, strcmp(a->buf, b->buf));
		break;
	}
	case S_FORMAT: op_format(vm, args); break;
	case S_OPEN_INPUT_FILE: { // (open-input-file path)
		Lisp_String *path = safe_ptr(vm, CAR(args), O_STRING);
		pushx(vm, lisp_open_input_file(vm, path));
		break;
	}
	case S_SEEK: /* (seek <port> <offset>) */
	{
		Lisp_Port *port = safe_ptr(vm, CAR(args), O_PORT);
		vm_check(port->vm);
		long offset = (long)lisp_safe_number(vm, CADR(args));
		if (port->closed || !port->stream || !port->stream->cls->seek)
			lisp_err(vm, "Bad port: seek not supported");
		if (port->out) {
			lisp_port_flush(port);
		} else {
			port->input_pos = 0;
			port->iobuf->length = 0;
		}
		if (0 == port->stream->cls->seek(port->stream->context, offset))
			lisp_push(vm, LISP_TRUE);
		else
			lisp_err(vm, "seek: failed");
		break;
	}
	case S_OPEN_OUTPUT_FILE:
	/* (open-output-file path &optional mode)
	 * List of modes
	 * 0 truncate existing file, start from beginning
	 * 1 append to file
	 */
	{
		Lisp_String *path = safe_ptr(vm, CAR(args), O_STRING);
		File_Output_Mode mode = 0;
		if (CDR(args) != LISP_NIL)
			mode = lisp_safe_int(vm, CADR(args));
		if (mode > 1)
			lisp_err(vm, "open-output-file: bad mode %d", mode);
		pushx(vm, lisp_open_output_file(vm, path, mode));
		break;
	}
	case S_OPEN_INPUT_BUFFER:
	/* (open-input-buffer buf &optional name)
	 */
	{
		Lisp_Object *o1 = CAR(args);
		Lisp_Object *o2 = CADR(args);
		if (o2->type != O_STRING) {
			if (o2 == LISP_UNDEF) o2 = NULL;
			else lisp_err(vm, "Bad name for input buffer");
		}
		if (o1->type == O_STRING) {
			pushx(vm, lisp_open_input_string(vm, (Lisp_String*)o1, (Lisp_String*)o2));
		} else if (o1->type == O_BUFFER) {
			pushx(vm, lisp_open_input_buffer(vm, (Lisp_Buffer*)o1, (Lisp_String*)o2));
		} else {
			lisp_err(vm, "can not open input buffer");
		}
		break;
	}
	case S_OPEN_OUTPUT_BUFFER: { // (open-output-buffer [buf])
		Lisp_Object *o = CAR(args);
		if (o == LISP_UNDEF)
			o = NULL;
		else if (o->type != O_BUFFER)
			lisp_err(vm, "bad buffer to output");
		pushx(vm, lisp_open_output_buffer(vm, (Lisp_Buffer*)o));
		break;
	}
	case S_GET_OUTPUT_BUFFER: {
		Lisp_Port *port = safe_ptr(vm, CAR(args), O_PORT);
		if (port->iobuf==NULL)
			lisp_err(vm, "not string output port");
		pushx(vm, lisp_port_get_buffer(port));
		break;
	}
	case S_GET_BYTE_COUNT: {
		Lisp_Port *port = safe_ptr(vm, CAR(args), O_PORT);
		lisp_push_number(vm, (double)port->byte_count);
		break;
	}
	case S_CURRENT_INPUT: pushx(vm, vm->input); break;
	case S_CURRENT_OUTPUT: pushx(vm, vm->output); break;
	case S_SET_CURRENT_INPUT: {
		Lisp_Port *port = safe_ptr(vm, CAR(args), O_PORT);
		if (port->out)
			lisp_err(vm, "not input port");
		vm->input = port;
		lisp_push(vm, LISP_NIL);
		break;
	}
	case S_SET_CURRENT_OUTPUT: {
		Lisp_Port *port = safe_ptr(vm, CAR(args), O_PORT);
		if (!port->out)
			lisp_err(vm, "not output port");
		vm->output = port;
		lisp_push(vm, LISP_NIL);
		break;
	}
	case S_SET_CURRENT_ERROR: {
		Lisp_Port *port = safe_ptr(vm, CAR(args), O_PORT);
		if (!port->out)
			lisp_err(vm, "not output port");
		vm->error = port;
		lisp_push(vm, LISP_NIL);
		break;
	}
	case S_BUFFER_SET: { /* (buffer-set! <buffer> <offset> <bytes>) */
		Lisp_Buffer *b1 = safe_ptr(vm, CAR(args), O_BUFFER);
		vm_check(b1->vm);
		int i = lisp_safe_int(vm, CADR(args));
		Lisp_Buffer *b2 = safe_ptr(vm, CAR(CDDR(args)), O_BUFFER);
		if (i < 0 || (unsigned)i >= b1->length)
			lisp_err(vm, "Bad offset");
		for (unsigned j = 0; (unsigned)i < b1->length && j < b2->length; i++, j++) {
			b1->buf[i] = b2->buf[j];
		}
		lisp_push(vm, LISP_UNDEF);
		break;
	}
	
	/*
	 * (buffer-setd! <buffer> <offset> <number>)
	 * (buffer-getd <buffer> <offset>)
	 */
	case S_BUFFER_SETI8:  op_buffer_set(int8_t, 1); break;
	case S_BUFFER_GETI8:  op_buffer_get(int8_t, 1); break;
	case S_BUFFER_SETI16: op_buffer_set(int16_t, 2); break;
	case S_BUFFER_GETI16: op_buffer_get(int16_t, 2); break;
	case S_BUFFER_SETI32: op_buffer_set(int32_t, 4); break;
	case S_BUFFER_GETI32: op_buffer_get(int32_t, 4); break;
	case S_BUFFER_SETU8:  op_buffer_set(uint8_t, 1); break;
	case S_BUFFER_GETU8:  op_buffer_get(uint8_t, 1); break;
	case S_BUFFER_SETU16: op_buffer_set(uint16_t, 2); break;
	case S_BUFFER_GETU16: op_buffer_get(uint16_t, 2); break;
	case S_BUFFER_SETU32: op_buffer_set(uint32_t, 4); break;
	case S_BUFFER_GETU32: op_buffer_get(uint32_t, 4); break;
	case S_BUFFER_SETF:   op_buffer_set(float, 4); break;
	case S_BUFFER_GETF:   op_buffer_get(float, 4); break;
	case S_BUFFER_SETD:   op_buffer_set(double, 8); break;
	case S_BUFFER_GETD:   op_buffer_get(double, 8); break;

	case S_BUFFER_TO_STRING: {
		Lisp_Buffer *b = safe_ptr(vm, CAR(args), O_BUFFER);
		int pos = check_utf8((char*)b->buf, b->length);
		if (pos == -1) {
			pushx(vm, lisp_string_new(vm, (char*)b->buf, b->length));
		} else {
			lisp_err(vm, "Invalid UTF-8 buffer -- %d", pos);			
		}
		break;
	}
	case S_DATE: { /* (date &optional format unixtime) */
		const char *format = NULL;
		time_t t = 0;
		char buf[256] = {0};

		if (CAR(args) == LISP_UNDEF)
			format = "%Y-%m-%d %H:%M:%S";
		else
			format = lisp_safe_cstring(vm, CAR(args));
		
		if (CADR(args) == LISP_UNDEF) 
			t = time(NULL);
		else 
			t = (time_t)safe_num(vm, CADR(args));
		struct tm *tm = localtime(&t);
		if (tm == NULL) {
			lisp_push(vm, LISP_UNDEF);
		} else {
			size_t size = strftime(buf, sizeof(buf), format, tm);
			if (size > 0) {
				pushx(vm, lisp_string_new(vm, buf, size));
			} else {
				lisp_push(vm, LISP_UNDEF);
			}
		}
		break;
	}
	case S_PUMP: op_pump(vm, args); break;
	case S_TIME: push_num(vm, (double)time(NULL)); break;
	default:
		lisp_err(vm, "Not a primitive: %s", SYM(sid)->buf);break;
	}
}

/**
 ** Lisp Virtual Machine
 **/

Lisp_VM *lisp_vm_new()
{
	jmp_buf jbuf;
	Lisp_VM *vm = calloc(1, sizeof(Lisp_VM));
	if (vm == NULL)
		return NULL;
	vm->catch = &jbuf;
	if (setjmp(jbuf) == 0) {
		vm->pool = lisp_pool_new(vm, INIPOOLSIZE);
		vm->stack = lisp_array_new(vm, INISTACKSIZE);
		vm->symbols = lisp_dict_new(vm, INISYMLISTSIZE);
		vm->source_files = lisp_dict_new(vm, INIFILELISTSIZE);
		vm->keep_alive_pool = lisp_array_new(vm, 16);
		vm->env = lisp_env_new(vm, NULL);
		vm->root_env = vm->env;
		//vm->input = lisp_open_input_file(vm, SYM(S_STDIN));
		// TODO we should use a dummy output port here
		//   let client decide the actual implementation.
		vm->output = lisp_open_output_file(vm, SYM(S_STDOUT), 0);
		vm->error = lisp_open_output_file(vm, SYM(S_STDERR), 0);
		vm->token = lisp_buffer_new(vm, TOKENBUFSIZE);
		vm->last_eval = LISP_UNDEF;
		vm->rand_next = (uint32_t)((intptr_t)vm & 0x7fffffff);
	} else {
		lisp_vm_delete(vm);
		return NULL;
	}
	vm->catch = NULL;
	return vm;
}

void lisp_vm_delete(Lisp_VM *vm)
{
#if 0
	fprintf(stderr, "Lisp VM Clean up: %zu/%zu objects, memsize=%zu\n",
		vm->pool->count + 1,vm->pool->cap, vm->memsize);
#endif		
	if (vm->pool) {
		for (unsigned i = 0; i < vm->pool->count; i++)
			delete_obj(vm, vm->pool->items[i]);
		delete_obj(vm, (Lisp_Object*)vm->pool);
		vm->pool = NULL;
	}
	for (int i = 0; i < MAX_CACHED_OBJECT_SIZE/BLKSIZE; i++) {
		lisp_memblock_t *p, *next;
		size_t bsize = (i + 1) * BLKSIZE;
		for (p = vm->freelist[i]; p != NULL; p = next) {
			next = p->next;
			assert(vm->memsize >= bsize);
			vm->memsize -= bsize;
			free(p);
		}
		vm->freelist[i] = NULL;
	}
	assert(vm->memsize == 0);
	free(vm);
}

void lisp_keep_alive(Lisp_VM *vm, Lisp_Object* obj)
{
	lisp_array_push(vm->keep_alive_pool, obj);
}

void lisp_vm_set_parent(Lisp_VM *vm, Lisp_VM *parent)
{
	vm->parent = parent;
	vm->root_env->parent = parent->root_env;
}

void lisp_vm_set_client(Lisp_VM* vm, void *client)
{
	vm->client = client;
}
void* lisp_vm_client(Lisp_VM* vm)
{
	return vm->client;
}


// REPL
static void load(Lisp_VM *vm)
{
	Lisp_Object *obj;
	bool interactive = vm->input->isatty && vm->output->isatty;
	int n = 0;
	if (vm->debugging) {
		lisp_printf(vm, "Entering debug mode, type '/quit' to resume\n");
	}
	for (;;n++) {
		if (vm->debugging) {
			lisp_printf(vm, "DEBUG> ");
			lisp_port_flush(vm->output);
		} else if (interactive) {
			lisp_printf(vm, "[%d] %s> ", vm->input->line, COLOR_GREEN(PROGNAME));
			lisp_port_flush(vm->output);
		}
		vm->reading = 1;
		obj = lisp_read(vm);
		vm->reading = 0;
		if (vm->debugging) {
			if (obj->type == O_SYMBOL && strcmp(((Lisp_String*)obj)->buf, "/quit")==0)
				break;
		}
		if (interactive) {
			lisp_println(vm, obj);
		}
		if (obj == LISP_EOF) {
			lisp_pop(vm, 1);
			if (n == 0) lisp_push(vm, LISP_UNDEF);
			break;
		}
		if (n > 0) // Pop previous result
			lisp_push(vm, lisp_pop(vm, 2));
		obj = lisp_eval(vm);
		if (obj != LISP_UNDEF) {
			if (interactive) {
				lisp_port_puts(vm->output, "=> ");
				lisp_println(vm, obj);
			} else if (vm->verbose) {
				lisp_println(vm, obj);
			}
		}
	}
}

Lisp_Object *lisp_vm_last_eval(Lisp_VM* vm)
{
	return vm->last_eval;
}
void lisp_vm_set_verbose(Lisp_VM*vm, bool verbose)
{
	vm->verbose = verbose;
}

bool lisp_vm_run(Lisp_VM *vm)
{
	jmp_buf jbuf;
	jmp_buf *prev = vm->catch;
	bool ok = false;
	int ret;
	int oldlevel = vm->eval_level;
	assert(vm->stack->cap - vm->stack->count > 3);
	assert(vm->input);
	assert(vm->output);
	bool interactive = vm->input->isatty;
	lisp_push(vm, (Lisp_Object*)vm->env);
	/* Input and Output can be changed during execution
	 * but we may need to restore them in interactive mode
	 * so that we can continue to type in code.
	 */
	lisp_push(vm, (Lisp_Object*)vm->input);
	lisp_push(vm, (Lisp_Object*)vm->output);
	size_t oldcnt = vm->stack->count;
Loop:
	vm->catch = &jbuf;
	ret = setjmp(jbuf);
	if (ret == 0) {
		vm->last_eval = LISP_UNDEF;
		load(vm);
		vm->last_eval = lisp_pop(vm, 1);
		ok = true;
	} else {
		if (ret == 2) {
			lisp_port_printf(vm->error, "error: Uncaught throw: ");
			lisp_port_print(vm->error, lisp_top(vm, 0));
			lisp_port_putc(vm->error, '\n');
			show_callstack(vm);
		}
		vm->reading = 0;
		vm->stack->count = oldcnt;
		vm->eval_level = oldlevel;
		vm->env = (Lisp_Env*)lisp_top(vm, 2);
		vm->catch = NULL;
		if (interactive) {
			/* We are receiving input from terminal interactively
			 * so we should restore input and output port
			 * to where we were.
		 	 */
			vm->output = (Lisp_Port*)lisp_top(vm, 0);
			vm->input = (Lisp_Port*)lisp_top(vm, 1);

			/* Discard input buffer because it's not in good state */
			lisp_port_drain(vm->input, -1);
			goto Loop;
		}
	}
	assert(vm->stack->count == oldcnt);
	assert(vm->env == (Lisp_Env*)lisp_top(vm, 2));

	vm->stack->count -= 3;
	vm->catch = prev;
	return ok;
}

#define LISP_VM_GUARD_BEGIN(vm) do { \
	jmp_buf jbuf; \
	assert(vm->catch == NULL); \
	vm->catch = &jbuf; \
	size_t oldcnt = vm->stack->count; \
	if (setjmp(jbuf) == 0) {

#define LISP_VM_GUARD_END(vm) \
	} \
	vm->catch = NULL; \
	vm->stack->count = oldcnt; \
} while (0)

Lisp_VM *lisp_procedure_owner(Lisp_Object *obj)
{
	if (obj->type != O_PROC)
		return NULL;
	return ((Lisp_Proc*)obj)->env->bindings->vm;
}

/*
 * lisp_try(vm,fn,data)
 *
 * Call <fn> with <data> pointer, and when any error
 * occurs, it will be caught, and NULL is returned.
 * If everything goes all right, fn should add exactly one
 * object to the stack upon its return, which will then
 * be returned by try.
 */
Lisp_Object* lisp_try(Lisp_VM *vm, void (*fn)(Lisp_VM*, void *), void *data)
{
	Lisp_Object *ret = NULL;
	jmp_buf jbuf;
	jmp_buf *prev = vm->catch;
	vm->catch = &jbuf;
	size_t oldcnt = vm->stack->count;
	if (setjmp(jbuf) == 0) {
		fn(vm, data);
		assert(vm->stack->count == oldcnt+1);
		ret = lisp_pop(vm, 1);
	}
	vm->catch = prev;
	vm->stack->count = oldcnt;
	return ret;
}

Lisp_Port *lisp_current_output(Lisp_VM* vm)
{
	return vm->output;
}

void lisp_set_current_output(Lisp_VM* vm, Lisp_Port *port)
{
	if (port->vm != vm)
		lisp_err(vm, "foreign port");
	vm->output = port;
}

Lisp_Port *lisp_current_input(Lisp_VM* vm)
{
	return vm->input;
}

bool lisp_vm_set_input_file(Lisp_VM *vm, const char *filename)
{
	bool ok = false;
	LISP_VM_GUARD_BEGIN(vm);
	Lisp_String *s = lisp_string_new(vm, filename, strlen(filename));
	pushx(vm, s);
	vm->input = lisp_open_input_file(vm, s);
	ok = true;
	LISP_VM_GUARD_END(vm);
	return ok;
}

bool lisp_vm_set_output_file(Lisp_VM *vm, const char *filename)
{
	bool ok = false;
	LISP_VM_GUARD_BEGIN(vm);
	Lisp_String *s = lisp_string_new(vm, filename, strlen(filename));
	pushx(vm, s);
	vm->output = lisp_open_output_file(vm, s, 0);
	ok = true;
	LISP_VM_GUARD_END(vm);
	return ok;
}

bool lisp_vm_set_error_file(Lisp_VM *vm, const char *filename)
{
	bool ok = false;
	LISP_VM_GUARD_BEGIN(vm);
	Lisp_String *s = lisp_string_new(vm, filename, strlen(filename));
	pushx(vm, s);
	vm->error = lisp_open_output_file(vm, s, 0);
	vm->error->no_buf = 1;
	ok = true;
	LISP_VM_GUARD_END(vm);
	return ok;
}

bool lisp_vm_set_error_stream(Lisp_VM *vm, Lisp_Stream *stream)
{
    if (!vm->error)
        return false;
    lisp_port_set_output_stream(vm->error, stream);
	return true;
}

bool lisp_vm_set_output_stream(Lisp_VM *vm, Lisp_Stream *stream)
{
    if (!vm->output)
        return false;
    lisp_port_set_output_stream(vm->output, stream);
	return true;
}

bool lisp_vm_set_input_stream(Lisp_VM *vm, Lisp_Stream *stream)
{
    if (!vm->input)
        return false;
    lisp_port_set_input_stream(vm->input, stream);
    return true;
}

bool lisp_vm_set_input_string(Lisp_VM *vm, const char *cs, const char *_name)
{
	bool ok = false;
	LISP_VM_GUARD_BEGIN(vm);
	Lisp_String *source = lisp_string_new(vm, cs, strlen(cs));
	pushx(vm, source);
	Lisp_String *name = NULL;
	if (_name) {
		name = lisp_string_new(vm, _name, strlen(_name));
		pushx(vm, name);
	}
	vm->input = lisp_open_input_string(vm, source, name);
	ok = true;
	LISP_VM_GUARD_END(vm);
	return ok;
}

bool lisp_vm_load(Lisp_VM *vm, const char *filename)
{
	bool ok = lisp_vm_set_input_file(vm, filename);
	if (ok) {
		vm->input->src_file = ensure_source_file(vm, vm->input->name);
		ok = lisp_vm_run(vm);
	}
	return ok;
}

bool lisp_vm_load_string(Lisp_VM *vm, const char *s, const char *name)
{
	bool ok = lisp_vm_set_input_string(vm, s, name);
	if (ok) {
		ok = lisp_vm_run(vm);
	}
	return ok;
}

/*
 * Load and execute a lisp program from file at `path`,
 * leaves last evaluated result on stack.
 * path must be absolute or relative to current working directory.
 * Stack Size +1, Unguarded
 */
void lisp_load_file(Lisp_VM *vm, const char *path)
{
	Lisp_String *s = lisp_string_new(vm, path, strlen(path));
	pushx(vm, s);
	vm->input = lisp_open_input_file(vm, s);
	vm->input->src_file = ensure_source_file(vm, vm->input->name);
	load(vm);
	lisp_exch(vm);
	lisp_port_close(vm->input);
	vm->input = NULL;
	lisp_exch(vm);
	lisp_pop(vm, 1);
}

void lisp_parse(Lisp_VM *vm, const char *cs)
{
	Lisp_String *source = lisp_string_new(vm, cs, strlen(cs));
	pushx(vm, source);
	vm->input = lisp_open_input_string(vm, source, NULL);
	lisp_read(vm);
	lisp_push(vm, lisp_pop(vm, 2));
}

void lisp_eval_object(Lisp_VM *vm, Lisp_Object* obj)
{
	lisp_push(vm, obj);
	lisp_eval(vm);
}

/* stack size No change */
void lisp_def(Lisp_VM *vm, const char *name, Lisp_Object *o)
{
	Lisp_String *s = lisp_make_symbol(vm, name);
	lisp_defvar(vm, s, o);
	lisp_pop(vm, 1);
}

bool lisp_vm_defn(Lisp_VM *vm, const char *name, lisp_func fn)
{
	bool ok = false;
	LISP_VM_GUARD_BEGIN(vm);
	Lisp_String *s = lisp_make_symbol(vm, name);
	Lisp_Native_Proc *proc = new_obj(vm, O_NATIVE_PROC);
	pushx(vm, proc);
	proc->fn = fn;
	proc->env = vm->env;
	proc->name = s;
	lisp_defvar(vm, s, (Lisp_Object*)proc);
	ok = true;
	LISP_VM_GUARD_END(vm);
	return ok;
}

void lisp_defn(Lisp_VM *vm, const char *name, lisp_func fn)
{
	Lisp_String *s = lisp_make_symbol(vm, name);
	Lisp_Native_Proc *proc = new_obj(vm, O_NATIVE_PROC);
	pushx(vm, proc);
	proc->fn = fn;
	proc->env = vm->env;
	proc->name = s;
	lisp_defvar(vm, s, (Lisp_Object*)proc);
	lisp_pop(vm, 2);
}

void lisp_vm_gc(Lisp_VM *vm, bool clear_keep_alive)
{
	if (clear_keep_alive)
		vm->keep_alive_pool->count = 0;
    gc(vm);
}

void lisp_vm_enable_debug(Lisp_VM *vm, bool enabled)
{
	vm->debug_enabled = enabled;
}

Lisp_Env *lisp_vm_get_root_env(Lisp_VM *vm)
{
	assert(vm->root_env);
	return vm->root_env;
}

#if 0
void lisp_vm_save_state(Lisp_VM *vm, lisp_vm_state_t *state)
{
	state->env = vm->env;
	state->input = vm->input;
	state->output = vm->output;
	state->error = vm->error;
	state->kaboom = vm->kaboom;
	state->stack_count = vm->stack->count;
}

void lisp_vm_restore_state(Lisp_VM *vm, lisp_vm_state_t *state)
{
	vm->env = state->env;
	vm->input = state->input;
	vm->output = state->output;
	vm->error = state->error;
	vm->kaboom = state->kaboom;
	assert(vm->stack->count >= state->stack_count);
	vm->stack->count = state->stack_count;
}
#endif

jmp_buf* lisp_vm_set_error_trap(Lisp_VM *vm, jmp_buf *jbuf)
{
	jmp_buf *old = vm->catch;
	vm->catch = jbuf;
	return old;
}

void lisp_vm_resume_error(Lisp_VM *vm, jmp_buf *old)
{
	assert(old);
	vm->catch = old;
	longjmp(*vm->catch, 1);
}


/** Main Program. */
#ifdef LISP_MAIN
int main(int argc, char *argv[])
{
	bool ok = false;
	Lisp_VM *vm = lisp_vm_new();
	if (!vm) goto Done;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-l") == 0) {
			if (++i < argc) {
				if (!lisp_vm_load(vm, argv[i]))
					goto Done;
			}
		} else if (strcmp(argv[i], "-e") == 0) {
			if (++i < argc) {
				ok = lisp_vm_load_string(vm, argv[i], NULL);
				if (!ok) goto Done;
			}
		} else if (strcmp(argv[i], "-v") == 0) {
			vm->verbose = 1;
		} else {
			ok = lisp_vm_load(vm, argv[i]);
			if (!ok) goto Done;
		}
	}
	if (!ok) {/* Run interactively */
		ok = lisp_vm_load(vm, "*stdin*");
	}
Done:
	if (vm) lisp_vm_delete(vm);
	return ok ? 0 : -1;
}
#endif
