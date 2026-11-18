/* Unit tests for the strict JSON parser (src/json.h, T7a). Every input is
 * copied into a heap buffer of exactly its length, without a terminator,
 * so a read past the length is caught by ASan. */
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/expect.h"

static const clpd_json_limits big = {1u << 20, 600, 1u << 16, 1u << 16,
                                     1u << 20};

static clpd_json *parse_n(const char *text, size_t len,
                          const clpd_json_limits *lim, clpd_json_error *err) {
  char *buf = malloc(len != 0 ? len : 1);
  clpd_json *v;

  if (buf == NULL) exit(2);
  memcpy(buf, text, len);
  v = clpd_json_parse(buf, len, lim, err);
  free(buf);
  return v;
}

/* Parses with limits lim; NULL and a printed failure when rejected. */
static clpd_json *accept_l(int line, const char *text, size_t len,
                           const clpd_json_limits *lim) {
  clpd_json_error err;
  clpd_json *v = parse_n(text, len, lim, &err);

  if (v == NULL) {
    failures++;
    fprintf(stderr, "FAIL json_test.c:%d: rejected: %s\n", line,
            err.message);
  }
  return v;
}

/* Expects rejection with exactly msg ("offset <n>: <text>") and err.offset
 * equal to <n>. */
static void reject_l(int line, const char *text, size_t len,
                     const clpd_json_limits *lim, const char *msg) {
  clpd_json_error err;
  clpd_json *v;
  size_t want = (size_t)-1;

  memset(&err, 0, sizeof err);
  v = parse_n(text, len, lim, &err);
  sscanf(msg, "offset %zu", &want);
  if (v != NULL || strcmp(err.message, msg) != 0 || err.offset != want) {
    failures++;
    fprintf(stderr, "FAIL json_test.c:%d: want '%s', got %s '%s' (%zu)\n",
            line, msg, v != NULL ? "accepted" : "rejected", err.message,
            err.offset);
  }
  clpd_json_free(v);
}

#define ACCEPT(t) accept_l(__LINE__, t, strlen(t), &big)
#define ACCEPT_L(t, lim) accept_l(__LINE__, t, strlen(t), lim)
#define REJECT(t, msg) reject_l(__LINE__, t, strlen(t), &big, msg)
#define REJECT_N(t, n, msg) reject_l(__LINE__, t, n, &big, msg)
#define REJECT_L(t, lim, msg) reject_l(__LINE__, t, strlen(t), lim, msg)

/* Accepts t and expects one value of type ty; returns it for checks. */
static clpd_json *one(int line, const char *t, clpd_json_type ty) {
  clpd_json *v = accept_l(line, t, strlen(t), &big);

  if (v != NULL && v->type != ty) {
    failures++;
    fprintf(stderr, "FAIL json_test.c:%d: '%s' type %d, want %d\n", line, t,
            (int)v->type, (int)ty);
    clpd_json_free(v);
    return NULL;
  }
  return v;
}
#define ONE(t, ty) one(__LINE__, t, ty)

static void expect_int(int line, const char *t, int64_t want) {
  clpd_json *v = one(line, t, CLPD_JSON_INT);

  if (v != NULL && v->u.i != want) {
    failures++;
    fprintf(stderr, "FAIL json_test.c:%d: '%s' value\n", line, t);
  }
  clpd_json_free(v);
}

static void expect_float(int line, const char *t, double want) {
  clpd_json *v = one(line, t, CLPD_JSON_FLOAT);

  if (v != NULL && v->u.f != want) {
    failures++;
    fprintf(stderr, "FAIL json_test.c:%d: '%s' value %g\n", line, t, v->u.f);
  }
  clpd_json_free(v);
}

static void expect_str(int line, const char *t, const char *want,
                       size_t wlen) {
  clpd_json *v = one(line, t, CLPD_JSON_STRING);

  if (v != NULL && (v->u.str.len != wlen ||
                    memcmp(v->u.str.s, want, wlen) != 0 ||
                    v->u.str.s[wlen] != '\0')) {
    failures++;
    fprintf(stderr, "FAIL json_test.c:%d: '%s' decoded value\n", line, t);
  }
  clpd_json_free(v);
}
#define INT(t, w) expect_int(__LINE__, t, w)
#define FLT(t, w) expect_float(__LINE__, t, w)
#define STR(t, w) expect_str(__LINE__, t, w, sizeof w - 1)

