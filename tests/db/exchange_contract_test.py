#!/usr/bin/env python3
"""Runs the SQL of docs/exchange-contract.md on a throwaway PostgreSQL cluster (T12a).

usage: pg_cluster.sh <python> tests/db/exchange_contract_test.py [--doc <contract.md>]
           [--schemas <dir>]
       (pg_cluster.sh appends "<conninfo>" <password>)

The script extracts every ```sql block of the contract itself, runs the DDL of
section 3 under the roles the contract states (the role block as a
CREATEROLE role, the rest as the table owner clpd_engine), PREPAREs every
parameterized statement exactly as written, and runs a scenario: connector
statements as connector roles (one logs in with a password), intake and
engine statements as a member of the owner role, review and reprocess as an
operator; then row-level security cases and a concurrent claim run.

It connects as the cluster superuser clpd_admin (the password comes from the
PGPASSFILE that pg_cluster.sh exports), so the cluster must be throwaway.

Exit status: 0 all checks passed, 1 a check failed, 2 usage or setup error,
77 psycopg (3) or jsonschema is not installed (ctest SKIP_RETURN_CODE).
"""
import argparse
import json
import os
import random
import re
import sys
import threading
import time

try:
    import psycopg
    from jsonschema import Draft202012Validator as V
except ImportError as e:
    print('exchange_contract_test.py: %s; skipped' % e, file=sys.stderr)
    sys.exit(77)

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
ap = argparse.ArgumentParser()
ap.add_argument('--doc', default=os.path.join(ROOT, 'docs', 'exchange-contract.md'))
ap.add_argument('--schemas', default=os.path.join(ROOT, 'docs', 'schemas'))
ap.add_argument('conninfo')
ap.add_argument('password', nargs='?')
args = ap.parse_args()

# ---------------------------------------------------------------- extraction
FENCE_OPEN = re.compile(r'^(\s*)```sql\s*$')
FENCE_CLOSE = re.compile(r'^\s*```\s*$')
HEADING = re.compile(r'^#{2,3} (\d+(?:\.\d+)?)[ .]')


def extract(path):
    """[(section, statement)] for every ```sql block, in document order."""
    out, section, block, indent = [], None, None, ''
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')
            if block is None:
                m = HEADING.match(line)
                if m:
                    section = m.group(1)
                m = FENCE_OPEN.match(line)
                if m:
                    block, indent = [], m.group(1)
                continue
            if FENCE_CLOSE.match(line):
                text = '\n'.join(block)
                for st in re.split(r';[ \t]*(?:\n|$)', text):
                    body = '\n'.join(x for x in st.split('\n') if not x.strip().startswith('--')).strip()
                    if body:
                        out.append((section, st.strip()))
                block = None
                continue
            block.append(line[len(indent):] if line.startswith(indent) else line)
    return out


try:
    STMTS = extract(args.doc)
    SCHEMAS = {n: json.load(open(os.path.join(args.schemas, n), encoding='utf-8'))
               for n in ('envelope.schema.json', 'delivery-failed@1.json')}
except (OSError, ValueError) as e:
    print('exchange_contract_test.py: %s' % e, file=sys.stderr)
    sys.exit(2)

fails = 0
checks = 0


def chk(name, ok, extra=''):
    global fails, checks
    checks += 1
    print(('PASS ' if ok else 'FAIL ') + name + (' ' + repr(extra) if extra != '' and not ok else ''), flush=True)
    if not ok:
        fails += 1


def pick(section, *needles, exclude=()):
    hits = [s for sec, s in STMTS if sec == section and all(n in s for n in needles)
            and not any(x in s for x in exclude)]
    if len(hits) != 1:
        print('exchange_contract_test.py: %d statements in section %s match %r' % (len(hits), section, needles),
              file=sys.stderr)
        sys.exit(2)
    return hits[0]


DML = {
    'insert': pick('6.2', 'ON CONFLICT (endpoint_logico, direccion, msg_id_externo) DO NOTHING'),
    'dupid': pick('6.2', 'SELECT id', 'msg_id_externo = $2::text'),
    'assign': pick('6.5', "SET estado = 'ASIGNADO'"),
    'nocorr': pick('6.5', "SET estado = 'SIN_CORRELACION'"),
    'consume': pick('6.5', "SET estado = 'CONSUMIDO', actualizado = now()"),
    'drop': pick('6.5', "SET estado = 'CONSUMIDO', error = $3::text"),
    'engerr': pick('6.5', "SET estado = 'ERROR', error = $3::text"),
    'claim': pick('7.2', "SET estado = 'EN_ENVIO'"),
    'extend': pick('7.3', 'SET lease_hasta'),
    'settle': pick('7.4', "$4::text IN ('ENVIADO', 'CONFIRMADO')"),
    'retry': pick('7.5', "SET estado = 'REINTENTO'"),
    'dead': pick('7.6', "'delivery-failed:' || id"),
    'confirm': pick('7.7', "SET estado = 'CONFIRMADO'"),
    'review': pick('9', 'SELECT id'),
    'reproc': pick('9', "SET estado = 'RECIBIDO'", exclude=('wf_id = $2',)),
    'reproc_fix': pick('9', "SET estado = 'RECIBIDO'", 'wf_id = $2::text', 'payload = $3::jsonb'),
    'myeps': pick('3.5', 'SELECT endpoint_logico FROM conector_endpoint'),
}
DDL = [(sec, s) for sec, s in STMTS if sec.startswith('3') and s not in DML.values()]
chk('contract: %d sql statements, %d DDL in section 3' % (len(STMTS), len(DDL)), len(DDL) >= 20, len(DDL))

# ---------------------------------------------------------------- connections
BASE = args.conninfo.replace('user=clpd_test', 'user=clpd_admin')
if BASE == args.conninfo:
    print('exchange_contract_test.py: conninfo has no user=clpd_test (run through pg_cluster.sh)', file=sys.stderr)
    sys.exit(2)


def conn(role=None, login=None, password=None):
    if login:
        c = psycopg.connect(BASE, user=login, password=password, autocommit=True,
                            cursor_factory=psycopg.ClientCursor)
    else:
        c = psycopg.connect(BASE, autocommit=True, cursor_factory=psycopg.ClientCursor)
        if role:
            c.execute('SET ROLE ' + role)
    for k, s in DML.items():
        c.execute('PREPARE %s AS %s' % (k, s))  # no explicit types: the casts in the text fix them
    return c


