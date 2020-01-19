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
#include "lisp_fs.h"
#include "common.h"
#ifndef WIN32
#include <unistd.h>
#endif

struct dir_reader {
	DIR *dir;
	bool (*ignore)(const char *filename);
};

static void dir_reader_read(Lisp_VM *vm, struct dir_reader *reader)
{
	struct dirent *e;
	
	if (!reader || !reader->dir) {
		lisp_push(vm, lisp_undef);
		return;
	}
	
	while ((e = readdir(reader->dir)) != NULL) {
		if (strcmp(e->d_name,"." ) == 0
		 || strcmp(e->d_name,"..") == 0)
			continue;
		if (reader->ignore && reader->ignore(e->d_name))
			continue;
		break;
	}
	if (e == NULL) {
		lisp_push(vm, lisp_nil);
	} else {
		PUSHX(vm, lisp_string_new(vm, e->d_name, strlen(e->d_name)));
		PUSHX(vm, ((e->d_type == DT_DIR) ? lisp_true : lisp_false));
		lisp_cons(vm);
	}
}

static void dir_reader_close(Lisp_VM *vm, void *ctx)
{
	struct dir_reader *reader = ctx;
	if (reader->dir) {
		closedir(reader->dir);
		reader->dir = NULL;
	}
}

static struct lisp_object_ex_class_t dir_reader_class = {
	.size = sizeof(struct dir_reader),
	.finalize = dir_reader_close
};

static bool should_ignore(const char *filename)
{
    size_t len = strlen(filename);
    if (len == 0)
        return true;
    if (filename[len-1] == '~')
        return true;
    if (filename[0] == '.') {
        return
            strcmp(filename, ".git")      == 0 ||
            strcmp(filename, ".cvs")      == 0 ||
            strcmp(filename, ".twinkle")  == 0 ||
            strcmp(filename, ".")         == 0 ||
            strcmp(filename, "..")        == 0 ||
            strcmp(filename, ".DS_Store") == 0 ||
            strcmp(filename, ".svn")      == 0;
    }
    return false;
}

/* Return base58 encoded sha256 hash of the file at path */
static void hash_file(Lisp_VM *vm, const char *path)
{

}

static void build_tree(Lisp_VM *vm, const char *path, const char *repo_dir)
{
        int n = 0;
        printf("build tree: %s\n", path);
        DIR *dir = opendir(path);
        if (!dir)
            goto Done;
        struct dirent *entry;
        char buf[256];
        strcpy(buf, path);
        strcat(buf, "/");
        size_t parent_len = strlen(buf);

        while ((entry = readdir(dir))!=NULL) {
            if (should_ignore(entry->d_name))
                continue;
            strcpy(buf+parent_len, entry->d_name);
            if (entry->d_type == DT_DIR) {
                build_tree(vm, buf, repo_dir);
                n++;
            } else if (entry->d_type == DT_REG){
                struct stat statbuf;
                stat(buf, &statbuf);

                PUSHX(vm, lisp_string_new(vm, entry->d_name, strlen(entry->d_name)));
                hash_file(vm, buf);
                lisp_cons(vm);
                
                n++;
            }
        }
Done:
        lisp_push(vm, lisp_nil);
        while (n > 0) {
            lisp_cons(vm);
            n--;
        }
        if (dir)
            closedir(dir);
}


static void op_scan_tree(Lisp_VM*vm, Lisp_Pair*args)
{
    const char *dir = lisp_safe_cstring(vm, CAR(args));
    const char *repo_dir = lisp_safe_cstring(vm, CADR(args));
    build_tree(vm, dir, repo_dir);
}

static void op_opendir(Lisp_VM *vm, Lisp_Pair *args)
{
    	const char *dir = lisp_safe_cstring(vm, CAR(args));
    	Lisp_Object *o = lisp_make_object_ex(vm, &dir_reader_class);
	struct dir_reader *reader = lisp_object_ex_ptr(o);
	reader->dir = opendir(dir);
}

static void op_readdir(Lisp_VM *vm, Lisp_Pair *args)
{
    	Lisp_Object *o = CAR(args);
    	if (lisp_object_ex_class(o) != &dir_reader_class) {
    		lisp_err(vm, "not directory object");
	}
	struct dir_reader *reader = lisp_object_ex_ptr(o);
	dir_reader_read(vm, reader);
}

