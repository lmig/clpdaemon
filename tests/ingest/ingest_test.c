/* Unit tests for the ingest pipeline (docs/ingest.md, T7b): fact JSON
 * against a real CLIPS environment with a package that lists `ingest`
 * templates, and the bounded queue. */
#define _POSIX_C_SOURCE 200809L

#include "ingest.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../common/expect.h"

static char root[] = "/tmp/clpd_ingest_test.XXXXXX";
static Environment *env;
static clpd_packages pkgs;

static void write_file(const char *path, const char *text) {
  FILE *f = fopen(path, "w");
  if (f == NULL || fputs(text, f) < 0 || fclose(f) != 0) {
    perror("ingest_test");
    exit(2);
  }
}

static const char templates[] =
    "(deftemplate order (slot wf-id) (slot qty (type INTEGER)) (slot price)\n"
    "  (slot kind) (multislot tags (cardinality 0 4))\n"
    "  (slot status (default new)))\n"
    "(deftemplate plain (slot wf-id (default ?NONE))\n"
    "  (slot n (type INTEGER) (range 0 10))\n"
    "  (slot color (allowed-symbols red green))\n"
    "  (multislot nums (type INTEGER) (range 0 9)))\n"
    "(deftemplate hidden (slot wf-id))\n";

static void setup(void) {
  char dir[sizeof root + 8], path[sizeof root + 32];
  clpd_list dirs;
  char *item;
  if (mkdtemp(root) == NULL) {
    perror("ingest_test");
    exit(2);
  }
  snprintf(dir, sizeof dir, "%s/pkg", root);
  mkdir(dir, 0700);
  snprintf(path, sizeof path, "%s/package.conf", dir);
  write_file(path, "definition = shop\nversion = 1\nfiles = t.clp\n"
                   "ingest = order, plain\n");
  snprintf(path, sizeof path, "%s/t.clp", dir);
  write_file(path, templates);
  env = CreateEnvironment();
  item = dir;
  dirs.items = &item;
  dirs.count = 1;
  if (env == NULL || clpd_packages_load(env, &dirs, &pkgs, stderr) != 0) {
    fputs("ingest_test: cannot load the test package\n", stderr);
    exit(2);
  }
  snprintf(path, sizeof path, "%s/package.conf", dir);
  remove(path);
  snprintf(path, sizeof path, "%s/t.clp", dir);
  remove(path);
  rmdir(dir);
  rmdir(root);
}

static Deftemplate *tpl(const char *name) {
  char q[64];
  snprintf(q, sizeof q, "shop@1::%s", name);
  return FindDeftemplate(env, q);
}

static size_t fact_count(void) {
  size_t n = 0;
  Fact *f;
  for (f = GetNextFact(env, NULL); f != NULL; f = GetNextFact(env, f)) n++;
  return n;
}

/* The last fact of the template, or NULL. */
static Fact *last_fact(const char *name) {
  Fact *f, *last = NULL;
  for (f = GetNextFactInTemplate(tpl(name), NULL); f != NULL;
       f = GetNextFactInTemplate(tpl(name), f))
    last = f;
  return last;
}

static int slot_is(Fact *f, const char *slot, unsigned short type,
                   const char *text) {
  CLIPSValue v;
  if (f == NULL || GetFactSlot(f, slot, &v) != GSE_NO_ERROR ||
      v.header->type != type)
    return 0;
  switch (type) {
  case STRING_TYPE:
  case SYMBOL_TYPE: return strcmp(v.lexemeValue->contents, text) == 0;
  case INTEGER_TYPE: return v.integerValue->contents == atoll(text);
  case FLOAT_TYPE: return v.floatValue->contents == atof(text);
  default: return 0;
  }
}

static int field_is(const CLIPSValue *v, unsigned short type,
                    const char *text) {
  if (v->header->type != type) return 0;
  switch (type) {
  case STRING_TYPE:
  case SYMBOL_TYPE: return strcmp(v->lexemeValue->contents, text) == 0;
  case INTEGER_TYPE: return v->integerValue->contents == atoll(text);
  case FLOAT_TYPE: return v->floatValue->contents == atof(text);
  default: return 0;
  }
}