def ex(c, k, *a):
    ph = ','.join(['%s'] * len(a))
    cur = c.execute('EXECUTE %s%s' % (k, '(' + ph + ')' if a else ''), a)
    if cur.description is None:
        return cur.rowcount
    return cur.fetchall()


def sqlstate(f):
    try:
        f()
        return None
    except psycopg.Error as e:
        return e.sqlstate


try:
    adm = psycopg.connect(BASE, autocommit=True)
except psycopg.Error as e:
    print('exchange_contract_test.py: cannot connect: %s' % e, file=sys.stderr)
    sys.exit(2)


def one(q, *a):
    return adm.execute(q, a).fetchone()


# Installation roles the contract leaves to the DBA: the owner, a node that is a
# member of it, a CREATEROLE role for the role block, a second connector, an operator.
CONN_PW = os.urandom(16).hex()
adm.execute("""
CREATE ROLE clpd_engine NOLOGIN;
GRANT CREATE ON SCHEMA public TO clpd_engine;
CREATE ROLE clpd_node LOGIN IN ROLE clpd_engine;
CREATE ROLE clpd_dba NOLOGIN CREATEROLE;
""")

# ---------------------------------------------------------------- DDL, under the stated roles
ddl_err = None
for sec, s in DDL:
    who = 'clpd_dba' if 'CREATE ROLE' in s else 'clpd_engine'
    try:
        adm.execute('SET ROLE ' + who)
        adm.execute(s)
    except psycopg.Error as e:
        ddl_err = (sec, who, s[:60], e.sqlstate, str(e).strip())
        break
    finally:
        adm.execute('RESET ROLE')
chk('DDL of section 3 runs as stated (role block as CREATEROLE, rest as owner)', ddl_err is None, ddl_err)
if ddl_err:
    print('checks %d fails %d' % (checks, fails))
    sys.exit(1)
adm.execute("""
CREATE ROLE connector_crm LOGIN IN ROLE clpd_connector;
CREATE ROLE operator_x LOGIN IN ROLE clpd_operator;
ALTER ROLE connector_installer PASSWORD '%s';
SET ROLE clpd_engine;
INSERT INTO conector_endpoint (rol, endpoint_logico) VALUES ('connector_crm', 'crm');
RESET ROLE;
""" % CONN_PW)

nidx = one("select count(*) from pg_indexes where tablename='mensaje_intercambio' "
           "and indexname not in ('mensaje_intercambio_pkey','mensaje_intercambio_dedup')")[0]
npart = one("select count(*) from pg_indexes where tablename='mensaje_intercambio' and indexdef like '%%WHERE%%'")[0]
chk('8 extra indexes, 7 partial', (nidx, npart) == (8, 7), (nidx, npart))
rev = one("select indexdef from pg_indexes where indexname='mensaje_intercambio_revision'")[0]
chk('A5: revision index holds only IN rows', "direccion = 'IN'" in rev, rev)
own = adm.execute("""select c.relname, pg_get_userbyid(c.relowner), c.relrowsecurity, c.relforcerowsecurity
  from pg_class c where relname in ('mensaje_intercambio','correlacion','conector_endpoint') order by 1""").fetchall()
chk('A7: tables owned by clpd_engine, RLS enabled, not forced',
    [r[1:] for r in own] == [('clpd_engine', True, False)] * 3, own)
npol = one("select count(*) from pg_policies where tablename in ('mensaje_intercambio','correlacion','conector_endpoint')")[0]
chk('A7: 9 policies', npol == 9, npol)

# claim log (superuser trigger, outside the contract) for the concurrency checks
adm.execute("""CREATE TABLE claimlog (seq bigserial, id bigint, wf text, ev text, who text, tok int);
CREATE FUNCTION lg() RETURNS trigger LANGUAGE plpgsql SECURITY DEFINER AS $$ BEGIN
 IF NEW.direccion='OUT' AND (NEW.estado IS DISTINCT FROM OLD.estado OR NEW.intentos<>OLD.intentos) THEN
  INSERT INTO claimlog(id,wf,ev,who,tok) VALUES (NEW.id,NEW.wf_id,NEW.estado,NEW.asignado_a,NEW.intentos); END IF;
 RETURN NEW; END $$;
CREATE TRIGGER lg AFTER UPDATE ON mensaje_intercambio FOR EACH ROW EXECUTE FUNCTION lg();""")

eng = conn('clpd_node')          # engine node / intake: member of the owner role
cn = conn(login='connector_installer', password=CONN_PW)   # a real connector login
crm = conn('connector_crm')
op = conn('operator_x')
chk('connector session: session_user and current_user are the connector role',
    cn.execute('select session_user::text, current_user::text').fetchone() == ('connector_installer',) * 2)


def P(t, **d):
    return json.dumps({"schema": t + "@1", "data": d})


def out(wf, cid, ep='installer', t='work-order-request'):
    return eng.execute("INSERT INTO mensaje_intercambio (direccion,wf_id,correlacion_id,endpoint_logico,tipo_mensaje,"
                       "payload,estado) VALUES ('OUT',%s,%s,%s,%s,%s,'PENDIENTE') RETURNING id",
                       (wf, cid, ep, t, P(t, address='x'))).fetchone()[0]


def row(i, cols='estado'):
    return one('select %s from mensaje_intercambio where id=%%s' % cols, i)


def in_row(mid, ep='installer', **kw):
    cols = {'direccion': 'IN', 'endpoint_logico': ep, 'tipo_mensaje': 'work-order-closed',
            'msg_id_externo': mid, 'payload': woc, 'estado': 'RECIBIDO'}
    cols.update(kw)
    q = 'INSERT INTO mensaje_intercambio (%s) VALUES (%s) RETURNING id' % (','.join(cols), ','.join(['%s'] * len(cols)))
    return eng.execute(q, list(cols.values())).fetchone()[0]


woc = json.dumps({"schema": "work-order-closed@1",
                  "data": {"work-order": "OI-55", "closed-at": "2026-11-17T10:22:00Z", "result": "ok"}})

