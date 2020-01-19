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
#include "lisp_sqlite3.h"
#include "common.h"

struct sqlite3_db {
    sqlite3 *instance;
    Lisp_VM *vm;
    int refcnt;
};

static struct sqlite3_db* sqlite3_db_new()
{
    struct sqlite3_db *db;
    
    db = calloc(1, sizeof(struct sqlite3_db));
    assert(db != NULL);
    db->refcnt = 1;
    return db;
}

// No need to use atomic, since ref() is only used in opening blob stream
// and it's required that blob stream must be in same VM as the db
// therefore ref() and unref() will always operate in same thread.
static struct sqlite3_db* sqlite3_db_ref(struct sqlite3_db *db)
{
    db->refcnt++;
    return db;
}

static void sqlite3_db_unref(struct sqlite3_db *db)
{
    assert(db->refcnt > 0);
    if (--db->refcnt == 0) {
        if (db->instance) {
            /* GC friendly */
            sqlite3_close_v2(db->instance);
        }
        free(db);
    }
}

static void sqlite3_db_close(Lisp_VM *vm, void *ctx)
{
    if (ctx)
        sqlite3_db_unref(ctx);
}

static void sqlite3_stmt_close(Lisp_VM *vm, void *ctx)
{
    if (ctx)
    {
        sqlite3_stmt *stmt = (sqlite3_stmt*)ctx;
        sqlite3_finalize(stmt);
    }
}

static void sqlite_err(Lisp_VM* vm, sqlite3* db)
{
    int code = sqlite3_errcode(db);
    lisp_err(vm, "sqlite3: %s[%d]: %s",
        sqlite3_errstr(code), code, sqlite3_errmsg(db));
}

struct lisp_object_ex_class_t sqlite3_db_class =
{
    .name = "sqlite3",
    .finalize = sqlite3_db_close
};

struct lisp_object_ex_class_t sqlite3_stmt_class =
{
    .name = "sqlite3-statement",
    .finalize = sqlite3_stmt_close
};

static struct sqlite3_db *safe_db(Lisp_VM *vm, Lisp_Object *o)
{
    if (lisp_object_ex_class(o) != &sqlite3_db_class)
    {
        lisp_err(vm, "not sqlite3 db");
    }
    return (struct sqlite3_db *)lisp_object_ex_ptr(o);
}

static sqlite3 *safe_db_instance(Lisp_VM *vm, Lisp_Object *o)
{
    return safe_db(vm, o)->instance;
}

static sqlite3_stmt *safe_stmt(Lisp_VM *vm, Lisp_Object *o)
{
    if (lisp_object_ex_class(o) != &sqlite3_stmt_class)
    {
        lisp_err(vm, "not sqlite3 statement");
    }
    return lisp_object_ex_ptr(o);
}


/* (sqlite3-open <path> &optional ro) */
static void op_sqlite3_open(Lisp_VM *vm, Lisp_Pair*args)
{
    const char *path = lisp_safe_cstring(vm, CAR(args));
    int ro = (CADR(args) == lisp_true);
    Lisp_Object *o = lisp_make_object_ex(vm, &sqlite3_db_class);
    struct sqlite3_db *db = sqlite3_db_new();
    db->vm = vm;
    int rc = sqlite3_open_v2(
        path,
        &db->instance,
        ro ? SQLITE_OPEN_READONLY
           : (SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE),
        NULL);
    if (rc != SQLITE_OK)
    {
        sqlite3_db_unref(db);
        lisp_err(vm, "can not open file `%s': %s[%d]",
            path, sqlite3_errstr(rc), rc);
    }
    lisp_object_ex_set_ptr(o, db);
    rc = sqlite3_busy_timeout(db->instance, 10*1000); /* 10 seconds */
    if (rc != SQLITE_OK)
        sqlite_err(vm, db->instance);
}

/* (sqlite3-exec db sql) */
static void op_sqlite3_exec(Lisp_VM *vm, Lisp_Pair *args)
{
    sqlite3 *db = safe_db_instance(vm, CAR(args));
    const char *s = lisp_safe_cstring(vm, CADR(args));
    int rc = sqlite3_exec(db, s, NULL, NULL, NULL);
    if (rc != SQLITE_OK)
        sqlite_err(vm, db);
    lisp_push(vm, rc==SQLITE_OK?lisp_true:lisp_false);
}

