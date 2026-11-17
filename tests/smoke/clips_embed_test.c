/*
 * Smoke test: the vendored CLIPS 6.4.2 core can be embedded through its
 * public C API (create environment, load a rule file, assert, run, query).
 *
 * Usage: clips_embed_test <path/to/smoke.clp>
 */
#include <stdio.h>
#include <string.h>

#include "clips.h"

static int failures = 0;

static void check(int condition, const char *what)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

int main(int argc, char *argv[])
{
    Environment *env;
    CLIPSValue result;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <fixture.clp>\n", argv[0]);
        return 2;
    }

    env = CreateEnvironment();
    if (env == NULL) {
        fprintf(stderr, "FAIL: CreateEnvironment returned NULL\n");
        return 1;
    }

    check(Load(env, argv[1]) == LE_NO_ERROR, "Load(fixture) returns LE_NO_ERROR");
    check(AssertString(env, "(ping (id p-1))") != NULL,
          "AssertString((ping (id p-1))) returns a fact");
    check(Run(env, -1) == 1, "Run(env, -1) fires exactly 1 rule");

    check(Eval(env, "(length$ (find-all-facts ((?f pong)) TRUE))", &result) == EE_NO_ERROR,
          "Eval(count pong facts) succeeds");
    check(result.header->type == INTEGER_TYPE && result.integerValue->contents == 1,
          "exactly one pong fact exists");

    check(Eval(env, "(fact-slot-value (nth$ 1 (find-all-facts ((?f pong)) TRUE)) status)",
               &result) == EE_NO_ERROR,
          "Eval(pong status) succeeds");
    check(result.header->type == SYMBOL_TYPE &&
              strcmp(result.lexemeValue->contents, "ok") == 0,
          "pong status is the symbol ok");

    check(DestroyEnvironment(env), "DestroyEnvironment returns true");

    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("clips_embed_test: all checks passed\n");
    return 0;
}
