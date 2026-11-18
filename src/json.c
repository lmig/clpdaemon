/* Strict JSON parser (src/json.h): recursive descent over buf[0..len),
 * one level of recursion per nesting level (bounded by max_depth). */
#include "json.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const unsigned char *s;
  size_t len, pos, nodes;
  const clpd_json_limits *lim;
  clpd_json_error *err;
} parser;

static int fail_at(parser *p, size_t off, const char *fmt, ...) {
  char text[64];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(text, sizeof text, fmt, ap);
  va_end(ap);
  p->err->offset = off;
  snprintf(p->err->message, sizeof p->err->message, "offset %zu: %s", off,
           text);
  return -1;
}

/* A mismatch at the current byte: the end of input and NUL bytes take
 * precedence over msg. */
static int bad_byte(parser *p, const char *msg) {
  if (p->pos >= p->len) return fail_at(p, p->len, "unexpected end of input");
  if (p->s[p->pos] == '\0') return fail_at(p, p->pos, "NUL byte not allowed");
  return fail_at(p, p->pos, "%s", msg);
}

static void skip_ws(parser *p) {
  while (p->pos < p->len && (p->s[p->pos] == ' ' || p->s[p->pos] == '\t' ||
                             p->s[p->pos] == '\n' || p->s[p->pos] == '\r'))
    p->pos++;
}

static int peek(parser *p) { return p->pos < p->len ? p->s[p->pos] : -1; }

static void free_value(clpd_json *v) {
  size_t i;

  if (v->type == CLPD_JSON_STRING) {
    free(v->u.str.s);
  } else if (v->type == CLPD_JSON_ARRAY) {
    for (i = 0; i < v->u.arr.count; i++) free_value(&v->u.arr.items[i]);
    free(v->u.arr.items);
  } else if (v->type == CLPD_JSON_OBJECT) {
    for (i = 0; i < v->u.obj.count; i++) {
      free(v->u.obj.members[i].key);
      free_value(&v->u.obj.members[i].value);
    }
    free(v->u.obj.members);
  }
  v->type = CLPD_JSON_NULL;
}

/* Reads 4 hex digits after "\u" at p->pos (the backslash). */
static int read_hex4(parser *p, unsigned *out) {
  size_t i;
  unsigned c, v = 0;

  if (p->len - p->pos < 6) return fail_at(p, p->len, "unexpected end of input");
  for (i = 2; i < 6; i++) {
    c = p->s[p->pos + i];
    if (c >= '0' && c <= '9') c -= '0';
    else if (c >= 'a' && c <= 'f') c -= 'a' - 10;
    else if (c >= 'A' && c <= 'F') c -= 'A' - 10;
    else return fail_at(p, p->pos, "invalid \\u escape");
    v = v << 4 | c;
  }
  *out = v;
  return 0;
}

/* Length of the valid UTF-8 sequence starting at p->pos (lead >= 0x80),
 * or -1 with the error set (RFC 3629 table 4). */
static int utf8_seq(parser *p) {
  const unsigned char *s = p->s + p->pos;
  unsigned c = s[0], lo = 0x80, hi = 0xBF;
  size_t n, i;

  if (c >= 0xC2 && c <= 0xDF) n = 2;
  else if (c >= 0xE0 && c <= 0xEF) n = 3;
  else if (c >= 0xF0 && c <= 0xF4) n = 4;
  else return fail_at(p, p->pos, "invalid UTF-8");
  if (c == 0xE0) lo = 0xA0;
  else if (c == 0xED) hi = 0x9F;
  else if (c == 0xF0) lo = 0x90;
  else if (c == 0xF4) hi = 0x8F;
  for (i = 1; i < n; i++) {
    if (p->pos + i >= p->len)
      return fail_at(p, p->len, "unexpected end of input");
    if (s[i] < (i == 1 ? lo : 0x80) || s[i] > (i == 1 ? hi : 0xBF))
      return fail_at(p, p->pos, "invalid UTF-8");
  }
  return (int)n;
}