/* (sqlite3-errmsg db) */
static void op_sqlite3_errmsg(Lisp_VM *vm, Lisp_Pair *args)
{
    sqlite3 *db = safe_db_instance(vm, CAR(args));
    const char *s = sqlite3_errmsg(db);
    PUSHX(vm, lisp_string_new(vm, s, strlen(s)));
}

static void op_sqlite3_last_insert_rowid(Lisp_VM *vm, Lisp_Pair *args)
{
    struct sqlite3 *db = safe_db_instance(vm, CAR(args));
    PUSHX(vm, lisp_number_new(vm, (double)sqlite3_last_insert_rowid(db)));
}

/* (sqlite3-prepare db src) */
static void op_sqlite3_prepare(Lisp_VM *vm, Lisp_Pair *args)
{
    struct sqlite3 *db = safe_db_instance(vm, CAR(args));
    const char *src = lisp_safe_cstring(vm, CADR(args));
    Lisp_Object *oStmt = lisp_make_object_ex(vm, &sqlite3_stmt_class);
    struct sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, src, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        sqlite_err(vm, db);
    lisp_object_ex_set_ptr(oStmt, stmt);
}

/* (sqlite3-bind stmt args) */
static void op_sqlite3_bind(Lisp_VM *vm, Lisp_Pair *args)
{
    struct sqlite3_stmt * stmt = safe_stmt(vm, CAR(args));
    args = (Lisp_Pair*)CADR(args);
    int index = 1;
    for (; args != (void*)lisp_nil; args = (Lisp_Pair*)CDR(args)) {
        Lisp_Object *o = CAR(args);
        if (o == lisp_undef||o == lisp_nil) {
            sqlite3_bind_null(stmt, index);
        } else if (o == lisp_true) {
            sqlite3_bind_int(stmt, index, 1);
        } else if (o == lisp_false) {
            sqlite3_bind_int(stmt, index, 0);
        } else if (lisp_string_p(o)||lisp_symbol_p(o)) {
            sqlite3_bind_text(stmt, index,
                lisp_string_cstr((Lisp_String*)o), -1,
                SQLITE_STATIC);
        } else if (lisp_integer_p(o)) {
            sqlite3_bind_int64(stmt, index, (int64_t)lisp_number_value((Lisp_Number*)o));
        } else if (lisp_number_p(o)) {
            sqlite3_bind_double(stmt, index, lisp_number_value((Lisp_Number*)o));
        } else if (lisp_buffer_p(o)) {
            Lisp_Buffer *b = (void*)o;
            sqlite3_bind_blob(stmt, index, lisp_buffer_bytes(b), (int)lisp_buffer_size(b), NULL);
        } else {
            lisp_err(vm, "sqlite3-bind: bad arguments");
        }
        index++;
    }
    lisp_push(vm, lisp_true);
}

static void fetch_row(Lisp_VM *vm, sqlite3_stmt *stmt)
{
    int cnt = sqlite3_column_count(stmt);
    for (int i = 0; i < cnt; i++)
    {
        const char *name = sqlite3_column_name(stmt, i);
        lisp_make_symbol(vm, name);
        switch (sqlite3_column_type(stmt, i))
        {
            case SQLITE_INTEGER:
                PUSHX(vm, lisp_number_new(vm, (double)sqlite3_column_int64(stmt, i)));
                break;
            case SQLITE_FLOAT:
                PUSHX(vm, lisp_number_new(vm, sqlite3_column_double(stmt, i)));
                break;
            case SQLITE_TEXT:
            {
                const char *s = (const char*)sqlite3_column_text(stmt, i);
                int n = sqlite3_column_bytes(stmt, i);
                PUSHX(vm, lisp_string_new(vm, s, n));
                break;
            }
            case SQLITE_BLOB:
            {
                const void *d = sqlite3_column_blob(stmt, i);
                int n = sqlite3_column_bytes(stmt, i);
                PUSHX(vm, lisp_buffer_copy(vm, d, n));
                break;
            }
            case SQLITE_NULL:
                // We don't return NULL because it can not
                // preserve the alist structure
                //  (k . undefined)
                // => (k)
                lisp_push(vm, lisp_undef);
                break;
            default:
                lisp_err(vm, "Bad result type");
                break;
        }
        lisp_cons(vm);
    }
    lisp_push(vm, lisp_nil);
    while (cnt-- > 0)
        lisp_cons(vm);
}

