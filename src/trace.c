#define _GNU_SOURCE

#include "trace.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  char *path; /* NULL = format disabled */
  FILE *f;
  int write_failed; /* reported once until the next successful reopen */
} sink;

struct clpd_trace {
  sink text, jsonl;
  char *node_id;
  clpd_trace_clock *clock;
  FILE *err; /* daemon messages (write failures) */
  /* Serializes every event, reopen and setter: the main thread (CLIPS
   * events, SIGHUP) and the fd capture reader thread share the sink. */
  pthread_mutex_t lock;
};

/* Growable line buffer. On allocation failure it is marked broken and the
 * line is dropped rather than written truncated. */
typedef struct {
  char *s;
  size_t len, cap;
  int broken;
} line;

static void put(line *b, const char *s, size_t n) {
  if (b->broken) return;
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 256;
    char *p;
    while (b->len + n + 1 > cap) cap *= 2;
    p = realloc(b->s, cap);
    if (p == NULL) {
      b->broken = 1;
      return;
    }
    b->s = p;
    b->cap = cap;
  }
  memcpy(b->s + b->len, s, n);
  b->len += n;
  b->s[b->len] = '\0';
}

static void puts_(line *b, const char *s) { put(b, s, strlen(s)); }

/* Formats one integer with a short fixed affix; longer text goes through
 * puts_. A result that does not fit is cut, never over-read. */
static void putf(line *b, const char *fmt, long long v) {
  char tmp[48];
  int n = snprintf(tmp, sizeof tmp, fmt, v);
  if (n < 0) return;
  if ((size_t)n >= sizeof tmp) n = (int)(sizeof tmp - 1);
  put(b, tmp, (size_t)n);
}

/* Text format: control bytes become \xHH so one event stays one line. */
static void put_text(line *b, const char *s, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x20 || c == 0x7f) {
      char esc[5];
      snprintf(esc, sizeof esc, "\\x%02x", c);
      put(b, esc, 4);
    } else {
      put(b, s + i, 1);
    }
  }
}

/* JSON string body (RFC 8259 escapes). */
static void put_json(line *b, const char *s, size_t n) {
  size_t i;
  put(b, "\"", 1);
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    switch (c) {
      case '"': puts_(b, "\\\""); break;
      case '\\': puts_(b, "\\\\"); break;
      case '\n': puts_(b, "\\n"); break;
      case '\r': puts_(b, "\\r"); break;
      case '\t': puts_(b, "\\t"); break;
      case '\b': puts_(b, "\\b"); break;
      case '\f': puts_(b, "\\f"); break;
      default:
        if (c < 0x20) {
          char esc[7];
          snprintf(esc, sizeof esc, "\\u%04x", c);
          put(b, esc, 6);
        } else {
          put(b, s + i, 1);
        }
    }
  }
  put(b, "\"", 1);
}

static void put_json_str(line *b, const char *s) { put_json(b, s, strlen(s)); }

static void real_clock(long long *sec, long *nsec) {
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  *sec = (long long)now.tv_sec;
  *nsec = now.tv_nsec;
}

/* UTC ISO-8601 with milliseconds (truncated), e.g. 2026-11-18T10:37:25.641Z */
static void timestamp(const clpd_trace *t, char out[32]) {
  long long sec;
  long nsec;
  time_t secs;
  struct tm tm;
  size_t n;
  t->clock(&sec, &nsec);
  secs = (time_t)sec;
  gmtime_r(&secs, &tm);
  n = strftime(out, 32, "%Y-%m-%dT%H:%M:%S", &tm);
  snprintf(out + n, 32 - n, ".%03ldZ", nsec / 1000000);
}

/* Starts a text line: "<ts> <node_id> <wf_id|-> ". */
static void text_head(line *b, const clpd_trace *t, const char *ts,
                      const char *wf_id) {
  puts_(b, ts);
  puts_(b, " ");
  puts_(b, t->node_id);
  puts_(b, " ");
  if (wf_id != NULL)
    put_text(b, wf_id, strlen(wf_id));
  else
    puts_(b, "-");
  puts_(b, " ");
}