static int post(const char *json) {
  return clpd_ingest_post(NULL, json, strlen(json));
}

/* Reject callback: records the last origin and message. */
static const void *rejected_origin;
static char rejected[CLPD_INGEST_MESSAGE_MAX];
static int rejections;

static void on_reject(void *ctx, const void *origin, const char *message) {
  (void)ctx;
  rejected_origin = origin;
  snprintf(rejected, sizeof rejected, "%s", message);
  rejections++;
}

static void test_assert_all_types(void) {
  size_t before = fact_count();
  Fact *f;
  CLIPSValue v;
  EXPECT(post("{\"template\":\"order\",\"slots\":{\"wf-id\":\"w1\",\"qty\":3,"
              "\"price\":2.5,\"kind\":{\"sym\":\"rush\"},"
              "\"tags\":[\"a\",{\"sym\":\"b\"},7,1e2]}}") == 0);
  EXPECT(clpd_ingest_queue_length() == 1);
  EXPECT(fact_count() == before); /* nothing asserted before the drain */
  EXPECT(clpd_ingest_drain(env, &pkgs, on_reject, NULL) == 1);
  EXPECT(clpd_ingest_queue_length() == 0 && rejections == 0);
  EXPECT(fact_count() == before + 1);
  f = last_fact("order");
  EXPECT(slot_is(f, "wf-id", STRING_TYPE, "w1"));
  EXPECT(slot_is(f, "qty", INTEGER_TYPE, "3"));
  EXPECT(slot_is(f, "price", FLOAT_TYPE, "2.5"));
  EXPECT(slot_is(f, "kind", SYMBOL_TYPE, "rush"));
  EXPECT(slot_is(f, "status", SYMBOL_TYPE, "new")); /* default */
  EXPECT(f != NULL && GetFactSlot(f, "tags", &v) == GSE_NO_ERROR &&
         v.header->type == MULTIFIELD_TYPE &&
         v.multifieldValue->length == 4 &&
         field_is(&v.multifieldValue->contents[0], STRING_TYPE, "a") &&
         field_is(&v.multifieldValue->contents[1], SYMBOL_TYPE, "b") &&
         field_is(&v.multifieldValue->contents[2], INTEGER_TYPE, "7") &&
         field_is(&v.multifieldValue->contents[3], FLOAT_TYPE, "100"));
  EXPECT(strcmp(DefmoduleName(GetCurrentModule(env)), "MAIN") == 0);
}

static void test_defaults(void) {
  size_t before = fact_count();
  Fact *f;
  CLIPSValue v;
  char msg[CLPD_INGEST_MESSAGE_MAX];
  const char *json = "{\"slots\":{\"qty\":-9},\"template\":\"order\"}";
  EXPECT(clpd_ingest_assert(env, &pkgs, json, strlen(json), msg,
                            sizeof msg) == 0);
  EXPECT(fact_count() == before + 1);
  f = last_fact("order");
  EXPECT(slot_is(f, "qty", INTEGER_TYPE, "-9"));
  EXPECT(slot_is(f, "wf-id", SYMBOL_TYPE, "nil"));
  EXPECT(slot_is(f, "status", SYMBOL_TYPE, "new"));
  EXPECT(f != NULL && GetFactSlot(f, "tags", &v) == GSE_NO_ERROR &&
         v.header->type == MULTIFIELD_TYPE && v.multifieldValue->length == 0);
  json = "{\"template\":\"plain\",\"slots\":{\"wf-id\":{\"sym\":\"p1\"},"
         "\"color\":{\"sym\":\"red\"},\"n\":10,\"nums\":[0,9]}}";
  EXPECT(clpd_ingest_assert(env, &pkgs, json, strlen(json), msg,
                            sizeof msg) == 0);
  f = last_fact("plain");
  EXPECT(slot_is(f, "wf-id", SYMBOL_TYPE, "p1") &&
         slot_is(f, "color", SYMBOL_TYPE, "red") &&
         slot_is(f, "n", INTEGER_TYPE, "10"));
}

