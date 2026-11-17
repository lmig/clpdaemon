/* CLIPS environment wrapper: one environment, cycles with a rule limit,
 * and a halt check for pending stop signals (FR-4, FR-5). */
#ifndef CLPD_ENGINE_H
#define CLPD_ENGINE_H

#include "clips.h"

typedef struct clpd_engine clpd_engine;

/* Creates the environment and installs the halt check. NULL on failure. */
clpd_engine *clpd_engine_create(void);
Environment *clpd_engine_env(clpd_engine *e);
/* Runs at most max_rules firings (-1 = unlimited). Stops early when SIGTERM
 * or SIGINT is pending; the signal stays pending. Returns the firings. */
long long clpd_engine_run_cycle(clpd_engine *e, int max_rules);
/* 1 when the last cycle was stopped by a pending stop signal. */
int clpd_engine_halted(const clpd_engine *e);
void clpd_engine_destroy(clpd_engine *e);

#endif
