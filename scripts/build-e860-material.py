"""Assemble exact owner excerpts and version-qualified actual experiment results."""
import copy
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
H = ROOT / 'evidence/e860-history-20261116'
V = ROOT / 'validation/e860-opt-20261116'
history = json.loads((H/'approved-owner-history.json').read_text(encoding='utf-8'))
latest = json.loads((H/'latest-owner-history.json').read_text(encoding='utf-8'))
messages = copy.deepcopy(history['messages'])
for i, message in enumerate(messages):
    message['id'] = 'history-' + str(i+1)
    message['version_scope'] = 'Historical protocol review and reference-derived candidate; precedes the renewed e860 optimization.'
    if message['role'] == 'user' and '18446744073709551616' in message['content']:
        message['attribution_note'] = 'The owner quotes an Agent finding, then challenges the review method. The quoted discovery is not relabeled as the owner finding it.'
messages += copy.deepcopy(latest['messages'])

material = dict(
    format='agent-learning-evidence-v3-unbounded', synthetic=False,
    status='authentic_selected_history_with_versioned_native_results', date='2026-11-16',
    material_summary='做题人主导目标、验收标准和路线选择，持续要求按协议逐条检查、以已得分公开实现寻找反例，并明确选择 e860 主线，将原始性能目标提高至 E≥4500，取消赶时限提测。Agent 根据这些要求实施和验证。下面保留真实原话及实际结果，不把 Agent 或参考作者的方案写成做题人原创。',
    owner_leadership_overview=dict(author='Agent 整理说明，不是历史用户发言',
        text='做题人多次纠正仅凭公开用例或代码来源判断正确性的做法，要求寻找参考实现通过而本实现失败的边界，并以 MySQL 全部准入作为得分前提。在实现路线反复调整后，做题人决定保留参考派生候选作备选，重新以已有正式成绩的 e860 为主线，参考新的得分公开代码继续优化。做题人把目标从原始性能 E≥4000 提高为 E≥4500，并撤销当晚提测和时间限制，使实现者可以继续核验正确性与资源取舍。',
        role_boundary='目标、质疑、验收要求、选型和是否提测归属于做题人的真实指令。具体存储实现、数学界、实验及工具操作归属于 Agent 或实际参考作者。未补写做题人没有说过的机制解释、操作或理解。'),
    current_context=dict(base='e86045306f5940613bfcf4aa84b0ee4bf74c20f1', production_core='ba96b7b048d3c1ae8a910a0cf6f5e0e46bd87396',
        target='正式原始性能指标 E≥4500；本地数据不能确认达到该值', formal_submission='做题人已取消本轮正式提测；跨日不自动恢复授权。',
        route_history='早期重写与参考派生候选的结果均保留原版本归属。本轮主线由做题人重新指定为 e860，参考派生 6fca5f7e8 仍独立保留。',
        source_attribution='e860 原有 juanzhi 参考来源保留。Freaybru/seekdb-2026 的 3dfd062502aed05de32b269c64bd32995b0ffb5e 提供已提交前缀映射与向量来源复用思路；4427.302903 仅为作者公开记录对应的成绩。新页回收、共享寿命接入及本地实验由 Agent 实施。未复制参考作者的学习对话。'),
    excerpt_policy='真实选摘，包含跨阶段上下文，并非一段完整连续对话。content 不改写；版本说明、索引和总结均由 Agent 另外标注。历史指令不覆盖最新取消提测的指令。',
    conversations=messages,
    experiments=[], candidate_evidence_chains=[])

def include(ident, path, stage, note):
    raw = path.read_bytes()
    material['experiments'].append(dict(id=ident, source='Actual retained report; not a new owner utterance',
        stage=stage, source_file=path.relative_to(ROOT).as_posix(), source_sha256=hashlib.sha256(raw).hexdigest(),
        note=note, raw_report=raw.decode('utf-8')))

include('historical-wide-binding',H/'historical-wide-result.json','reference-derived candidate before the current e860 route',
    'Only supports the historical assistant excerpts about a 4M-dimension binding workload. It is not a current e860 or official performance result.')
include('observed-formal-e860',H/'formal-result-at-20261115-2156.json','formal result observed 2026-11-15 21:56 Beijing',
    'Task 47254621 belongs to the original e860: MySQL passed, E=3115.658047. The combined Prompt message does not identify its exact cause. This snapshot is not a new formal request or a new candidate score.')
