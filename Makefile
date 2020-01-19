TARGET=twk

SQLITE3_DIR=lib/sqlcipher
SQLITE3_FLAGS= -DSQLITE_HAS_CODEC  -DSQLITE_TEMP_STORE=3 -DSQLCIPHER_CRYPTO_OPENSSL -DSQLITE_ENABLE_FTS5

#SQLITE3_DIR=lib/sqlite3
#SQLITE3_FLAGS=-DSQLITE_ENABLE_FTS5

CFLAGS=-g -std=c99 -DLISP_ENABLE_SYSTEM $(SQLITE3_FLAGS) -Wall -Ilib-dev/lisp -I$(SQLITE3_DIR) -Ilib/http
LFLAGS=

ifeq ($(OS),Windows_NT)
	SSL=c:/openssl-1.1.0h-win32-mingw
	CFLAGS+= -I${SSL}/include -Ic:\mingw\msys\1.0\include \
	  -D_WIN32_WINNT=0x0501
	LFLAGS+= -L${SSL}/lib -Lc:\mingw\msys\1.0\lib  \
	  -Wl,-Bstatic -lpthread -lcrypto -lz  \
	  -Wl,-Bdynamic -lWs2_32 -lwsock32 -liphlpapi
	UNAME=Windows_NT
else
	UNAME:=$(shell uname -s)

	ifeq ($(UNAME),Darwin)
		SSL=/usr/local/Cellar/openssl@1.1/1.1.0g
	else
		SSL=/usr/local
	endif

	ifeq ($(UNAME),OpenBSD)
	endif

	ifeq ($(UNAME),Linux)
		CFLAGS+=-D_DEFAULT_SOURCE
		LFLAGS+=-ldl
	endif

	CFLAGS+= -I${SSL}/include
	LFLAGS+= -L${SSL}/lib -lcrypto -lm -lpthread -lz
endif

SRCS+= \
    src/lisp.c \
    src/lisp_crypto.c \
    src/lisp_fs.c \
    src/lisp_socket.c \
    src/lisp_sqlite3.c \
    src/lisp_zstream.c \
    src/lisp_regexp.c \
    src/httpd.c \
    src/twk.c \
    src/fifo.c \
    src/utf8.c \
    src/regexp.c \
    src/microtime.c \
    src/base58.c \
    src/base64.c \
    $(SQLITE3_DIR)/sqlite3.c \
    src/win32/w32_compat.c

OBJS=$(SRCS:%.c=%.o)    

all: sysinfo $(TARGET) libtwk.a

sysinfo:
	@echo OS=$(OS)
	@echo UNAME=$(UNAME)

sqlite3: $(SQLITE3_DIR)/sqlite3.c $(SQLITE3_DIR)/shell.c
	gcc -DSQLITE_HAS_CODEC -DSQLITE_TEMP_STORE=2 -DSQLCIPHER_CRYPTO_OPENSSL $(CFLAGS) $(LFLAGS) -o $@ $^

%.o: %.c
	gcc $(CFLAGS) -o $@ -c $<

$(TARGET): $(OBJS) src/main.o
	gcc $^ -o $@ $(LFLAGS)

libtwk.a: $(OBJS)
	ar -rv $@ $(OBJS)

clean:
	rm -f $(TARGET) $(TARGET).exe $(OBJS) src/main.o
