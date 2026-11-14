"""Deterministic evidence from actual messages, qualified tests and red cases."""
import hashlib
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
validation=ROOT/'validation/full-review-20261114'
previous=ROOT/'validation/edge-review-20261114'
provenance=json.loads((validation/'provenance.json').read_text(encoding='utf-8'))
assert len(provenance)==54
trace=json.loads((validation/'protocol-traceability.json').read_text(encoding='utf-8'))
assert trace['all_requirements_have_passing_evidence'] and len(trace['requirements'])==20
reports=[]
for name,source in sorted(provenance.items()):
    value=json.loads((validation/name).read_text(encoding='utf-8'));assert value['passed'] is True
    reports.append(dict(id=Path(name).stem,stage='current_core_ci',source_commit=source['commit'],
                        source=source['ci_url'],output=json.dumps(value,ensure_ascii=False,indent=2)))
baseline=json.loads((previous/'red-predecessor-qualified-boundaries.json').read_text(encoding='utf-8'))
budget=json.loads((previous/'red-vector-response-budget.json').read_text(encoding='utf-8'))
assert baseline['passed'] is False and budget['passed'] is False
groups={}
for item in baseline['qualifying_failures']:groups[item['category']]=groups.get(item['category'],0)+1
examples=[]
for category in ['protocol_id','vector_numeric','limits','schema_extents','select_grammar']:
    candidates=[x for x in baseline['qualifying_failures'] if x['category']==category]
    if candidates:examples.append(candidates[0])
red_summary={'passed':False,'baseline_commit':'2b96adab6659952701b876a4468e4011ac05793f',
             'qualifying_failure_observations':len(baseline['qualifying_failures']),'by_category':groups,'actual_examples':examples,
             'full_trace_file':'validation/edge-review-20261114/red-predecessor-qualified-boundaries.json'}
reports.extend([
    dict(id='red-predecessor-boundaries',stage='retained_actual_failure',source_commit=red_summary['baseline_commit'],
         source='validation/edge-review-20261114/red-predecessor-qualified-boundaries.json',output=json.dumps(red_summary,ensure_ascii=False,indent=2)),
    dict(id='red-vector-response-budget',stage='retained_actual_failure',source_commit='f7c1e39663ad21836fc811250574a0f971da399e',
         source='https://github.com/xxxhm0524/seekdb/actions/runs/43597854434',output=json.dumps(budget,ensure_ascii=False,indent=2))])
for name in ['red-nested.json','red-token-kinds.json']:
    value=json.loads((validation/name).read_text(encoding='utf-8'));assert value['passed'] is False
    reports.append(dict(id=Path(name).stem,stage='retained_actual_failure',
                        source_commit='7becc49614f25ac67bdcd1385402e37bc31cdb7c',
                        source='validation/full-review-20261114/'+name,
                        output=json.dumps(value,ensure_ascii=False,indent=2)))
paired=json.loads((previous/'scored-reference-paired.json').read_text(encoding='utf-8'))
comparisons=[]
for width in ['16','1024']:
    m=paired['medians'][width]
    comparisons.append(f"Historical 65aa862 core, {width}-byte text: write/query {m['ours']['write_seconds']:.6f}/{m['ours']['query_seconds']:.6f}s; qualified reference {m['reference']['write_seconds']:.6f}/{m['reference']['query_seconds']:.6f}s. Not a timing of the latest parser fixes or a platform score.")