for name, note in [
    ('public.json','Unchanged official f4d driver and assertions, default native constrained entrypoint; no Docker image build.'),
    ('regression.json','All 26 unchanged native assertion groups passed against the dynamic production build so fault injection can actually intercept calls.'),
    ('baseline-parity.json','7862 strict comparisons passed. The old helper hardcoded reference_commit=a08; the actual reference binary SHA is the pinned e860 binary 6a89320d...7934. New CI passes the source identity explicitly; original output is retained.'),
    ('reclaim-lifecycle.json','Actual before/after-fallocate SIGKILL, EIO/ENOSYS injection, both recovery-root live-byte manifests and reads by unmodified e860. Changed unreachable garbage is not compared as live data.'),
    ('sanitizer.json','ASan, UBSan and leak detection; eight selected semantic groups, native same production sources.'),
    ('capacity.json','Actual 2 GiB/no-swap cgroup with over 2 GiB physical persisted user data; exact/ann and committed/uncommitted SIGKILL. Sampled memory is not memory.peak.'),
    ('ordered-resources.json','Three counterbalanced rounds with sequential insertion; include both baseline and scored public reference. Native measurements, not a score.'),
    ('reclaim-resources.json','Random insertion: reclaim trades about 2% write time for a large page-cache-inclusive memory reduction. Slow samples remain.'),
    ('lto-queries.json','Rejected reference-inspired link-time optimization: six paired rounds, about 1% worse ID/distance queries. Default build unchanged.'),
    ('lto-writes.json','Rejected LTO: four shapes by four paired rounds, roughly 3-5% worse writes despite identical files. Negative evidence is retained.')]:
    include(name,V/name,'e860 optimization, production core ba96b7b',note)

def owner(needle):
    for i,m in enumerate(messages):
        if m['role']=='user' and needle in m['content']:return dict(pointer=f'/conversations/{i}/content',quote=needle)
    raise AssertionError(needle)

def result(ident, quote):
    for i,e in enumerate(material['experiments']):
        if e['id']==ident:
            assert quote in e['raw_report'];return dict(pointer=f'/experiments/{i}/raw_report',quote=quote)
    raise AssertionError(ident)

material['candidate_evidence_chains']=[
    dict(topic='做题人要求逐条核对协议与参考边界',owner_evidence=[owner('你不应该对着协议一个一个排查吗')],
        observed_result=[result('baseline-parity.json','"observations": 7862')],
        attribution='原始编号缺陷是做题人在引号中引用 Agent 发现；做题人的贡献是质疑遗漏并要求完整排查。结果是有限对拍证据，不能写成所有输入的证明。'),
    dict(topic='由做题人指定重新以已得分 e860 优化',owner_evidence=[owner('在e86045306f5940613bfcf4aa84b0ee4bf74c20f1上优化'),owner('改成e大于等于4500吧')],
        observed_result=[result('public.json','"passed": true'),result('regression.json','"completed": 26')],
        attribution='路线与目标由做题人指定；实现和验证由 Agent 执行。目标尚未通过正式平台确认。'),
    dict(topic='正确性和存储约束优先于提速',owner_evidence=[owner('请你确定mysol的样例要不通过，否则不得分'),owner('请你检查一些边界情况')],
        observed_result=[result('reclaim-lifecycle.json','"both_recovery_roots_live_bytes_identical": true'),result('capacity.json','"logical_text_bytes": 2621440000')],
        attribution='Agent 以不改变有效记录和两恢复根为边界设计回收，实际注入中断并验证旧程序可读；没有归为做题人亲自提出或执行该算法。'),
    dict(topic='做题人取消赶时限提测，允许继续验证取舍',owner_evidence=[owner('你不用提交了，请你继续持续优化，没有时间限制')],
        observed_result=[result('lto-writes.json','"passed": true')],
        attribution='该报告通过指正确性及测量完成，配对比值小于 1 表示性能回退。Agent 排除这项优化，没有消耗正式提测机会。')]
material['limitations']=[
    '没有当前候选的隐藏准入、正式性能 E、Prompt 分数或排名。最后一份正式成绩属于旧 e860，不能贴到新分支上。',
    '参考作者成绩与本地同工具链实验是两件事。没有复现作者的正式容器及平台分数。',
    '原生 GCC 11.4、内存采样和 raw io.stat 不等于平台 GCC 13.5 Docker 冷构建、专用设备 B 或 memory.peak。',
    '历史参考候选的宽向量内存收益不能泛化到标准128维；新旧阶段和报告版本分别注明。',
    '做题人的真实要求与质疑足以体现主导，但材料没有补造其未发表的算法解释或理解，也不自行评学习分。']
for chain in material['candidate_evidence_chains']:
    for evidence in chain['owner_evidence']+chain['observed_result']:
        value=material
        for part in evidence['pointer'].split('/')[1:]:value=value[int(part)] if isinstance(value,list) else value[part]
        assert isinstance(value,str) and evidence['quote'] in value and len(evidence['quote'])<=240
assert [m['content'] for m in messages[:len(history['messages'])]] == [m['content'] for m in history['messages']]
raw=(json.dumps(material,ensure_ascii=False,indent=2,allow_nan=False)+'\n').encode('utf-8')
assert len(raw)<=128*1024
for name in ('agent/conversation.json','conversation.json'):(ROOT/name).write_bytes(raw)
digest=dict(bytes=len(raw),sha256=hashlib.sha256(raw).hexdigest(),synthetic=False,owner_messages=sum(m['role']=='user' for m in messages),
    source_quotes_unchanged=True,production_core=material['current_context']['production_core'],scope='Local integrity only; no platform Prompt score claimed')
(ROOT/'agent/material-digest.json').write_bytes((json.dumps(digest,indent=2)+'\n').encode())
print(json.dumps(digest))
