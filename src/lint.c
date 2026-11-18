/* wf-id lint for workflow rule packages (FR-7, L38).
 *
 * Templates are checked through the CLIPS API. Rules are checked on the
 * LHS of their pretty-print form, read with a small s-expression reader. */
#include "lint.h"

#include <string.h>

#define LINT_PREFIX "clpdaemon: %s: lint: "

typedef enum { TOK_END, TOK_OPEN, TOK_CLOSE, TOK_ATOM, TOK_STRING } tok_kind;

typedef struct {
  tok_kind kind;
  const char *text;
  size_t len;
} token;

typedef struct {
  const char *p;
} reader;

/* State of one rule while its LHS is analyzed. */
typedef struct {
  const char *pkg;
  const char *rule;
  const clpd_lint_exempt *exempt;
  FILE *err;
  int errors;
  int pattern;       /* number of the last pattern seen (1-based) */
  const char *expected; /* variable name (without '?') of the first checked
                           pattern, pointing into the pretty-print form */
  size_t expected_len;
  int expected_from; /* pattern number that set `expected`, 0 = none */
} rule_ctx;

static token next_token(reader *r) {
  token t;
  const char *p = r->p;

  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' ||
         *p == '\v')
    p++;
  t.text = p;
  t.len = 0;
  if (*p == '\0') {
    t.kind = TOK_END;
  } else if (*p == '(') {
    t.kind = TOK_OPEN;
    p++;
  } else if (*p == ')') {
    t.kind = TOK_CLOSE;
    p++;
  } else if (*p == '"') {
    t.kind = TOK_STRING;
    p++;
    while (*p != '\0' && *p != '"') {
      if (*p == '\\' && p[1] != '\0') p++;
      p++;
    }
    if (*p == '"') p++;
  } else {
    t.kind = TOK_ATOM;
    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' &&
           *p != '\r' && *p != '\f' && *p != '\v' && *p != '(' && *p != ')' &&
           *p != '"')
      p++;
  }
  t.len = (size_t)(p - t.text);
  r->p = p;
  return t;
}

static int atom_is(token t, const char *s) {
  return t.kind == TOK_ATOM && t.len == strlen(s) &&
         strncmp(t.text, s, t.len) == 0;
}

/* Consumes tokens until the list currently open `depth` levels closes. */
static void skip_list(reader *r, int depth) {
  while (depth > 0) {
    token t = next_token(r);
    if (t.kind == TOK_END) return;
    if (t.kind == TOK_OPEN) depth++;
    if (t.kind == TOK_CLOSE) depth--;
  }
}

static int name_in(const char *name, size_t len, const char *const *names,
                   size_t count) {
  size_t i;
  for (i = 0; i < count; i++)
    if (names[i] != NULL && strlen(names[i]) == len &&
        strncmp(names[i], name, len) == 0)
      return 1;
  return 0;
}

/* Length of the single-field variable name at the start of atom `t`
 * ("?name", optionally followed by '&', '|' constraints), 0 if none. */
static size_t variable_name_len(token t) {
  size_t n = 1;
  if (t.kind != TOK_ATOM || t.len < 2 || t.text[0] != '?') return 0;
  if (t.text[1] == '*' || t.text[1] == '&' || t.text[1] == '|' ||
      t.text[1] == '~' || t.text[1] == ':' || t.text[1] == '=')
    return 0;
  while (n < t.len && t.text[n] != '&' && t.text[n] != '|') n++;
  return n - 1;
}

/* Reads the rest of a pattern whose head atom is `head` (the opening
 * parenthesis and the head are consumed) and checks its wf-id slot. */
static void check_pattern(reader *r, rule_ctx *c, token head) {
  const char *name = head.text;
  size_t name_len = head.len;
  const char *q;
  int found = 0;
  token value;
  size_t var_len = 0;

  for (q = head.text; q + 1 < head.text + head.len; q++)
    if (q[0] == ':' && q[1] == ':') {
      name = q + 2;
      name_len = (size_t)(head.text + head.len - name);
      break;
    }
  c->pattern++;

  value.kind = TOK_END;
  for (;;) {
    token t = next_token(r);
    if (t.kind == TOK_END || t.kind == TOK_CLOSE) break;
    if (t.kind != TOK_OPEN) continue;
    t = next_token(r);
    if (!found && atom_is(t, "wf-id")) {
      found = 1;
      value = next_token(r);
      if (value.kind == TOK_CLOSE) continue;
      skip_list(r, value.kind == TOK_OPEN ? 2 : 1);
    } else if (t.kind == TOK_OPEN) {
      skip_list(r, 2);
    } else if (t.kind != TOK_CLOSE && t.kind != TOK_END) {
      skip_list(r, 1);
    }
  }

  if (c->exempt != NULL && name_in(name, name_len, c->exempt->templates,
                                   c->exempt->template_count))
    return;
  if (found) var_len = variable_name_len(value);
  if (var_len == 0) {
    fprintf(c->err,
            LINT_PREFIX
            "rule '%s': pattern %d (%.*s) does not bind wf-id to a variable\n",
            c->pkg, c->rule, c->pattern, (int)name_len, name);
    c->errors++;
    return;
  }
  if (c->expected_from == 0) {
    c->expected = value.text + 1;
    c->expected_len = var_len;
    c->expected_from = c->pattern;
    return;
  }
  if (var_len != c->expected_len ||
      strncmp(value.text + 1, c->expected, var_len) != 0) {
    fprintf(c->err,
            LINT_PREFIX "rule '%s': pattern %d (%.*s) binds wf-id to ?%.*s, "
                        "expected ?%.*s from pattern %d\n",
            c->pkg, c->rule, c->pattern, (int)name_len, name, (int)var_len,
            value.text + 1, (int)c->expected_len, c->expected,
            c->expected_from);
    c->errors++;
  }
}