static void op_listdir(Lisp_VM *vm, Lisp_Pair *args)
{
	const char *path = lisp_safe_cstring(vm, CAR(args));
        int n = 0;
	jmp_buf jbuf;
        DIR *dir = opendir(path);
        if (!dir)
            	lisp_err(vm, "listdir: can not open directory: %s", path);
	
	jmp_buf *old = lisp_vm_set_error_trap(vm, &jbuf);
	if (setjmp(jbuf) == 0) {
        	struct dirent *entry;
		while ((entry = readdir(dir))!=NULL) {
			if (strcmp(entry->d_name, "." ) == 0
			 || strcmp(entry->d_name, "..") == 0)
				continue;
			PUSHX(vm, lisp_string_new(vm, entry->d_name, strlen(entry->d_name)));
			n++;
		}
	} else {
		closedir(dir);
		lisp_vm_resume_error(vm, old);
	}
	lisp_vm_set_error_trap(vm, old);
        lisp_push(vm, lisp_nil);
        while (n > 0) {
            lisp_cons(vm);
            n--;
        }
        if (dir)
            closedir(dir);
}

static void op_stat(Lisp_VM *vm, Lisp_Pair *args)
{
	const char *path = lisp_safe_cstring(vm, CAR(args));
	struct stat sb;
	lisp_begin_list(vm);
	if (stat(path, &sb) == 0) {
		if (sb.st_mode & S_IFDIR) {
			lisp_make_symbol(vm, "dir");
			lisp_push_number(vm, 0);
		} else if (sb.st_mode & S_IFREG) {
#ifdef WIN32
			lisp_make_symbol(vm, "file");
#else
			if (sb.st_mode & S_IXUSR) {
				lisp_make_symbol(vm, "filex");
			} else {
				lisp_make_symbol(vm, "file");
			}
#endif
			lisp_push_number(vm, sb.st_size);
		} else {
			lisp_push(vm, lisp_undef);
			lisp_push_number(vm, 0);
		}
#ifdef _WIN32
		lisp_push_number(vm, (double)sb.st_mtime);
#elif defined(__linux__)
		lisp_push_number(vm, (double)sb.st_mtime);
#else
		lisp_push_number(vm, sb.st_mtimespec.tv_sec);
#endif		
	}
	lisp_end_list(vm);
}

static void op_mkdir(Lisp_VM *vm, Lisp_Pair*args)
{
	const char *path = lisp_safe_cstring(vm, CAR(args));
#ifdef WIN32
	if (_mkdir(path) != 0)
#else
	if (mkdir(path, 0700) != 0)
#endif
		lisp_push(vm, lisp_false);
	else
		lisp_push(vm, lisp_true);
}

#define UTF8_TO_WCS(s, ws) MultiByteToWideChar(CP_UTF8,0,s,strlen(s),ws,sizeof(ws)-1)

int dir_exists(const char *path)
{
#ifdef _WIN32
	struct _stat sb;
	wchar_t wpath[MAX_PATH];
	int n = UTF8_TO_WCS(path, wpath);
	if (n > 0) {
		if (wpath[n-1] == '/')
			wpath[n-1] = 0;
		else
			wpath[n] = 0;
		if (_wstat(wpath, &sb) == 0) {
			if (sb.st_mode & S_IFDIR) {
				return 1;
			}
		}
	}
#else
	struct stat sb;
	if (stat(path, &sb) == 0) {
		if (sb.st_mode & S_IFDIR) {
			return 1;
		}
	}
#endif
	return 0;
}

int file_exists(const char *path)
{
#ifdef _WIN32
	struct _stat sb;
	wchar_t wpath[MAX_PATH];
	int n = UTF8_TO_WCS(path, wpath);
	if (n > 0) {
		if (wpath[n-1] == '/')
			wpath[n-1] = 0;
		else
			wpath[n] = 0;
		if (_wstat(wpath, &sb) == 0) {
			if (sb.st_mode & S_IFREG) {
				return 1;
			}
		}
	}
#else
	struct stat sb;
	if (stat(path, &sb) == 0) {
		if (sb.st_mode & S_IFREG) {
			return 1;
		}
	}
#endif
	return 0;
}

static void op_dir_exists_p(Lisp_VM *vm, Lisp_Pair *args)
{
	const char *path = lisp_safe_cstring(vm, CAR(args));
	lisp_push(vm, dir_exists(path)?lisp_true:lisp_false);
}

static void op_file_exists_p(Lisp_VM *vm, Lisp_Pair *args)
{
	const char *path = lisp_safe_cstring(vm, CAR(args));
	lisp_push(vm, file_exists(path)?lisp_true:lisp_false);
}