/* Check passes without asserting anything. */
static void test_check_does_not_assert(void) {
  size_t before = fact_count();
  char msg[CLPD_INGEST_MESSAGE_MAX] = "x";
  const char *json =
      "{\"template\":\"order\",\"slots\":{\"qty\":12345,\"tags\":[]}}";
  EXPECT(clpd_ingest_check(env, &pkgs, json, strlen(json), msg,
                           sizeof msg) == 0);
  EXPECT(fact_count() == before);
  EXPECT(clpd_ingest_queue_length() == 0);
}

/* rejects <json> <message>: check, assert and drain all reject it with the
 * same message, assert nothing and leave the queue empty. */
static void rejects(const char *json, const char *want) {
  size_t before = fact_count();
  char msg[CLPD_INGEST_MESSAGE_MAX];
  int ok = 1;
  if (clpd_ingest_check(env, &pkgs, json, strlen(json), msg, sizeof msg) !=
          -1 ||
      strcmp(msg, want) != 0)
    ok = 0;
  if (ok && (clpd_ingest_assert(env, &pkgs, json, strlen(json), msg,
                                sizeof msg) != -1 ||
             strcmp(msg, want) != 0))
    ok = 0;
  if (ok && fact_count() != before) ok = 0;
  if (!ok) {
    failures++;
    fprintf(stderr, "FAIL rejects %s\n  got:  [%s]\n  want: [%s]\n", json,
            msg, want);
    return;
  }
  /* The same text through the queue, when it is JSON at all. */
  if (post(json) == 0) {
    int seen = rejections;
    static const int marker = 0;
    clpd_ingest_drain(env, &pkgs, on_reject, NULL);
    EXPECT(rejections == seen + 1 && strcmp(rejected, want) == 0);
    EXPECT(fact_count() == before && clpd_ingest_queue_length() == 0);
    EXPECT(clpd_ingest_post(&marker, json, strlen(json)) == 0);
    clpd_ingest_drain(env, &pkgs, on_reject, NULL);
    EXPECT(rejected_origin == &marker);
  }
}