/* (sqlite3-step stmt) */
static void op_sqlite3_step(Lisp_VM *vm, Lisp_Pair *args)
{
    struct sqlite3_stmt * stmt = safe_stmt(vm, CAR(args));
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        lisp_push(vm, lisp_nil);
    } else if (rc == SQLITE_ROW) {
        fetch_row(vm, stmt);
    } else {
        lisp_err(vm, "sqlite3: %s", sqlite3_errstr(rc));
//        lisp_push(vm, lisp_false);
    }
}

/* (sqlite3-run stmt) */
static void op_sqlite3_run(Lisp_VM *vm, Lisp_Pair *args)
{
    struct sqlite3_stmt * stmt = safe_stmt(vm, CAR(args));
    int rows = 0;
    while (true)
    {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            lisp_make_list(vm, rows);
            return;
        } else if (rc == SQLITE_ROW) {
            fetch_row(vm, stmt);
            rows++;
        } else {
            lisp_pop(vm, rows);
            lisp_err(vm, "sqlite3: %s", sqlite3_errstr(rc));
            //lisp_push(vm, lisp_false);
            return;
        }
    }
}

/* (sqlite3-reset stmt) */
static void op_sqlite3_reset(Lisp_VM *vm, Lisp_Pair *args)
{
    struct sqlite3_stmt * stmt = safe_stmt(vm, CAR(args));
    int rc = sqlite3_reset(stmt);
    if (rc == SQLITE_OK)
        lisp_push(vm, lisp_true);
    else
        lisp_push(vm, lisp_false);
}

struct blob_stream {
    struct sqlite3_db *db;
    struct sqlite3_blob *blob;
    int total;
    int offset;
};

size_t blob_stream_read(void *context, void *buf, size_t size)
{
    struct blob_stream *bs = context;
    int n = bs->total - bs->offset;
    if ((unsigned)n > size)
        n = (int)size;
    int rc = sqlite3_blob_read(bs->blob, buf, n, bs->offset);
    if (rc != SQLITE_OK)
        return 0;
    bs->offset += n;
    return (size_t)n;
}

size_t blob_stream_write(void *context, const void *buf, size_t size)
{
    struct blob_stream *bs = context;
    int n = bs->total - bs->offset;
    if ((unsigned)n > size)
        n = (int)size;
    if (n == 0)
        return 0;
    int rc = sqlite3_blob_write(bs->blob, buf, n, bs->offset);
    if (rc != SQLITE_OK)
        return 0;
    bs->offset += n;
    return (size_t)n;
}

void blob_stream_close(void *context)
{
    struct blob_stream *bs = context;
    if (bs->blob)
        sqlite3_blob_close(bs->blob);
    bs->blob = NULL;
    bs->total = 0;
    bs->offset = 0;
    if (bs->db)
        sqlite3_db_unref(bs->db);
    bs->db = NULL;
}

int blob_stream_seek(void *context, long offset) // return 0 if ok
{
    struct blob_stream *bs = context;
    if (offset < 0 || offset > bs->total)
        return -1;
    bs->offset = (int)offset;
    return 0;
}

struct lisp_stream_class_t blob_stream_class = {
    .context_size = sizeof(struct blob_stream),
    .read = blob_stream_read,
    .write = blob_stream_write,
    .seek = blob_stream_seek,
    .close = blob_stream_close
};

static void op_open_blob_input(Lisp_VM *vm, Lisp_Pair *args)
{
    struct sqlite3_db *db = safe_db(vm, CAR(args));
    const char *db_name = lisp_safe_cstring(vm, lisp_nth(args, 1));
    const char *tbl_name = lisp_safe_cstring(vm, lisp_nth(args, 2));
    const char *col_name = lisp_safe_cstring(vm, lisp_nth(args, 3));
    double row = lisp_safe_number(vm, lisp_nth(args, 4));

    if (db->vm != vm)
        lisp_err(vm, "blob input must be in same VM with db");

    lisp_push_buffer(vm, NULL, 1024);
    Lisp_Stream *stream = lisp_push_stream(vm, &blob_stream_class, NULL);
    struct blob_stream *bs = lisp_stream_context(stream);
    if (SQLITE_OK != sqlite3_blob_open(db->instance, db_name, tbl_name, col_name, (sqlite3_int64)row, 0, &bs->blob))
        sqlite_err(vm, db->instance);
    bs->db = sqlite3_db_ref(db);
    bs->total = sqlite3_blob_bytes(bs->blob);
    lisp_make_input_port(vm);
}