static size_t put_utf8(char *o, unsigned cp) {
  if (cp < 0x80) {
    o[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    o[0] = (char)(0xC0 | cp >> 6);
    o[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    o[0] = (char)(0xE0 | cp >> 12);
    o[1] = (char)(0x80 | (cp >> 6 & 0x3F));
    o[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  o[0] = (char)(0xF0 | cp >> 18);
  o[1] = (char)(0x80 | (cp >> 12 & 0x3F));
  o[2] = (char)(0x80 | (cp >> 6 & 0x3F));
  o[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

/* Parses the string at p->pos (the opening quote) into a new buffer.
 * Decoded output never exceeds the raw bytes, so cap bytes suffice. */
static int parse_string(parser *p, char **out, size_t *out_len) {
  size_t start = p->pos, n = 0, cap, k;
  char *buf, tmp[4];
  unsigned cp, lo;
  int c, r;
  const char *esc;

  cap = p->len - start - 1;
  if (cap > p->lim->max_string) cap = p->lim->max_string;
  buf = malloc(cap + 1);
  if (buf == NULL) return fail_at(p, start, "out of memory");
  p->pos++;
  for (;;) {
    c = peek(p);
    if (c == '"') break;
    if (c < 0x20) { /* also the end of input */
      r = c == -1 || c == 0
              ? bad_byte(p, "")
              : fail_at(p, p->pos, "unescaped control character in string");
      goto fail;
    }
    k = 1;
    if (c == '\\') {
      if (p->pos + 1 >= p->len) {
        r = fail_at(p, p->len, "unexpected end of input");
        goto fail;
      }
      c = p->s[p->pos + 1];
      esc = strchr("\"\\/bfnrt", c);
      if (c != 0 && esc != NULL) {
        tmp[0] = "\"\\/\b\f\n\r\t"[esc - "\"\\/bfnrt"];
        p->pos += 2;
      } else if (c == 'u') {
        if (read_hex4(p, &cp) != 0) goto fail_set;
        if (cp == 0) {
          r = fail_at(p, p->pos, "\\u0000 not allowed");
          goto fail;
        }
        if (cp >= 0xDC00 && cp <= 0xDFFF) goto bad_surrogate;
        if (cp >= 0xD800) {
          if (cp <= 0xDBFF) {
            if (p->len - p->pos < 8 || p->s[p->pos + 6] != '\\' ||
                p->s[p->pos + 7] != 'u')
              goto bad_surrogate;
            p->pos += 6;
            if (read_hex4(p, &lo) != 0) goto fail_set;
            p->pos -= 6;
            if (lo < 0xDC00 || lo > 0xDFFF) goto bad_surrogate;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            p->pos += 6;
          }
        }
        k = put_utf8(tmp, cp);
        p->pos += 6;
      } else {
        r = fail_at(p, p->pos, "invalid escape");
        goto fail;
      }
    } else if (c >= 0x80) {
      r = utf8_seq(p);
      if (r < 0) goto fail;
      k = (size_t)r;
      memcpy(tmp, p->s + p->pos, k);
      p->pos += k;
    } else {
      tmp[0] = (char)c;
      p->pos++;
    }
    if (k > cap - n) {
      r = fail_at(p, start, "string exceeds max_string (%zu)",
                  p->lim->max_string);
      goto fail;
    }
    memcpy(buf + n, tmp, k);
    n += k;
  }
  p->pos++;
  buf[n] = '\0';
  *out = buf;
  *out_len = n;
  return 0;
bad_surrogate:
  fail_at(p, p->pos, "invalid surrogate escape");
fail_set:
fail:
  free(buf);
  return -1;
}

static int digits(parser *p) {
  int c = peek(p);

  if (c < '0' || c > '9') return bad_byte(p, "expected digit");
  while ((c = peek(p)) >= '0' && c <= '9') p->pos++;
  return 0;
}

static int parse_number(parser *p, clpd_json *v) {
  size_t start = p->pos, i, n;
  int neg = 0, is_float = 0;
  uint64_t mag = 0, max;
  char stackbuf[64], *tok, *end;
  double d;

  if (peek(p) == '-') {
    neg = 1;
    p->pos++;
  }
  if (peek(p) == '0') {
    p->pos++;
    if (peek(p) >= '0' && peek(p) <= '9')
      return fail_at(p, start, "leading zero not allowed");
  } else if (digits(p) != 0) {
    return -1;
  }
  if (peek(p) == '.') {
    is_float = 1;
    p->pos++;
    if (digits(p) != 0) return -1;
  }
  if (peek(p) == 'e' || peek(p) == 'E') {
    is_float = 1;
    p->pos++;
    if (peek(p) == '+' || peek(p) == '-') p->pos++;
    if (digits(p) != 0) return -1;
  }
  if (!is_float) {
    max = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    for (i = start + (size_t)neg; i < p->pos; i++) {
      unsigned dgt = p->s[i] - '0';
      if (mag > (max - dgt) / 10)
        return fail_at(p, start, "integer out of range");
      mag = mag * 10 + dgt;
    }
    v->type = CLPD_JSON_INT;
    v->u.i = neg ? (mag == (uint64_t)INT64_MAX + 1 ? INT64_MIN
                                                   : -(int64_t)mag)
                 : (int64_t)mag;
    return 0;
  }
  n = p->pos - start;
  tok = n < sizeof stackbuf ? stackbuf : malloc(n + 1);
  if (tok == NULL) return fail_at(p, start, "out of memory");
  memcpy(tok, p->s + start, n);
  tok[n] = '\0';
  d = strtod(tok, &end);
  i = (size_t)(end - tok);
  if (tok != stackbuf) free(tok);
  if (i != n || isinf(d) || isnan(d))
    return fail_at(p, start, "number out of range");
  v->type = CLPD_JSON_FLOAT;
  v->u.f = d;
  return 0;
}

static int parse_value(parser *p, clpd_json *v, size_t depth);

static int grow(parser *p, void **items, size_t count, size_t *cap,
                size_t size) {
  void *n;
  size_t c;

  if (count < *cap) return 0;
  c = *cap != 0 ? *cap * 2 : 4;
  n = realloc(*items, c * size);
  if (n == NULL) return fail_at(p, p->pos, "out of memory");
  *items = n;
  *cap = c;
  return 0;
}

/* Arrays and objects: p->pos is at the opening bracket. On failure *v
 * keeps the elements parsed so far, for the caller to free. */
static int parse_container(parser *p, clpd_json *v, size_t depth) {
  int obj = p->s[p->pos] == '{';
  char close = obj ? '}' : ']';
  size_t count = 0, cap = 0, i, at, klen;
  void *items = NULL;
  clpd_json tmp;
  char *key;
  int r;

  if (depth >= p->lim->max_depth)
    return fail_at(p, p->pos, "nesting exceeds max_depth (%zu)",
                   p->lim->max_depth);
  if (obj) {
    v->type = CLPD_JSON_OBJECT;
    v->u.obj.members = NULL;
    v->u.obj.count = 0;
  } else {
    v->type = CLPD_JSON_ARRAY;
    v->u.arr.items = NULL;
    v->u.arr.count = 0;
  }
  p->pos++;
  skip_ws(p);
  if (peek(p) == close) {
    p->pos++;
    return 0;
  }
  for (;;) {
    at = p->pos;
    if (count == p->lim->max_elements)
      return fail_at(p, at, "%s exceeds max_elements (%zu)",
                     obj ? "object" : "array", p->lim->max_elements);
    if (obj) {
      clpd_json_member *m;
      if (peek(p) != '"') return bad_byte(p, "expected string key");
      if (parse_string(p, &key, &klen) != 0) return -1;
      for (i = 0; i < count; i++) {
        m = &v->u.obj.members[i];
        if (m->key_len == klen && memcmp(m->key, key, klen) == 0) {
          free(key);
          return fail_at(p, at, "duplicate key");
        }
      }
      skip_ws(p);
      if (peek(p) != ':') {
        free(key);
        return bad_byte(p, "expected ':'");
      }
      p->pos++;
      skip_ws(p);
      tmp.type = CLPD_JSON_NULL;
      r = parse_value(p, &tmp, depth + 1);
      if (r == 0) r = grow(p, &items, count, &cap, sizeof *m);
      if (r != 0) {
        free(key);
        free_value(&tmp);
        return -1;
      }
      v->u.obj.members = items;
      m = &v->u.obj.members[count];
      m->key = key;
      m->key_len = klen;
      m->value = tmp;
      v->u.obj.count = ++count;
    } else {
      tmp.type = CLPD_JSON_NULL;
      r = parse_value(p, &tmp, depth + 1);
      if (r == 0) r = grow(p, &items, count, &cap, sizeof tmp);
      if (r != 0) {
        free_value(&tmp);
        return -1;
      }
      v->u.arr.items = items;
      v->u.arr.items[count] = tmp;
      v->u.arr.count = ++count;
    }
    skip_ws(p);
    if (peek(p) == close) {
      p->pos++;
      return 0;
    }
    if (peek(p) != ',')
      return bad_byte(p, obj ? "expected ',' or '}'" : "expected ',' or ']'");
    p->pos++;
    skip_ws(p);
  }
}

static int literal(parser *p, const char *word, clpd_json_type type,
                   clpd_json *v) {
  size_t n = strlen(word);

  if (p->len - p->pos < n || memcmp(p->s + p->pos, word, n) != 0)
    return fail_at(p, p->pos, "invalid literal");
  p->pos += n;
  v->type = type;
  return 0;
}

/* p->pos is at the first byte of the value; *v is always left freeable. */
static int parse_value(parser *p, clpd_json *v, size_t depth) {
  int c = peek(p);

  v->type = CLPD_JSON_NULL;
  if (c != -1 && c != 0 && p->nodes == p->lim->max_nodes)
    return fail_at(p, p->pos, "document exceeds max_nodes (%zu)",
                   p->lim->max_nodes);
  p->nodes++;
  switch (c) {
  case '{':
  case '[':
    return parse_container(p, v, depth);
  case '"':
    if (parse_string(p, &v->u.str.s, &v->u.str.len) != 0) return -1;
    v->type = CLPD_JSON_STRING;
    return 0;
  case 't': return literal(p, "true", CLPD_JSON_TRUE, v);
  case 'f': return literal(p, "false", CLPD_JSON_FALSE, v);
  case 'n': return literal(p, "null", CLPD_JSON_NULL, v);
  default:
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(p, v);
    return bad_byte(p, "expected value");
  }
}

clpd_json *clpd_json_parse(const char *buf, size_t len,
                           const clpd_json_limits *limits,
                           clpd_json_error *err) {
  parser p;
  clpd_json *root;

  p.s = (const unsigned char *)buf;
  p.len = len;
  p.pos = 0;
  p.nodes = 0;
  p.lim = limits;
  p.err = err;
  if (len > limits->max_bytes) {
    fail_at(&p, 0, "input exceeds max_bytes (%zu)", limits->max_bytes);
    return NULL;
  }
  if (len >= 3 && memcmp(buf, "\xEF\xBB\xBF", 3) == 0) {
    fail_at(&p, 0, "byte order mark not allowed");
    return NULL;
  }
  root = malloc(sizeof *root);
  if (root == NULL) {
    fail_at(&p, 0, "out of memory");
    return NULL;
  }
  skip_ws(&p);
  if (parse_value(&p, root, 0) == 0) {
    skip_ws(&p);
    if (p.pos == len) return root;
    bad_byte(&p, "unexpected data after value");
  }
  clpd_json_free(root);
  return NULL;
}

void clpd_json_free(clpd_json *root) {
  if (root == NULL) return;
  free_value(root);
  free(root);
}

const clpd_json *clpd_json_get(const clpd_json *v, const char *key) {
  size_t i, n;

  if (v == NULL || v->type != CLPD_JSON_OBJECT) return NULL;
  n = strlen(key);
  for (i = 0; i < v->u.obj.count; i++)
    if (v->u.obj.members[i].key_len == n &&
        memcmp(v->u.obj.members[i].key, key, n) == 0)
      return &v->u.obj.members[i].value;
  return NULL;
}