static void test_scalars(void) {
  clpd_json_free(ONE("null", CLPD_JSON_NULL));
  clpd_json_free(ONE("true", CLPD_JSON_TRUE));
  clpd_json_free(ONE("false", CLPD_JSON_FALSE));
  clpd_json_free(ONE(" \t\r\n null \t\r\n", CLPD_JSON_NULL));
  INT("0", 0);
  INT("-0", 0);
  INT("42", 42);
  INT("-17", -17);
  INT("9223372036854775807", INT64_MAX);
  INT("-9223372036854775808", INT64_MIN);
  FLT("1.5", 1.5);
  FLT("-0.25", -0.25);
  FLT("0.0", 0.0);
  FLT("1e3", 1000.0);
  FLT("1E+2", 100.0);
  FLT("-2.5e-3", -2.5e-3);
  FLT("1e-400", 0.0); /* underflow is finite */
  FLT("1.7976931348623157e308", 1.7976931348623157e308);
}

static void test_long_number(void) {
  char buf[1200];

  memset(buf, '0', sizeof buf - 1);
  buf[0] = '1';
  buf[1] = '.';
  buf[sizeof buf - 2] = '1';
  buf[sizeof buf - 1] = '\0';
  FLT(buf, 1.0); /* token longer than any stack copy */
}

static void test_strings(void) {
  STR("\"\"", "");
  STR("\"abc\"", "abc");
  STR("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"", "\"\\/\b\f\n\r\t");
  STR("\"\\u0041\\u00e9\\u20AC\"", "A\xC3\xA9\xE2\x82\xAC");
  STR("\"\\uD83D\\uDE00\"", "\xF0\x9F\x98\x80");
  STR("\"\\udbff\\udfff\"", "\xF4\x8F\xBF\xBF");
  STR("\"\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\"",
      "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");
  STR("\"\xF4\x8F\xBF\xBF\xED\x9F\xBF\xEE\x80\x80\x7F\"",
      "\xF4\x8F\xBF\xBF\xED\x9F\xBF\xEE\x80\x80\x7F");
}