# ================================================================ IN: insert, dedup, F1
r1042 = out('fi-000123', 'fi-000123:7')
chk('engine (member of owner) inserts OUT row 1042', r1042 > 0)
r = ex(cn, 'insert', 'installer', 'work-order-closed', 'OI-55', 'evt-9f3c', None, woc, None)
chk('6.2 insert as connector returns id', len(r) == 1, r)
r2210 = r[0][0]
r = ex(cn, 'insert', 'installer', 'work-order-closed', 'OI-55', 'evt-9f3c', None, woc, None)
chk('6.2 duplicate insert returns no row', r == [], r)
chk('F1: lookup returns the first insert id', ex(cn, 'dupid', 'installer', 'evt-9f3c') == [(r2210,)])
chk('duplicate stored once', one("select count(*) from mensaje_intercambio where msg_id_externo='evt-9f3c'")[0] == 1)
for outcome in ('commit', 'rollback'):
    a = conn('connector_installer')
    b = conn('connector_installer')
    mid = 'evt-conc-' + outcome
    a.execute('BEGIN')
    ra = ex(a, 'insert', 'installer', 'work-order-closed', None, mid, None, woc, None)
    res = {}

    def second():
        b.execute('BEGIN')
        res['ins'] = ex(b, 'insert', 'installer', 'work-order-closed', None, mid, None, woc, None)
        res['id'] = ex(b, 'dupid', 'installer', mid)
        b.execute('COMMIT')
    th = threading.Thread(target=second)
    th.start()
    blocked = False   # the second backend waits on a lock while the first transaction is open
    for _ in range(200):
        if one("select count(*) from pg_stat_activity where pid = %s and wait_event_type = 'Lock'",
               b.info.backend_pid)[0] == 1:
            blocked = True
            break
        time.sleep(0.05)
    a.execute(outcome.upper())
    th.join(10)
    if outcome == 'commit':
        ok = blocked and res['ins'] == [] and res['id'] == ra
    else:
        ok = blocked and len(res['ins']) == 1 and res['id'] == res['ins']
    chk('F1: concurrent duplicate (%s): blocked, then the lookup returns the stored id' % outcome, ok, (blocked, ra, res))
    a.close()
    b.close()

# ================================================================ CHECK constraints
negs = {
    'IN without msg_id_externo': "('IN',NULL,NULL,'installer','work-order-closed',NULL,'%s','RECIBIDO',NULL,NULL)",
    'OUT without wf_id': "('OUT',NULL,'c1','installer','work-order-closed',NULL,'%s','PENDIENTE',NULL,NULL)",
    'IN with OUT state': "('IN',NULL,NULL,'installer','work-order-closed','m1','%s','PENDIENTE',NULL,NULL)",
    'OUT with IN state': "('OUT','w','c2','installer','work-order-closed',NULL,'%s','RECIBIDO',NULL,NULL)",
    'ASIGNADO without node': "('IN','w',NULL,'installer','work-order-closed','m2','%s','ASIGNADO',NULL,NULL)",
    'EN_ENVIO without lease': "('OUT','w','c3','installer','work-order-closed',NULL,'%s','EN_ENVIO','gw',NULL)",
    'bad endpoint': "('IN',NULL,NULL,'Installer','work-order-closed','m3','%s','RECIBIDO',NULL,NULL)",
}
for n, v in negs.items():
    st = sqlstate(lambda: eng.execute(
        "INSERT INTO mensaje_intercambio (direccion,wf_id,correlacion_id,endpoint_logico,tipo_mensaje,msg_id_externo,"
        "payload,estado,asignado_a,lease_hasta) VALUES " + v % woc.replace("'", "''")))
    chk('CHECK rejects ' + n, st == '23514', st)
for n, p in {'schema/tipo mismatch': json.dumps({"schema": "other@1", "data": {}}),
             'data not object': json.dumps({"schema": "work-order-closed@1", "data": []}),
             'payload array': '[]',
             'over 64KiB': json.dumps({"schema": "work-order-closed@1", "data": {"s": "a" * 70000}})}.items():
    st = sqlstate(lambda: ex(cn, 'insert', 'installer', 'work-order-closed', None, 'neg-' + n, None, p, None))
    chk('CHECK rejects payload ' + n + ' (connector insert)', st == '23514', st)

# ================================================================ A1: drop of a message for an ended instance
asg = in_row('late-2')
chk('intake assigns as engine member', ex(eng, 'assign', asg, 'fi-9', 'n1') == 1)
reason = "instance 'fi-9' is completed; message dropped"
chk('A1: drop statement 1 row', ex(eng, 'drop', asg, 'n1', reason) == 1)
chk('A1: dropped row CONSUMIDO with the reason', row(asg, 'estado, error') == ('CONSUMIDO', reason))
asg2 = in_row('late-3')
ex(eng, 'assign', asg2, 'fi-9', 'n1')
chk('consume statement 1 row, error NULL', ex(eng, 'consume', asg2, 'n1') == 1
    and row(asg2, 'estado, error') == ('CONSUMIDO', None))

# ================================================================ OUT: claim order, fencing, dead letter
eps = ['installer']
o1 = out('wfA', 'wfA:1')
o2 = out('wfA', 'wfA:2')
o3 = out('wfA', 'wfA:3')
b1 = out('wfB', 'wfB:1')
rows = ex(cn, 'claim', 'gw-1', 120000, eps, 20)
ids = sorted(x[0] for x in rows)
chk('claim: one row per instance (1042, wfA:1, wfB:1)', ids == sorted([r1042, o1, b1]), ids)
chk('claim token = intentos 1', all(x[7] == 1 for x in rows))
cn.execute('BEGIN')
s = ex(cn, 'settle', r1042, 'gw-1', 1, 'CONFIRMADO')
ins = ex(cn, 'insert', 'installer', 'work-order-created', 'OI-55', 'response:fi-000123:7', 'fi-000123:7',
         json.dumps({"schema": "work-order-created@1", "data": {"order": "OI-55"}}), None)