static void op_file_size(Lisp_VM *vm, Lisp_Pair *args)
{
	const char *path = lisp_safe_cstring(vm, CAR(args));
	struct stat sb;
	if (stat(path, &sb) == 0) {
		if (sb.st_mode & S_IFREG) {
			PUSHX(vm, lisp_number_new(vm, sb.st_size));
			return;
		}
	}
	lisp_push(vm, lisp_false);
}

static void op_read_file(Lisp_VM *vm, Lisp_Pair *args)
{
	const char *path = lisp_safe_cstring(vm, CAR(args));
	struct stat sb;
	if (stat(path, &sb) == 0) {
		if (sb.st_mode & S_IFREG) {
			Lisp_Buffer *b = lisp_buffer_new(vm, sb.st_size);
			PUSHX(vm, b);
			FILE *fp = fopen(path, "rb");
			if (fp) {
				size_t n = fread(lisp_buffer_bytes(b), 1, lisp_buffer_cap(b), fp);
				if (ferror(fp)) {
					fclose(fp);
					lisp_err(vm, "error reading file: %s", path);
				}
				lisp_buffer_set_size(b, n);
				fclose(fp);
				return;
			} else {
				lisp_pop(vm, 1);
			}
		}
	}
	lisp_push(vm, lisp_false);
}

static void op_chmod(Lisp_VM*vm, Lisp_Pair*args)
{
	const char *path = lisp_safe_cstring(vm, CAR(args));
	const char *mode = lisp_safe_cstring(vm, CADR(args));
#ifdef WIN32
	if (_chmod(path, strtol(mode, NULL, 8)) != 0)
#else
	if (chmod(path, strtol(mode, NULL, 8)) != 0)
#endif
		lisp_err(vm, "chmod error");
	lisp_push(vm, lisp_undef);
}

/*
 * (rename <old> <new>)
 * Rename file <old> with name <new>.
 * If <new> exists, it will be removed first.
 * If system crash in the middle of operation,
 * the original <new> file is guranteed to exists.
 */
static void op_rename(Lisp_VM *vm, Lisp_Pair *args)
{
	const char *oldname = lisp_safe_cstring(vm, CAR(args));
	const char *newname = lisp_safe_cstring(vm, CADR(args));
#ifdef WIN32
	wchar_t x[MAX_PATH], y[MAX_PATH];
	int n = UTF8_TO_WCS(oldname, x);
	assert(n > 0);
	x[n] = 0;
	n = UTF8_TO_WCS(newname, y);
	assert(n > 0);
	y[n] = 0;
	// Windows rename() will fail if newname already exists
	if (MoveFileExW(
	     x, y,
	     ( MOVEFILE_COPY_ALLOWED 
	     | MOVEFILE_REPLACE_EXISTING
	     | MOVEFILE_WRITE_THROUGH))
	){
		lisp_push(vm, lisp_true);
	} else {
		int err = GetLastError();
		lisp_push(vm, lisp_false);
	}
#else
	if (rename(oldname, newname) == 0) {
		lisp_push(vm, lisp_true);
	} else {
		lisp_push(vm, lisp_false);
	}
#endif
}

/* (unlink <path>)
 * Remove a file. Return true if success, otherwise false.
 */
static void op_unlink(Lisp_VM *vm, Lisp_Pair *args)
{
	const char *path = lisp_safe_cstring(vm, CAR(args));
	if (unlink(path) == 0) {
		lisp_push(vm, lisp_true);
	} else {
		lisp_push(vm, lisp_false);
	}
}

void lisp_fs_init(Lisp_VM *vm)
{
	lisp_defn(vm, "opendir", op_opendir);
	lisp_defn(vm, "readdir", op_readdir);
	lisp_defn(vm, "listdir", op_listdir);
	lisp_defn(vm, "mkdir", op_mkdir);
	lisp_defn(vm, "chmod", op_chmod);
	lisp_defn(vm, "dir-exists?", op_dir_exists_p);
	lisp_defn(vm, "file-exists?", op_file_exists_p);
	lisp_defn(vm, "stat", op_stat);
	lisp_defn(vm, "read-file", op_read_file);
	lisp_defn(vm, "filesize", op_file_size);
	lisp_defn(vm, "rename", op_rename);
    lisp_defn(vm, "fs-scan-tree", op_scan_tree);
	lisp_defn(vm, "unlink", op_unlink);
}