static void test_containers(void) {
  const char *fact =
      "{\"template\":\"order\",\"slots\":{\"wf-id\":\"w1\",\"qty\":3,"
      "\"tags\":[\"a\",\"b\"],\"s\":{\"sym\":\"x\"}}}";
  clpd_json *v = ONE("[]", CLPD_JSON_ARRAY);
  const clpd_json *slots, *m;

  EXPECT(v != NULL && v->u.arr.count == 0);
  clpd_json_free(v);
  v = ONE("{}", CLPD_JSON_OBJECT);
  EXPECT(v != NULL && v->u.obj.count == 0);
  EXPECT(clpd_json_get(v, "a") == NULL);
  clpd_json_free(v);

  v = ONE(" [ 1 , \"a\" , [ true ] , { } , null ] ", CLPD_JSON_ARRAY);
  if (v != NULL) {
    EXPECT(v->u.arr.count == 5);
    EXPECT(v->u.arr.items[0].type == CLPD_JSON_INT);
    EXPECT(v->u.arr.items[1].type == CLPD_JSON_STRING);
    EXPECT(v->u.arr.items[2].type == CLPD_JSON_ARRAY &&
           v->u.arr.items[2].u.arr.count == 1 &&
           v->u.arr.items[2].u.arr.items[0].type == CLPD_JSON_TRUE);
    EXPECT(v->u.arr.items[3].type == CLPD_JSON_OBJECT);
    EXPECT(v->u.arr.items[4].type == CLPD_JSON_NULL);
    EXPECT(clpd_json_get(v, "a") == NULL); /* not an object */
  }
  clpd_json_free(v);

  v = ONE(fact, CLPD_JSON_OBJECT);
  if (v != NULL) {
    EXPECT(v->u.obj.count == 2);
    EXPECT(strcmp(v->u.obj.members[0].key, "template") == 0);
    EXPECT(v->u.obj.members[0].key_len == 8);
    m = clpd_json_get(v, "template");
    EXPECT(m != NULL && m->type == CLPD_JSON_STRING &&
           strcmp(m->u.str.s, "order") == 0);
    slots = clpd_json_get(v, "slots");
    EXPECT(slots != NULL && slots->type == CLPD_JSON_OBJECT &&
           slots->u.obj.count == 4);
    EXPECT(strcmp(slots->u.obj.members[1].key, "qty") == 0);
    m = clpd_json_get(slots, "wf-id");
    EXPECT(m != NULL && m->type == CLPD_JSON_STRING && m->u.str.len == 2);
    m = clpd_json_get(slots, "qty");
    EXPECT(m != NULL && m->type == CLPD_JSON_INT && m->u.i == 3);
    m = clpd_json_get(slots, "tags");
    EXPECT(m != NULL && m->type == CLPD_JSON_ARRAY && m->u.arr.count == 2 &&
           strcmp(m->u.arr.items[1].u.str.s, "b") == 0);
    m = clpd_json_get(clpd_json_get(slots, "s"), "sym");
    EXPECT(m != NULL && strcmp(m->u.str.s, "x") == 0);
    EXPECT(clpd_json_get(slots, "wf") == NULL);
    EXPECT(clpd_json_get(slots, "wf-idx") == NULL);
  }
  clpd_json_free(v);

  v = ONE("{\"a\\u0062\":1,\"ab\\u00e9\":2}", CLPD_JSON_OBJECT);
  m = clpd_json_get(v, "ab");
  EXPECT(m != NULL && m->u.i == 1);
  m = clpd_json_get(v, "ab\xC3\xA9");
  EXPECT(m != NULL && m->u.i == 2);
  clpd_json_free(v);
  clpd_json_free(NULL);
}

static void test_syntax_errors(void) {
  REJECT("", "offset 0: unexpected end of input");
  REJECT("   ", "offset 3: unexpected end of input");
  REJECT("\xEF\xBB\xBF{}", "offset 0: byte order mark not allowed");
  REJECT_N("[1,\0]", 5, "offset 3: NUL byte not allowed");
  REJECT_N("\"a\0\"", 4, "offset 2: NUL byte not allowed");
  REJECT_N("1\0", 2, "offset 1: NUL byte not allowed");
  REJECT_N("\0", 1, "offset 0: NUL byte not allowed");
  REJECT("1 2", "offset 2: unexpected data after value");
  REJECT("{}x", "offset 2: unexpected data after value");
  REJECT("[1]]", "offset 3: unexpected data after value");
  REJECT("x", "offset 0: expected value");
  REJECT("tru", "offset 0: invalid literal");
  REJECT("nul", "offset 0: invalid literal");
  REJECT("trUe", "offset 0: invalid literal");
  REJECT("falsy", "offset 0: invalid literal");
  REJECT("truex", "offset 4: unexpected data after value");
  REJECT("NaN", "offset 0: expected value");
  REJECT("Infinity", "offset 0: expected value");
  REJECT("-Infinity", "offset 1: expected digit");
  REJECT("[1,]", "offset 3: expected value");
  REJECT("[,1]", "offset 1: expected value");
  REJECT("[1 2]", "offset 3: expected ',' or ']'");
  REJECT("[1", "offset 2: unexpected end of input");
  REJECT("{\"a\" 1}", "offset 5: expected ':'");
  REJECT("{\"a\"", "offset 4: unexpected end of input");
  REJECT("{\"a\":1,}", "offset 7: expected string key");
  REJECT("{1:2}", "offset 1: expected string key");
  REJECT("{,}", "offset 1: expected string key");
  REJECT("{\"a\":1 \"b\":2}", "offset 7: expected ',' or '}'");
  REJECT("{\"a\":}", "offset 5: expected value");
  REJECT("{\"a\":1,\"a\":2}", "offset 7: duplicate key");
  REJECT("{\"a\":1,\"\\u0061\":2}", "offset 7: duplicate key");
  REJECT("{\"b\":{\"a\":1},\"b\":2}", "offset 13: duplicate key");
}