/*
 * (sqlite3-open-blob-output db-handle db-name table-name column-name row-id)
 *
 * Note: blob storage must be prepared beforehand using UPDATE and ZEROBLOB(size)
 */
static void op_open_blob_output(Lisp_VM *vm, Lisp_Pair *args)
{
    struct sqlite3_db *db = safe_db(vm, CAR(args));
    const char *db_name = lisp_safe_cstring(vm, lisp_nth(args, 1));
    const char *tbl_name = lisp_safe_cstring(vm, lisp_nth(args, 2));
    const char *col_name = lisp_safe_cstring(vm, lisp_nth(args, 3));
    double row = lisp_safe_number(vm, lisp_nth(args, 4));

    if (db->vm != vm)
        lisp_err(vm, "blob output must be in same VM with db");

    lisp_push_buffer(vm, NULL, 1024);
    Lisp_Stream *stream = lisp_push_stream(vm, &blob_stream_class, NULL);
    struct blob_stream *bs = lisp_stream_context(stream);
    if (SQLITE_OK != sqlite3_blob_open(db->instance, db_name, tbl_name, col_name, (sqlite3_int64)row, 1, &bs->blob))
        sqlite_err(vm, db->instance);
    bs->total = sqlite3_blob_bytes(bs->blob);
    bs->db = sqlite3_db_ref(db);
    lisp_make_output_port(vm);
}

static void op_version(Lisp_VM *vm, Lisp_Pair *args)
{
    lisp_push_cstr(vm, sqlite3_version);
}

void lisp_sqlite3_init(Lisp_VM *vm)
{
    lisp_defn(vm, "sqlite3-open",    op_sqlite3_open);
    lisp_defn(vm, "sqlite3-exec",    op_sqlite3_exec);
    lisp_defn(vm, "sqlite3-prepare", op_sqlite3_prepare);
    lisp_defn(vm, "sqlite3-bind",    op_sqlite3_bind);
    lisp_defn(vm, "sqlite3-step",    op_sqlite3_step);
    lisp_defn(vm, "sqlite3-run",     op_sqlite3_run);
    lisp_defn(vm, "sqlite3-reset",   op_sqlite3_reset);
    lisp_defn(vm, "sqlite3-errmsg",  op_sqlite3_errmsg);
    lisp_defn(vm, "sqlite3-last-insert-rowid", op_sqlite3_last_insert_rowid);
    lisp_defn(vm, "sqlite3-open-blob-input", op_open_blob_input);
    lisp_defn(vm, "sqlite3-open-blob-output", op_open_blob_output);
    lisp_defn(vm, "sqlite3-version", op_version);
}

int sqlite3_exec_simple(sqlite3* db, sqlite3_stmt **ppStmt, const char *sql, const char *fmt, ...)
{
    va_list ap;
    sqlite3_stmt *stmt = NULL;
    int rc;
    
    assert(db != NULL);
    
    va_start(ap, fmt);
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        int i = 1;
        for (;*fmt;fmt++)
        {
            switch (*fmt) 
            {
                case 'i': 
                    sqlite3_bind_int(stmt, i++, va_arg(ap, int));  
                    break;
                case 'I':
                    sqlite3_bind_int64(stmt, i++, va_arg(ap, sqlite3_int64));  
                    break;
                case 'T':
                    sqlite3_bind_int64(stmt, i++, (sqlite3_int64)time(NULL));
                    break;
                case 'f':
                    sqlite3_bind_double(stmt, i++, va_arg(ap, double));
                    break;
                case 's':
                    sqlite3_bind_text(stmt, i++, va_arg(ap, const char*), -1, SQLITE_STATIC);
                    break;
                case 'b':
                {
                    const void *p = va_arg(ap, const void*);
                    int n = va_arg(ap, int);
                    sqlite3_bind_blob(stmt, i++, p, n, NULL);
                    break;
                }
                case ':':
                    break;

                default:
                    fprintf(stderr, "sqlite3_exec_simple(): invalid format char: %c\n", *fmt);
                    rc = SQLITE_ERROR;
                    break;
            }
        } /* End of Bindings */
        
        if (rc == SQLITE_OK)
        {
            rc = sqlite3_step(stmt);
        }
    } else {
        fprintf(stderr, "sqlite3_exec_simple(): %s\n", sqlite3_errmsg(db));
    }
    va_end(ap);
    if (ppStmt != NULL) 
        *ppStmt = stmt;
    else
        sqlite3_finalize(stmt);
    return rc;
}