static void test_rejections(void) {
  char long_key[200], want[200];
  rejects("[", "invalid JSON: offset 1: unexpected end of input");
  rejects("[]", "expected a JSON object");
  rejects("{\"slots\":{}}", "missing key 'template'");
  rejects("{\"template\":\"order\"}", "missing key 'slots'");
  rejects("{\"template\":\"order\",\"slots\":{},\"x\":1}", "unknown key 'x'");
  rejects("{\"x\":1}", "unknown key 'x'");
  rejects("{\"template\":1,\"slots\":{}}", "'template' must be a string");
  rejects("{\"template\":\"order\",\"slots\":[]}", "'slots' must be an object");
  rejects("{\"template\":\"hidden\",\"slots\":{}}",
          "template 'hidden' is not ingestible");
  rejects("{\"template\":\"shop@1::order\",\"slots\":{}}",
          "template 'shop@1::order' is not ingestible");
  rejects("{\"template\":\"order\",\"slots\":{\"nope\":1}}",
          "template 'order' has no slot 'nope'");
  rejects("{\"template\":\"order\",\"slots\":{\"qty\":[1]}}",
          "slot 'qty': array for a single-field slot");
  rejects("{\"template\":\"order\",\"slots\":{\"tags\":\"a\"}}",
          "slot 'tags': expected an array for a multislot");
  rejects("{\"template\":\"order\",\"slots\":{\"price\":true}}",
          "slot 'price': booleans are not allowed");
  rejects("{\"template\":\"order\",\"slots\":{\"tags\":[false]}}",
          "slot 'tags': booleans are not allowed");
  rejects("{\"template\":\"order\",\"slots\":{\"price\":null}}",
          "slot 'price': null is not allowed");
  rejects("{\"template\":\"order\",\"slots\":{\"tags\":[null]}}",
          "slot 'tags': null is not allowed");
  rejects("{\"template\":\"order\",\"slots\":{\"tags\":[[1]]}}",
          "slot 'tags': nested arrays are not allowed");
  rejects("{\"template\":\"order\",\"slots\":{\"price\":{\"a\":1}}}",
          "slot 'price': expected {\"sym\":\"<symbol>\"} for an object");
  rejects("{\"template\":\"order\",\"slots\":{\"price\":{\"sym\":1}}}",
          "slot 'price': expected {\"sym\":\"<symbol>\"} for an object");
  rejects("{\"template\":\"order\",\"slots\":{\"price\":{\"sym\":\"a\","
          "\"x\":1}}}",
          "slot 'price': expected {\"sym\":\"<symbol>\"} for an object");
  rejects("{\"template\":\"order\",\"slots\":{\"price\":{}}}",
          "slot 'price': expected {\"sym\":\"<symbol>\"} for an object");
  rejects("{\"template\":\"order\",\"slots\":{\"kind\":{\"sym\":\"a b\"}}}",
          "slot 'kind': invalid symbol");
  rejects("{\"template\":\"order\",\"slots\":{\"kind\":{\"sym\":\"\"}}}",
          "slot 'kind': invalid symbol");
  rejects("{\"template\":\"order\",\"slots\":{\"kind\":{\"sym\":\"?x\"}}}",
          "slot 'kind': invalid symbol");
  rejects("{\"template\":\"order\",\"slots\":{\"kind\":{\"sym\":\"$?x\"}}}",
          "slot 'kind': invalid symbol");
  rejects("{\"template\":\"order\",\"slots\":{\"kind\":{\"sym\":\"(x\"}}}",
          "slot 'kind': invalid symbol");
  rejects("{\"template\":\"order\",\"slots\":{\"qty\":\"3\"}}",
          "slot 'qty': value type not allowed by the slot");
  rejects("{\"template\":\"order\",\"slots\":{\"qty\":3.0}}",
          "slot 'qty': value type not allowed by the slot");
  rejects("{\"template\":\"plain\",\"slots\":{\"wf-id\":1,\"n\":11}}",
          "slot 'n': value outside the slot's range");
  rejects("{\"template\":\"plain\",\"slots\":{\"wf-id\":1,\"color\":"
          "{\"sym\":\"blue\"}}}",
          "slot 'color': value not among the slot's allowed values");
  rejects("{\"template\":\"order\",\"slots\":{\"tags\":[1,2,3,4,5]}}",
          "slot 'tags': value count outside the slot's cardinality");
  rejects("{\"template\":\"plain\",\"slots\":{\"wf-id\":1,\"nums\":"
          "[1,\"x\"]}}",
          "slot 'nums': value type not allowed by the slot");
  rejects("{\"template\":\"plain\",\"slots\":{\"wf-id\":1,\"nums\":"
          "[1,10]}}",
          "slot 'nums': value outside the slot's range");
  rejects("{\"template\":\"plain\",\"slots\":{\"n\":1}}",
          "slot 'wf-id' requires a value");
  /* The first error in document order wins. */
  rejects("{\"template\":\"order\",\"slots\":{\"qty\":true,\"nope\":1}}",
          "slot 'qty': booleans are not allowed");
  /* Names in messages: control characters become '?', at most 64 bytes. */
  rejects("{\"template\":\"order\",\"slots\":{},\"a\\nb\":1}",
          "unknown key 'a?b'");
  memset(long_key, 'k', 100);
  snprintf(long_key + 100, sizeof long_key - 100, "%s", "");
  {
    char json[300];
    snprintf(json, sizeof json, "{\"template\":\"%s\",\"slots\":{}}",
             long_key);
    snprintf(want, sizeof want, "template '%.64s...' is not ingestible",
             long_key);
    rejects(json, want);
  }
  /* Limits: depth 4 ({"sym":...} inside a multislot) is the maximum. */
  rejects("{\"template\":\"order\",\"slots\":{\"tags\":[[[1]]]}}",
          "invalid JSON: offset 38: nesting exceeds max_depth (4)");
}

