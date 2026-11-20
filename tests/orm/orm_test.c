/* Unit tests for the ORM mapping file parser (docs/orm.md, T9a). */
#define _POSIX_C_SOURCE 200809L

#include "orm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../common/expect.h"

static char root[] = "/tmp/clpd_orm_test.XXXXXX";
static char path[sizeof root + 32];

/* Writes `text` to <root>/orm.conf (unless NULL) and parses it; the error
 * stream is captured into errbuf. */
static int parse(const char *text, clpd_orm *orm, char *errbuf, size_t cap) {
  FILE *f, *err;
  size_t n;
  int rc;

  snprintf(path, sizeof path, "%s/orm.conf", root);
  if (text != NULL) {
    f = fopen(path, "w");
    if (f == NULL || fputs(text, f) < 0 || fclose(f) != 0) {
      perror("orm_test");
      exit(2);
    }
  }
  err = tmpfile();
  if (err == NULL) exit(2);
  rc = clpd_orm_parse(path, orm, err);
  rewind(err);
  n = fread(errbuf, 1, cap - 1, err);
  errbuf[n] = '\0';
  fclose(err);
  remove(path);
  return rc;
}

/* Expected text with every %s replaced by the mapping path. */
static int err_is(const char *errbuf, const char *fmt) {
  char want[8192];
  snprintf(want, sizeof want, fmt, path, path, path, path, path, path, path,
           path, path, path, path, path, path, path, path, path, path, path,
           path, path);
  if (strcmp(errbuf, want) != 0) {
    fprintf(stderr, "  got:  [%s]\n  want: [%s]\n", errbuf, want);
    return 0;
  }
  return 1;
}

static const char *valid_map =
    "# fibre-installation mapping\n"
    "order.table = fibre_order\n"
    "order.owned = yes\n"
    "order.key = line-no, region\n"
    "order.slot.wf-id = wf_id text string error\n"
    "order.slot.line-no = line_no bigint integer error\n"
    "order.slot.region = region text symbol error\n"
    "order.slot.amount = amount numeric float default\n"
    "order.slot.payload = payload jsonb string nil\n"
    "order.slot.photo = photo bytea string nil\n"
    "order.slot.done = done boolean symbol default\n"
    "order.slot.at = created_at timestamptz string nil\n"
    "order.slot.ratio = ratio double float nil\n"
    "customer.table = crm_customer\n"
    "customer.owned = no\n"
    "customer.key =\n"
    "customer.slot.wf-id = wf_id text string error\n"
    "seq.order_no = order_no_seq\n"
    "seq.order_no.owned = yes\n"
    "seq.ticket.owned = no\n"
    "seq.ticket = crm_ticket_seq\n";