static void test_number_errors(void) {
  REJECT("01", "offset 0: leading zero not allowed");
  REJECT("-01", "offset 0: leading zero not allowed");
  REJECT("00", "offset 0: leading zero not allowed");
  REJECT("+1", "offset 0: expected value");
  REJECT(".5", "offset 0: expected value");
  REJECT("1.", "offset 2: unexpected end of input");
  REJECT("[1.]", "offset 3: expected digit");
  REJECT("1.e5", "offset 2: expected digit");
  REJECT("-", "offset 1: unexpected end of input");
  REJECT("-a", "offset 1: expected digit");
  REJECT("1e", "offset 2: unexpected end of input");
  REJECT("1e+", "offset 3: unexpected end of input");
  REJECT("1ex", "offset 2: expected digit");
  REJECT("0x1F", "offset 1: unexpected data after value");
  REJECT("9223372036854775808", "offset 0: integer out of range");
  REJECT("-9223372036854775809", "offset 0: integer out of range");
  REJECT("[99999999999999999999]", "offset 1: integer out of range");
  REJECT("1e309", "offset 0: number out of range");
  REJECT("[-1.8e308]", "offset 1: number out of range");
}

static void test_string_errors(void) {
  REJECT("\"abc", "offset 4: unexpected end of input");
  REJECT("\"a\tb\"", "offset 2: unescaped control character in string");
  REJECT("\"\x1F\"", "offset 1: unescaped control character in string");
  REJECT("\"\n\"", "offset 1: unescaped control character in string");
  REJECT("\"\\x\"", "offset 1: invalid escape");
  REJECT("\"\\U0041\"", "offset 1: invalid escape");
  REJECT("\"\\", "offset 2: unexpected end of input");
  REJECT("\"\\u12G4\"", "offset 1: invalid \\u escape");
  REJECT("\"\\u12", "offset 5: unexpected end of input");
  REJECT("\"\\uD800\"", "offset 1: invalid surrogate escape");
  REJECT("\"\\uDC00\"", "offset 1: invalid surrogate escape");
  REJECT("\"\\uDFFF\"", "offset 1: invalid surrogate escape");
  REJECT("\"x\\uD800x\"", "offset 2: invalid surrogate escape");
  REJECT("\"\\uD800\\u0041\"", "offset 1: invalid surrogate escape");
  REJECT("\"\\uD800\\uD800\"", "offset 1: invalid surrogate escape");
  REJECT("\"\\uD800\\n\"", "offset 1: invalid surrogate escape");
  REJECT("\"\\uD800", "offset 1: invalid surrogate escape");
  REJECT("\"\\uD800\\u12G4\"", "offset 7: invalid \\u escape");
  REJECT("\"\\u0000\"", "offset 1: \\u0000 not allowed");
  REJECT("{\"\\u0000\":1}", "offset 2: \\u0000 not allowed");
  /* overlong forms */
  REJECT("\"\xC0\x80\"", "offset 1: invalid UTF-8");
  REJECT("\"\xC1\xBF\"", "offset 1: invalid UTF-8");
  REJECT("\"\xE0\x80\x80\"", "offset 1: invalid UTF-8");
  REJECT("\"\xE0\x9F\xBF\"", "offset 1: invalid UTF-8");
  REJECT("\"\xF0\x80\x80\x80\"", "offset 1: invalid UTF-8");
  REJECT("\"\xF0\x8F\xBF\xBF\"", "offset 1: invalid UTF-8");
  /* surrogates, above U+10FFFF */
  REJECT("\"\xED\xA0\x80\"", "offset 1: invalid UTF-8");
  REJECT("\"\xED\xBF\xBF\"", "offset 1: invalid UTF-8");
  REJECT("\"\xF4\x90\x80\x80\"", "offset 1: invalid UTF-8");
  REJECT("\"\xF5\x80\x80\x80\"", "offset 1: invalid UTF-8");
  REJECT("\"\xFF\"", "offset 1: invalid UTF-8");
  /* stray continuation, truncated sequences */
  REJECT("\"a\x80\"", "offset 2: invalid UTF-8");
  REJECT("\"a\xC3\"", "offset 2: invalid UTF-8");
  REJECT("\"\xE2\x82\"", "offset 1: invalid UTF-8");
  REJECT("\"\xF0\x9F\x98x\"", "offset 1: invalid UTF-8");
  REJECT("\"\xC3", "offset 2: unexpected end of input");
  REJECT("\xC3\xA9", "offset 0: expected value");
}