static void test_post_errors(void) {
  char *big;
  size_t len = 65537;
  errno = 0;
  EXPECT(clpd_ingest_post(NULL, NULL, 0) == -1 && errno == EINVAL);
  errno = 0;
  EXPECT(post("(order (wf-id w1))") == -1 && errno == EINVAL);
  errno = 0;
  EXPECT(post("") == -1 && errno == EINVAL);
  EXPECT(clpd_ingest_queue_length() == 0);
  big = malloc(len);
  if (big == NULL) exit(2);
  memset(big, ' ', len);
  big[0] = big[1] = '{';
  big[len - 1] = '}';
  errno = 0;
  EXPECT(clpd_ingest_post(NULL, big, len) == -1 && errno == EINVAL);
  EXPECT(clpd_ingest_post(NULL, big + 1, len - 1) == 0); /* 65536 bytes */
  EXPECT(clpd_ingest_queue_length() == 1);
  EXPECT(clpd_ingest_clear() == 1); /* the number of facts dropped */
  EXPECT(clpd_ingest_queue_length() == 0);
  EXPECT(clpd_ingest_clear() == 0);
  free(big);
  /* The text is copied: the caller's buffer may change after the post. */
  {
    char text[] = "{\"template\":\"order\",\"slots\":{\"qty\":77}}";
    size_t before = fact_count();
    EXPECT(post(text) == 0);
    memset(text, 'x', sizeof text - 1);
    EXPECT(clpd_ingest_drain(env, &pkgs, on_reject, NULL) == 1);
    EXPECT(fact_count() == before + 1 &&
           slot_is(last_fact("order"), "qty", INTEGER_TYPE, "77"));
  }
}

static void test_full_queue(void) {
  char json[96];
  size_t i, before = fact_count();
  int ok = 1;
  for (i = 0; i < CLPD_INGEST_QUEUE_MAX; i++) {
    snprintf(json, sizeof json,
             "{\"template\":\"order\",\"slots\":{\"qty\":%zu}}", 1000 + i);
    if (post(json) != 0) ok = 0;
  }
  EXPECT(ok);
  EXPECT(clpd_ingest_queue_length() == CLPD_INGEST_QUEUE_MAX);
  errno = 0;
  EXPECT(post("{\"template\":\"order\",\"slots\":{}}") == -1 &&
         errno == EAGAIN);
  errno = 0;
  EXPECT(post("nope") == -1 && errno == EINVAL);
  EXPECT(clpd_ingest_queue_length() == CLPD_INGEST_QUEUE_MAX);
  EXPECT(clpd_ingest_drain(env, &pkgs, on_reject, NULL) ==
         CLPD_INGEST_QUEUE_MAX);
  EXPECT(fact_count() == before + CLPD_INGEST_QUEUE_MAX);
  EXPECT(clpd_ingest_queue_length() == 0);
  /* Posting order is kept. */
  EXPECT(slot_is(last_fact("order"), "qty", INTEGER_TYPE, "2023"));
  EXPECT(post("{\"template\":\"order\",\"slots\":{}}") == 0);
  clpd_ingest_clear();
}

static int notified;
static void on_notify(void *ctx) { (*(int *)ctx)++; }

