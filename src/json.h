/* Strict JSON parser (RFC 8259) for untrusted input such as ingested fact
 * payloads (T7a). No global state: every call works only on its arguments,
 * so the parser may run on several threads at once. Numbers are converted
 * with strtod, which needs LC_NUMERIC "C" (the daemon never calls
 * setlocale).
 *
 * Accepted: exactly one value with optional surrounding whitespace (space,
 * tab, LF, CR). Rejected beyond the RFC grammar: a leading byte order mark,
 * NUL bytes, invalid UTF-8 (overlong forms, surrogates, code points above
 * U+10FFFF, truncated sequences), lone or invalid \u surrogate escapes,
 * \u0000, duplicate object keys, integers outside int64, numbers whose
 * value overflows a double (underflow to zero or a subnormal is accepted).
 *
 * Errors: clpd_json_parse fills err->offset (byte offset into the buffer)
 * and err->message as "offset <n>: <text>", e.g. "offset 12: expected ':'".
 * Every <text>, with the offset it reports:
 *   input exceeds max_bytes (<n>)          0
 *   byte order mark not allowed            0
 *   NUL byte not allowed                   the NUL byte
 *   unexpected end of input                the buffer length
 *   expected value                         the byte that cannot start a
 *                                          value (also '+', '.', NaN,
 *                                          Infinity, a leading ',')
 *   invalid literal                        start of a bad true/false/null
 *   unexpected data after value            first byte after the document
 *   expected ':'                           the byte found instead
 *   expected ',' or ']'                    the byte found instead
 *   expected ',' or '}'                    the byte found instead
 *   expected string key                    the byte found instead (also a
 *                                          trailing comma in an object)
 *   duplicate key                          the opening quote of the key
 *   leading zero not allowed               start of the number
 *   expected digit                         the byte found instead (after
 *                                          '-', '.', 'e' or the sign)
 *   integer out of range                   start of the number
 *   number out of range                    start of the number
 *   unescaped control character in string  the control byte
 *   invalid escape                         the backslash
 *   invalid \u escape                      the backslash (non-hex digits)
 *   invalid surrogate escape               the backslash of the lone or
 *                                          mismatched surrogate
 *   \u0000 not allowed                     the backslash
 *   invalid UTF-8                          first byte of the bad sequence
 *   string exceeds max_string (<n>)        the opening quote
 *   nesting exceeds max_depth (<n>)        the '[' or '{' one level deeper
 *   array exceeds max_elements (<n>)       the first element over the limit
 *   object exceeds max_elements (<n>)      the first member key over it
 *   document exceeds max_nodes (<n>)       the first value over the limit
 *   out of memory                          where allocation failed
 * A NUL byte or the end of the buffer inside a construct reports that
 * construct's error ("invalid escape", "invalid \u escape", "invalid UTF-8",
 * "invalid literal"), or "unexpected end of input" for a sequence cut off
 * by the buffer end; "NUL byte not allowed" is for a NUL where a token
 * starts. A limit can be reported before a later syntax error. Recursion
 * uses one stack frame per nesting level (a few hundred bytes), so keep
 * max_depth small (the ingest pipeline uses a single-digit depth).
 */
#ifndef CLPD_JSON_H
#define CLPD_JSON_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
  CLPD_JSON_NULL,
  CLPD_JSON_FALSE,
  CLPD_JSON_TRUE,
  CLPD_JSON_INT,    /* no fraction and no exponent, fits int64 */
  CLPD_JSON_FLOAT,  /* fraction or exponent, finite double */
  CLPD_JSON_STRING,
  CLPD_JSON_ARRAY,
  CLPD_JSON_OBJECT
} clpd_json_type;

typedef struct clpd_json clpd_json;
typedef struct clpd_json_member clpd_json_member;

struct clpd_json {
  clpd_json_type type;
  union {
    int64_t i;                                            /* INT */
    double f;                                             /* FLOAT */
    struct { char *s; size_t len; } str;   /* STRING: UTF-8, no NUL inside,
                                              s[len] == '\0' */
    struct { clpd_json *items; size_t count; } arr;       /* ARRAY */
    struct { clpd_json_member *members; size_t count; } obj; /* OBJECT, in
                                                     document order */
  } u;
};

struct clpd_json_member {
  char *key; /* decoded like a string: UTF-8, no NUL, key[key_len] == '\0' */
  size_t key_len;
  clpd_json value;
};

/* Every limit is inclusive: a value equal to the limit is accepted. */
typedef struct {
  size_t max_bytes;    /* input length */
  size_t max_depth;    /* nested arrays/objects; a scalar document is depth
                          0, "[]" is 1, "[[]]" is 2. The parser recurses
                          once per level, so keep it small (<= ~1000). */
  size_t max_string;   /* decoded bytes of one string or key */
  size_t max_elements; /* values of one array, members of one object
                          (duplicate keys are checked pairwise, so the cost
                          per object grows with its square) */
  size_t max_nodes;    /* values in the document, containers included,
                          keys excluded */
} clpd_json_limits;

typedef struct {
  size_t offset;
  char message[96];
} clpd_json_error;

/* Parses the whole buffer buf[0..len) (no NUL terminator needed; nothing
 * past len is read). Returns a tree owned by the caller (release it with
 * clpd_json_free), or NULL with *err filled in. */
clpd_json *clpd_json_parse(const char *buf, size_t len,
                           const clpd_json_limits *limits,
                           clpd_json_error *err);

/* Frees a tree returned by clpd_json_parse; NULL is ignored. */
void clpd_json_free(clpd_json *root);

/* Member value of an object by key (NUL-terminated), or NULL when v is not
 * an object or has no such key. */
const clpd_json *clpd_json_get(const clpd_json *v, const char *key);

#endif
