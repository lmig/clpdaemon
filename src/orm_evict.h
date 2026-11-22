/* Last activity of the workflow instances in working memory (T11c, FR-9;
 * docs/orm.md#idle-eviction).
 *
 * One entry per instance (the text of its wf-id, as workflow_instance keys
 * it): the time of its last activity and the time it is due for the idle
 * check, both from the caller's monotonic clock in milliseconds. A hash on
 * the wf-id finds an entry in O(1); a queue ordered by due time gives the
 * entries due at a given time without looking at the others. The
 * synchronization (src/orm_sync.h) feeds it from its hooks and runs the
 * idle eviction from it. Engine thread only; out of memory is fatal
 * (clpd_log_die_errno). */
#ifndef CLPD_ORM_EVICT_H
#define CLPD_ORM_EVICT_H

#include <stddef.h>

typedef struct clpd_orm_evict clpd_orm_evict;

/* An empty table. */
clpd_orm_evict *clpd_orm_evict_create(void);
/* Frees t and every entry. NULL is accepted. */
void clpd_orm_evict_free(clpd_orm_evict *t);

/* Activity of instance wf at now: its entry (created when missing) gets
 * last = now and is due at due. */
void clpd_orm_evict_touch(clpd_orm_evict *t, const char *wf, long long now,
                          long long due);
/* The entry of wf (if any) is due at due; its last activity is kept. */
void clpd_orm_evict_defer(clpd_orm_evict *t, const char *wf, long long due);
/* Forgets instance wf (nothing when it has no entry). */
void clpd_orm_evict_remove(clpd_orm_evict *t, const char *wf);

/* The entry due first, when it is due at or before now: returns 1 with
 * *wf (valid until that entry changes) and *last, else 0. */
int clpd_orm_evict_next(const clpd_orm_evict *t, long long now,
                        const char **wf, long long *last);
/* The last activity of wf: 1 with *last, or 0 when it has no entry. */
int clpd_orm_evict_last(const clpd_orm_evict *t, const char *wf,
                        long long *last);
/* The number of entries. */
size_t clpd_orm_evict_count(const clpd_orm_evict *t);

#endif
