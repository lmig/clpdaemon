#include "log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* NULL = stderr. Atomic because the fd capture reader thread may report
 * while the main thread switches the stream. */
static FILE *current;

FILE *clpd_log(void) {
  FILE *f = __atomic_load_n(&current, __ATOMIC_ACQUIRE);
  return f != NULL ? f : stderr;
}

FILE *clpd_log_set(FILE *f) {
  FILE *old = __atomic_exchange_n(&current, f, __ATOMIC_ACQ_REL);
  return old != NULL ? old : stderr;
}

void clpd_log_die_errno(void) {
  int e = errno;
  FILE *f = clpd_log();
  fprintf(f, "clpdaemon: %s\n", strerror(e));
  fflush(f);
  exit(1);
}