static void test_notify(void) {
  notified = 0;
  EXPECT(post("{\"template\":\"order\",\"slots\":{}}") == 0);
  EXPECT(notified == 0);
  /* Facts queued before the notify function is set wake it once. */
  clpd_ingest_set_notify(on_notify, &notified);
  EXPECT(notified == 1);
  EXPECT(post("{\"template\":\"order\",\"slots\":{}}") == 0);
  EXPECT(notified == 2);
  errno = 0;
  EXPECT(post("{") == -1 && notified == 2);
  clpd_ingest_clear();
  clpd_ingest_set_notify(on_notify, &notified);
  EXPECT(notified == 2); /* nothing queued */
  clpd_ingest_set_notify(NULL, NULL);
  EXPECT(post("{\"template\":\"order\",\"slots\":{}}") == 0);
  EXPECT(notified == 2);
  clpd_ingest_clear();
}

#define THREADS 4
#define PER_THREAD 200

static void *poster(void *arg) {
  int i, *fails = arg;
  for (i = 0; i < PER_THREAD; i++)
    if (post("{\"template\":\"order\",\"slots\":{\"qty\":1}}") != 0)
      (*fails)++;
  return NULL;
}

static void test_threads(void) {
  pthread_t t[THREADS];
  int fails[THREADS] = {0}, i;
  notified = 0;
  clpd_ingest_set_notify(on_notify, &notified);
  for (i = 0; i < THREADS; i++)
    if (pthread_create(&t[i], NULL, poster, &fails[i]) != 0) exit(2);
  for (i = 0; i < THREADS; i++) pthread_join(t[i], NULL);
  clpd_ingest_set_notify(NULL, NULL);
  for (i = 0; i < THREADS; i++) EXPECT(fails[i] == 0);
  EXPECT(clpd_ingest_queue_length() == THREADS * PER_THREAD);
  EXPECT(notified == THREADS * PER_THREAD);
  /* Equal facts: CLIPS keeps one, but the drain counts every fact CLIPS
   * accepted, so all 800 count. */
  EXPECT(clpd_ingest_drain(env, &pkgs, on_reject, NULL) ==
         THREADS * PER_THREAD);
  EXPECT(clpd_ingest_queue_length() == 0);
}

/* sym_accepted <symbol-json> <symbol>: {"sym":...} in slot kind asserts a
 * SYMBOL with exactly that text. */
static void sym_accepted(const char *sym, const char *want) {
  char json[160], msg[CLPD_INGEST_MESSAGE_MAX];
  size_t before = fact_count();
  snprintf(json, sizeof json,
           "{\"template\":\"order\",\"slots\":{\"kind\":{\"sym\":%s}}}", sym);
  if (clpd_ingest_assert(env, &pkgs, json, strlen(json), msg, sizeof msg) !=
          0 ||
      fact_count() != before + 1 ||
      !slot_is(last_fact("order"), "kind", SYMBOL_TYPE, want)) {
    failures++;
    fprintf(stderr, "FAIL symbol %s accepted: [%s]\n", sym, msg);
  }
}

static void test_symbols(void) {
  static const char *const bad[] = {
      "\"\"",    "\" a\"",  "\"a\\tb\"", "\"?x\"",  "\"$?x\"", "\"?\"",
      "\"$?\"",  "\"a(b\"", "\"a)b\"",   "\"a\\\"b\"", "\"a&b\"", "\"a|b\"",
      "\"<-\"",  "\"~a\"",  "\"a;b\"",   "\"\\u007f\""};
  char json[160];
  size_t i;
  for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    snprintf(json, sizeof json,
             "{\"template\":\"order\",\"slots\":{\"kind\":{\"sym\":%s}}}",
             bad[i]);
    rejects(json, "slot 'kind': invalid symbol");
  }
  sym_accepted("\"a?b\"", "a?b");
  sym_accepted("\"$x\"", "$x");
  sym_accepted("\"x$?\"", "x$?");
  sym_accepted("\"nil\"", "nil");
  sym_accepted("\"=>\"", "=>");
  /* Number-like text is a SYMBOL too (it prints like a number). */
  sym_accepted("\"3\"", "3");
}