cn.execute('COMMIT')
chk('settle CONFIRMADO + response in one tx', s == 1 and len(ins) == 1, (s, ins))
chk('1042 CONFIRMADO, lease NULL', row(r1042, 'estado, lease_hasta') == ('CONFIRMADO', None))
chk('settle rejects another target state', ex(cn, 'settle', b1, 'gw-1', 1, 'ERROR') == 0 and row(b1)[0] == 'EN_ENVIO')
ex(cn, 'retry', o1, 'gw-1', 1, 3600000, 'HTTP 503')
chk('REINTENTO not due holds back later rows of the instance', ex(cn, 'claim', 'gw-1', 120000, eps, 20) == [])
eng.execute("UPDATE mensaje_intercambio SET siguiente_intento=now()-interval '1s' WHERE id=%s", (o1,))
rows = ex(cn, 'claim', 'gw-1', 120000, eps, 20)
chk('due REINTENTO reclaimed with token 2, not wfA:2', [(x[0], x[7]) for x in rows] == [(o1, 2)], rows)
ex(cn, 'settle', o1, 'gw-1', 2, 'ENVIADO')
rows = ex(cn, 'claim', 'gw-1', 120000, eps, 20)
chk('ENVIADO releases wfA:2', [x[0] for x in rows] == [o2], rows)
cn.execute('BEGIN')
c1 = ex(cn, 'confirm', 'wfA:1', 'installer')
cn.execute('COMMIT')
chk('7.7 confirm ENVIADO -> CONFIRMADO', c1 == 1 and row(o1)[0] == 'CONFIRMADO')
eng.execute("UPDATE mensaje_intercambio SET lease_hasta=now()-interval '1ms' WHERE id=%s", (o2,))
c2 = conn('connector_installer')
rows = ex(c2, 'claim', 'gw-2', 120000, eps, 20)
chk('expired lease reclaimed by gw-2 with token 2', [(x[0], x[7]) for x in rows] == [(o2, 2)], rows)
chk('stale extend 0 rows', ex(cn, 'extend', o2, 'gw-1', 1, 60000) == 0)
cols = 'estado,asignado_a,intentos,lease_hasta,error'
before = row(o2, cols)
for k, a in {'settle': ('CONFIRMADO',), 'retry': (1000, 'x'), 'dead': ('x',)}.items():
    cn.execute('BEGIN')
    res = ex(cn, k, o2, 'gw-1', 1, *a)
    cn.execute('COMMIT')
    chk('stale %s by gw-1 (token 1) changes nothing' % k, res in (0, []) and row(o2, cols) == before)
eng.execute("UPDATE mensaje_intercambio SET lease_hasta=now()-interval '1ms' WHERE id=%s", (o2,))
ex(c2, 'claim', 'gw-2', 120000, eps, 20)
chk('same name, stale token 2 after reclaim token 3: 0 rows', ex(c2, 'settle', o2, 'gw-2', 2, 'CONFIRMADO') == 0)
chk('valid extend 1 row', ex(c2, 'extend', o2, 'gw-2', 3, 60000) == 1)
dl = ex(c2, 'dead', o2, 'gw-2', 3, 'HTTP 400 bad address')
chk('dead letter as connector returns 1 IN id', len(dl) == 1, dl)
chk('OUT ERROR with reason', row(o2, 'estado,error,lease_hasta') == ('ERROR', 'HTTP 400 bad address', None))
dfr = row(dl[0][0], 'direccion,estado,correlacion_id,endpoint_logico,tipo_mensaje,msg_id_externo,payload,wf_id')
chk('delivery-failed row fields', dfr[:6] == ('IN', 'RECIBIDO', 'wfA:2', 'installer', 'delivery-failed',
                                              'delivery-failed:%d' % o2) and dfr[7] is None, dfr)
chk('delivery-failed payload valid (own schema + envelope)',
    V(SCHEMAS['delivery-failed@1.json']).is_valid(dfr[6]) and V(SCHEMAS['envelope.schema.json']).is_valid(dfr[6]))
chk('dead letter again (fenced) -> 0 rows', ex(c2, 'dead', o2, 'gw-2', 3, 'again') == [])
rows = ex(c2, 'claim', 'gw-2', 120000, eps, 20)
chk('OUT ERROR releases wfA:3', o3 in [x[0] for x in rows], rows)
eng.execute("UPDATE mensaje_intercambio SET intentos=8, lease_hasta=now()-interval '1ms' WHERE id=%s", (o3,))
rows = ex(cn, 'claim', 'gw-1', 120000, eps, 20)
chk('reclaim after the last attempt gives intentos 9', [(x[0], x[7]) for x in rows if x[0] == o3] == [(o3, 9)], rows)
chk('rule-5 dead letter ok', len(ex(cn, 'dead', o3, 'gw-1', 9, 'lease expired on the last attempt')) == 1)
ex(cn, 'settle', b1, 'gw-1', 1, 'CONFIRMADO')

# ================================================================ A8: answer before the settle
a1 = out('wfC', 'wfC:1')
a2 = out('wfC', 'wfC:2')
rows = ex(cn, 'claim', 'gw-1', 120000, eps, 20)
chk('A8 setup: wfC:1 claimed', [(x[0], x[7]) for x in rows] == [(a1, 1)], rows)
cn.execute('BEGIN')
ex(cn, 'insert', 'installer', 'work-order-created', 'OI-60', 'evt-a8-1', 'wfC:1',
   json.dumps({"schema": "work-order-created@1", "data": {"order": "OI-60"}}), None)
k = ex(cn, 'confirm', 'wfC:1', 'installer')
cn.execute('COMMIT')
chk('A8: answer before the settle confirms the EN_ENVIO row',
    k == 1 and row(a1, 'estado, lease_hasta') == ('CONFIRMADO', None), (k, row(a1, 'estado, lease_hasta')))
chk('A8: the late settle ENVIADO updates 0 rows; row stays CONFIRMADO',
    ex(cn, 'settle', a1, 'gw-1', 1, 'ENVIADO') == 0 and row(a1)[0] == 'CONFIRMADO')