static void test_valid(void) {
  clpd_orm m;
  char eb[2048];
  const clpd_orm_template *o, *c;
  const clpd_orm_column *col;

  EXPECT(parse(valid_map, &m, eb, sizeof eb) == 0);
  EXPECT(err_is(eb, ""));
  EXPECT(strcmp(m.path, path) == 0);
  EXPECT(m.template_count == 2);
  if (m.template_count != 2) {
    clpd_orm_free(&m);
    return;
  }
  o = &m.templates[0];
  c = &m.templates[1];
  EXPECT(strcmp(o->name, "order") == 0 && strcmp(o->table, "fibre_order") == 0);
  EXPECT(o->owned == 1 && o->line == 2 && o->table_line == 2 &&
         o->owned_line == 3 && o->key_line == 4);
  EXPECT(o->key.count == 2 && strcmp(o->key.items[0], "line-no") == 0 &&
         strcmp(o->key.items[1], "region") == 0);
  EXPECT(o->column_count == 9);
  EXPECT(o->key_columns == NULL);
  col = clpd_orm_column_of(o, "amount");
  EXPECT(col != NULL && strcmp(col->slot, "amount") == 0 &&
         strcmp(col->column, "amount") == 0 &&
         col->sql_type == CLPD_ORM_NUMERIC &&
         col->clips_type == CLPD_ORM_FLOAT &&
         col->null == CLPD_ORM_NULL_DEFAULT && col->line == 8);
  col = clpd_orm_column_of(o, "at");
  EXPECT(col != NULL && strcmp(col->column, "created_at") == 0 &&
         col->sql_type == CLPD_ORM_TIMESTAMPTZ &&
         col->clips_type == CLPD_ORM_STRING && col->null == CLPD_ORM_NULL_NIL);
  col = clpd_orm_column_of(o, "region");
  EXPECT(col != NULL && col->sql_type == CLPD_ORM_TEXT &&
         col->clips_type == CLPD_ORM_SYMBOL && col->null == CLPD_ORM_NULL_ERROR);
  col = clpd_orm_column_of(o, "line-no");
  EXPECT(col != NULL && col->sql_type == CLPD_ORM_BIGINT &&
         col->clips_type == CLPD_ORM_INTEGER);
  EXPECT(clpd_orm_column_of(o, "payload")->sql_type == CLPD_ORM_JSONB);
  EXPECT(clpd_orm_column_of(o, "photo")->sql_type == CLPD_ORM_BYTEA);
  EXPECT(clpd_orm_column_of(o, "done")->sql_type == CLPD_ORM_BOOLEAN);
  EXPECT(clpd_orm_column_of(o, "ratio")->sql_type == CLPD_ORM_DOUBLE);
  EXPECT(clpd_orm_column_of(o, "nope") == NULL);
  EXPECT(clpd_orm_resolve_key(&m.templates[0]) == 0);
  EXPECT(o->key_columns != NULL && o->key_columns[0] == 0 &&
         o->key_columns[1] == 1 && o->key_columns[2] == 2);
  EXPECT(strcmp(c->name, "customer") == 0 && c->owned == 0 &&
         c->key.count == 0 && c->column_count == 1);
  EXPECT(clpd_orm_resolve_key(&m.templates[1]) == 0);
  EXPECT(c->key_columns != NULL && c->key_columns[0] == 0);
  EXPECT(m.sequence_count == 2);
  if (m.sequence_count == 2) {
    EXPECT(strcmp(m.sequences[0].alias, "order_no") == 0 &&
           strcmp(m.sequences[0].name, "order_no_seq") == 0 &&
           m.sequences[0].owned == 1 && m.sequences[0].line == 18);
    EXPECT(strcmp(m.sequences[1].alias, "ticket") == 0 &&
           strcmp(m.sequences[1].name, "crm_ticket_seq") == 0 &&
           m.sequences[1].owned == 0 && m.sequences[1].line == 20 &&
           m.sequences[1].name_line == 21 && m.sequences[1].owned_line == 20);
  }
  clpd_orm_free(&m);
  EXPECT(m.templates == NULL && m.template_count == 0 && m.path == NULL);
}

/* An empty file (only comments) is a valid, empty mapping. */
static void test_empty(void) {
  clpd_orm m;
  char eb[512];
  EXPECT(parse("# nothing mapped yet\n\n", &m, eb, sizeof eb) == 0);
  EXPECT(err_is(eb, ""));
  EXPECT(m.template_count == 0 && m.sequence_count == 0);
  clpd_orm_free(&m);
}

static void test_cannot_open(void) {
  clpd_orm m;
  char eb[512];
  EXPECT(parse(NULL, &m, eb, sizeof eb) != 0);
  EXPECT(err_is(eb, "clpdaemon: %s: cannot open: No such file or directory\n"));
  clpd_orm_free(&m);
}

/* A repeated slot line is a duplicate even when its first line was
 * rejected (review advisory R3 of T9a). */
static void test_duplicate_slot_after_invalid(void) {
  clpd_orm m;
  char eb[2048];
  EXPECT(parse("order.table = t\n"
               "order.owned = no\n"
               "order.slot.wf-id = wf_id text string bogus\n"
               "order.slot.wf-id = wf_id text string error\n",
               &m, eb, sizeof eb) != 0);
  EXPECT(err_is(eb,
                "clpdaemon: %s:3: order.slot.wf-id: unknown null policy "
                "'bogus' (expected nil, default, error)\n"
                "clpdaemon: %s:4: duplicate key 'order.slot.wf-id' (first set "
                "on line 3)\n"));
  EXPECT(m.template_count == 1 && m.templates[0].column_count == 0);
  clpd_orm_free(&m);
}

