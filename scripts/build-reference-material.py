"""Build selected authentic owner/agent records for this independently forked candidate."""
import hashlib
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
E=ROOT/'validation/reference-opt-20261115'
provenance=json.loads((E/'provenance.json').read_text(encoding='utf-8'))
messages=[
    dict(role='user',source='actual owner message',content='要不你也不要用docker了，你直接在a6000上面完成题目吧'),
    dict(role='user',source='actual owner message',content='[https://github.com/juanzhi-gcb/seekdb](https://github.com/juanzhi-gcb/seekdb)\n请你对照这个仓库进行修改，这个仓库的代码已经得分了'),
    dict(role='user',source='actual owner message',content='你能不能确定参考代码能通过的边界情况我们的代码也能完美执行'),
    dict(role='user',source='actual owner message',content='那么我觉得你可以继续检查'),
    dict(role='user',source='actual owner message',content='你的代码不是在别人的基础上构建的吗，为什么别人不会出现这种问题，但我们会'),
    dict(role='user',source='actual owner message',content='要不你直接在别人的代码基础上优化吧，我们现在的代码就废弃了吧'),
    dict(role='assistant',source='Codex visible progress, actual excerpt',content='可以，停止修补现有实现，改为以已核对得分反馈的参考版本为基础优化。现有代码和证据保留归档。'),
    dict(role='assistant',source='Codex visible progress, actual excerpt',content='三项无收益的尝试都已排除。现在测试一个较小的改动：减少向量解析时的中间缓冲分配，保持参考版本的数值转换结果和报错顺序，再检查端到端性能是否有稳定收益。'),
    dict(role='assistant',source='Codex visible progress, actual excerpt',content='缓冲优化的收益已测到：约 419 万维的合法查询，进程峰值内存从约 **74.8 MiB 降至 42.8 MiB**。标准 128 维负载的速度没有稳定提升，我不会宣称它更快。')
]
experiments=[]
for name,record in provenance['reports'].items():
    raw=(E/name).read_bytes();assert hashlib.sha256(raw).hexdigest()==record['sha256']
    result=json.loads(raw);assert result['passed'] is True
    if name=='wide-bind.json':result={k:v for k,v in result.items() if k!='samples'}
    if name=='native-replay.json':result={**result,'stages':[{k:v for k,v in s.items() if k!='argv'} for s in result['stages']]}
    experiments.append(dict(source='actual A6000 native tool result',file='validation/reference-opt-20261115/'+name,sha256=record['sha256'],retained_experiment=record['retained_experiment'],result=result))
material=dict(format='reference-derived-actual-evidence-v1',synthetic=False,date='2026-11-15',
    provenance=dict(code_repository='https://github.com/juanzhi-gcb/seekdb',upstream_commit=provenance['baseline_commit'],runtime_core=provenance['runtime_core'],code_attribution='The full implementation is derived from the named public reference. Codex changed three runtime files for quoted-vector buffering; the previous independent implementation is not the runtime base.'),
    owner_contribution_limit='Owner messages are instructions and reviews. They do not establish personal implementation or technical learning. The reference author dialogue, learning evidence and platform results are not this owner contribution. All new implementation, benchmark interpretation and tests are Agent work.',
    messages=messages,experiments=experiments,
    agent_observations=[
        'The owner explicitly abandoned the independent implementation. A clean fork of the scored-runtime-qualified reference replaced it as the development base; protocol, parser, store, transaction and recovery implementations were inherited directly.',
        'The source comparison against the upstream runtime is three files: binder.cpp, value.cpp and a shared float32 conversion header. The numerical path still converts decimal to binary64 and then binary32. Complete lexical validity, numeric failure, dimension and float32 range error priority is retained, including errors after an earlier range overflow.',
        'The optimization avoids the temporary double component array for simple quoted vectors; complex syntax keeps the original lexer path. Existing slow/fast binder checks plus added error priority combinations compare bitwise values and exact errors.',
        'Three preliminary changes did not yield stable gains and were discarded: IPO, disabling integer screening and disabling conservative screening. Their actual successful-correctness benchmark results remain available and are not called performance wins.',
        'Six paired rounds verify 48000 standard-workload query results. Median cycle speedups across four shapes are approximately 1.005, 1.002, 1.003 and 0.982. This does not establish a consistent 128D speed benefit. No slow sample was removed.',
        'The retained memory result is a separate valid wide-vector binding workload with LIMIT 0 on an empty table. Actual process VmHWM medians at dimension 4194304 are 76564 KiB and 43828 KiB, a 42.8 percent reduction. It is not formal M, full-database capacity or a default-workload speed claim.',
        'Original and candidate each pass 70 CTests and 20 unchanged public protocol cases. The candidate also passes 7862 reference comparisons, independent scalar expected values, large IDs, ASan/UBSan/leak diagnostics and actual hardened default-entrypoint transaction/restart checks.',
        'Initial local runner calibration failed on a temporary-directory guard and on copied executable permissions; logs were retained. Corrected runs are separately named. An original mixed-CRLF CMake file caused a normalized-hash/raw-byte mismatch during evidence checks; the input manifest now retains both raw Git byte hashes and normalized hashes, and all 38 exact runtime inputs were checked.',
        'The new native reproduction runner was actually replayed in a fresh temporary directory while reusing verified existing binaries, explicitly recorded as such. Docker builds and remote GitHub Actions were not run. Author-reported success is not a formal evaluation of this fork.'
    ],
    limitations=['No official hidden admission, performance score, Prompt score or rank exists for this candidate.','No new Docker build/cold pull or remote GitHub CI result exists. GCC 11.4 native runs do not establish the Alpine builder path.','Finite comparison cases are not a universal proof. The inherited depth/resource/history limits still apply.','Formal push/submission is not authorized. A later formal attempt needs owner approval for the complete final SHA, after 22:00 Asia/Shanghai, at most once per day, no adjacent midnight attempts, current quota/history checks and a durable one-attempt marker.'])
raw=(json.dumps(material,ensure_ascii=False,indent=2,allow_nan=False)+'\n').encode('utf-8');assert len(raw)<=131072
for name in ['agent/conversation.json','conversation.json']:(ROOT/name).write_bytes(raw)
digest=dict(material_sha256=hashlib.sha256(raw).hexdigest(),bytes=len(raw),runtime_core=provenance['runtime_core'],synthetic=False,scope='Local integrity only; not a platform Prompt digest or score')
(ROOT/'agent/material-digest.json').write_bytes((json.dumps(digest,indent=2)+'\n').encode())
print(json.dumps(digest))
