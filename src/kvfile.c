#define _POSIX_C_SOURCE 200809L

#include "kvfile.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

void clpd_kv_error(clpd_kv *kv, const char *fmt, ...) {
  va_list ap;
  fprintf(kv->err, "clpdaemon: %s:%ld: ", kv->path, kv->line);
  va_start(ap, fmt);
  vfprintf(kv->err, fmt, ap);
  va_end(ap);
  fputc('\n', kv->err);
  kv->errors++;
}

static char *trim(char *s) {
  char *end;
  while (*s == ' ' || *s == '\t') s++;
  end = s + strlen(s);
  while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' ||
                     end[-1] == '\r'))
    end--;
  *end = '\0';
  return s;
}

char *clpd_strdup(const char *s) {
  size_t n = strlen(s) + 1;
  char *d = malloc(n);
  if (d == NULL) {
    perror("clpdaemon");
    exit(1);
  }
  memcpy(d, s, n);
  return d;
}

void clpd_list_free(clpd_list *l) {
  size_t i;
  for (i = 0; i < l->count; i++) free(l->items[i]);
  free(l->items);
  l->items = NULL;
  l->count = 0;
}

int clpd_kv_read(clpd_kv *kv, const char *path, FILE *err, clpd_kv_fn *fn,
                 void *ctx) {
  char *buf = NULL;
  size_t cap = 0;
  ssize_t len;
  FILE *f;
  int read_errno;

  kv->path = path;
  kv->err = err;
  kv->errors = 0;
  kv->line = 0;
  f = fopen(path, "r");
  if (f == NULL) {
    fprintf(err, "clpdaemon: %s: cannot open: %s\n", path, strerror(errno));
    return 1;
  }
  for (;;) {
    char *line, *eq, *key, *value;
    errno = 0;
    len = getline(&buf, &cap, f);
    if (len == -1) break;
    kv->line++;
    if (memchr(buf, '\0', (size_t)len) != NULL) {
      clpd_kv_error(kv, "line contains a NUL byte");
      continue;
    }
    line = trim(buf);
    if (*line == '\0' || *line == '#') continue;
    eq = strchr(line, '=');
    if (eq == NULL) {
      clpd_kv_error(kv, "expected 'key = value'");
      continue;
    }
    *eq = '\0';
    key = trim(line);
    value = trim(eq + 1);
    if (*key == '\0') {
      clpd_kv_error(kv, "expected 'key = value'");
      continue;
    }
    fn(kv, key, value, ctx);
  }
  /* getline returned -1: end of file, or a failure (read error or ENOMEM)
   * whose errno was set by that call; errno was cleared before it. A failure
   * leaves the stream without its end-of-file flag, and errno may still be 0
   * when the cause is unknown, hence the EIO fallback. Save errno before
   * free() may change it. */
  read_errno = errno;
  free(buf);
  if (ferror(f) || !feof(f)) {
    int saved = read_errno != 0 ? read_errno : EIO;
    fclose(f);
    fprintf(err, "clpdaemon: %s: cannot read: %s\n", path, strerror(saved));
    return 1;
  }
  fclose(f);
  return 0;
}

int clpd_kv_key(clpd_kv *kv, const char *const *names, int count, long *seen,
                const char *key) {
  int k;
  for (k = 0; k < count && strcmp(names[k], key) != 0; k++) {
  }
  if (k == count) {
    clpd_kv_error(kv, "unknown key '%s'", key);
    return -1;
  }
  if (seen[k] != 0) {
    clpd_kv_error(kv, "duplicate key '%s' (first set on line %ld)", key,
                  seen[k]);
    return -1;
  }
  seen[k] = kv->line;
  return k;
}

int clpd_kv_list(clpd_kv *kv, const char *key, char *value, clpd_list *out,
                 clpd_kv_item_fn *check, void *ctx) {
  char *item = value, *comma;
  clpd_list list = {NULL, 0};

  if (*value != '\0') {
    for (;;) {
      comma = strchr(item, ',');
      if (comma != NULL) *comma = '\0';
      item = trim(item);
      if (*item == '\0') {
        clpd_kv_error(kv, "%s: empty list item", key);
        clpd_list_free(&list);
        return 0;
      }
      if (check != NULL && !check(kv, key, item, ctx)) {
        clpd_list_free(&list);
        return 0;
      }
      if (out != NULL) {
        char **grown = realloc(list.items, (list.count + 1) * sizeof *grown);
        if (grown == NULL) {
          perror("clpdaemon");
          exit(1);
        }
        list.items = grown;
        list.items[list.count++] = clpd_strdup(item);
      }
      if (comma == NULL) break;
      item = comma + 1;
    }
  }
  if (out != NULL) {
    clpd_list_free(out);
    *out = list;
  }
  return 1;
}