static void test_unknown_and_duplicate_keys(void) {
  clpd_orm m;
  char eb[4096];
  EXPECT(parse("order.table = t\n"
               "order.owned = no\n"
               "order.slot.wf-id = wf_id text string error\n"
               "order\n"
               "order = x\n"
               ".table = x\n"
               "order.bogus = x\n"
               "order.slot. = x text string nil\n"
               "order.slot = x\n"
               "seq. = s\n"
               "seq.a.bogus = s\n"
               "seq..owned = yes\n"
               "order.table = u\n"
               "order.slot.wf-id = wf2 text string error\n"
               "seq.s = s1\n"
               "seq.s = s2\n"
               "seq.s.owned = yes\n"
               "seq.s.owned = no\n",
               &m, eb, sizeof eb) != 0);
  EXPECT(err_is(eb,
                "clpdaemon: %s:4: expected 'key = value'\n"
                "clpdaemon: %s:5: unknown key 'order'\n"
                "clpdaemon: %s:6: unknown key '.table'\n"
                "clpdaemon: %s:7: unknown key 'order.bogus'\n"
                "clpdaemon: %s:8: unknown key 'order.slot.'\n"
                "clpdaemon: %s:9: unknown key 'order.slot'\n"
                "clpdaemon: %s:10: unknown key 'seq.'\n"
                "clpdaemon: %s:11: seq.a.bogus: sequence alias: expected an "
                "identifier matching [a-z_][a-z0-9_]* (at most 63 bytes), got "
                "'a.bogus'\n"
                "clpdaemon: %s:12: unknown key 'seq..owned'\n"
                "clpdaemon: %s:13: duplicate key 'order.table' (first set on "
                "line 1)\n"
                "clpdaemon: %s:14: duplicate key 'order.slot.wf-id' (first set "
                "on line 3)\n"
                "clpdaemon: %s:16: duplicate key 'seq.s' (first set on line "
                "15)\n"
                "clpdaemon: %s:18: duplicate key 'seq.s.owned' (first set on "
                "line 17)\n"));
  clpd_orm_free(&m);
}

/* Identifiers: [a-z_][a-z0-9_]*, at most 63 bytes. */
static void test_identifiers(void) {
  clpd_orm m;
  char eb[4096], text[2048];
  const char *ok63 =
      "a23456789012345678901234567890123456789012345678901234567890123";
  const char *bad64 =
      "a234567890123456789012345678901234567890123456789012345678901234";
  snprintf(text, sizeof text,
           "a.table = %s\na.owned = no\na.slot.wf-id = %s text string error\n"
           "b.table = %s\nb.owned = no\nb.slot.wf-id = wf_id text string error\n"
           "c.table = Order\nc.owned = no\nc.slot.wf-id = Wf_id text string error\n"
           "d.table = 9lives\nd.owned = no\nd.slot.wf-id = wf-id text string error\n"
           "e.table = \ne.owned = no\ne.slot.wf-id = _x9 text string error\n"
           "seq.Order = s\nseq.ok = \"quoted\"\nseq.ok.owned = no\n"
           "seq._under_9 = %s\nseq._under_9.owned = yes\n",
           ok63, ok63, bad64, ok63);
  EXPECT(parse(text, &m, eb, sizeof eb) != 0);
  EXPECT(err_is(eb,
                "clpdaemon: %s:4: b.table: expected an identifier matching "
                "[a-z_][a-z0-9_]* (at most 63 bytes), got "
                "'a234567890123456789012345678901234567890123456789012345678901234'\n"
                "clpdaemon: %s:7: c.table: expected an identifier matching "
                "[a-z_][a-z0-9_]* (at most 63 bytes), got 'Order'\n"
                "clpdaemon: %s:9: c.slot.wf-id: column: expected an identifier "
                "matching [a-z_][a-z0-9_]* (at most 63 bytes), got 'Wf_id'\n"
                "clpdaemon: %s:10: d.table: expected an identifier matching "
                "[a-z_][a-z0-9_]* (at most 63 bytes), got '9lives'\n"
                "clpdaemon: %s:12: d.slot.wf-id: column: expected an identifier "
                "matching [a-z_][a-z0-9_]* (at most 63 bytes), got 'wf-id'\n"
                "clpdaemon: %s:13: e.table: expected an identifier matching "
                "[a-z_][a-z0-9_]* (at most 63 bytes), got ''\n"
                "clpdaemon: %s:16: seq.Order: sequence alias: expected an "
                "identifier matching [a-z_][a-z0-9_]* (at most 63 bytes), got "
                "'Order'\n"
                "clpdaemon: %s:17: seq.ok: expected an identifier matching "
                "[a-z_][a-z0-9_]* (at most 63 bytes), got '\"quoted\"'\n"));
  clpd_orm_free(&m);
  EXPECT(clpd_orm_valid_ident(ok63) == 1);
  EXPECT(clpd_orm_valid_ident(bad64) == 0);
  EXPECT(clpd_orm_valid_ident("_") == 1 && clpd_orm_valid_ident("a_1") == 1);
  EXPECT(clpd_orm_valid_ident("") == 0 && clpd_orm_valid_ident("1a") == 0 &&
         clpd_orm_valid_ident("a-b") == 0 && clpd_orm_valid_ident("aB") == 0 &&
         clpd_orm_valid_ident("a b") == 0 && clpd_orm_valid_ident("a.b") == 0);
}