static void test_limits(void) {
  clpd_json_limits l;
  clpd_json *v;
  char deep[2 * 513 + 1];
  size_t i;

  l = big;
  l.max_bytes = 3;
  clpd_json_free(ACCEPT_L("[1]", &l));
  REJECT_L("[1] ", &l, "offset 0: input exceeds max_bytes (3)");
  l.max_bytes = 0;
  REJECT_L("1", &l, "offset 0: input exceeds max_bytes (0)");

  l = big;
  l.max_depth = 3;
  clpd_json_free(ACCEPT_L("[[[]]]", &l));
  clpd_json_free(ACCEPT_L("{\"a\":[{}]}", &l));
  REJECT_L("[[[[]]]]", &l, "offset 3: nesting exceeds max_depth (3)");
  REJECT_L("{\"a\":[{\"b\":[]}]}", &l,
           "offset 11: nesting exceeds max_depth (3)");
  l.max_depth = 0;
  clpd_json_free(ACCEPT_L("1", &l));
  REJECT_L("[]", &l, "offset 0: nesting exceeds max_depth (0)");
  REJECT_L("{}", &l, "offset 0: nesting exceeds max_depth (0)");

  /* deep nesting at the limit, freed recursively */
  l = big;
  l.max_depth = 512;
  for (i = 0; i < 512; i++) {
    deep[i] = '[';
    deep[512 + i] = ']';
  }
  deep[1024] = '\0';
  v = ACCEPT_L(deep, &l);
  EXPECT(v != NULL && v->type == CLPD_JSON_ARRAY && v->u.arr.count == 1);
  clpd_json_free(v);
  for (i = 0; i < 513; i++) {
    deep[i] = '[';
    deep[513 + i] = ']';
  }
  deep[1026] = '\0';
  REJECT_L(deep, &l, "offset 512: nesting exceeds max_depth (512)");

  l = big;
  l.max_string = 3;
  clpd_json_free(ACCEPT_L("\"abc\"", &l));
  clpd_json_free(ACCEPT_L("\"\\u00e9a\"", &l)); /* 3 decoded bytes */
  clpd_json_free(ACCEPT_L("{\"abc\":\"xyz\"}", &l));
  REJECT_L("\"abcd\"", &l, "offset 0: string exceeds max_string (3)");
  REJECT_L("\"\\u00e9ab\"", &l, "offset 0: string exceeds max_string (3)");
  REJECT_L("{\"abcd\":1}", &l, "offset 1: string exceeds max_string (3)");
  l.max_string = 0;
  clpd_json_free(ACCEPT_L("\"\"", &l));
  REJECT_L("\"a\"", &l, "offset 0: string exceeds max_string (0)");

  l = big;
  l.max_elements = 2;
  clpd_json_free(ACCEPT_L("[1,2]", &l));
  clpd_json_free(ACCEPT_L("{\"a\":1,\"b\":2}", &l));
  REJECT_L("[1,2,3]", &l, "offset 5: array exceeds max_elements (2)");
  REJECT_L("{\"a\":1,\"b\":2,\"c\":3}", &l,
           "offset 13: object exceeds max_elements (2)");
  l.max_elements = 0;
  REJECT_L("[[],{}]", &l, "offset 1: array exceeds max_elements (0)");
  clpd_json_free(ACCEPT_L("{}", &l));
  REJECT_L("[1]", &l, "offset 1: array exceeds max_elements (0)");
  REJECT_L("{\"a\":1}", &l, "offset 1: object exceeds max_elements (0)");

  l = big;
  l.max_nodes = 3;
  clpd_json_free(ACCEPT_L("[1,2]", &l));
  clpd_json_free(ACCEPT_L("{\"a\":[1]}", &l)); /* keys are not nodes */
  REJECT_L("[1,2,3]", &l, "offset 5: document exceeds max_nodes (3)");
  REJECT_L("{\"a\":[1,2]}", &l, "offset 8: document exceeds max_nodes (3)");
  l.max_nodes = 1;
  clpd_json_free(ACCEPT_L("7", &l));
  REJECT_L("[1]", &l, "offset 1: document exceeds max_nodes (1)");
  l.max_nodes = 0;
  REJECT_L("7", &l, "offset 0: document exceeds max_nodes (0)");
}

