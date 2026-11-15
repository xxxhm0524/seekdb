"""Deterministic selection of real history; editorial prose never replaces quotes."""
import copy
import hashlib
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
E=ROOT/'validation/reference-opt-20261115'
template=ROOT/'evidence/prompt-history-20261115/approved-history-draft.json'
raw=template.read_bytes()
assert hashlib.sha256(raw).hexdigest()=='444587056e43cbaa8eca3ca1b2dc67d0623eb079714276dd460792b614a8fde8'
material=copy.deepcopy(json.loads(raw))
identity=json.loads((E/'runtime-inputs.json').read_text(encoding='utf-8'))
material['format']='reference-derived-authentic-history-v2'
material['status']='selected_actual_history_for_current_candidate'
material['provenance'].update(runtime_core=identity['runtime_core'],production_source_core=identity['production_source_core'],
    approved_history_draft_sha256=hashlib.sha256(raw).hexdigest(),
    editorial_scope='Owner accepted the real-history supplement and requested a clearer account of their direction-setting. Original messages remain exact; the added overview is Agent-authored editorial text, not historical dialogue.')
for message in material['messages']:
    if message['role']=='user' and '18446744073709551616' in message['content']:
        message['attribution_note']='The defect description inside quotation marks is the owner quoting an Agent finding. The owner contribution is the subsequent challenge and request for item-by-item protocol review; the finding is not relabeled as an owner discovery.'
material['owner_leadership_overview']={
    'author':'Agent editorial summary based on the cited original owner messages; not a historical user utterance',
    'text':'本题由做题人主导目标、验收要求和关键路线选择。做题人强调以 MySQL 准入为前提，要求逐项核对协议，重点寻找“参考实现能够通过而本实现失败”的边界，不把公开用例通过或借鉴过代码当成完全正确的证明。针对反复修补仍暴露问题的情况，做题人持续质疑，并最终决定弃用旧实现，直接以具有得分反馈的公开参考版本为基础开展优化。Agent 按这些明确要求执行实现、对照测试、问题定位和记录整理。',
    'role_boundary':'项目目标、验收标准和路线选择按用户原话归属；参考作者的实现、Agent 提出的具体优化和实际工具操作保留实际来源。本说明不声称所有技术方案由用户原创。',
    'presentation_order':'用户提出要求或质疑 → Agent 执行与真实结果 → 用户后续要求或路线调整。未发生的用户解释、操作或结论不补写。'
}
material['historical_agent_observations_at_4da36e6']=material.pop('agent_observations')
material['agent_observations']=[
    'The prior material selection was too focused on acceptance logs. The approved supplement restores the actual initial goal and specific owner challenges, preserves exact messages and stages, and indexes observations without claiming a score.',
    'Ubuntu 24.04 CI initially failed three read-failure tests for both the unchanged reference and the candidate. Symbol inspection showed fortified __pread_chk reads bypassed the pread-only test injector; diagnostics observed zero injected failures. This was not presented as a database semantic failure or a passing CI.',
    'A test-only forwarding wrapper retains libc bounds checks and routes safe fortified reads through the existing injector. Both versions then passed the same three assertions; their default database binary hashes remained unchanged. The main test configuration applies the same compatibility repair to all five read-wrapped suites.',
    'The new source snapshot changes test linkage and the learning material. Database production source, public protocol, persistence format and commit semantics remain those of the prior runtime. Existing reports retain their original core and binary attribution; new binary identity and tests are recorded separately.'
]
for name in ('fortified-read-compatibility.json','binary-identity-728c.json'):
    path=E/name
    if path.exists():
        report=path.read_bytes();assert json.loads(report)['passed'] is True
        material['experiments'].append(dict(id=name,source='Actual CI/native result or explicitly derived verification summary',
            source_file='validation/reference-opt-20261115/'+name,source_sha256=hashlib.sha256(report).hexdigest(),raw_report=report.decode('utf-8')))
material['limitations']=[
    'Original message wording is unchanged. Owner contribution and Agent/reference-author work are distinguished; editorial summaries and evidence-chain indexes are not historical user messages or official grading reports.',
    'The old e860453 formal result belongs to another implementation. This new candidate has no formal hidden admission, performance score, Prompt result or rank.',
    'The wide-vector binding memory result is a separate workload. The standard 128D paired runs did not show a consistent speed gain; slow samples and rejected experiments remain.',
    'Earlier CI failure and the test compatibility diagnosis are preserved. A subsequent complete CI must be checked for the final full SHA; neither a diagnostic job nor local checks substitute for formal evaluation.',
    'Docker was not run in this native workflow. Native compiler evidence does not establish the Alpine image build path. Finite checks are not a proof for all inputs.'
]
material['material_summary']='做题人明确目标与准入优先级，持续要求逐项协议核对和参考对照，并在反复暴露问题后决定更换实现基础。Agent 执行实现、实验及记录整理。下文先展示角色分工与原话索引，再给出完整选摘和真实结果；整理文字不替代原话，也不预设评分。'
front=['format','synthetic','status','date','material_summary','owner_leadership_overview','provenance','context','attribution_policy','candidate_evidence_chains','messages','experiments']
material={**{k:material[k] for k in front},**{k:v for k,v in material.items() if k not in front}}
assert [m['content'] for m in material['messages']]==[m['content'] for m in json.loads(raw)['messages']]
for chain in material['candidate_evidence_chains']:
    for evidence in chain['owner_evidence']+chain['observed_result']:
        value=material
        for part in evidence['pointer'].split('/')[1:]:value=value[int(part)] if isinstance(value,list) else value[part]
        assert isinstance(value,str) and evidence['quote'] in value and len(evidence['quote'])<=240
result=(json.dumps(material,ensure_ascii=False,indent=2,allow_nan=False)+'\n').encode('utf-8');assert len(result)<=131072
for name in ('agent/conversation.json','conversation.json'):(ROOT/name).write_bytes(result)
digest=dict(material_sha256=hashlib.sha256(result).hexdigest(),bytes=len(result),runtime_core=identity['runtime_core'],synthetic=False,
            owner_messages=sum(m['role']=='user' for m in material['messages']),source_quotes_unchanged=True,
            scope='Local integrity and attribution only; not a platform Prompt score or summary')
(ROOT/'agent/material-digest.json').write_bytes((json.dumps(digest,indent=2)+'\n').encode('utf-8'))
print(json.dumps(digest))