rows = ex(cn, 'claim', 'gw-1', 120000, eps, 20)
chk('A8: CONFIRMADO releases wfC:2', [x[0] for x in rows] == [a2], rows)
ex(cn, 'retry', a2, 'gw-1', 1, 3600000, 'timeout')
chk('A8: answer to a REINTENTO row confirms it', ex(cn, 'confirm', 'wfC:2', 'installer') == 1
    and row(a2, 'estado, siguiente_intento, lease_hasta') == ('CONFIRMADO', None, None))
chk('A8: confirm on a CONFIRMADO or ERROR row: 0 rows',
    ex(cn, 'confirm', 'wfC:2', 'installer') == 0 and ex(cn, 'confirm', 'wfA:2', 'installer') == 0 and row(o2)[0] == 'ERROR')

# ================================================================ intake window, engine ERROR, review, reprocess
nc = in_row('evt-nc', ref_externa='OI-77')
seq = []
for i in range(4):
    n = ex(eng, 'nocorr', nc, 'no instance for installer/OI-77', 30000, 3)
    seq.append((n,) + row(nc, 'intentos, siguiente_intento is not null'))
chk('SIN_CORRELACION: 3 attempts, then siguiente_intento NULL',
    seq[:3] == [(1, 1, True), (1, 2, True), (1, 3, False)], seq)
nc0 = in_row('evt-nc0')
ex(eng, 'nocorr', nc0, 'x', 30000, 0)
chk('window 0: no retry', row(nc0, 'siguiente_intento')[0] is None)
ee = in_row('evt-ee')
ex(eng, 'assign', ee, 'fi-1', 'n1')
chk('engine ERROR 1 row', ex(eng, 'engerr', ee, 'n1', "slot 'x' unknown") == 1)
rv = ex(op, 'review')
rvids = [x[0] for x in rv]
chk('review (operator) lists given-up SIN_CORRELACION and IN ERROR rows', {nc, nc0, ee} <= set(rvids), rvids)
chk('A5: review lists no OUT ERROR row', o2 not in rvids and o3 not in rvids, rvids)
eng.execute('SET enable_seqscan = off')
plan2 = '\n'.join(r[0] for r in eng.execute('EXPLAIN (COSTS OFF) ' + DML['review'].replace(';', '')).fetchall())
eng.execute('RESET enable_seqscan')
chk('A5: the review query can use mensaje_intercambio_revision', 'mensaje_intercambio_revision' in plan2, plan2)
for i in (nc, ee):
    chk('reprocess %d as operator 1 row' % i, ex(op, 'reproc', i) == 1)
chk('reprocessed ERROR row: RECIBIDO, 0, keeps wf_id',
    row(ee, 'estado,intentos,asignado_a,error,wf_id') == ('RECIBIDO', 0, None, None, 'fi-1'))
chk('reprocess on an OUT ERROR row: 0 rows', ex(op, 'reproc', o2) == 0 and row(o2)[0] == 'ERROR')
ex(eng, 'assign', ee, 'fi-1', 'n1')
ex(eng, 'engerr', ee, 'n1', 'wrong instance')
fixed = json.dumps({"schema": "work-order-closed@1",
                    "data": {"work-order": "OI-56", "closed-at": "2026-11-17T10:22:00Z", "result": "failed"}})
chk('A6: reprocess with wf_id = NULL and a new payload as operator', ex(op, 'reproc_fix', ee, None, fixed) == 1
    and row(ee, 'estado,wf_id,payload') == ('RECIBIDO', None, json.loads(fixed)))
ex(eng, 'nocorr', nc0, 'x', 30000, 0)
chk('A6: reprocess routing to an instance (wf_id set) as operator',
    ex(op, 'reproc_fix', nc0, 'fi-42', woc) == 1 and row(nc0, 'estado,wf_id') == ('RECIBIDO', 'fi-42'))
ex(eng, 'assign', ee, 'fi-1', 'n1')
ex(eng, 'engerr', ee, 'n1', 'again')
chk('A6: operator cannot set another state (policy WITH CHECK)', sqlstate(lambda: op.execute(
    "UPDATE mensaje_intercambio SET estado='CONSUMIDO', actualizado=now() WHERE id=%s" % ee)) == '42501'
    and row(ee)[0] == 'ERROR')
ex(op, 'reproc', ee)   # puts ee back to RECIBIDO (it is ERROR after the denied update) for the checks below
for st_ in ('RECIBIDO', 'ASIGNADO', 'CONSUMIDO'):
    oi = in_row('evt-op-' + st_.lower())
    if st_ != 'RECIBIDO':
        ex(eng, 'assign', oi, 'fi-1', 'n1')
    if st_ == 'CONSUMIDO':
        ex(eng, 'consume', oi, 'n1')
    before = row(oi, 'estado,wf_id,asignado_a,payload')
    chk('R1-001: operator cannot put a %s row back to RECIBIDO (0 rows)' % st_, op.execute(
        "UPDATE mensaje_intercambio SET estado='RECIBIDO', wf_id='fi-x', asignado_a=NULL, lease_hasta=NULL, "
        "actualizado=now() WHERE id=%s" % oi).rowcount == 0 and row(oi, 'estado,wf_id,asignado_a,payload') == before,
        row(oi, 'estado,wf_id,asignado_a,payload'))
op_qual = one("select qual from pg_policies where policyname='mensaje_intercambio_operador_update'")[0]
chk('R1-001: operator update policy USING limits the direction and the state',
    all(x in op_qual for x in ('direccion', 'SIN_CORRELACION', 'ERROR')), op_qual)
chk('A6: operator cannot update OUT rows (0 rows)', op.execute(
    "UPDATE mensaje_intercambio SET error='x' WHERE id=%s" % o2).rowcount == 0 and row(o2, 'error')[0] != 'x')
chk('operator cannot change endpoint_logico (42501)', sqlstate(lambda: op.execute(
    "UPDATE mensaje_intercambio SET endpoint_logico='crm' WHERE id=%s" % ee)) == '42501')
chk('operator reads every endpoint and correlacion', op.execute(
    'select count(*) from mensaje_intercambio').fetchone()[0] == one('select count(*) from mensaje_intercambio')[0])

# ================================================================ A7: row-level security
eng.execute("INSERT INTO correlacion (endpoint_logico, ref_externa, wf_id) VALUES ('installer','OI-55','fi-000123'),"
            " ('crm','C-1','fi-000200')")
