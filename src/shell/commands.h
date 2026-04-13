#ifndef COMMANDS_H
#define COMMANDS_H

typedef void (*cmd_handler_t)(int argc, char **argv);

typedef struct {
    const char    *name;
    const char    *description;
    cmd_handler_t  handler;
} command_t;

const command_t *commands_get_table(int *count);

#endif