/* Starts a JSON object up to and including the event name. */
static void json_head(line *b, const clpd_trace *t, const char *ts,
                      const char *wf_id, const char *event) {
  puts_(b, "{\"ts\":\"");
  puts_(b, ts);
  puts_(b, "\",\"node_id\":");
  put_json_str(b, t->node_id);
  if (wf_id != NULL) {
    puts_(b, ",\"wf_id\":");
    put_json_str(b, wf_id);
  }
  puts_(b, ",\"event\":\"");
  puts_(b, event);
  puts_(b, "\"");
}

/* Writes a finished line and flushes it, so the trace is current when the
 * daemon stops abnormally. A failed write or flush loses the line (it may be
 * left partial) and is reported once on err until the next successful
 * reopen. */
static void emit(const clpd_trace *t, sink *s, line *b) {
  if (s->f != NULL && !b->broken) {
    put(b, "\n", 1);
    if (!b->broken &&
        (fwrite(b->s, 1, b->len, s->f) != b->len || fflush(s->f) != 0)) {
      if (!s->write_failed)
        fprintf(t->err, "clpdaemon: %s: cannot write: %s\n", s->path,
                strerror(errno));
      s->write_failed = 1;
      clearerr(s->f);
    }
  }
  free(b->s);
}

static FILE *open_append(const char *path) { return fopen(path, "ae"); }

clpd_trace *clpd_trace_open(const char *text_path, const char *jsonl_path,
                            const char *node_id, FILE *err) {
  const char *paths[2];
  sink *sinks[2];
  int i, failed = 0;
  clpd_trace *t = calloc(1, sizeof *t);

  if (t == NULL) {
    fprintf(err, "clpdaemon: cannot open the trace: %s\n", strerror(errno));
    return NULL;
  }
  pthread_mutex_init(&t->lock, NULL);
  t->clock = real_clock;
  t->err = err;
  t->node_id = strdup(node_id);
  if (t->node_id == NULL) {
    fprintf(err, "clpdaemon: cannot open the trace: %s\n", strerror(errno));
    pthread_mutex_destroy(&t->lock);
    free(t);
    return NULL;
  }
  paths[0] = text_path;
  paths[1] = jsonl_path;
  sinks[0] = &t->text;
  sinks[1] = &t->jsonl;
  for (i = 0; i < 2; i++) {
    if (paths[i] == NULL) continue;
    sinks[i]->path = strdup(paths[i]);
    if (sinks[i]->path == NULL) {
      fprintf(err, "clpdaemon: %s: cannot open: %s\n", paths[i],
              strerror(errno));
      failed = 1;
      continue;
    }
    sinks[i]->f = open_append(paths[i]);
    if (sinks[i]->f == NULL) {
      fprintf(err, "clpdaemon: %s: cannot open: %s\n", paths[i],
              strerror(errno));
      failed = 1;
    }
  }
  if (failed) {
    clpd_trace_close(t);
    return NULL;
  }
  return t;
}

void clpd_trace_set_clock(clpd_trace *t, clpd_trace_clock *clock) {
  pthread_mutex_lock(&t->lock);
  t->clock = clock;
  pthread_mutex_unlock(&t->lock);
}

void clpd_trace_set_err(clpd_trace *t, FILE *err) {
  pthread_mutex_lock(&t->lock);
  t->err = err;
  pthread_mutex_unlock(&t->lock);
}

int clpd_trace_active(const clpd_trace *t) {
  return t->text.path != NULL || t->jsonl.path != NULL;
}

int clpd_trace_reopen(clpd_trace *t, FILE *err) {
  sink *sinks[2];
  int i, failed = 0;

  pthread_mutex_lock(&t->lock);
  sinks[0] = &t->text;
  sinks[1] = &t->jsonl;
  for (i = 0; i < 2; i++) {
    FILE *f;
    if (sinks[i]->path == NULL) continue;
    f = open_append(sinks[i]->path);
    if (f == NULL) {
      fprintf(err, "clpdaemon: %s: cannot reopen: %s\n", sinks[i]->path,
              strerror(errno));
      failed++;
      continue;
    }
    fclose(sinks[i]->f);
    sinks[i]->f = f;
    sinks[i]->write_failed = 0;
  }
  pthread_mutex_unlock(&t->lock);
  return failed;
}

