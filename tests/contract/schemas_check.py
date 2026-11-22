#!/usr/bin/env python3
"""Checks docs/schemas/ against the exchange contract (T12a, contract 1.0).

usage: schemas_check.py [<schemas-dir> [<contract.md>]]

Defaults come from the repository layout: <repo>/docs/schemas and
<repo>/docs/exchange-contract.md, <repo> being two levels above this file.
<schemas-dir> must hold envelope.schema.json, work-order-closed@1.json and
delivery-failed@1.json: the negative and boundary payloads use them.

Checks: every file is a valid JSON Schema draft 2020-12 and follows the file
rules of the contract section 5.1; every `examples` entry validates against
its file and the envelope; the `json` blocks of the contract validate against
the envelope (and their own schema when it is published); invalid payloads are
rejected and boundary payloads accepted (section 4.2, 4.3).

Exit status: 0 all checks passed, 1 a check failed, 2 usage or input error
(including a missing required schema file),
77 the Python package jsonschema is not installed (ctest SKIP_RETURN_CODE).
"""
import copy
import glob
import json
import os
import re
import sys

try:
    from jsonschema import Draft202012Validator as V
    from jsonschema import FormatChecker
except ImportError:
    print('schemas_check.py: Python package jsonschema not installed; skipped', file=sys.stderr)
    sys.exit(77)

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
if len(sys.argv) > 3:
    print('usage: schemas_check.py [<schemas-dir> [<contract.md>]]', file=sys.stderr)
    sys.exit(2)
SDIR = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'docs', 'schemas')
DOC = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'docs', 'exchange-contract.md')

fails = 0
checks = 0


def chk(name, ok, extra=''):
    global fails, checks
    checks += 1
    print(('PASS ' if ok else 'FAIL ') + name + (' ' + str(extra) if extra and not ok else ''), flush=True)
    if not ok:
        fails += 1


def load(path):
    with open(path, encoding='utf-8') as f:
        return json.load(f)


try:
    files = sorted(glob.glob(os.path.join(SDIR, '*.json')))
    schemas = {os.path.basename(f): load(f) for f in files}
    with open(DOC, encoding='utf-8') as f:
        doc = f.read()
except (OSError, ValueError) as e:
    print('schemas_check.py: %s' % e, file=sys.stderr)
    sys.exit(2)
# the negative and boundary payloads below use these published schemas
for required in ('envelope.schema.json', 'work-order-closed@1.json', 'delivery-failed@1.json'):
    if required not in schemas:
        print('schemas_check.py: %s has no %s' % (SDIR, required), file=sys.stderr)
        sys.exit(2)

env = schemas['envelope.schema.json']
FC = FormatChecker()
ve = V(env, format_checker=FC)
TYPE_RE = re.compile(r'^([a-z][a-z0-9-]{0,62})@([1-9][0-9]*)\.json$')
SLOT_TYPES = {'string', 'integer', 'number', 'array'}

# --- every file: metaschema, section 5.1 rules, examples
for n, s in schemas.items():
    try:
        V.check_schema(s)
        chk('metaschema ' + n, True)
    except Exception as e:  # SchemaError
        chk('metaschema ' + n, False, e)
    examples = s.get('examples', [])
    chk('%s has examples' % n, len(examples) >= 1)
    v = V(s, format_checker=FC)
    for i, ex in enumerate(examples):
        chk('%s example %d valid against its file' % (n, i), v.is_valid(ex))
        chk('%s example %d valid against the envelope' % (n, i), ve.is_valid(ex))
    if n == 'envelope.schema.json':
        continue
    m = TYPE_RE.match(n)
    chk('%s file name is <tipo_mensaje>@<version>.json' % n, m is not None)
    if not m:
        continue
    tipo = n[:-5]
    chk('%s $schema 2020-12' % n, s.get('$schema') == 'https://json-schema.org/draft/2020-12/schema')
    chk('%s $id' % n, s.get('$id') == 'urn:clpdaemon:schema:' + tipo)
    chk('%s title and description' % n, bool(s.get('title')) and bool(s.get('description')))
    chk('%s root object, required, closed' % n,
        s.get('type') == 'object' and s.get('required') == ['schema', 'data']
        and s.get('additionalProperties') is False)
    chk('%s schema const' % n, s.get('properties', {}).get('schema') == {'const': tipo})
    data = s.get('properties', {}).get('data', {})
    props = data.get('properties', {})
    chk('%s data object, closed' % n, data.get('type') == 'object' and data.get('additionalProperties') is False)
    chk('%s data has no wf-id / correlacion-id' % n, 'wf-id' not in props and 'correlacion-id' not in props)
    chk('%s data required are properties' % n, set(data.get('required', [])) <= set(props))
    bad = [k for k, p in props.items()
           if p.get('type') not in SLOT_TYPES
           or (p.get('type') == 'array' and p.get('items', {}).get('type') not in SLOT_TYPES - {'array'})]
    chk('%s properties typed string/integer/number/array (rule 5)' % n, bad == [], bad)
    for ex in examples:
        chk('%s example schema member is %s' % (n, tipo), ex.get('schema') == tipo)