static void test_values(void) {
  clpd_orm m;
  char eb[4096];
  EXPECT(parse("o.table = t\n"
               "o.owned = true\n"
               "o.slot.wf-id = wf_id text string error\n"
               "o.slot.a = a text string\n"
               "o.slot.b = b text string nil extra\n"
               "o.slot.c = c varchar string nil\n"
               "o.slot.d = d text String nil\n"
               "o.slot.e = e text string null\n"
               "o.slot.f =\n"
               "o.slot.g = g\tbigint  integer\t default\n"
               "o.slot.h = h BIGINT integer nil\n"
               "seq.s = s\n"
               "seq.s.owned = 1\n",
               &m, eb, sizeof eb) != 0);
  EXPECT(err_is(eb,
                "clpdaemon: %s:2: o.owned: expected yes or no, got 'true'\n"
                "clpdaemon: %s:4: o.slot.a: expected '<column> <sql-type> "
                "<clips-type> <null>', got 'a text string'\n"
                "clpdaemon: %s:5: o.slot.b: expected '<column> <sql-type> "
                "<clips-type> <null>', got 'b text string nil extra'\n"
                "clpdaemon: %s:6: o.slot.c: unknown sql type 'varchar' "
                "(expected bigint, double, text, bytea, boolean, timestamptz, "
                "jsonb, numeric)\n"
                "clpdaemon: %s:7: o.slot.d: unknown clips type 'String' "
                "(expected integer, float, string, symbol)\n"
                "clpdaemon: %s:8: o.slot.e: unknown null policy 'null' "
                "(expected nil, default, error)\n"
                "clpdaemon: %s:9: o.slot.f: expected '<column> <sql-type> "
                "<clips-type> <null>', got ''\n"
                "clpdaemon: %s:11: o.slot.h: unknown sql type 'BIGINT' "
                "(expected bigint, double, text, bytea, boolean, timestamptz, "
                "jsonb, numeric)\n"
                "clpdaemon: %s:13: seq.s.owned: expected yes or no, got '1'\n"));
  clpd_orm_free(&m);
  /* Tabs and runs of blanks separate the four fields. */
  EXPECT(parse("o.table = t\no.owned = no\n"
               "o.slot.wf-id = wf_id text string error\n"
               "o.slot.g = g\tbigint  integer\t default\n",
               &m, eb, sizeof eb) == 0);
  EXPECT(err_is(eb, ""));
  EXPECT(m.template_count == 1 && m.templates[0].column_count == 2 &&
         strcmp(m.templates[0].columns[1].column, "g") == 0 &&
         m.templates[0].columns[1].sql_type == CLPD_ORM_BIGINT &&
         m.templates[0].columns[1].clips_type == CLPD_ORM_INTEGER &&
         m.templates[0].columns[1].null == CLPD_ORM_NULL_DEFAULT);
  clpd_orm_free(&m);
}