/* Reads one conditional element whose opening parenthesis is consumed. */
static void check_ce(reader *r, rule_ctx *c) {
  token head = next_token(r);

  if (head.kind != TOK_ATOM) {
    if (head.kind == TOK_OPEN)
      skip_list(r, 2);
    else if (head.kind != TOK_CLOSE)
      skip_list(r, 1);
    return;
  }
  if (atom_is(head, "declare") || atom_is(head, "test")) {
    skip_list(r, 1);
  } else if (atom_is(head, "not") || atom_is(head, "exists") ||
             atom_is(head, "and") || atom_is(head, "or") ||
             atom_is(head, "forall") || atom_is(head, "logical")) {
    for (;;) {
      token t = next_token(r);
      if (t.kind == TOK_END || t.kind == TOK_CLOSE) return;
      if (t.kind == TOK_OPEN) check_ce(r, c);
    }
  } else {
    check_pattern(r, c, head);
  }
}

static int lint_rule(const char *pkg, const char *rule, const char *ppform,
                     const clpd_lint_exempt *exempt, FILE *err) {
  rule_ctx c;
  reader r;
  token t;

  memset(&c, 0, sizeof c);
  c.pkg = pkg;
  c.rule = rule;
  c.exempt = exempt;
  c.err = err;
  r.p = ppform;

  /* "(defrule <name> [comment] CE... => ...)" */
  if (next_token(&r).kind != TOK_OPEN) return 0;
  next_token(&r); /* defrule */
  next_token(&r); /* name */
  for (;;) {
    t = next_token(&r);
    if (t.kind == TOK_END || t.kind == TOK_CLOSE || atom_is(t, "=>")) break;
    if (t.kind == TOK_OPEN) check_ce(&r, &c);
    /* comment string, "?f" and "<-" are skipped */
  }
  return c.errors;
}

static int template_exists(Environment *env, const char *name) {
  Deftemplate *t;
  for (t = GetNextDeftemplate(env, NULL); t != NULL;
       t = GetNextDeftemplate(env, t))
    if (strcmp(DeftemplateName(t), name) == 0) return 1;
  return 0;
}

static int rule_exists(Environment *env, const char *name) {
  Defrule *r;
  for (r = GetNextDefrule(env, NULL); r != NULL; r = GetNextDefrule(env, r))
    if (strcmp(DefruleName(r), name) == 0) return 1;
  return 0;
}

int clpd_lint_package(Environment *env, Defmodule *module, const char *pkg_name,
                      const clpd_lint_exempt *exempt, FILE *err) {
  Defmodule *saved = GetCurrentModule(env);
  Deftemplate *t;
  Defrule *rule;
  int errors = 0;
  size_t i;

  SetCurrentModule(env, module);

  if (exempt != NULL) {
    for (i = 0; i < exempt->template_count; i++)
      if (!template_exists(env, exempt->templates[i])) {
        fprintf(err, LINT_PREFIX "lint_exempt names unknown template '%s'\n",
                pkg_name, exempt->templates[i]);
        errors++;
      }
    for (i = 0; i < exempt->rule_count; i++)
      if (!rule_exists(env, exempt->rules[i])) {
        fprintf(err, LINT_PREFIX "lint_exempt names unknown rule '%s'\n",
                pkg_name, exempt->rules[i]);
        errors++;
      }
  }

  for (t = GetNextDeftemplate(env, NULL); t != NULL;
       t = GetNextDeftemplate(env, t)) {
    const char *name = DeftemplateName(t);
    if (exempt != NULL &&
        name_in(name, strlen(name), exempt->templates, exempt->template_count))
      continue;
    if (!DeftemplateSlotExistP(t, "wf-id")) {
      fprintf(err, LINT_PREFIX "template '%s' has no wf-id slot\n", pkg_name,
              name);
      errors++;
    }
  }

  for (rule = GetNextDefrule(env, NULL); rule != NULL;
       rule = GetNextDefrule(env, rule)) {
    const char *name = DefruleName(rule);
    const char *pp = DefrulePPForm(rule);
    if (exempt != NULL &&
        name_in(name, strlen(name), exempt->rules, exempt->rule_count))
      continue;
    if (pp != NULL) errors += lint_rule(pkg_name, name, pp, exempt, err);
  }

  SetCurrentModule(env, saved);
  return errors;
}