chk('engine writes correlacion', one('select count(*) from correlacion')[0] == 2)
x1 = out('wfX', 'wfX:1', ep='crm')
xin = in_row('evt-crm-1', ep='crm')
crm_before = row(x1, cols)
ninst = one("select count(*) from mensaje_intercambio where endpoint_logico='installer'")[0]
chk('A7: connector sees only rows of its endpoints',
    cn.execute("select count(*), count(*) filter (where endpoint_logico <> 'installer') from mensaje_intercambio"
               ).fetchone() == (ninst, 0))
chk('A7: connector cannot read a crm row by id', cn.execute(
    'select count(*) from mensaje_intercambio where id in (%s,%s)' % (x1, xin)).fetchone()[0] == 0)
chk('A7: claim of a foreign endpoint returns nothing', ex(cn, 'claim', 'gw-1', 120000, ['crm', 'installer'], 20) == []
    and row(x1, cols) == crm_before)
chk('A7: F1 lookup of a foreign row returns nothing', ex(cn, 'dupid', 'crm', 'evt-crm-1') == [])
eng.execute("UPDATE mensaje_intercambio SET estado='EN_ENVIO', asignado_a='crm-gw', intentos=1, "
            "lease_hasta=now()+interval '1 h' WHERE id=%s" % x1)
crm_before = row(x1, cols)
for k, a in {'extend': (60000,), 'settle': ('CONFIRMADO',), 'retry': (1000, 'x')}.items():
    chk('A7: %s of a foreign row with its real token: 0 rows' % k,
        ex(cn, k, x1, 'crm-gw', 1, *a) == 0 and row(x1, cols) == crm_before)
chk('A7: dead letter of a foreign row: 0 rows, no IN row',
    ex(cn, 'dead', x1, 'crm-gw', 1, 'x') == [] and row(x1, cols) == crm_before
    and one("select count(*) from mensaje_intercambio where msg_id_externo='delivery-failed:%s'" % x1)[0] == 0)
chk('A7: confirm of a foreign row: 0 rows', ex(cn, 'confirm', 'wfX:1', 'crm') == 0 and row(x1, cols) == crm_before)
chk('A7: raw UPDATE of a foreign row: 0 rows', cn.execute(
    "UPDATE mensaje_intercambio SET estado='CONFIRMADO' WHERE id=%s" % x1).rowcount == 0 and row(x1, cols) == crm_before)
inown = row(r2210, 'estado,error')
chk('A7: connector cannot update its own IN rows (0 rows)', cn.execute(
    "UPDATE mensaje_intercambio SET estado='CONSUMIDO', error='x' WHERE id=%s" % r2210).rowcount == 0
    and row(r2210, 'estado,error') == inown)
st = sqlstate(lambda: ex(cn, 'insert', 'crm', 'work-order-closed', None, 'forged-1', None, woc, None))
chk('A7: insert for a foreign endpoint: 42501, nothing stored', st == '42501'
    and one("select count(*) from mensaje_intercambio where msg_id_externo='forged-1'")[0] == 0, st)
# M03: the endpoint check of the insert policy, without RETURNING (no SELECT policy involved)
st = sqlstate(lambda: cn.execute(
    "INSERT INTO mensaje_intercambio (direccion,endpoint_logico,tipo_mensaje,msg_id_externo,payload,estado) "
    "VALUES ('IN','crm','work-order-closed','forged-6','%s','RECIBIDO')" % woc.replace("'", "''")))
chk('M03: plain INSERT (no RETURNING) for a foreign endpoint: 42501, nothing stored', st == '42501'
    and one("select count(*) from mensaje_intercambio where msg_id_externo='forged-6'")[0] == 0, st)
ins_pol = one("select with_check from pg_policies where policyname='mensaje_intercambio_conector_insert'")[0]
chk('M03: insert policy keeps wf_id IS NULL and the conector_endpoint check',
    'wf_id IS NULL' in ins_pol and 'conector_endpoint' in ins_pol and 'CURRENT_USER' in ins_pol.upper(), ins_pol)
grants = [(r, priv) for r in ('clpd_connector', 'connector_installer', 'connector_crm')
          for priv in ('INSERT', 'UPDATE')
          if one("select has_column_privilege(%s, 'mensaje_intercambio', 'wf_id', %s)", r, priv)[0]]
chk('M03: wf_id in no connector column grant', grants == [], grants)
wm1 = out('wfM', 'wfM:1')   # non-final own OUT rows for the WHERE-less update
wm2 = out('wfM2', 'wfM2:1')
pm = row(wm2, cols)
st = sqlstate(lambda: cn.execute("UPDATE mensaje_intercambio SET actualizado = now() WHERE id = %s" % wm2))
chk('T12b: an update that leaves an own OUT row PENDIENTE: 42501 (WITH CHECK)', st == '42501' and row(wm2, cols) == pm, st)
eng.execute("UPDATE mensaje_intercambio SET estado='ENVIADO', asignado_a='gw-m', intentos=1 WHERE id IN (%s,%s)" % (wm1, wm2))
OWN_OPEN_OUT = ("direccion='OUT' and endpoint_logico='installer' "
                "and estado in ('PENDIENTE','EN_ENVIO','REINTENTO','ENVIADO')")
updatable = one('select count(*) from mensaje_intercambio where ' + OWN_OPEN_OUT)[0]
pending = one("select count(*) from mensaje_intercambio where direccion='OUT' and endpoint_logico='installer' "
              "and estado='PENDIENTE'")[0]
chk('M03 setup: at least 2 own non-final OUT rows', updatable >= 2, updatable)
chk('M03 setup: no own OUT row PENDIENTE (the update would fail WITH CHECK)', pending == 0, pending)
OTHERS = 'select id, actualizado from mensaje_intercambio where not (' + OWN_OPEN_OUT + ') order by id'
snap = adm.execute(OTHERS).fetchall()
n = cn.execute("UPDATE mensaje_intercambio SET actualizado = now()").rowcount
chk('M03: a WHERE-less connector update touches only its own non-final OUT rows',
    n == updatable and adm.execute(OTHERS).fetchall() == snap, (n, updatable))