/* The length bounds every read: bytes after it in the caller's buffer are
 * never consumed (parse_n copies exactly len bytes, no terminator). */
/* T7a verification: escaped duplicate spellings, surrogate offsets, float
 * edges and NUL bytes inside constructs. */
static void test_verifier_regressions(void) {
  REJECT("{\"\\u00e9\":1,\"\xC3\xA9\":2}", "offset 12: duplicate key");
  REJECT("{\"\\ud83d\\ude00\":1,\"\xF0\x9F\x98\x80\":2}", "offset 18: duplicate key");
  REJECT("\"\\ud800\\u0041\"", "offset 1: invalid surrogate escape");
  REJECT("\"\\ud800\\uzzzz\"", "offset 7: invalid \\u escape");
  REJECT("-Infinity", "offset 1: expected digit");
  REJECT("1.7976931348623159e308", "offset 0: number out of range");
  FLT("1.7976931348623157e308", 1.7976931348623157e308);
  clpd_json_free(ONE("\"\x7f\"", CLPD_JSON_STRING));
  REJECT_N("\"a\\\0\"", 5, "offset 2: invalid escape");
  REJECT_N("\"\\u00\0\"", 7, "offset 1: invalid \\u escape");
  REJECT_N("\"\xC3\0\"", 4, "offset 1: invalid UTF-8");
  REJECT_N("tr\0e", 4, "offset 0: invalid literal");
  REJECT_N("[1,\0]", 5, "offset 3: NUL byte not allowed");
  REJECT(" \xEF\xBB\xBF{}", "offset 1: expected value");
}

static void test_length_bound(void) {
  clpd_json_error err;
  clpd_json *v;

  v = parse_n("[1]]", 3, &big, &err);
  EXPECT(v != NULL && v->type == CLPD_JSON_ARRAY && v->u.arr.count == 1);
  clpd_json_free(v);
  v = parse_n("123", 2, &big, &err);
  EXPECT(v != NULL && v->type == CLPD_JSON_INT && v->u.i == 12);
  clpd_json_free(v);
  v = parse_n("1.25", 3, &big, &err);
  EXPECT(v != NULL && v->type == CLPD_JSON_FLOAT && v->u.f == 1.2);
  clpd_json_free(v);
  v = parse_n("\"ab\"", 3, &big, &err);
  EXPECT(v == NULL &&
         strcmp(err.message, "offset 3: unexpected end of input") == 0);
  REJECT_N("true", 3, "offset 0: invalid literal");
  REJECT_N("1.5", 2, "offset 2: unexpected end of input");
  REJECT_N("1e5", 2, "offset 2: unexpected end of input");
  REJECT_N("\"\\u0041\"", 6, "offset 6: unexpected end of input");
  REJECT_N("\"\\uD83D\\uDE00\"", 10, "offset 10: unexpected end of input");
  REJECT_N("\"\xC3\xA9\"", 2, "offset 2: unexpected end of input");
  REJECT_N("{\"a\":1}", 6, "offset 6: unexpected end of input");
}

int main(void) {
  test_scalars();
  test_long_number();
  test_strings();
  test_containers();
  test_syntax_errors();
  test_number_errors();
  test_string_errors();
  test_limits();
  test_length_bound();
  test_verifier_regressions();
  if (failures != 0) {
    fprintf(stderr, "%d json checks failed\n", failures);
    return 1;
  }
  puts("json checks passed");
  return 0;
}
