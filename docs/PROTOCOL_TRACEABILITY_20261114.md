# 协议逐条验收矩阵

本次完整题意复核发现原参考解析器的 64 层保护也会违反不透明编号要求，并修复 SQL 操作符词法类型遗漏。当前依据为 [全题复核](ALL_REQUIREMENTS_20261114.md)；原始上一轮记录保留在 `validation/edge-review-20261114/`。正式提测继续暂缓。

[当前核心完整 CI](https://github.com/xxxhm0524/seekdb/actions/runs/43607590405) 有 54 份报告通过。20 条协议要求映射到这些实际报告，不作为额外独立得分项；报告版本和 SHA256 见 `validation/full-review-20261114/protocol-traceability.json`。

| 编号 | 条款 | 验收要求 | 实际证据 |
| --- | --- | --- | --- |
| P01 | STDIO_PROTOCOL §1.1–2 | UTF-8、完整单行 JSON 对象和物理 LF | [linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json)、[asan-protocol-contract](../validation/full-review-20261114/asan-protocol-contract.json) |
| P02 | STDIO_PROTOCOL §1.3、§9 | 每条响应及时刷新，交互式等待而非预写全部输入 | [linux-official](../validation/full-review-20261114/linux-official.json)、[linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json) |
| P03 | STDIO_PROTOCOL §1.4、§1.6 | stdout 没有日志、空行、提示符或额外状态行 | [linux-official](../validation/full-review-20261114/linux-official.json)、[linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json) |
| P04 | STDIO_PROTOCOL §1.5 | 每个请求恰好一个响应 | [linux-official](../validation/full-review-20261114/linux-official.json)、[linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json) |
| P05 | STDIO_PROTOCOL §1.5、§3 id | 所有响应路径按值和 JSON 类型保留 id，包括超 64 位整数与深层嵌套编号 | [linux-protocol-ids](../validation/full-review-20261114/linux-protocol-ids.json)、[asan-protocol-ids](../validation/full-review-20261114/asan-protocol-ids.json)、[linux-nested-ids](../validation/full-review-20261114/linux-nested-ids.json)、[asan-nested-ids](../validation/full-review-20261114/asan-nested-ids.json) |
| P06 | STDIO_PROTOCOL §1.7 | 响应没有 NaN、Infinity、重复对象键 | [linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json)、[linux-vector-response-budget](../validation/full-review-20261114/linux-vector-response-budget.json) |
| P07 | STDIO_PROTOCOL §2 | 输入前主动发出准确 ready；恢复完成后才就绪 | [linux-official](../validation/full-review-20261114/linux-official.json)、[linux-crash-commit](../validation/full-review-20261114/linux-crash-commit.json)、[linux-recovery-boundaries](../validation/full-review-20261114/linux-recovery-boundaries.json) |
| P08 | STDIO_PROTOCOL §3 请求字段 | 校验 execute、sql、search_mode 的值和类型 | [linux-protocol-ids](../validation/full-review-20261114/linux-protocol-ids.json)、[linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json) |
| P09 | STDIO_PROTOCOL §3 完整解析 | 先验证整帧及深层容器再执行；分号和换行在 SQL 字符串内保持原值 | [linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json)、[linux-complete-input](../validation/full-review-20261114/linux-complete-input.json)、[linux-nested-ids](../validation/full-review-20261114/linux-nested-ids.json) |
| P10 | STDIO_PROTOCOL §3 查询成功 | columns 与 rows 顺序一致，二维数组，空集为 [] | [linux-official](../validation/full-review-20261114/linux-official.json)、[linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json) |
| P11 | STDIO_PROTOCOL §3 修改成功 | DML affected_rows 是实际整数；DDL/TX 可省略可选字段 | [linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json)、[linux-reference-boundaries](../validation/full-review-20261114/linux-reference-boundaries.json) |
| P12 | STDIO_PROTOCOL §3 执行失败 | ok 是布尔值，error.code 非空；预期 SQL 错误后进程可继续 | [linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json)、[linux-invariants](../validation/full-review-20261114/linux-invariants.json)、[linux-token-kinds](../validation/full-review-20261114/linux-token-kinds.json) |
| P13 | STDIO_PROTOCOL §3 数据类型 | BIGINT 不经浮点或字符串，向量数组和距离都是有限数值 | [linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json)、[linux-sql-numeric-forms](../validation/full-review-20261114/linux-sql-numeric-forms.json)、[linux-vector-response-budget](../validation/full-review-20261114/linux-vector-response-budget.json) |
| P14 | STDIO_PROTOCOL §4 | shutdown 回滚未提交事务，原编号确认，flush 后退出 0，无尾随输出 | [linux-official](../validation/full-review-20261114/linux-official.json)、[linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json)、[linux-protocol-ids](../validation/full-review-20261114/linux-protocol-ids.json) |
| P15 | STDIO_PROTOCOL §4 持久性 | 确认提交不依赖 shutdown，异常重启保留已确认数据 | [linux-crash-commit](../validation/full-review-20261114/linux-crash-commit.json)、[linux-commit-publish](../validation/full-review-20261114/linux-commit-publish.json)、[linux-capacity-2g](../validation/full-review-20261114/linux-capacity-2g.json) |
| P16 | STDIO_PROTOCOL §6 | 驱动 restart/mode 指令正确包装，每个用例独立目录，重启复用目录 | [linux-official](../validation/full-review-20261114/linux-official.json)、[linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json) |
| P17 | STDIO_PROTOCOL §7、§9 | 默认 Docker JSONL 入口、只读根、断网、2 CPU/2 GiB 和权限限制 | [linux-official](../validation/full-review-20261114/linux-official.json)、[linux-capacity-2g](../validation/full-review-20261114/linux-capacity-2g.json) |
| P18 | protocol_check/config.json max_response_bytes | 给定 1 万行×128维向量回归中，结果在 16 MiB 内且全部 float32 位模式往返不变 | [linux-vector-response-budget](../validation/full-review-20261114/linux-vector-response-budget.json) |
| P19 | 赛题关闭与事务语义；补充 EOF 验证 | EOF 回滚未提交数据且没有额外输出 | [linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json)、[asan-protocol-contract](../validation/full-review-20261114/asan-protocol-contract.json) |
| P20 | STDIO_PROTOCOL §1.5、§3 成功/失败 | 响应暂存失败只返回一条完整错误，编号类型仍保留 | [linux-response-failure](../validation/full-review-20261114/linux-response-failure.json) |

深层编号用显式栈完整验证，最大 8192 层的独立测试通过；每层重复键、非法转义、尾逗号及不匹配括号仍须在执行 SQL 前拒绝。大整数、原 JSON 类型、完整行、就绪/刷新、关闭/EOF、响应暂存和进程恢复仍按原要求核对。

赛题不要求 SQL NULL，未支持语法返回错误；`id:null` 已验证。响应字节测试为给定规模，恢复测试为进程崩溃模型。更完整的边界及平台未验证项见全题复核表。