static const char *const fact_text_tag[] = {
    [CLPD_TRACE_ASSERT] = "FACT+",
    [CLPD_TRACE_RETRACT] = "FACT-",
    [CLPD_TRACE_MODIFY] = "FACT~"};
static const char *const fact_event[] = {[CLPD_TRACE_ASSERT] = "assert",
                                         [CLPD_TRACE_RETRACT] = "retract",
                                         [CLPD_TRACE_MODIFY] = "modify"};

void clpd_trace_fact(clpd_trace *t, clpd_trace_fact_kind kind,
                     const char *wf_id, long long index, const char *tmpl,
                     const char *text) {
  char ts[32];
  if (!clpd_trace_active(t)) return;
  pthread_mutex_lock(&t->lock);
  timestamp(t, ts);
  if (t->text.f != NULL) {
    line b = {0};
    text_head(&b, t, ts, wf_id);
    puts_(&b, fact_text_tag[kind]);
    putf(&b, " f-%lld ", index);
    put_text(&b, text, strlen(text));
    emit(t, &t->text, &b);
  }
  if (t->jsonl.f != NULL) {
    line b = {0};
    json_head(&b, t, ts, wf_id, fact_event[kind]);
    putf(&b, ",\"fact\":%lld", index);
    puts_(&b, ",\"template\":");
    put_json_str(&b, tmpl);
    puts_(&b, ",\"text\":");
    put_json_str(&b, text);
    puts_(&b, "}");
    emit(t, &t->jsonl, &b);
  }
  pthread_mutex_unlock(&t->lock);
}

void clpd_trace_fire(clpd_trace *t, const char *wf_id, const char *module,
                     const char *rule, const long long *facts, size_t n) {
  char ts[32];
  size_t i;
  if (!clpd_trace_active(t)) return;
  pthread_mutex_lock(&t->lock);
  timestamp(t, ts);
  if (t->text.f != NULL) {
    line b = {0};
    text_head(&b, t, ts, wf_id);
    puts_(&b, "FIRE ");
    put_text(&b, module, strlen(module));
    puts_(&b, "::");
    put_text(&b, rule, strlen(rule));
    for (i = 0; i < n; i++) putf(&b, i == 0 ? " f-%lld" : ",f-%lld", facts[i]);
    emit(t, &t->text, &b);
  }
  if (t->jsonl.f != NULL) {
    line b = {0};
    json_head(&b, t, ts, wf_id, "fire");
    puts_(&b, ",\"module\":");
    put_json_str(&b, module);
    puts_(&b, ",\"rule\":");
    put_json_str(&b, rule);
    puts_(&b, ",\"facts\":[");
    for (i = 0; i < n; i++) putf(&b, i == 0 ? "%lld" : ",%lld", facts[i]);
    puts_(&b, "]}");
    emit(t, &t->jsonl, &b);
  }
  pthread_mutex_unlock(&t->lock);
}

void clpd_trace_output(clpd_trace *t, const char *wf_id, const char *stream,
                       const char *text, size_t len) {
  char ts[32];
  if (!clpd_trace_active(t)) return;
  pthread_mutex_lock(&t->lock);
  timestamp(t, ts);
  if (t->text.f != NULL) {
    line b = {0};
    text_head(&b, t, ts, wf_id);
    puts_(&b, stream);
    puts_(&b, " ");
    put_text(&b, text, len);
    emit(t, &t->text, &b);
  }
  if (t->jsonl.f != NULL) {
    line b = {0};
    json_head(&b, t, ts, wf_id, "output");
    puts_(&b, ",\"stream\":");
    put_json_str(&b, stream);
    puts_(&b, ",\"text\":");
    put_json(&b, text, len);
    puts_(&b, "}");
    emit(t, &t->jsonl, &b);
  }
  pthread_mutex_unlock(&t->lock);
}

void clpd_trace_close(clpd_trace *t) {
  if (t == NULL) return;
  if (t->text.f != NULL) fclose(t->text.f);
  if (t->jsonl.f != NULL) fclose(t->jsonl.f);
  free(t->text.path);
  free(t->jsonl.path);
  free(t->node_id);
  pthread_mutex_destroy(&t->lock);
  free(t);
}
