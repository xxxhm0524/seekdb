"""Published SQL/type/transaction requirements, with an independent state oracle.

Checks both disk-only and bounded-index paths; no database parser supplies the
expected results. Existing full CI covers large storage, crash cuts and Docker.
"""
import argparse
import copy
import json
import math
from pathlib import Path
import sys
from client import Client, vector_bits


def quote(value):
    return "'" + value.replace('\\', '\\\\').replace("'", "''") + "'"


def run_variant(out, cache):
    db = Client(out / 'data', env={'MVD_VECTOR_CACHE_MIB': str(cache)})
    checks = 0; scenarios = []
    def execute(sql, ok=True, affected=None):
        nonlocal checks
        response = db.execute(sql, ok=ok); checks += 1
        if affected is not None:
            assert type(response.get('affected_rows')) is int and response['affected_rows'] == affected, response
        if not ok:
            assert isinstance(response.get('error', {}).get('code'), str) and response['error']['code'], response
        return response
    def snapshot(state):
        result = execute('SELECT * FROM t ORDER BY id;')
        assert result['columns'] == ['id', 'n', 's', 'v']
        assert len(result['rows']) == len(state)
        for row, key in zip(result['rows'], sorted(state)):
            assert type(row[0]) is int and type(row[1]) is int and row[:3] == [key, *state[key][:2]]
            assert vector_bits(row[3]) == vector_bits(state[key][2])
    def topk(state, phase):
        snapshot(state)
        query = [0.5, -0.25]
        groups = [('', lambda key, row: True),
                  ('WHERE n>=1 AND n<=2', lambda key, row: 1 <= row[0] <= 2),
                  ("WHERE s<'😀'", lambda key, row: row[1] < '😀'),
                  ('WHERE n>999', lambda key, row: False)]
        for where, keep in groups:
            distances = {key: math.sqrt(sum((float(x)-y)**2 for x, y in zip(row[2], query)))
                         for key, row in state.items() if keep(key, row)}
            ordered = sorted(distances, key=lambda key: (distances[key], key))
            for mode in ['exact', 'ann']:
                for k in [0, 1, 3, 50]:
                    response = db.execute(f"SELECT id,L2_DISTANCE(v,'[0.5,-0.25]') AS d FROM t {where} ORDER BY d LIMIT {k};", mode)
                    assert response['columns'] == ['id', 'd']
                    assert [row[0] for row in response['rows']] == ordered[:k], (phase, mode, k, where)
                    assert len({row[0] for row in response['rows']}) == len(response['rows'])
                    for key, distance in response['rows']:
                        assert math.isfinite(distance) and math.isclose(distance, distances[key], rel_tol=1e-12, abs_tol=1e-12)
                    nonlocal_checks[0] += 1
        scenarios.append(phase)
    nonlocal_checks = [0]
    try:
        bad_schemas = ['id BIGINT', 'id VARCHAR(3) PRIMARY KEY',
                       'id BIGINT PRIMARY KEY,n BIGINT PRIMARY KEY',
                       'id BIGINT PRIMARY KEY,v VECTOR(2),w VECTOR(2)',
                       'id BIGINT PRIMARY KEY,ID BIGINT', 'id BIGINT PRIMARY KEY,s VARCHAR(0)',
                       'id BIGINT PRIMARY KEY,v VECTOR(0)', 'id BIGINT PRIMARY KEY,x INTEGER',
                       'id BIGINT PRIMARY KEY,n BIGINT,FOREIGN KEY(n) REFERENCES t(id)']
        for index, schema in enumerate(bad_schemas):
            execute(f'CREATE TABLE invalid_{index}({schema});', ok=False)
            execute(f'SELECT * FROM invalid_{index};', ok=False)
        scenarios.append('schema_bigint_pk_single_vector_duplicate_names_positive_widths_and_unsupported_types')
        execute('CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT,s VARCHAR(3),v VECTOR(2));')
        keys = [-(2**63), -4, -1, 0, 1, 2, 2**53+1, 2**63-1]
        texts = ['', 'e\u0301', 'é', '中', '😀', '\U00010000', '👩\u200d💻', '中']
        state = {key: [i % 3, texts[i], [float(i % 3), float((i + 1) % 2)]] for i, key in enumerate(keys)}
        values = ','.join(f'({quote(row[1])},{key},{quote(json.dumps(row[2]))},{row[0]})' for key, row in state.items())
        execute('INSERT INTO t(s,id,v,n) VALUES' + values + ';', affected=len(state))
        execute('INSERT INTO t VALUES(999,1,' + quote('👩\u200d💻x') + ",'[0,0]');", ok=False)
        snapshot(state)
        for descending in [False, True]:
            ordered = sorted(state)
            ordered.sort(key=lambda key: tuple(map(ord, state[key][1])), reverse=descending)
            assert execute('SELECT id,s FROM t ORDER BY s' + (' DESC' if descending else '') + ';')['rows'] == [[key, state[key][1]] for key in ordered]
        comparisons = {'=': lambda a,b:a==b, '<': lambda a,b:a<b, '<=': lambda a,b:a<=b, '>': lambda a,b:a>b, '>=': lambda a,b:a>=b}
        for operator, compare in comparisons.items():
            for column, value, predicate in [('n', '1', lambda key, row: compare(row[0], 1)),
                                              ('s', quote('é'), lambda key, row: compare(row[1], 'é')),
                                              ('id', str(2**53+1), lambda key, row: compare(key, 2**53+1))]:
                wanted = [[key] for key in sorted(state) if predicate(key, state[key])]
                assert execute(f'SELECT id FROM t WHERE {column}{operator}{value} ORDER BY id;')['rows'] == wanted
        assert execute("SELECT id FROM t WHERE s='this comparison can exceed the column width';")['rows'] == []
        assert execute('SELECT v,s FROM t WHERE id=999;')['columns'] == ['v', 's']
        assert execute('SELECT v,s FROM t WHERE id=999;')['rows'] == []
        scenarios.append('unicode_codepoint_length_and_order_ties_bigint_precision_projection_and_all_comparisons')
        for raw in ['[NaN,0]', '[Infinity,0]', '[-Infinity,0]', '[3.5e38,0]', '[0]', '[0,0,0]', '[true,0]', '["0",0]']:
            execute('INSERT INTO t VALUES(999,1,\'x\',' + quote(raw) + ');', ok=False)
        snapshot(state); scenarios.append('invalid_vectors_rejected_without_mutation')
        unsupported = ['SELECT id FROM t JOIN t AS other ON t.id=other.id;',
                       'SELECT COUNT(*) FROM t;', 'SELECT id FROM t WHERE id IN (SELECT id FROM t);',
                       'ALTER TABLE t ADD extra BIGINT;', 'UPDATE t SET id=100;',
                       'INSERT INTO t VALUES(999,1,NULL,\'[0,0]\');',
                       'UPDATE t SET s=NULL;', 'SELECT id FROM t WHERE n=NULL;',
                       'SELECT id FROM t WHERE n=1 OR n=2;', 'SELECT id FROM t; DELETE FROM t;']
        for sql in unsupported:
            execute(sql, ok=False); snapshot(state)
        scenarios.append('all_named_unsupported_features_rejected_and_no_prefix_execution')
        topk(state, 'exact_and_ann_before_index_all_filtered_empty_short_k_and_ties')
        execute('CREATE VECTOR INDEX idx ON t(v);')
        topk(state, 'same_requirements_after_index')
        # Multi-column ordering also uses the exact fallback rather than the
        # single-distance specialized path. Primary key breaks the final tie.
        ordered = sorted(state)
        ordered.sort(key=lambda key: state[key][1], reverse=True)
        ordered.sort(key=lambda key: state[key][0])
        assert execute('SELECT id FROM t ORDER BY n ASC,s DESC;')['rows'] == [[key] for key in ordered]
        committed = copy.deepcopy(state)
        execute('BEGIN;'); execute('UPDATE t SET v=\'[0.5,-0.25]\',n=2 WHERE id>=0;', affected=sum(key >= 0 for key in state))
        for key in state:
            if key >= 0: state[key][0] = 2; state[key][2] = [0.5, -0.25]
        execute('DELETE FROM t WHERE id=-1;', affected=1); del state[-1]
        execute("INSERT INTO t VALUES(55,1,'新','[0.5,-0.25]');", affected=1); state[55] = [1, '新', [0.5, -0.25]]
        execute("INSERT INTO t VALUES(56,1,'未','[0,0]'),(55,1,'重','[1,0]');", ok=False)
        execute('BEGIN;', ok=False)
        topk(state, 'read_own_insert_update_delete_index_and_failed_statement_in_transaction')
        execute('ROLLBACK;'); state = committed
        topk(state, 'rollback_restores_rows_and_index')
        execute('BEGIN;'); execute("UPDATE t SET s='改' WHERE id=0;", affected=1); state[0][1] = '改'
        execute("UPDATE t SET n=8,s='超过三个字符' WHERE id>=0;", ok=False)
        snapshot(state); execute('COMMIT;')
        db.restart(crash=True); topk(state, 'confirmed_commit_after_failed_statement_survives_sigkill')
        execute('BEGIN;'); execute('DELETE FROM t;', affected=len(state))
        db.restart(crash=True); topk(state, 'uncommitted_delete_does_not_survive_sigkill')
        execute('BEGIN;'); execute('CREATE TABLE temp(id BIGINT PRIMARY KEY,v VECTOR(2));')
        execute("INSERT INTO temp VALUES(1,'[0,0]');", affected=1); execute('CREATE VECTOR INDEX temp_idx ON temp(v);')
        execute('ROLLBACK;'); execute('SELECT * FROM temp;', ok=False)
        db.restart(); execute('SELECT * FROM temp;', ok=False); snapshot(state)
        scenarios.append('ddl_rollback_and_normal_restart')
        return dict(passed=True, cache_mib=cache, requests=checks + nonlocal_checks[0], scenarios=scenarios,
                    oracle='Python signed integers, Unicode codepoints, explicit state and stored-float32 L2',
                    no_reference_database_as_truth=True)
    finally: db.kill()


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    p = argparse.ArgumentParser(); p.add_argument('--output', required=True); args = p.parse_args()
    out = Path(args.output).resolve(); out.mkdir(parents=True, exist_ok=False)
    try: result = dict(passed=True, variants=[run_variant(out / 'cache', 16), run_variant(out / 'disk', 0)])
    except Exception as error:
        import traceback
        result = dict(passed=False, error=str(error), traceback=traceback.format_exc())
    (out / 'report.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False), flush=True)
    raise SystemExit(0 if result['passed'] else 1)