# --- the json blocks of the contract
blocks = re.findall(r'^```json\n(.*?)^```', doc, re.M | re.S)
chk('contract has json blocks', len(blocks) >= 1, len(blocks))
for i, b in enumerate(blocks):
    try:
        p = json.loads(b)
    except ValueError as e:
        chk('contract json block %d parses' % i, False, e)
        continue
    chk('contract json block %d valid against the envelope' % i, ve.is_valid(p))
    own = schemas.get(str(p.get('schema')) + '.json')
    if own is not None:
        chk('contract json block %d valid against %s' % (i, p['schema']), V(own, format_checker=FC).is_valid(p))

# --- negative and boundary payloads
woc = V(schemas['work-order-closed@1.json'], format_checker=FC)
dfv = V(schemas['delivery-failed@1.json'], format_checker=FC)
base = {"schema": "work-order-closed@1",
        "data": {"work-order": "OI-55", "closed-at": "2026-11-17T10:22:00Z", "result": "ok"}}
dfbase = {"schema": "delivery-failed@1",
          "data": {"out-id": 1042, "out-type": "work-order-request", "attempts": 8, "reason": "HTTP 503"}}


def mut(f, b=base):
    p = copy.deepcopy(b)
    f(p)
    return p


def setd(k, val, b=base):
    return mut(lambda p: p['data'].__setitem__(k, val), b)


neg = {
    'wf-id in data (env)': (ve, setd('wf-id', 'x')),
    'correlacion-id in data (env)': (ve, setd('correlacion-id', 'x')),
    'boolean value (env)': (ve, setd('x', True)),
    'null value (env)': (ve, setd('x', None)),
    'object value (env)': (ve, setd('x', {})),
    'nested array (env)': (ve, setd('x', [[1]])),
    'extra top member (env)': (ve, mut(lambda p: p.__setitem__('x', 1))),
    'missing data (env)': (ve, mut(lambda p: p.pop('data'))),
    'schema leading zero (env)': (ve, mut(lambda p: p.__setitem__('schema', 'work-order-closed@01'))),
    'schema no version (env)': (ve, mut(lambda p: p.__setitem__('schema', 'work-order-closed'))),
    'upper-case slot name (env)': (ve, setd('Bad', 1)),
    'int64 max + 1 (env)': (ve, setd('n', 9223372036854775808)),
    'int64 min - 1 (env)': (ve, setd('n', -9223372036854775809)),
    # section 4.3: every double of magnitude >= 2^63 is integral, so FLOAT values are capped too
    'float 1.0e20 (env)': (ve, setd('f', 1.0e20)),
    'float -1.0e20 (env)': (ve, setd('f', -1.0e20)),
    'float 9.3e18 (env)': (ve, setd('f', 9.3e18)),
    'string 8193 ASCII characters (env)': (ve, setd('s', 'a' * 8193)),
    'string 8193 two-byte characters (env)': (ve, setd('s', 'é' * 8193)),
    'array 1025 elements (env)': (ve, setd('a', [1] * 1025)),
    'unknown slot (woc)': (woc, setd('x', 'y')),
    'missing result (woc)': (woc, mut(lambda p: p['data'].pop('result'))),
    'bad enum (woc)': (woc, setd('result', 'meh')),
    'non-UTC time (woc)': (woc, setd('closed-at', '2026-11-17T10:22:00+01:00')),
    'wrong const (woc)': (woc, mut(lambda p: p.__setitem__('schema', 'work-order-closed@2'))),
    'out-id 0 (df)': (dfv, setd('out-id', 0, dfbase)),
    'null reason (df)': (dfv, setd('reason', None, dfbase)),
    'reason 1001 characters (df)': (dfv, setd('reason', 'r' * 1001, dfbase)),
    'fractional attempts 2.5 (df)': (dfv, setd('attempts', 2.5, dfbase)),
}
for n, (v, p) in neg.items():
    chk('rejects ' + n, not v.is_valid(p))

pos = {
    'int64 max (env)': (ve, setd('n', 9223372036854775807)),
    'int64 min (env)': (ve, setd('n', -9223372036854775808)),
    'empty array (env)': (ve, setd('a', [])),
    'array 1024 elements (env)': (ve, setd('a', [1] * 1024)),
    'float 2.5 (env)': (ve, setd('f', 2.5)),
    'float 9.2e18 (env)': (ve, setd('f', 9.2e18)),
    'float -9.2e18 (env)': (ve, setd('f', -9.2e18)),
    'string 8192 ASCII characters (env)': (ve, setd('s', 'a' * 8192)),
    # section 4.2: the limit counts characters, so 8192 x U+00E9 (16384 bytes) is allowed
    'string 8192 two-byte characters (env)': (ve, setd('s', 'é' * 8192)),
    # section 4.3: 2.0 is an integral number; integer properties accept it
    'integral 2.0 (env)': (ve, setd('n', 2.0)),
    'attempts 8.0 for an INTEGER slot (df)': (dfv, setd('attempts', 8.0, dfbase)),
    'reason 1000 characters (df)': (dfv, setd('reason', 'r' * 1000, dfbase)),
    'notes present (woc)': (woc, setd('notes', 'No access')),
}
for n, (v, p) in pos.items():
    chk('accepts ' + n, v.is_valid(p))

print('checks %d fails %d' % (checks, fails))
sys.exit(1 if fails else 0)