static void test_keys(void) {
  clpd_orm m;
  char eb[4096];
  EXPECT(parse("o.table = t\n"
               "o.owned = no\n"
               "o.key = a, b, a\n"
               "p.table = p\n"
               "p.owned = no\n"
               "p.key = wf-id, a\n"
               "q.table = q\n"
               "q.owned = no\n"
               "q.key = a,,b\n"
               "r.table = r\n"
               "r.owned = no\n"
               "r.slot.wf-id = wf_id text string nil\n"
               "r.slot.a = a bigint integer default\n"
               "r.slot.b = b bigint integer nil\n"
               "r.slot.c = c bigint integer error\n"
               "r.key = b, a\n",
               &m, eb, sizeof eb) != 0);
  EXPECT(err_is(eb,
                "clpdaemon: %s:3: o.key: 'a' listed more than once\n"
                "clpdaemon: %s:6: p.key: 'wf-id' is always the first key slot "
                "and must not be listed\n"
                "clpdaemon: %s:9: q.key: empty list item\n"
                "clpdaemon: %s:12: r.slot.wf-id: a key slot needs null policy "
                "error, got 'nil'\n"
                "clpdaemon: %s:13: r.slot.a: a key slot needs null policy "
                "error, got 'default'\n"
                "clpdaemon: %s:14: r.slot.b: a key slot needs null policy "
                "error, got 'nil'\n"));
  clpd_orm_free(&m);
  /* A key slot or wf-id without a column line is left to the package
   * check (unknown or unmapped slot); resolve_key then fails. */
  EXPECT(parse("o.table = t\no.owned = no\no.key = a\n"
               "o.slot.wf-id = wf_id text string error\n",
               &m, eb, sizeof eb) == 0);
  EXPECT(err_is(eb, ""));
  EXPECT(m.template_count == 1 && clpd_orm_resolve_key(&m.templates[0]) == -1 &&
         m.templates[0].key_columns == NULL);
  clpd_orm_free(&m);
  EXPECT(parse("o.table = t\no.owned = no\n", &m, eb, sizeof eb) == 0);
  EXPECT(m.template_count == 1 && clpd_orm_resolve_key(&m.templates[0]) == -1);
  clpd_orm_free(&m);
}

/* Within one file: one column per slot of a template, one template per
 * table, one alias per sequence; the same column name in two tables and
 * a table named like a sequence alias are fine. */
static void test_shared_names(void) {
  clpd_orm m;
  char eb[4096];
  EXPECT(parse("o.table = t\n"
               "o.owned = no\n"
               "o.slot.wf-id = wf_id text string error\n"
               "o.slot.a = amount bigint integer nil\n"
               "o.slot.b = amount double float nil\n"
               "o.slot.c = wf_id text string nil\n"
               "p.table = t\n"
               "p.owned = yes\n"
               "p.slot.wf-id = wf_id text string error\n"
               "p.slot.a = amount bigint integer nil\n"
               "q.table = order_no\n"
               "q.owned = yes\n"
               "seq.order_no = s\n"
               "seq.order_no.owned = yes\n"
               "seq.other = s\n"
               "seq.other.owned = no\n",
               &m, eb, sizeof eb) != 0);
  EXPECT(err_is(eb,
                "clpdaemon: %s:5: o.slot.b: column 'amount' is already mapped "
                "by slot 'a' on line 4\n"
                "clpdaemon: %s:6: o.slot.c: column 'wf_id' is already mapped "
                "by slot 'wf-id' on line 3\n"
                "clpdaemon: %s:7: p.table: table 't' is already mapped by "
                "template 'o' on line 1\n"
                "clpdaemon: %s:15: seq.other: sequence 's' is already mapped "
                "by alias 'order_no' on line 13\n"));
  clpd_orm_free(&m);
}