for fid, fst in ((a1, 'CONFIRMADO'), (o2, 'ERROR')):
    before = row(fid, 'estado,siguiente_intento,error')
    chk('T12b: connector cannot reopen its own %s OUT row (0 rows)' % fst,
        before[0] == fst and cn.execute("UPDATE mensaje_intercambio SET estado='REINTENTO', siguiente_intento=now(), "
                                        "error='x', actualizado=now() WHERE id=%s" % fid).rowcount == 0
        and row(fid, 'estado,siguiente_intento,error') == before, before)
forged = {
    'insert with wf_id': "INSERT INTO mensaje_intercambio (direccion,wf_id,endpoint_logico,tipo_mensaje,msg_id_externo,"
                         "payload,estado) VALUES ('IN','fi-000123','installer','work-order-closed','forged-2','%s',"
                         "'RECIBIDO')",
    'insert an OUT row': "INSERT INTO mensaje_intercambio (direccion,correlacion_id,endpoint_logico,tipo_mensaje,"
                         "payload,estado) VALUES ('OUT','forged-3','installer','work-order-closed','%s','PENDIENTE')",
    'insert IN row as CONSUMIDO': "INSERT INTO mensaje_intercambio (direccion,endpoint_logico,tipo_mensaje,"
                                  "msg_id_externo,payload,estado) VALUES ('IN','installer','work-order-closed',"
                                  "'forged-4','%s','CONSUMIDO')",
    'insert with asignado_a': "INSERT INTO mensaje_intercambio (direccion,endpoint_logico,tipo_mensaje,msg_id_externo,"
                              "payload,estado,asignado_a) VALUES ('IN','installer','work-order-closed','forged-5','%s',"
                              "'RECIBIDO','n1')",
}
for n, q in forged.items():
    st = sqlstate(lambda: cn.execute(q % woc.replace("'", "''")))
    chk('A7: connector denied %s (42501)' % n, st == '42501', st)
chk('A7: none of the forged rows stored', one(
    "select count(*) from mensaje_intercambio where msg_id_externo like 'forged-%%' or correlacion_id='forged-3'")[0] == 0)
o4 = out('wfD', 'wfD:1')
ex(cn, 'claim', 'gw-1', 120000, eps, 20)
mine = row(o4, 'estado,wf_id,endpoint_logico,payload')
denied = {
    'update wf_id': "UPDATE mensaje_intercambio SET wf_id='fi-x' WHERE id=%d",
    'update wf_id to NULL': "UPDATE mensaje_intercambio SET wf_id=NULL WHERE id=%d",
    'update payload': "UPDATE mensaje_intercambio SET payload='{}' WHERE id=%d",
    'move a row to another endpoint': "UPDATE mensaje_intercambio SET endpoint_logico='crm' WHERE id=%d",
    'put a row back to PENDIENTE': "UPDATE mensaje_intercambio SET estado='PENDIENTE' WHERE id=%d",
    'delete a row': "DELETE FROM mensaje_intercambio WHERE id=%d",
    'write correlacion': "INSERT INTO correlacion VALUES ('installer','z','w') -- %d",
    'write conector_endpoint': "INSERT INTO conector_endpoint VALUES ('connector_installer','crm') -- %d",
}
for n, q in denied.items():
    st = sqlstate(lambda: cn.execute(q % o4))
    chk('A7: connector denied %s (42501)' % n, st == '42501', st)
chk('A7: own OUT row unchanged after the denials', row(o4, 'estado,wf_id,endpoint_logico,payload') == mine)
chk('A7: connector reads only its correlacion rows', cn.execute(
    'select endpoint_logico, ref_externa from correlacion').fetchall() == [('installer', 'OI-55')])
chk('A7: connector lists its endpoints (3.5)', ex(cn, 'myeps') == [('installer',)])
chk('A7: connector reads only its conector_endpoint rows', cn.execute(
    'select rol, endpoint_logico from conector_endpoint').fetchall() == [('connector_installer', 'installer')])
cn.execute('SET ROLE clpd_connector')
chk('A7: SET ROLE clpd_connector sees no rows', cn.execute(
    'select count(*) from mensaje_intercambio').fetchone()[0] == 0 and ex(cn, 'myeps') == [])
cn.execute('RESET ROLE')
st = sqlstate(lambda: cn.execute('SET ROLE clpd_engine'))
chk('A7: connector cannot SET ROLE to the owner (42501)', st == '42501', st)
# the crm connector: its own flow works and installer rows are invisible to it
chk('A7: crm connector sees no installer rows', crm.execute(
    "select count(*) from mensaje_intercambio where endpoint_logico='installer'").fetchone()[0] == 0)
eng.execute("UPDATE mensaje_intercambio SET lease_hasta=now()-interval '1ms' WHERE id=%s" % x1)
rows = ex(crm, 'claim', 'crm-gw-1', 120000, ['crm', 'installer'], 20)
chk('A7: crm connector claims its row, never installer rows', [(x[0], x[3], x[7]) for x in rows] == [(x1, 'crm', 2)], rows)
crm.execute('BEGIN')
s = ex(crm, 'settle', x1, 'crm-gw-1', 2, 'CONFIRMADO')
ins = ex(crm, 'insert', 'crm', 'customer-updated', 'C-1', 'response:wfX:1', 'wfX:1',
         json.dumps({"schema": "customer-updated@1", "data": {"id": "C-1"}}), None)
crm.execute('COMMIT')
chk('A7: crm connector settles and inserts its answer', s == 1 and len(ins) == 1, (s, ins))
chk('A7: crm connector reads only its correlacion rows', crm.execute(
    'select ref_externa from correlacion').fetchall() == [('C-1',)])
# R3-confirm-endpoint-mismatch: connector_crm also serves 'billing'; an answer on billing never confirms a crm row
eng.execute("INSERT INTO conector_endpoint (rol, endpoint_logico) VALUES ('connector_crm', 'billing')")
y1 = out('wfY', 'wfY:1', ep='crm')
rows = ex(crm, 'claim', 'crm-gw-1', 120000, ['crm', 'billing'], 20)
chk('two endpoints: crm connector claims wfY:1', [x[0] for x in rows] == [y1], rows)
crm.execute('BEGIN')
ex(crm, 'insert', 'billing', 'invoice-issued', None, 'evt-bill-1', 'wfY:1',
   json.dumps({"schema": "invoice-issued@1", "data": {}}), None)
