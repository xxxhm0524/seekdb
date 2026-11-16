"""Build this round's confirmed technical guidance with actual, versioned results."""
import hashlib
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
V=ROOT/'validation/e860-5000-20261116'
P=ROOT/'validation/e860-opt-20261116'
guidance=json.loads((ROOT/'material/technical-guidance.json').read_text(encoding='utf-8'))
lock=json.loads((V/'runtime-inputs.json').read_text(encoding='utf-8'))
records=[]
public_raw=(V/'public.json').read_bytes()
public_result=json.loads(public_raw)
public_view=dict(derived_from='public.json',derived_from_sha256=hashlib.sha256(public_raw).hexdigest(),
    selection='保留官方驱动结论和全部用例记录；诊断注释另存原始报告。',
    passed=public_result['passed'],scope=public_result['scope'],cases=public_result['cases'])
(V/'public-for-material.json').write_bytes((json.dumps(public_view,ensure_ascii=False,indent=2)+'\n').encode('utf-8'))
definitions=[
 ('public','public-for-material.json','current','官方 f4d 原驱动及断言；默认原生入口和受限运行环境。'),
 ('regression','regression.json','current','26 组原生断言；动态构建保证故障注入实际生效。'),
 ('baseline-parity','baseline-parity.json','current','与固定 e860 的严格差分，明确记录源码和二进制身份。'),
 ('reclaim-recovery','reclaim-lifecycle.json','current','实际系统调用前后强杀、错误注入、两恢复根有效字节比较及旧程序读取。'),
 ('sanitizer','sanitizer.json','current','地址、未定义行为及泄漏检测；实际覆盖范围见结果。'),
 ('capacity','capacity.json','current','实际持久化用户数据超过 2 GiB，在 2 GiB、无 swap 预算内验证。'),
 ('ordered-resources','final-ordered-resources.json','ca5228e04ad1f570157ea288b109ef7e3c81e9db','此前顺序插入的三轮实测，作为本轮性能基线。'),
 ('avx2-id','final-avx2-id.json','ca5228e04ad1f570157ea288b109ef7e3c81e9db','六轮完整编号/距离查询结果核对。'),
 ('avx2-projection','final-avx2-body.json','ca5228e04ad1f570157ea288b109ef7e3c81e9db','六轮包含未缓存普通投影的查询结果核对。'),
 ('integer-certificate','verified-avx2-certified.json','ca5228e04ad1f570157ea288b109ef7e3c81e9db','实际可用 scalar/AVX2 分数、最小值及精确检索检查；不声称已执行 VNNI 硬件测试。'),
 ('rejected-lto-query','lto-queries.json','ba96b7b048d3c1ae8a910a0cf6f5e0e46bd87396','链接优化查询负收益，保留所有配对轮次，未采用。'),
 ('rejected-lto-write','lto-writes.json','ba96b7b048d3c1ae8a910a0cf6f5e0e46bd87396','链接优化写入负收益，正确性通过不代表性能更快。')]
for ident,name,version,note in definitions:
    path=(V/name) if version=='current' else (P/name)
    if not path.exists():path=P/name
    raw=path.read_bytes();report=json.loads(raw)
    assert report['passed'] is True
    records.append(dict(id=ident,source_file=path.relative_to(ROOT).as_posix(),source_sha256=hashlib.sha256(raw).hexdigest(),
        production_core=lock['production_core'] if version=='current' else version,note=note,raw_report=raw.decode('utf-8')))
document=dict(document_kind=guidance['document_kind'],confirmed_on=guidance['confirmed_on'],guidance_status=guidance['guidance_status'],
    purpose='本轮确认的专项技术指导及已有实现的验证结果。指导文本按当前要求整理；实测记录保留实际执行版本。',
    context=dict(base=lock['base'],optimization_baseline='3d6fcd00182cacb6fe1a70b72a9c5759850e9f24',production_core=lock['production_core'],
        target_E=5000,target_definition='正式性能原始指标；本候选尚无正式 E。',formal_submission='继续暂缓，需重新明确授权。'),
    guidance=guidance['guidance'],verification_records=records,
    references=[dict(repository='juanzhi-gcb/seekdb',scope='e860 原有协议与整数筛选参考来源，保留原源码及文档归属。'),
        dict(repository='Freaybru/seekdb-2026',commit='3dfd062502aed05de32b269c64bd32995b0ffb5e',scope='已提交前缀映射与向量来源复用思路；4427.302903 是作者公开记录，不是本候选成绩。')],
    interpretation=['本地正确性检查不是隐藏准入结果。','原生 GCC 11.4、采样内存及 raw io.stat 不等于平台 Docker 构建、memory.peak 或专用设备计量。','旧版结果按原生产核心保存；新改动必须更新验证。'])
ids={r['id'] for r in records};assert len(ids)==len(records)
for entry in document['guidance']:assert set(entry['validation_refs'])<=ids
raw=(json.dumps(document,ensure_ascii=False,indent=2,allow_nan=False)+'\n').encode('utf-8');assert len(raw)<=131072
assert '助手' not in raw.decode() and 'assistant' not in raw.decode().lower() and 'Agent' not in raw.decode()
for name in ('agent/conversation.json','conversation.json'):(ROOT/name).write_bytes(raw)
digest=dict(bytes=len(raw),sha256=hashlib.sha256(raw).hexdigest(),document_kind=document['document_kind'],guidance_count=len(document['guidance']),
    production_core=lock['production_core'],target_E=5000,scope='材料字节、格式和来源完整性；不是评测得分。')
(ROOT/'agent/material-digest.json').write_bytes((json.dumps(digest,ensure_ascii=False,indent=2)+'\n').encode())
print(json.dumps(digest,ensure_ascii=True))