/* T9e (L87): clpd_sequence is the daemon's own counter table (the SQLite
 * stand-in for sequences), reserved on every backend: a mapping cannot
 * name it, and the template is not reported as missing its table. A
 * sequence of that name stays allowed (it is no table). */
static void test_reserved_table(void) {
  clpd_orm m;
  char eb[2048];
  EXPECT(parse("o.table = clpd_sequence\n"
               "o.owned = yes\n"
               "o.slot.wf-id = wf_id text string error\n"
               "p.table = clpd_sequence_x\n"
               "p.owned = no\n"
               "p.slot.wf-id = wf_id text string error\n"
               "seq.n = clpd_sequence\n"
               "seq.n.owned = yes\n",
               &m, eb, sizeof eb) != 0);
  EXPECT(err_is(eb, "clpdaemon: %s:1: o.table: table name 'clpd_sequence' is "
                    "reserved (the daemon's sequence counter table)\n"));
  clpd_orm_free(&m);
  EXPECT(parse("p.table = clpd_sequence_x\n"
               "p.owned = no\n"
               "p.slot.wf-id = wf_id text string error\n"
               "seq.n = clpd_sequence\n"
               "seq.n.owned = yes\n",
               &m, eb, sizeof eb) == 0);
  EXPECT(err_is(eb, ""));
  clpd_orm_free(&m);
}

/* T10a: workflow_instance is the daemon's instance table, reserved like
 * clpd_sequence; a sequence of that name is not a table mapping. */
static void test_reserved_instance_table(void) {
  clpd_orm m;
  char eb[2048];
  EXPECT(parse("o.table = workflow_instance\n"
               "o.owned = no\n"
               "o.slot.wf-id = wf_id text string error\n"
               "p.table = workflow_instances\n"
               "p.owned = no\n"
               "p.slot.wf-id = wf_id text string error\n",
               &m, eb, sizeof eb) != 0);
  EXPECT(err_is(eb, "clpdaemon: %s:1: o.table: table name 'workflow_instance' "
                    "is reserved (the daemon's workflow instance table)\n"));
  clpd_orm_free(&m);
  /* T10b (L91): also as a sequence name (PostgreSQL keeps tables and
   * sequences in one namespace); a similar name is fine. */
  EXPECT(parse("seq.n = workflow_instance\n"
               "seq.n.owned = no\n"
               "seq.m = workflow_instance_seq\n"
               "seq.m.owned = yes\n",
               &m, eb, sizeof eb) != 0);
  EXPECT(err_is(eb, "clpdaemon: %s:1: seq.n: sequence name 'workflow_instance' "
                    "is reserved (the daemon's workflow instance table)\n"));
  clpd_orm_free(&m);
}

/* T10a (L90): <tpl>.sync = auto|manual, default auto. */
static void test_sync(void) {
  clpd_orm m;
  char eb[2048];
  EXPECT(parse("a.table = ta\n"
               "a.owned = yes\n"
               "a.slot.wf-id = wf_id text string error\n"
               "b.table = tb\n"
               "b.owned = yes\n"
               "b.sync = manual\n"
               "b.slot.wf-id = wf_id text string error\n"
               "c.sync = auto\n"
               "c.table = tc\n"
               "c.owned = no\n"
               "c.slot.wf-id = wf_id text string error\n",
               &m, eb, sizeof eb) == 0);
  EXPECT(err_is(eb, ""));
  EXPECT(m.template_count == 3);
  if (m.template_count == 3) {
    EXPECT(m.templates[0].sync == CLPD_ORM_SYNC_AUTO &&
           m.templates[0].sync_line == 0);
    EXPECT(m.templates[1].sync == CLPD_ORM_SYNC_MANUAL &&
           m.templates[1].sync_line == 6);
    EXPECT(m.templates[2].sync == CLPD_ORM_SYNC_AUTO &&
           m.templates[2].sync_line == 8 && m.templates[2].line == 8);
  }
  clpd_orm_free(&m);
  EXPECT(parse("a.table = ta\n"
               "a.owned = yes\n"
               "a.sync = Manual\n"
               "a.sync = manual\n"
               "a.slot.wf-id = wf_id text string error\n"
               "b.table = tb\n"
               "b.owned = yes\n"
               "b.sync =\n"
               "b.slot.wf-id = wf_id text string error\n",
               &m, eb, sizeof eb) != 0);
  EXPECT(err_is(eb, "clpdaemon: %s:3: a.sync: expected auto or manual, got "
                    "'Manual'\n"
                    "clpdaemon: %s:4: duplicate key 'a.sync' (first set on line "
                    "3)\n"
                    "clpdaemon: %s:8: b.sync: expected auto or manual, got ''\n"));
  if (m.template_count == 2) {
    EXPECT(m.templates[0].sync == CLPD_ORM_SYNC_AUTO);
    EXPECT(m.templates[1].sync == CLPD_ORM_SYNC_AUTO);
  }
  clpd_orm_free(&m);
  EXPECT(strcmp(clpd_orm_sync_name(CLPD_ORM_SYNC_AUTO), "auto") == 0);
  EXPECT(strcmp(clpd_orm_sync_name(CLPD_ORM_SYNC_MANUAL), "manual") == 0);
  EXPECT(clpd_orm_sync_name(CLPD_ORM_SYNC_COUNT) == NULL);
}

