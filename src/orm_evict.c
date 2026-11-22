/* Last activity of the workflow instances (T11c, src/orm_evict.h).
 *
 * Each entry is in a hash bucket (chained, the table doubled when it holds
 * more entries than buckets) and in a doubly linked queue sorted by due
 * time. A touch or a defer moves the entry towards the tail, scanning back
 * from the tail: with one idle period and a monotonic clock every new due
 * time is the largest, so the move is O(1). */
#include "orm_evict.h"

#include "log.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BUCKETS_MIN 64

typedef struct node {
  char *wf;
  long long last, due;
  struct node *prev, *next; /* the queue, by due time */
  struct node *chain;       /* the next entry of the bucket */
} node;

struct clpd_orm_evict {
  node **b;
  size_t nb; /* a power of two */
  size_t n;
  node *head, *tail;
};

static void *xalloc(void *p, size_t n) {
  void *q = realloc(p, n);
  if (q == NULL) clpd_log_die_errno();
  return q;
}

/* FNV-1a. */
static size_t hash(const char *s) {
  uint64_t h = UINT64_C(14695981039346656037);
  for (; *s != '\0'; s++) {
    h ^= (unsigned char)*s;
    h *= UINT64_C(1099511628211);
  }
  return (size_t)h;
}

clpd_orm_evict *clpd_orm_evict_create(void) {
  clpd_orm_evict *t = xalloc(NULL, sizeof *t);
  memset(t, 0, sizeof *t);
  t->nb = BUCKETS_MIN;
  t->b = xalloc(NULL, t->nb * sizeof *t->b);
  memset(t->b, 0, t->nb * sizeof *t->b);
  return t;
}

void clpd_orm_evict_free(clpd_orm_evict *t) {
  node *e, *next;
  if (t == NULL) return;
  for (e = t->head; e != NULL; e = next) {
    next = e->next;
    free(e->wf);
    free(e);
  }
  free(t->b);
  free(t);
}

static node *find(const clpd_orm_evict *t, const char *wf) {
  node *e;
  for (e = t->b[hash(wf) & (t->nb - 1)]; e != NULL; e = e->chain)
    if (strcmp(e->wf, wf) == 0) return e;
  return NULL;
}

static void unqueue(clpd_orm_evict *t, node *e) {
  if (e->prev != NULL) e->prev->next = e->next;
  else t->head = e->next;
  if (e->next != NULL) e->next->prev = e->prev;
  else t->tail = e->prev;
  e->prev = e->next = NULL;
}

/* Queues e by its due time, after the entries due at the same time. */
static void enqueue(clpd_orm_evict *t, node *e) {
  node *p = t->tail;
  while (p != NULL && p->due > e->due) p = p->prev;
  e->prev = p;
  e->next = p != NULL ? p->next : t->head;
  if (e->next != NULL) e->next->prev = e;
  else t->tail = e;
  if (p != NULL) p->next = e;
  else t->head = e;
}

static void grow(clpd_orm_evict *t) {
  size_t nb = 2 * t->nb, i;
  node **b = xalloc(NULL, nb * sizeof *b);
  memset(b, 0, nb * sizeof *b);
  for (i = 0; i < t->nb; i++) {
    node *e = t->b[i], *chain;
    for (; e != NULL; e = chain) {
      size_t k = hash(e->wf) & (nb - 1);
      chain = e->chain;
      e->chain = b[k];
      b[k] = e;
    }
  }
  free(t->b);
  t->b = b;
  t->nb = nb;
}

void clpd_orm_evict_touch(clpd_orm_evict *t, const char *wf, long long now,
                          long long due) {
  node *e = find(t, wf);
  if (e == NULL) {
    size_t n = strlen(wf) + 1, k;
    if (t->n + 1 > t->nb) grow(t);
    e = xalloc(NULL, sizeof *e);
    memset(e, 0, sizeof *e);
    e->wf = memcpy(xalloc(NULL, n), wf, n);
    k = hash(wf) & (t->nb - 1);
    e->chain = t->b[k];
    t->b[k] = e;
    t->n++;
  } else {
    unqueue(t, e);
  }
  e->last = now;
  e->due = due;
  enqueue(t, e);
}

void clpd_orm_evict_defer(clpd_orm_evict *t, const char *wf, long long due) {
  node *e = find(t, wf);
  if (e == NULL) return;
  unqueue(t, e);
  e->due = due;
  enqueue(t, e);
}

void clpd_orm_evict_remove(clpd_orm_evict *t, const char *wf) {
  node **p = &t->b[hash(wf) & (t->nb - 1)], *e;
  while (*p != NULL && strcmp((*p)->wf, wf) != 0) p = &(*p)->chain;
  if ((e = *p) == NULL) return;
  *p = e->chain;
  unqueue(t, e);
  t->n--;
  free(e->wf);
  free(e);
}

int clpd_orm_evict_next(const clpd_orm_evict *t, long long now,
                        const char **wf, long long *last) {
  if (t->head == NULL || t->head->due > now) return 0;
  *wf = t->head->wf;
  *last = t->head->last;
  return 1;
}

int clpd_orm_evict_last(const clpd_orm_evict *t, const char *wf,
                        long long *last) {
  const node *e = find(t, wf);
  if (e == NULL) return 0;
  *last = e->last;
  return 1;
}

size_t clpd_orm_evict_count(const clpd_orm_evict *t) { return t->n; }
