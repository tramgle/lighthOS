/* envtest — exercise envp end-to-end:
 *   1. in-process setenv/getenv/unsetenv round-trip
 *   2. setenv ENVTEST_MARKER=inherit, spawn /bin/env, verify the
 *      child sees it in its envp.
 *
 * We implement setenv/getenv/unsetenv over a local env array
 * bootstrapped from main's envp (copied into a mutable buffer so
 * we can add / overwrite / remove entries). Prints "OK" to stdout
 * on success of the in-process tests, then execs /bin/env with
 * the mutated env so its output follows ours in the redirect file.
 */
#include "ulib_x64.h"

#define MAX_ENV 32
#define ENV_BUF 2048

static char *env_ptrs[MAX_ENV + 1];
static int  env_count;
static char env_buf[ENV_BUF];
static int  env_buf_used;

static int env_key_eq(const char *entry, const char *key) {
    int i = 0;
    while (key[i] && entry[i] && entry[i] != '=' && entry[i] == key[i]) i++;
    return key[i] == 0 && entry[i] == '=';
}

static void env_init(char **envp) {
    env_count = 0;
    if (!envp) return;
    for (int i = 0; envp[i] && env_count < MAX_ENV; i++) {
        size_t len = u_strlen(envp[i]) + 1;
        if (env_buf_used + (int)len > ENV_BUF) break;
        char *dst = env_buf + env_buf_used;
        for (size_t j = 0; j < len; j++) dst[j] = envp[i][j];
        env_buf_used += (int)len;
        env_ptrs[env_count++] = dst;
    }
    env_ptrs[env_count] = 0;
}

static int env_set(const char *key, const char *val, int overwrite) {
    for (int i = 0; i < env_count; i++) {
        if (env_key_eq(env_ptrs[i], key)) {
            if (!overwrite) return 0;
            /* Overwrite in-place if the new entry fits; otherwise
               append new + skip old. */
            size_t need = u_strlen(key) + 1 + u_strlen(val) + 1;
            if (env_buf_used + (int)need > ENV_BUF) return -1;
            char *dst = env_buf + env_buf_used;
            int k = 0;
            for (int j = 0; key[j]; j++) dst[k++] = key[j];
            dst[k++] = '=';
            for (int j = 0; val[j]; j++) dst[k++] = val[j];
            dst[k++] = 0;
            env_buf_used += k;
            env_ptrs[i] = dst;
            return 0;
        }
    }
    if (env_count >= MAX_ENV) return -1;
    size_t need = u_strlen(key) + 1 + u_strlen(val) + 1;
    if (env_buf_used + (int)need > ENV_BUF) return -1;
    char *dst = env_buf + env_buf_used;
    int k = 0;
    for (int j = 0; key[j]; j++) dst[k++] = key[j];
    dst[k++] = '=';
    for (int j = 0; val[j]; j++) dst[k++] = val[j];
    dst[k++] = 0;
    env_buf_used += k;
    env_ptrs[env_count++] = dst;
    env_ptrs[env_count] = 0;
    return 0;
}

static char *env_get(const char *key) {
    for (int i = 0; i < env_count; i++)
        if (env_key_eq(env_ptrs[i], key))
            return env_ptrs[i] + u_strlen(key) + 1;
    return 0;
}

static int env_unset(const char *key) {
    for (int i = 0; i < env_count; i++) {
        if (env_key_eq(env_ptrs[i], key)) {
            for (int j = i; j < env_count; j++) env_ptrs[j] = env_ptrs[j + 1];
            env_count--;
            return 0;
        }
    }
    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv;
    env_init(envp);

    if (env_set("FOO", "bar", 1) != 0) { u_puts_n("FAIL setenv\n"); return 1; }
    char *v = env_get("FOO");
    if (!v || u_strcmp(v, "bar") != 0) { u_puts_n("FAIL getenv\n"); return 1; }
    if (env_set("FOO", "baz", 0) != 0) { u_puts_n("FAIL no-overwrite\n"); return 1; }
    v = env_get("FOO");
    if (!v || u_strcmp(v, "bar") != 0) { u_puts_n("FAIL kept-old\n"); return 1; }
    if (env_unset("FOO") != 0) { u_puts_n("FAIL unsetenv\n"); return 1; }
    if (env_get("FOO") != 0)   { u_puts_n("FAIL unsetenv-stuck\n"); return 1; }

    u_puts_n("OK\n");

    if (env_set("ENVTEST_MARKER", "inherit", 1) != 0) return 1;
    char *child_argv[] = { (char *)"env", 0 };
    sys_execve("/bin/env", child_argv, env_ptrs);
    u_puts_n("FAIL exec\n");
    return 1;
}