static void test_missing_required(void) {
  clpd_orm m;
  char eb[4096];
  EXPECT(parse("o.slot.wf-id = wf_id text string error\n"
               "p.table = p\n"
               "q.owned = no\n"
               "q.key =\n"
               "seq.a.owned = no\n"
               "seq.b = b\n"
               "o.table = Bad\n",
               &m, eb, sizeof eb) != 0);
  /* An invalid value still counts as set: o.table is not missing. */
  EXPECT(err_is(eb,
                "clpdaemon: %s:7: o.table: expected an identifier matching "
                "[a-z_][a-z0-9_]* (at most 63 bytes), got 'Bad'\n"
                "clpdaemon: %s: missing required key 'o.owned'\n"
                "clpdaemon: %s: missing required key 'p.owned'\n"
                "clpdaemon: %s: missing required key 'q.table'\n"
                "clpdaemon: %s: missing required key 'seq.a'\n"
                "clpdaemon: %s: missing required key 'seq.b.owned'\n"));
  clpd_orm_free(&m);
}

/* Line errors keep file order; NUL bytes and CRLF go through kvfile. */
static void test_kvfile_rules(void) {
  clpd_orm m;
  char eb[1024];
  FILE *f;
  snprintf(path, sizeof path, "%s/orm.conf", root);
  EXPECT(parse("o.table = t\r\no.owned = yes\r\n"
               "o.slot.wf-id = wf_id text string error\r\n",
               &m, eb, sizeof eb) == 0);
  EXPECT(err_is(eb, ""));
  EXPECT(m.template_count == 1 && strcmp(m.templates[0].table, "t") == 0 &&
         m.templates[0].columns[0].null == CLPD_ORM_NULL_ERROR);
  clpd_orm_free(&m);
  f = fopen(path, "w");
  if (f == NULL) exit(2);
  fwrite("o.table = t\no.ow\0ned = yes\n", 1, 27, f);
  fclose(f);
  {
    FILE *err = tmpfile();
    size_t n;
    if (err == NULL) exit(2);
    EXPECT(clpd_orm_parse(path, &m, err) != 0);
    rewind(err);
    n = fread(eb, 1, sizeof eb - 1, err);
    eb[n] = '\0';
    fclose(err);
    remove(path);
  }
  EXPECT(err_is(eb, "clpdaemon: %s:2: line contains a NUL byte\n"
                    "clpdaemon: %s: missing required key 'o.owned'\n"));
  clpd_orm_free(&m);
}

