#ifndef SHELL_H
#define SHELL_H

#include "fs/vfs.h"

#define SHELL_CWD_MAX VFS_MAX_PATH

void        shell_run(void);
const char *shell_get_cwd(void);
void        shell_set_cwd(const char *path);
void        shell_resolve_path(const char *input, char *output, int out_max);
void        shell_save_history(void);

#endif
