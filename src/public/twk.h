#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*twk_console_output_fn)(void *ctx, const char *s);
void twk_set_console_output(twk_console_output_fn output, void *ctx);

typedef void (*twk_receive_message_fn)(void *ctx, const char *s);
void twk_set_receive_message(twk_receive_message_fn recv, void *ctx);

const char *twk_get_dist_path(void);
const char *twk_get_var_path(void);

/*
 * Define the dist path, where the executable is at.
 * Fixed files, like <lisp> folder should be placed in dist folder.
 * If no dist path is provided programatically,
 * TWK_DIST will be examined, and finally current working
 * directory will be used as the last resort.
 */
void twk_set_dist_path(const char *path);

/*
 * Define the var path, where variable files should be placed.
 * Looking order same as dist path.
 */
void twk_set_var_path(const char *path);

/*
 * twk_start() -- Start the twinkle runtime.
 *
 * Return 0 if success, otherwise -1.
 */
int twk_start(int n, const char *args[]);

#ifdef __cplusplus
}
#endif