static void test_type_names(void) {
  static const char *const sql[] = {"bigint", "double", "text", "bytea",
                                    "boolean", "timestamptz", "jsonb",
                                    "numeric"};
  static const char *const pg[] = {"BIGINT", "DOUBLE PRECISION", "TEXT",
                                   "BYTEA", "BOOLEAN", "TIMESTAMPTZ",
                                   "JSONB", "NUMERIC"};
  static const char *const lite[] = {"INTEGER", "REAL", "TEXT", "BLOB",
                                     "INTEGER", "TEXT", "TEXT", "TEXT"};
  int i;
  EXPECT(CLPD_ORM_SQL_TYPE_COUNT == 8);
  for (i = 0; i < CLPD_ORM_SQL_TYPE_COUNT; i++) {
    EXPECT(strcmp(clpd_orm_sql_type_name(i), sql[i]) == 0);
    EXPECT(strcmp(clpd_orm_backend_type(i, "postgresql"), pg[i]) == 0);
    EXPECT(strcmp(clpd_orm_backend_type(i, "sqlite"), lite[i]) == 0);
    EXPECT(clpd_orm_backend_type(i, "mysql") == NULL);
  }
  EXPECT(strcmp(clpd_orm_clips_type_name(CLPD_ORM_INTEGER), "integer") == 0 &&
         strcmp(clpd_orm_clips_type_name(CLPD_ORM_FLOAT), "float") == 0 &&
         strcmp(clpd_orm_clips_type_name(CLPD_ORM_STRING), "string") == 0 &&
         strcmp(clpd_orm_clips_type_name(CLPD_ORM_SYMBOL), "symbol") == 0);
  EXPECT(strcmp(clpd_orm_null_name(CLPD_ORM_NULL_NIL), "nil") == 0 &&
         strcmp(clpd_orm_null_name(CLPD_ORM_NULL_DEFAULT), "default") == 0 &&
         strcmp(clpd_orm_null_name(CLPD_ORM_NULL_ERROR), "error") == 0);
}

/* T9b (L83): which CLIPS types each sql type holds, and the list the
 * load-time message names. */
static void test_type_fits(void) {
  /* integer, float, string, symbol */
  static const int fits[CLPD_ORM_SQL_TYPE_COUNT][4] = {
      {1, 0, 0, 0}, /* bigint */
      {0, 1, 0, 0}, /* double */
      {0, 0, 1, 1}, /* text */
      {0, 0, 1, 0}, /* bytea */
      {1, 0, 0, 0}, /* boolean */
      {0, 0, 1, 1}, /* timestamptz */
      {0, 0, 1, 1}, /* jsonb */
      {0, 1, 1, 1}, /* numeric */
  };
  static const char *const lists[] = {
      "integer", "float", "string or symbol", "string", "integer",
      "string or symbol", "string or symbol", "float, string or symbol"};
  char buf[CLPD_ORM_TYPE_LIST_SIZE];
  int i, j;
  for (i = 0; i < CLPD_ORM_SQL_TYPE_COUNT; i++) {
    for (j = 0; j < CLPD_ORM_CLIPS_TYPE_COUNT; j++)
      EXPECT(clpd_orm_type_fits(i, j) == fits[i][j]);
    EXPECT(clpd_orm_type_fits_list(i, buf, sizeof buf) == buf &&
           strcmp(buf, lists[i]) == 0);
  }
  EXPECT(clpd_orm_type_fits(CLPD_ORM_SQL_TYPE_COUNT, CLPD_ORM_INTEGER) == 0);
  EXPECT(clpd_orm_type_fits_list(CLPD_ORM_SQL_TYPE_COUNT, buf, sizeof buf) ==
         NULL);
}

int main(void) {
  if (mkdtemp(root) == NULL) {
    perror("orm_test");
    return 2;
  }
  test_valid();
  test_empty();
  test_cannot_open();
  test_unknown_and_duplicate_keys();
  test_duplicate_slot_after_invalid();
  test_identifiers();
  test_values();
  test_keys();
  test_shared_names();
  test_reserved_table();
  test_reserved_instance_table();
  test_sync();
  test_missing_required();
  test_kvfile_rules();
  test_type_names();
  test_type_fits();
  rmdir(root);
  if (failures != 0) {
    fprintf(stderr, "%d orm checks failed\n", failures);
    return 1;
  }
  puts("orm checks passed");
  return 0;
}