k = ex(crm, 'confirm', 'wfY:1', 'billing')
crm.execute('COMMIT')
chk('R3: an answer on another endpoint of the same role does not confirm the OUT row',
    k == 0 and row(y1)[0] == 'EN_ENVIO', (k, row(y1)))
chk('R3: the answer on the OUT row\'s endpoint confirms it',
    ex(crm, 'confirm', 'wfY:1', 'crm') == 1 and row(y1)[0] == 'CONFIRMADO')
# §3.5: the column grant, not only the policy, blocks moving an OUT row to another endpoint of the same role
y2 = out('wfY2', 'wfY2:1', ep='crm')
before = row(y2, 'endpoint_logico,estado')
try:
    crm.execute("UPDATE mensaje_intercambio SET endpoint_logico='billing' WHERE id=%s" % y2)
    st, msg = None, ''
except psycopg.Error as e:
    st, msg = e.sqlstate, str(e)
chk('3.5: move an own OUT row to another endpoint of the same role: 42501 permission denied (column grant)',
    st == '42501' and 'permission denied' in msg and row(y2, 'endpoint_logico,estado') == before, (st, msg))
chk('3.5: endpoint_logico in no connector UPDATE grant',
    not one("select has_column_privilege('connector_crm', 'mensaje_intercambio', 'endpoint_logico', 'UPDATE')")[0])
# settle y2 so the concurrency run below claims only its own rows
eng.execute("UPDATE mensaje_intercambio SET estado='CONFIRMADO', actualizado=now() WHERE id=%s" % y2)
# installer answers with a correlacion_id of a crm OUT row: stored, but the intake (8.2) never routes it there
r = ex(cn, 'insert', 'installer', 'work-order-created', None, 'evt-cross', 'wfX:1',
       json.dumps({"schema": "work-order-created@1", "data": {}}), None)
chk('A9: a correlacion_id of another endpoint is stored with wf_id NULL',
    len(r) == 1 and row(r[0][0], 'wf_id, estado') == (None, 'RECIBIDO'))
chk('A9: no OUT row of the same endpoint has that correlacion_id', one(
    "select count(*) from mensaje_intercambio where direccion='OUT' and correlacion_id='wfX:1' "
    "and endpoint_logico='installer'")[0] == 0)

# ================================================================ concurrency: 2 connector roles x 4 instances
adm.execute('TRUNCATE claimlog')
eng.execute("""INSERT INTO mensaje_intercambio (direccion,wf_id,correlacion_id,endpoint_logico,tipo_mensaje,payload,estado)
 SELECT 'OUT', 'cw'||w, 'cw'||w||':'||k||':'||e, e, 't', '{"schema":"t@1","data":{}}', 'PENDIENTE'
   FROM generate_series(1,10) k, generate_series(1,120) w, unnest(array['installer','crm']) e ORDER BY k, w, e""")
total = one("select count(*) from mensaje_intercambio where wf_id like 'cw%%'")[0]
errs = []


def worker(n):
    try:
        role, ep = ('connector_installer', 'installer') if n % 2 else ('connector_crm', 'crm')
        c = conn(role)
        name = 'cgw-%s-%d' % (ep, n)
        idle = 0
        while idle < 5:
            rows = ex(c, 'claim', name, 120000, ['installer', 'crm'], 7)
            if not rows:
                idle += 1
                time.sleep(0.05)
                continue
            idle = 0
            for r in sorted(rows):
                if r[3] != ep:
                    errs.append(('foreign row claimed', role, r[0]))
                time.sleep(random.random() * 0.002)
                if random.random() < 0.15 and r[7] < 3:
                    k = ex(c, 'retry', r[0], name, r[7], 1, 'transient')
                else:
                    k = ex(c, 'settle', r[0], name, r[7], 'CONFIRMADO')
                if k != 1:
                    errs.append(('lost', r[0]))
        c.close()
    except Exception as e:  # report any driver error as a check failure
        errs.append(repr(e))


ths = [threading.Thread(target=worker, args=(i,)) for i in range(8)]
t0 = time.time()
for t in ths:
    t.start()
for t in ths:
    t.join(300)
chk('concurrency: no worker error, lost claim or foreign row', errs == [], errs[:5])
done = one("select count(*) from mensaje_intercambio where wf_id like 'cw%%' and estado='CONFIRMADO'")[0]
chk('concurrency: all %d rows CONFIRMADO' % total, done == total, done)
chk('concurrency: no (row, token) claimed twice', one(
    "select count(*) from (select id, tok from claimlog where ev='EN_ENVIO' group by id, tok having count(*)>1) x")[0] == 0)
chk('concurrency: no row settled twice', one(
    "select count(*) from (select id from claimlog where ev='CONFIRMADO' group by id having count(*)>1) x")[0] == 0)
chk('concurrency: each endpoint claimed only by its role', one(
    "select count(*) from claimlog l join mensaje_intercambio m on m.id=l.id where l.ev='EN_ENVIO' and "
    "l.who NOT LIKE 'cgw-' || m.endpoint_logico || '-%%'")[0] == 0)
viol = one("""
 WITH m AS (SELECT id, lag(id) OVER (PARTITION BY wf_id, endpoint_logico ORDER BY id) prev
              FROM mensaje_intercambio WHERE wf_id LIKE 'cw%%'),
      fc AS (SELECT id, min(seq) s FROM claimlog WHERE ev='EN_ENVIO' GROUP BY id),
      st AS (SELECT id, max(seq) s FROM claimlog WHERE ev='CONFIRMADO' GROUP BY id)
 SELECT count(*) FROM m JOIN fc ON fc.id=m.id JOIN st ON st.id=m.prev WHERE fc.s < st.s""")[0]
chk('concurrency: per-instance/endpoint order kept', viol == 0, viol)
retries = one("select count(*) from claimlog where ev='REINTENTO'")[0]
print('OBS concurrency: rows %d retries %d secs %.1f' % (total, retries, time.time() - t0))
print('checks %d fails %d' % (checks, fails))
sys.exit(1 if fails else 0)
