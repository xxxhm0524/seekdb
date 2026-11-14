"""Build a truthful, bounded evidence document; never invent student dialogue."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
validation = ROOT/"validation/faster-review-20261114"
history = ROOT/"validation/audit-20261114"
provenance = json.loads((validation/"provenance.json").read_text(encoding="utf-8"))
assert len(provenance) == 41
assert sum(p["kind"] == "normal" for p in provenance.values()) == 37
reports = []
passed_count = 0
paths = [validation/name for name in provenance] + list(validation.glob("red-*.json")) + [p for p in history.glob("*.json") if not p.name.startswith(("linux-", "asan-", "ci-source"))]
for path in sorted(paths):
    if path.name == "ci-source.json": continue
    value = json.loads(path.read_text(encoding="utf-8"))
    name = path.stem
    current = path.parent == validation and path.name in provenance
    if current:
        assert value.get("passed") is True
        passed_count += 1
    reports.append(dict(id=name, stage=("verified_"+provenance[path.name]["kind"]) if current else "retained_development_observation",
                        source_commit=provenance[path.name]["commit"] if current else "pre-fix development build; see preserved original report",
                        source=provenance[path.name]["ci_url"] if current else path.relative_to(ROOT).as_posix(),
                        output=json.dumps(value, ensure_ascii=False, indent=2)))
assert passed_count == 41
paired = json.loads((validation/"scored-reference-paired.json").read_text(encoding="utf-8"))
comparison = []
for width in ["16", "1024"]:
    values = paired["medians"][width]
    comparison.append(f"{width}-byte text: candidate write/query {values['ours']['write_seconds']:.6f}/{values['ours']['query_seconds']:.6f}s; successful reference {values['reference']['write_seconds']:.6f}/{values['reference']['query_seconds']:.6f}s; frozen bcd4a3f baseline {values['frozen']['write_seconds']:.6f}/{values['frozen']['query_seconds']:.6f}s.")
material = {
    "format": "actual-clean-rewrite-evidence-v1",
    "synthetic": False,
    "own_formal_evaluation": "Not submitted. The user explicitly deferred formal submission pending further review. Formal feedback about the public reference belongs only to that third party.",
    "date": "2026-11-14",
    "scope": "All user instructions for the clean rewrite are included verbatim below. Agent responses and tool results are selected factual records. The older implementation and older conversations are outside this rewrite. Hidden reasoning, credentials, unrelated conversations and generated test databases are omitted.",
    "attribution": "The implementation, technical explanations and test design below are Agent work. The user supplied the task and the explicit decision to replace the earlier implementation. No student explanation of database mechanisms or experiments has been invented, and Agent work must not be treated as proof of student understanding.",
    "conversations": [
        {"id":"user-task", "role":"user", "content":"/goal [https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/lg8enxlmwg69unt6](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/lg8enxlmwg69unt6)请你独立完成这道赛题，之前对话写的代码有很多问题，来来回回修改了10次也无法得分。对此你也可以看别人执行成功并得分的github的公开仓库中的代码"},
        {"id":"user-rewrite", "role":"user", "content":"我觉得你在之前代码的基础上完成不如完全重新完成，那里已经成为了史山代码"},
        {"id":"agent-rewrite", "role":"assistant", "content":"可以，改为从零重写。我会在新的独立目录中实现，旧代码保留作备份；只使用官方规则、协议和测试来建立验收标准。新实现会把 SQL、存储、事务恢复和向量查询分开，便于逐层验证。"},
        {"id":"agent-red-loop", "role":"assistant", "content":"验收循环已跑通：官方空壳程序在 20 个公开用例中全部返回 NOT_IMPLEMENTED，测试能明确检出功能缺失。Windows 下官方驱动的进程通信层无法运行，我补了本地通信适配，断言仍使用未修改的官方比较器；后续还会在 Linux 容器中复测。"},
        {"id":"agent-linux-progress", "role":"assistant", "content":"完整 Linux 容器验证已通过，包括把约 164 MB 业务数据放进 96 MiB 内存容器后进行查询、提交、回滚和强杀恢复。接下来整理运行说明、真实的 conversation.json 和提交包，再将验证版本合入规定的 2026-competition-round2 分支。"},
        {"id":"user-formal-hold", "role":"user", "content":"我觉得先不要提测，确保没有任何问题后再提测"},
        {"id":"user-expert-audit", "role":"user", "content":"你作为一名经验老道的数据库专家，这明显不是你应该犯的问题，请你吸取经验并仔细查找还有没有类似的问题"},
        {"id":"user-successful-reference", "role":"user", "content":"你可以看看别人公开仓库的代码（前提是别人已经成功提交）"},
        {"id":"agent-audit-progress", "role":"assistant", "content":"这处容量问题本应在设计阶段就用“有效数据不变、持续更新”的负载检出。之前的验证覆盖不足，我会据此调整审查方式：除了结果正确，还要检查资源是否持续增长、失败前后状态是否一致，以及非法输入是否被意外接受。发现的每个问题都会保留失败用例和修复后的验证记录。"},
        {"id":"user-formal-hold-again", "role":"user", "content":"继续暂缓正式提测"},
        {"id":"user-continued-successful-reference", "role":"user", "content":"你可以继续看看别人github的代码（前提对方通过），确保我们这次可以得到分数"},
        {"id":"user-borrow-and-improve", "role":"user", "content":"你可以直接借鉴别人的代码，并且最好还能比他们的快"},
        {"id":"user-continue-faster-review", "role":"user", "content":"继续"},
        {"id":"user-direct-reference-question", "role":"user", "content":"你为什么不直接看别人的代码呢，这是符合比赛规定的"}
    ],
    "agent_design": [
        "Committed B+ tree pages are immutable. Each SQL statement owns a new set of pages and a tail mark. On statement failure the new tail is discarded, preserving earlier successful statements in the transaction.",
        "All record/catalog/page writes complete before publishing the alternate checksummed root; the whole file is then synced before success is acknowledged. Like the reference, this uses the specified process-crash model with a surviving kernel. Machine-power-loss ordering would require an additional earlier barrier and is not established by these tests. Recovery selects a valid root and truncates only uncommitted tail data.",
        "Table data and primary-key directories remain on disk. A bounded page cache, bounded vector snapshot/KD tree, external sorting and spooled responses avoid loading the whole database or result set.",
        "Both search modes return exact results. Ordinary predicates are applied before candidate selection. KD bounds only prune strictly worse subtrees, retaining ties and primary-key ordering. Float32 storage/query conversion and ordered double accumulation are independently checked.",
        "Between transactions, committed history is compacted by streaming current rows to a separate fully synced database and atomically replacing the main file. The old database remains intact until replacement; the current row/page references are refreshed before any later statement executes.",
        "Projected reads skip unused variable-length payloads. Response spill files finish flush/close/open before stdout publication. A failure after publication terminates the channel rather than appending a second JSON object.",
        "The continued reference review found a performance admission cliff caused by retaining unrelated document text. The snapshot now owns only vector/filter/sort working columns and loads uncached winner fields by stored row reference. Its budget remains 16 MiB.",
        "Coordinates are stored once in 32-candidate blocks. Every lane accumulates dimensions in the same scalar float64 order without FMA. CPU dispatch has scalar, AVX2 and AVX-512 implementations. Direct numeric binding validates the entire vector grammar and finite float32 boundaries; raw NUL tails are explicitly rejected in both vector literals and JSONL frames.",
        "Following the user's request to borrow successful designs, working rows are physically clustered by a filter column and searched with binary bounds. A bounded int8 auxiliary index uses conservative triangle-inequality error certificates; every returned distance is computed from original float32 values with the existing ordered-double loop. Unsupported or unproved cases fall back to complete exact search. The remaining shortlist can be skipped only when its entire suffix is certified worse, including rounded-sqrt ties. The successful reference's TypedScanCache::finish also keeps source rows contiguous for certificate reranking; this layout choice is now used directly, with a one-time return to blocked layout when a full scan is needed.",
        "INSERT groups at most 64 KiB of record bytes and 1024 positions; larger records bypass the group. B+ tree lookups and unsplit updates work directly on owned pages. The copy-on-write ownership and SQL-statement rewind boundaries remain unchanged.",
        "A diagnostic Linux CPU profile identified per-character input decoding and copying. SQL quoted spans now use bulk copies while preserving escape handling. The pinned general-purpose RapidJSON decoder validates raw UTF-8 and JSON escapes, checks duplicate keys at every depth, uses owned SSE padding, and delegates float or out-of-range numeric frames to the original strict decoder. Neither JSON library executes database core functions."
    ],
    "experiments": reports,
    "agent_retrospective": [
        "The first newly written version passed the 20 public cases, but that was not treated as formal admission. Independent state-machine, large-tree, external-sort, SIGKILL, hostile float32 and low-memory tests were then run.",
        "An added boundary test initially failed for the finite float32 decimal 3.4028235e38. Comparing the input double directly to FLT_MAX incorrectly rejected a value that rounds to finite float32. The threshold was changed to the overflow rounding midpoint. The failing record and successful retest are preserved below.",
        "The low-memory test actually stored 163840000 payload bytes in a container limited to 100663296 bytes. This is evidence for external-memory handling, not a claim based on an in-memory simulation.",
        "The user's objection led to a broader lifecycle audit. Four real defects were reproduced: unbounded committed history, token concatenation accepting malformed vectors, unneeded wide-text reads, and partial JSON publication when the spill file's final flush failed. The preserved red reports and passing regressions support the fixes; they do not retroactively establish that the earlier candidate was bug-free.",
        "A frozen public commit with author-published formal feedback was compiled as a reference. 1600 operations / 1629 requests across three tables agreed with both implementations and independent Python state. The original backslash escaping difference is recorded separately because the task does not specify that dialect choice. Later clustering and certification designs were borrowed and reimplemented with explicit attribution; the reference's learning material was not copied.",
        "The expanded capacity test used the actual 2 GiB memory budget with 2621440000 bytes of business text. Compaction was interrupted exactly before and after file replacement. Recovery was then repeatedly interrupted at the real truncation syscall, twice before and twice after it, without readiness between interruptions. Committed rows survived and uncommitted changes were rolled back. ASan, UBSan and library assertions covered the protocol, state machine, large tree, compaction and all 120 column layouts.",
        "The final file-opening audit reproduced two data-preservation defects: unknown nonempty files were initialized as empty, and an injected allocation failure while decoding the latest catalog caused rollback to an older committed root and truncation. Unknown formats now fail without changing bytes. Only recognized interrupted-empty-initialization patterns are retried. Recovery validates candidates newest-first and propagates allocation/schema failures without downgrading acknowledged state. Both failures have retained red cases and passing production/sanitizer regressions.",
        "The renewed user request led to controlled same-toolchain performance comparisons with the publicly documented successful commit. Before the change, 1024-byte text exhausted the whole-row snapshot and caused much slower disk queries. A working-set pread probe reproduced that cause, then passed with projected snapshots. Blocked exact distance and direct numeric parsing were tested separately before integration.",
        "An earlier controlled replay rotated the original 977fb6d baseline, then-current candidate and successful reference for three rounds. Its candidate query medians were 0.5805/0.5625s, before the subsequent clustering, integer-certificate and parser changes. Those earlier observations are retained under the earlier review directory rather than relabeled as the final candidate.",
        "The final comparison rotates the bcd4a3f baseline, candidate and successful reference on the same runner for three rounds using the same toolchain and resource limits. " + " ".join(comparison) + " These are measured development timings, not official E, rank or scores. The raw io.stat is retained without claiming official isolated-device B.",
        "The added container byte-invariance test first failed because the host user could not read the container's mode-0600 file. Its hash is now read inside the owning container, without weakening production permissions or removing assertions. A secondary missing-binary failure followed the skipped extraction step; both failed records are retained and the full workflow was rerun.",
        "Exact root-publication and sync syscall cuts preserve two-table atomicity. An ordinary acknowledged transaction performs one complete-file durability sync, and a 5000-row batched insertion performed 66 file writes. This is a process-crash experiment, not a power-loss experiment.",
        "The quoted-span tests round-tripped 528 literals including an 80 KB value, backslashes, doubled delimiters, NUL and Unicode. The request decoder matched the established strict parser on 48279 valid/malformed cases, with float IDs and unusual exponents retaining the original decoder. The normal and sanitized production-source tests are listed below.",
        "Local benchmark times and CI results are not formal platform scores. The nine retained formal inputs are unavailable. Formal admission and ranking require the platform to evaluate the locked submission."
    ],
    "limitations": [
        "No real student technical interpretation was supplied during this autonomous rewrite; no such interpretation is synthesized here.",
        "Compaction runs between transactions and needs temporary space for both old and new files; long transactions still need space for their uncommitted tails.",
        "The active catalog, one SQL request and one decoded row must individually fit memory. The database as a whole need not fit memory.",
        "For tables beyond the vector cache budget, exact disk scans preserve correctness and recall but can be slower."
    ]
}
data = (json.dumps(material, ensure_ascii=False, indent=2, allow_nan=False)+"\n").encode("utf-8")
assert len(data) <= 128*1024
(ROOT/"agent").mkdir(exist_ok=True)
for path in [ROOT/"conversation.json", ROOT/"agent/conversation.json"]:
    path.write_bytes(data)
print(json.dumps(dict(material_bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), experiments=len(reports))))
