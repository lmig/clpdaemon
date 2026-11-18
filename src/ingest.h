/* Ingest pipeline (S4, FR-19; docs/ingest.md): fact JSON
 * {"template":"<name>","slots":{...}} validated against the packages'
 * `ingest` lists and asserted with the CLIPS FactBuilder, and one bounded
 * queue that any thread may post to and the engine thread drains before
 * each cycle.
 *
 * The queue is process-wide (host->post_fact has no context argument): a
 * mutex guards it, so clpd_ingest_post, clpd_ingest_queue_length and
 * clpd_ingest_set_notify are safe from any thread, and the queue works
 * before the event loop exists. Every function that takes an Environment
 * runs on the engine thread only. */
#ifndef CLPD_INGEST_H
#define CLPD_INGEST_H

#include <stddef.h>

#include "clips.h"
#include "json.h"
#include "packages.h"

#define CLPD_INGEST_QUEUE_MAX 1024 /* queued facts */
#define CLPD_INGEST_MESSAGE_MAX 256 /* rejection message buffer, NUL included */

/* JSON limits of a fact document: max_bytes 65536, max_depth 4 (fact,
 * slots, multislot array, {"sym":...} inside it), max_string 8192,
 * max_elements 1024, max_nodes 4096. */
extern const clpd_json_limits clpd_ingest_limits;

/* Called with the queue lock held after a fact is queued; it must only wake
 * the engine thread (clpd_loop_notify), never call back into this module. */
typedef void clpd_ingest_notify_fn(void *ctx);
/* Called by clpd_ingest_drain for each queued fact that is rejected, with
 * the origin given to clpd_ingest_post and the rejection message. */
typedef void clpd_ingest_reject_fn(void *ctx, const void *origin,
                                   const char *message);

/* Copies json[0..len) into the queue when it parses as JSON within
 * clpd_ingest_limits (the template and slots are checked at drain), then
 * calls the notify function if one is set. origin is opaque and returned to
 * the reject function. Returns 0 when queued; otherwise -1 with errno
 * EINVAL (json is NULL or not valid JSON), EAGAIN (CLPD_INGEST_QUEUE_MAX
 * facts queued) or ENOMEM, and the queue unchanged. Any thread. */
int clpd_ingest_post(const void *origin, const char *json, size_t len);
/* Facts currently queued. Any thread. */
size_t clpd_ingest_queue_length(void);
/* Sets (fn != NULL) or clears the notify function; when set while facts
 * are queued, it is called once at once. Set it after the event loop opens
 * and clear it before the loop closes. Any thread. */
void clpd_ingest_set_notify(clpd_ingest_notify_fn *fn, void *ctx);
/* Frees every queued fact without asserting it (teardown). Any thread. */
void clpd_ingest_clear(void);

/* Validates one fact document against env and the ingest lists of p
 * without asserting it. Returns 0 when it would be asserted; otherwise -1
 * with the exact rejection message (docs/ingest.md) in msg (size n, at
 * least 1). The admin socket's synchronous inject uses it. */
int clpd_ingest_check(Environment *env, const clpd_packages *p,
                      const char *json, size_t len, char *msg, size_t n);
/* Validates and asserts one fact document; same results as
 * clpd_ingest_check. */
int clpd_ingest_assert(Environment *env, const clpd_packages *p,
                       const char *json, size_t len, char *msg, size_t n);
/* Asserts the facts queued when it is called, in posting order; each one
 * rejected is passed to reject (if not NULL). Leaves the current module
 * unchanged. Returns the number of facts asserted. Engine thread, outside
 * Run. */
size_t clpd_ingest_drain(Environment *env, const clpd_packages *p,
                         clpd_ingest_reject_fn *reject, void *ctx);

#endif