static void test_constraint_edges(void) {
  char msg[CLPD_INGEST_MESSAGE_MAX], name[80], json[200], want[200];
  const char *ok =
      "{\"template\":\"plain\",\"slots\":{\"wf-id\":1,\"n\":0,\"nums\":[9,0]}}";
  EXPECT(clpd_ingest_assert(env, &pkgs, ok, strlen(ok), msg, sizeof msg) ==
         0);
  EXPECT(slot_is(last_fact("plain"), "n", INTEGER_TYPE, "0"));
  rejects("{\"template\":\"plain\",\"slots\":{\"wf-id\":1,\"n\":-1}}",
          "slot 'n': value outside the slot's range");
  rejects("{\"template\":\"plain\",\"slots\":{\"wf-id\":1,\"nums\":[10]}}",
          "slot 'nums': value outside the slot's range");
  rejects("{\"template\":\"plain\",\"slots\":{\"wf-id\":1,\"nums\":"
          "[-1,5]}}",
          "slot 'nums': value outside the slot's range");
  rejects("{\"template\":\"plain\",\"slots\":{\"wf-id\":1,\"nums\":"
          "[1.5,2]}}",
          "slot 'nums': value type not allowed by the slot");
  rejects("{\"template\":\"plain\",\"slots\":{\"wf-id\":1,\"nums\":"
          "[2,{\"sym\":\"x\"}]}}",
          "slot 'nums': value type not allowed by the slot");
  /* A 64-byte template name is shown in full. */
  memset(name, 't', 64);
  name[64] = '\0';
  snprintf(json, sizeof json, "{\"template\":\"%s\",\"slots\":{}}", name);
  snprintf(want, sizeof want, "template '%s' is not ingestible", name);
  rejects(json, want);
  /* 63 bytes and a 2-byte character: cut before the character, not in it. */
  memset(name, 't', 63);
  snprintf(name + 63, sizeof name - 63, "%s", "\xc3\xa9");
  snprintf(json, sizeof json, "{\"template\":\"%s\",\"slots\":{}}", name);
  snprintf(want, sizeof want, "template '%.63s...' is not ingestible", name);
  rejects(json, want);
}

/* Reject callback that posts a valid fact from inside the drain. */
static int reposted;
static void post_on_reject(void *ctx, const void *origin,
                           const char *message) {
  on_reject(ctx, origin, message);
  reposted = post("{\"template\":\"order\",\"slots\":{\"qty\":555}}") == 0;
}

static void test_post_during_drain(void) {
  Defmodule *main_module = GetCurrentModule(env);
  Defmodule *pkg_module = FindDefmodule(env, "shop@1");
  size_t before = fact_count();
  int seen = rejections;
  EXPECT(post("{\"template\":\"order\",\"slots\":{\"nope\":1}}") == 0);
  SetCurrentModule(env, pkg_module);
  EXPECT(clpd_ingest_drain(env, &pkgs, post_on_reject, NULL) == 0);
  EXPECT(GetCurrentModule(env) == pkg_module); /* unchanged */
  EXPECT(reposted && rejections == seen + 1);
  /* The fact posted during the drain waits for the next one. */
  EXPECT(clpd_ingest_queue_length() == 1 && fact_count() == before);
  SetCurrentModule(env, main_module);
  EXPECT(clpd_ingest_drain(env, &pkgs, post_on_reject, NULL) == 1);
  EXPECT(GetCurrentModule(env) == main_module);
  EXPECT(clpd_ingest_queue_length() == 0 && fact_count() == before + 1 &&
         slot_is(last_fact("order"), "qty", INTEGER_TYPE, "555"));
}

int main(void) {
  setup();
  test_assert_all_types();
  test_defaults();
  test_check_does_not_assert();
  test_rejections();
  test_symbols();
  test_constraint_edges();
  test_post_during_drain();
  test_post_errors();
  test_full_queue();
  test_notify();
  test_threads();
  clpd_ingest_clear();
  clpd_packages_free(&pkgs);
  DestroyEnvironment(env);
  if (failures != 0) {
    fprintf(stderr, "%d ingest checks failed\n", failures);
    return 1;
  }
  puts("ingest checks passed");
  return 0;
}
