/* Shared reader for "key = value" files: the daemon configuration and the
 * package manifest (docs/configuration.md, docs/packages.md). */
#ifndef CLPD_KVFILE_H
#define CLPD_KVFILE_H

#include <stddef.h>
#include <stdio.h>

typedef struct {
  char **items;
  size_t count;
} clpd_list;

typedef struct {
  const char *path;
  FILE *err;
  int errors; /* errors reported so far */
  long line;  /* current line, 1-based */
} clpd_kv;

/* Called for each "key = value" line with trimmed, writable strings. */
typedef void clpd_kv_fn(clpd_kv *kv, char *key, char *value, void *ctx);

/* Reports "clpdaemon: <path>:<line>: <message>" and counts the error. */
void clpd_kv_error(clpd_kv *kv, const char *fmt, ...);

/* Reads `path`: skips blank and '#' lines, reports syntax errors and NUL
 * bytes, and calls `fn` for each pair. Returns 1 when the file cannot be
 * opened or read (message already written; file-level checks should be
 * skipped), otherwise 0 with kv->errors counting the line errors. */
int clpd_kv_read(clpd_kv *kv, const char *path, FILE *err, clpd_kv_fn *fn,
                 void *ctx);

/* Finds `key` in `names`; reports an unknown key, or a duplicate using
 * `seen` (line of the first occurrence, 0 = not yet). Returns the index or
 * -1 after reporting. */
int clpd_kv_key(clpd_kv *kv, const char *const *names, int count, long *seen,
                const char *key);

/* Validates one list item; reports and returns 0 to reject it. */
typedef int clpd_kv_item_fn(clpd_kv *kv, const char *key, const char *item,
                            void *ctx);

/* Splits a comma-separated value (empty value = empty list), trimming items;
 * an empty item or a rejected item is reported and stops the split. On
 * success replaces *out (if not NULL) and returns 1. */
int clpd_kv_list(clpd_kv *kv, const char *key, char *value, clpd_list *out,
                 clpd_kv_item_fn *check, void *ctx);

void clpd_list_free(clpd_list *l);
char *clpd_strdup(const char *s); /* exits on allocation failure */

#endif
