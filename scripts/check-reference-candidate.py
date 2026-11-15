"""Read-only checks of this fork's actual source, evidence and authentic material."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
E=ROOT/'validation/reference-opt-20261115'
def pairs(items):
    out={}
    for k,v in items:assert k not in out,k;out[k]=v
    return out
def read(path):return json.loads(path.read_text(encoding='utf-8'),object_pairs_hook=pairs,parse_constant=lambda x:(_ for _ in ()).throw(ValueError(x)))
runtime=read(E/'runtime-inputs.json');provenance=read(E/'provenance.json')
assert runtime['runtime_core']==provenance['runtime_core']
assert hashlib.sha256((E/'runtime-inputs.json').read_bytes()).hexdigest()==provenance['runtime_inputs_sha256']
for n,h in runtime['sha256_lf'].items():assert hashlib.sha256((ROOT/n).read_bytes().replace(b'\r\n',b'\n')).hexdigest()==h,n
for n,v in provenance['reports'].items():
    data=(E/n).read_bytes();assert hashlib.sha256(data).hexdigest()==v['sha256'],n
    assert read(E/n)['passed'] is True,n
for n in ('candidate-ctest.json','baseline-ctest.json'):assert read(E/n)['tests']==70
for n in ('candidate-public.json','baseline-public.json','asan-public.json'):
    v=read(E/n);assert len(v['cases'])==20 and all(x['status']=='PASS' for x in v['cases'])
assert read(E/'parity.json')['observations']==7862 and read(E/'asan-parity.json')['observations']==7862
assert read(E/'benchmark.json')['verified_queries']==48000
assert read(E/'parity.json')['candidate_sha256']==provenance['release_binary_sha256']
data=(ROOT/'agent/conversation.json').read_bytes();assert data==(ROOT/'conversation.json').read_bytes() and len(data)<=131072 and not data.startswith(b'\xef\xbb\xbf')
material=read(ROOT/'agent/conversation.json');digest=read(ROOT/'agent/material-digest.json')
assert material['synthetic'] is False and material['provenance']['upstream_commit']==provenance['baseline_commit']
assert digest['material_sha256']==hashlib.sha256(data).hexdigest() and digest['runtime_core']==runtime['runtime_core']
if material.get('format')=='reference-derived-authentic-history-v2':
    template=read(ROOT/'evidence/prompt-history-20261115/approved-history-draft.json')
    assert [m['content'] for m in material['messages']]==[m['content'] for m in template['messages']]
    assert sum(m['role']=='user' for m in material['messages'])==18
    assert 'Agent editorial summary' in material['owner_leadership_overview']['author']
if 'binary-identity-728c.json' in provenance['reports']:
    binding=read(E/'binary-identity-728c.json')
    assert binding['runtime_core']==runtime['runtime_core'] and binding['identical_to_previous_release'] is True
    assert binding['release_binary_sha256']==provenance['release_binary_sha256']
subprocess.run([sys.executable,'-B',str(ROOT/'scripts/check-submission-contract.py'),'--repository',str(ROOT),'--check-internal-references'],check=True,stdout=subprocess.PIPE)
print(json.dumps(dict(passed=True,runtime_core=runtime['runtime_core'],runtime_inputs=len(runtime['sha256_lf']),reports=len(provenance['reports']),material_bytes=len(data),material_sha256=hashlib.sha256(data).hexdigest(),formal_submitted=False)))