full_trace=json.loads((validation/'requirements-traceability.json').read_text(encoding='utf-8'))
assert len(full_trace['requirements'])==64 and len(full_trace['platform_pending'])==4
material={
    'format':'actual-clean-rewrite-evidence-v3','synthetic':False,'date':'2026-11-14',
    'own_formal_evaluation':'Not submitted. The user explicitly continued to defer formal evaluation. Author-published formal success belongs only to the public reference.',
    'scope':'Selected actual user/assistant messages and factual tool evidence from this clean rewrite. Credentials, unrelated chats, hidden reasoning and generated test databases are omitted.',
    'attribution':'Implementation, integration and test work are Agent work. The user requested reference-based boundary auditing and explicitly challenged missing clause-by-clause protocol verification. No student database explanation or experiment is invented. The request parser and several cache/certificate designs are adapted from the attributed frozen public version.',
    'conversations':json.loads((ROOT/'agent/actual_conversation.json').read_text(encoding='utf-8')),
    'agent_design':[
        'B+ tree pages are immutable after their creating SQL statement. Each statement copies the catalog and owns a new tail/page set; failure rewinds only that statement, retaining earlier successful statements in the transaction.',
        'All row/catalog/page writes finish before alternate-root publication; a complete-file sync finishes before success is acknowledged. The declared competition model is process termination in a surviving kernel, not machine-power-loss ordering. Recovery preserves the latest acknowledged root and propagates allocation/schema failures.',
        'A bounded page cache, 16 MiB working-column snapshot, external sorting and response spooling support a database larger than memory. History is compacted between transactions using a separate synced file and atomic replacement.',
        'Both search modes return exact results. Scalar, AVX2 and AVX-512 lanes preserve dimension-ordered double accumulation without FMA. Integer screening is auxiliary: original float32 values remain authoritative and an exclusion certificate must succeed, otherwise all eligible records are evaluated.',
        'Physical filtering clusters and contiguous-source certificate layout follow the frozen successful implementation. A full scan reblocks once. Only explicit AS names participate in ORDER BY alias resolution; repeated ordinary projection labels are legal.',
        'The request parser is adapted directly from juanzhi-gcb/seekdb dfee55b84284f4c39d2df45951cdbcb1d1baf1a2 src/protocol/jsonl.hpp, source hash f9d04476c94e565a2e8efff6845b9c869ac02e351d04a4ab40e564f18ca18f23. It validates the complete frame and nested objects before dispatch and returns raw ID spans. Success, error, commit, shutdown and spooled replies echo them without numeric reinterpretation.',
        'SQL vector binding accepts SQL decimal signs, leading/trailing decimal points, leading zeros, formfeed and comments. Delimiters, dimensions and finite float32 overflow-rounding limits remain checked. Numeric tokens cannot be concatenated across whitespace or comments.',
        'Projected float32 components use shortest float32 round-trip decimals instead of double JSON nodes. Signed zero is retained, emitted numbers are finite, BIGINT stays exact and L2 distances keep double precision. Response spills are sealed before the first output byte.'
    ],
    'protocol_requirements':[{k:v for k,v in item.items() if k!='passing_evidence'} for item in trace['requirements']],
    'other_contest_requirements':[{k:v for k,v in item.items() if k!='passing_evidence'} for item in full_trace['requirements'][20:]],
    'experiments':reports,
    'agent_retrospective':[
        'The user correctly challenged the verification process. Previous parity used the old bounded-number DOM as its oracle. Shared integer ID precision/type loss could pass that test. The new oracle uses the protocol, Python arbitrary-precision JSON values/types and validated raw spans.',
        f"The frozen predecessor produced {len(baseline['qualifying_failures'])} qualifying failure observations in a replay against the actual reference with the corrected harness. These repeat root causes across indexed/unindexed and quoted/unquoted variants, not 81 independent bugs. Optional DDL/TX affected_rows:0 differences are excluded according to the protocol.",
        'Confirmed gaps were arbitrary-precision/composite IDs and large ignored metadata, SQL vector decimal spellings, signed nonnegative LIMIT/schema extents, and duplicate ordinary projections incorrectly treated as ambiguous aliases. Full failing traces are retained.',
        'A real-wire comparison found 25393654 bytes for a 10000-row, 128-dimensional vector projection versus reference 14330893, exceeding the 16777216-byte limit. Corrected output is 14330893 bytes; all 1280000 float32 components round-trip to identical bits.',
        'Projection checks compare stored float32 bit patterns after JSON decoding rather than one exact-double decimal spelling. BIGINT types and text stay exact; double-distance tolerances are unchanged. This preserves a lossless float32 check while permitting compact correct text.',
        'The 20-item protocol matrix maps UTF-8/framing, ready timing, flush, ID types, duplicates/nonfinite rejection, fields, errors, row types, shutdown/EOF, durability, Docker restrictions and byte limits to actual report hashes. This is finite-test evidence, not a proof for all input trees.',
        'The current 9716ef5 core CI has 54 passing reports: official 20/20, IDs, nested JSON, token kinds, independent published-requirement scenarios, protocol, numeric syntax, aliases, SIGKILL/syscall cuts, byte invariance, ASan/UBSan, large-tree/external-sort and actual 2 GiB memory with 2621440000 payload bytes.',
        'The full-process boundary corpus has 439 observations with no qualifying gap or independent-truth failure. The old image is also replayed to prove the harness detects the original gaps.',
        ' '.join(comparisons),
        'The user asked for every contest requirement to be rechecked. That audit found two more reproducible defects: a reference-derived depth-64 guard rejects valid opaque IDs at depth 65, and WHERE accepted a quoted comparison symbol as an operator. The user subsequently emphasized MySQL admission. Neither earlier passing CI nor a successful reference proved complete compliance.',
        'Opaque container validation now uses an explicit stack and checks every nested object for duplicate decoded keys. The regression validates 44 good requests up to depth 8192 and five malformed deep frames before SQL execution. Native and ASan/UBSan process runs pass.',
        'WHERE operators now retain lexical type checks. A 140-case rejection corpus covers quoted operator strings/identifiers and quoted syntax tokens across SELECT/UPDATE/DELETE, plus 14 legal comparisons, transaction continuation and restart. The pre-fix failure and corrected results are both preserved.',
        'The independent published-requirement suite checks 293 SQL requests with cache and 293 without cache: schema rules, named unsupported features, Unicode codepoint length/order, BIGINT, malformed vectors, all required comparisons, filtered exact/ann TopK, ties, index mutations, failed statements, commit/rollback and process restart.',
        'The current Yuque page was read again through its public document API. Relative to the old local text, substantive changes are the public-kit link and benchmark default exact. All 49 official kit files were fetched again and match the frozen hashes.',
        'The full matrix has 64 traceability entries, not 64 formal tests: 20 protocol, 14 SQL, 5 vector, 11 storage/transaction, 4 environment, 6 delivery and 4 platform-only. The latter four are explicitly unverified. Final branch/source/material provenance is checked separately when publishing.',
        'Earlier evidence stays under original directories and commits. Old passing tests did not establish that the old candidate was protocol-complete; new failures are preserved.'
    ],
    'limitations':[
        'The nine retained formal inputs are unavailable. Admission and competition points require platform evaluation of the locked commit; development CI is not a formal score.',
        'Raw io.stat and file sizes are not inferred official isolated-device B. Timings and memory remain environment-dependent; superiority is not universal.',
        'One catalog, one input and one row must fit memory. Compaction needs space for old and new files; long transactions need tail space.',
        'The former reference-style nesting-depth guard is removed. One input and its explicit validation stack must still fit the resource budget. SQL NULL is outside the required SQL subset; null IDs are supported and tested.',
        'The public config does not give all formal disk-capacity, I/O-bandwidth or index-preparation limits. Current tests verify the published configuration and finite workloads, not unspecified limits.',
        'No student technical interpretation is synthesized from autonomous Agent work.'
    ]
}
data=(json.dumps(material,ensure_ascii=False,indent=2,allow_nan=False)+'\n').encode('utf-8')
assert len(data)<=128*1024
for path in [ROOT/'conversation.json',ROOT/'agent/conversation.json']:path.write_bytes(data)
print(json.dumps({'material_bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),'verified_reports':len(provenance),'experiments':len(reports),'protocol_requirements':len(trace['requirements'])}))
