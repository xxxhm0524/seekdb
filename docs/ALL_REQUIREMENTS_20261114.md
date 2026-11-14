# 全题要求复核与 MySQL 逐用例结果

本次复核发现并修复了两处此前遗漏，不能把上一轮 47 项通过或参考实现成功解释为完全符合协议。修复核心为 `9716ef5ad8b5fd82dd8ab3e5389a7820f9655b30`。正式提测继续暂缓。

## 依据与核对范围

* 重新读取[语雀赛题原文](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/lg8enxlmwg69unt6)，页面内容更新时间为 2026-11-11T10:25:23Z。相较旧本地副本，实质差异是补充公开题包链接、明确性能查询默认 exact。本实现默认 exact，两个模式均精确计算。源内容与转换文本哈希保存在 `validation/full-review-20261114/current-problem-*.json`。
* 从官方规定分支重新获取并核对 49 个题包文件，全部与冻结基点一致。协议、驱动、配置和断言均未修改。原文 §3.1 仍有旧封装表述；本实现采用题包 `STDIO_PROTOCOL.md` 明确规定且实际驱动使用的 JSONL。
* 64 条要求中，通信 20 条，SQL 14 条，向量 5 条，存储/事务 11 条，环境 4 条，交付 6 条，平台专属 4 条。该数量是追溯条目数，不是 64 个正式测试或得分项。
* [完整 Linux CI](https://github.com/xxxhm0524/seekdb/actions/runs/43607590405) 保存 54 份通过报告，包括正常构建、ASan/UBSan、官方逐用例结果、两处新缺陷、独立模型、强杀/恢复故障注入和大于内存的实际数据。原始报告及来源哈希见 `validation/full-review-20261114/`。最终发布的源码逐文件哈希和二进制还须与该核心一致；材料在最终分支 CI 再次验收。

## 官方 MySQL 公开样例：20/20

这些结果来自实际 Linux 镜像中的未修改官方 `protocol_check/run.py`，使用原 `.test` 与 `.result`，不是替代断言或空壳程序。每个样例是独立数据目录；同用例重启复用目录。

| 用例 | 原断言中的 SQL 数量 | 结果 |
| --- | ---: | --- |
| `sql_crud` | 7 | PASS |
| `sql_comparisons` | 4 | PASS |
| `sql_ordering` | 4 | PASS |
| `sql_empty` | 3 | PASS |
| `sql_primary_key` | 4 | PASS |
| `sql_unicode` | 3 | PASS |
| `sql_bigint` | 3 | PASS |
| `sql_unsupported` | 3 | PASS |
| `storage_restart` | 3 | PASS |
| `vector_euclidean` | 3 | PASS |
| `transaction_commit` | 6 | PASS |
| `transaction_rollback` | 6 | PASS |
| `storage_mutations` | 6 | PASS |
| `storage_multiple_tables` | 6 | PASS |
| `vector_ties_and_filter` | 5 | PASS |
| `vector_float32` | 3 | PASS |
| `vector_index_maintenance` | 9 | PASS |
| `transaction_failed_insert` | 8 | PASS |
| `transaction_autocommit_atomic` | 4 | PASS |
| `transaction_shutdown_rollback` | 5 | PASS |

完整报告：[linux-official.json](../validation/full-review-20261114/linux-official.json)。20 个公开样例全部通过，正式还有 9 个隐藏用例；本轮没有正式准入成绩。

## 本次复现与修复

| 缺陷 | 原状态 | 处理与回归 |
| --- | --- | --- |
| 合法编号嵌套 65 层 | 固定 64 层保护拒绝整帧，错误响应将 id 变成 null。整数、null、字符串叶子都能复现。 | 使用显式栈验证容器，保留完整原始编号，不随深度递归调用；44 个有效请求、5 个深层非法帧，最大 8192 层。原始失败与正常/Sanitizer 结果均保留。 |
| `WHERE n '=' 20` | 操作符位置只比较 token 内容，把字符串或引用标识符误当符号，SELECT/UPDATE/DELETE 都受影响。 | 操作符必须为未引用符号；140 个无效语句拒绝、14 个合法比较通过，核对无副作用、之前事务修改可提交及强杀重启。 |

补充的题意测试在有界向量缓存与关闭缓存两种路径各执行 293 次 SQL 检查，覆盖模式、过滤、并列、Unicode、BIGINT、非法类型/语法、索引维护、失败语句、提交回滚和重启。预期值来自独立 Python 状态与数值模型，不以参考解析器作真值。

## 要求与证据逐条对应

| 编号 | 来源 | 要求 | 核对状态及证据 |
| --- | --- | --- | --- |
| P01 | STDIO_PROTOCOL §1.1–2 | UTF-8、完整单行 JSON 对象和物理 LF | 已执行，有限覆盖；[linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json)、[asan-protocol-contract](../validation/full-review-20261114/asan-protocol-contract.json) |
| P02 | STDIO_PROTOCOL §1.3、§9 | 每条响应及时刷新，交互式等待而非预写全部输入 | 已执行，有限覆盖；[linux-official](../validation/full-review-20261114/linux-official.json)、[linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json) |
| P03 | STDIO_PROTOCOL §1.4、§1.6 | stdout 没有日志、空行、提示符或额外状态行 | 已执行，有限覆盖；[linux-official](../validation/full-review-20261114/linux-official.json)、[linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json) |
| P04 | STDIO_PROTOCOL §1.5 | 每个请求恰好一个响应 | 已执行，有限覆盖；[linux-official](../validation/full-review-20261114/linux-official.json)、[linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json) |
| P05 | STDIO_PROTOCOL §1.5、§3 id | 所有响应路径按值和 JSON 类型保留 id，包括超 64 位整数与深层嵌套编号 | 已执行，有限覆盖；[linux-protocol-ids](../validation/full-review-20261114/linux-protocol-ids.json)、[asan-protocol-ids](../validation/full-review-20261114/asan-protocol-ids.json)、[linux-nested-ids](../validation/full-review-20261114/linux-nested-ids.json)、[asan-nested-ids](../validation/full-review-20261114/asan-nested-ids.json) |
| P06 | STDIO_PROTOCOL §1.7 | 响应没有 NaN、Infinity、重复对象键 | 已执行，有限覆盖；[linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json)、[linux-vector-response-budget](../validation/full-review-20261114/linux-vector-response-budget.json) |
| P07 | STDIO_PROTOCOL §2 | 输入前主动发出准确 ready；恢复完成后才就绪 | 已执行，有限覆盖；[linux-official](../validation/full-review-20261114/linux-official.json)、[linux-crash-commit](../validation/full-review-20261114/linux-crash-commit.json)、[linux-recovery-boundaries](../validation/full-review-20261114/linux-recovery-boundaries.json) |
| P08 | STDIO_PROTOCOL §3 请求字段 | 校验 execute、sql、search_mode 的值和类型 | 已执行，有限覆盖；[linux-protocol-ids](../validation/full-review-20261114/linux-protocol-ids.json)、[linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json) |
| P09 | STDIO_PROTOCOL §3 完整解析 | 先验证整帧及深层容器再执行；分号和换行在 SQL 字符串内保持原值 | 已执行，有限覆盖；[linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json)、[linux-complete-input](../validation/full-review-20261114/linux-complete-input.json)、[linux-nested-ids](../validation/full-review-20261114/linux-nested-ids.json) |
| P10 | STDIO_PROTOCOL §3 查询成功 | columns 与 rows 顺序一致，二维数组，空集为 [] | 已执行，有限覆盖；[linux-official](../validation/full-review-20261114/linux-official.json)、[linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json) |
| P11 | STDIO_PROTOCOL §3 修改成功 | DML affected_rows 是实际整数；DDL/TX 可省略可选字段 | 已执行，有限覆盖；[linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json)、[linux-reference-boundaries](../validation/full-review-20261114/linux-reference-boundaries.json) |
| P12 | STDIO_PROTOCOL §3 执行失败 | ok 是布尔值，error.code 非空；预期 SQL 错误后进程可继续 | 已执行，有限覆盖；[linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json)、[linux-invariants](../validation/full-review-20261114/linux-invariants.json)、[linux-token-kinds](../validation/full-review-20261114/linux-token-kinds.json) |
| P13 | STDIO_PROTOCOL §3 数据类型 | BIGINT 不经浮点或字符串，向量数组和距离都是有限数值 | 已执行，有限覆盖；[linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json)、[linux-sql-numeric-forms](../validation/full-review-20261114/linux-sql-numeric-forms.json)、[linux-vector-response-budget](../validation/full-review-20261114/linux-vector-response-budget.json) |
| P14 | STDIO_PROTOCOL §4 | shutdown 回滚未提交事务，原编号确认，flush 后退出 0，无尾随输出 | 已执行，有限覆盖；[linux-official](../validation/full-review-20261114/linux-official.json)、[linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json)、[linux-protocol-ids](../validation/full-review-20261114/linux-protocol-ids.json) |
| P15 | STDIO_PROTOCOL §4 持久性 | 确认提交不依赖 shutdown，异常重启保留已确认数据 | 已执行，有限覆盖；[linux-crash-commit](../validation/full-review-20261114/linux-crash-commit.json)、[linux-commit-publish](../validation/full-review-20261114/linux-commit-publish.json)、[linux-capacity-2g](../validation/full-review-20261114/linux-capacity-2g.json) |
| P16 | STDIO_PROTOCOL §6 | 驱动 restart/mode 指令正确包装，每个用例独立目录，重启复用目录 | 已执行，有限覆盖；[linux-official](../validation/full-review-20261114/linux-official.json)、[linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json) |
| P17 | STDIO_PROTOCOL §7、§9 | 默认 Docker JSONL 入口、只读根、断网、2 CPU/2 GiB 和权限限制 | 已执行，有限覆盖；[linux-official](../validation/full-review-20261114/linux-official.json)、[linux-capacity-2g](../validation/full-review-20261114/linux-capacity-2g.json) |
| P18 | protocol_check/config.json max_response_bytes | 给定 1 万行×128维向量回归中，结果在 16 MiB 内且全部 float32 位模式往返不变 | 已执行，有限覆盖；[linux-vector-response-budget](../validation/full-review-20261114/linux-vector-response-budget.json) |
| P19 | 赛题关闭与事务语义；补充 EOF 验证 | EOF 回滚未提交数据且没有额外输出 | 已执行，有限覆盖；[linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json)、[asan-protocol-contract](../validation/full-review-20261114/asan-protocol-contract.json) |
| P20 | STDIO_PROTOCOL §1.5、§3 成功/失败 | 响应暂存失败只返回一条完整错误，编号类型仍保留 | 已执行，有限覆盖；[linux-response-failure](../validation/full-review-20261114/linux-response-failure.json) |
| S01 | 赛题 §2.1 表结构 | CREATE TABLE；仅支持 BIGINT、VARCHAR(n)、VECTOR(n) | 已执行，有限覆盖；[linux-published-requirements](../validation/full-review-20261114/linux-published-requirements.json)、[linux-official](../validation/full-review-20261114/linux-official.json) |
| S02 | 赛题 §2.1 表结构 | 每表一个 BIGINT 主键、至多一个向量列；重复列与错误定义须失败 | 已执行，有限覆盖；[linux-published-requirements](../validation/full-review-20261114/linux-published-requirements.json)、[asan-published-requirements](../validation/full-review-20261114/asan-published-requirements.json) |
| S03 | 赛题 §2.1 数据类型 | BIGINT 精确有符号整数，不丢失 2^53 以上精度，拒绝溢出 | 已执行，有限覆盖；[linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json)、[linux-invariants](../validation/full-review-20261114/linux-invariants.json) |
| S04 | 赛题 §2.1 数据类型 | VARCHAR 按 Unicode 码点计长度，比较按码点顺序，不归一化或语言排序 | 已执行，有限覆盖；[linux-published-requirements](../validation/full-review-20261114/linux-published-requirements.json)、[linux-cache-ranges](../validation/full-review-20261114/linux-cache-ranges.json)、[linux-sql-literals](../validation/full-review-20261114/linux-sql-literals.json) |
| S05 | 赛题 §2.1 数据操作 | 单行、多行 INSERT 和完整列名重排；主键不能重复 | 已执行，有限覆盖；[linux-official](../validation/full-review-20261114/linux-official.json)、[linux-insert-batches](../validation/full-review-20261114/linux-insert-batches.json)、[linux-column-layouts](../validation/full-review-20261114/linux-column-layouts.json) |
| S06 | 赛题 §2.1 数据操作 | 按主键和普通条件 UPDATE、DELETE，受影响记录数准确 | 已执行，有限覆盖；[linux-invariants](../validation/full-review-20261114/linux-invariants.json)、[linux-published-requirements](../validation/full-review-20261114/linux-published-requirements.json) |
| S07 | 赛题 §2.1 条件查询 | 普通列投影、重复投影、SELECT *，列名与列顺序正确 | 已执行，有限覆盖；[linux-official](../validation/full-review-20261114/linux-official.json)、[linux-order-aliases](../validation/full-review-20261114/linux-order-aliases.json)、[linux-column-layouts](../validation/full-review-20261114/linux-column-layouts.json) |
| S08 | 赛题 §2.1 条件查询 | =、<、<=、>、>= 和 AND，对 BIGINT、VARCHAR 正确比较 | 已执行，有限覆盖；[linux-published-requirements](../validation/full-review-20261114/linux-published-requirements.json)、[linux-cache-ranges](../validation/full-review-20261114/linux-cache-ranges.json)、[linux-invariants](../validation/full-review-20261114/linux-invariants.json) |
| S09 | 赛题 §2.2 结果顺序 | ORDER BY 支持升降序与多列；所有排序项并列后按主键升序 | 已执行，有限覆盖；[linux-published-requirements](../validation/full-review-20261114/linux-published-requirements.json)、[linux-invariants](../validation/full-review-20261114/linux-invariants.json)、[linux-reference-boundaries](../validation/full-review-20261114/linux-reference-boundaries.json) |
| S10 | 赛题 §2.1、§2.2 LIMIT | LIMIT 为非负整数；0 返回空集，超过记录数返回所有匹配记录 | 已执行，有限覆盖；[linux-reference-boundaries](../validation/full-review-20261114/linux-reference-boundaries.json)、[linux-published-requirements](../validation/full-review-20261114/linux-published-requirements.json) |
| S11 | 赛题 §2.1 不要求功能 | NULL、JOIN、聚合、子查询、ALTER、外键、嵌套事务、主键更新均返回错误，不能静默改义 | 已执行，有限覆盖；[linux-published-requirements](../validation/full-review-20261114/linux-published-requirements.json)、[asan-published-requirements](../validation/full-review-20261114/asan-published-requirements.json) |
| S12 | 赛题 §2.1 错误；STDIO_PROTOCOL §3 | 完整 SQL 解析后才执行，拒绝多语句与非符号操作符，不执行有效前缀 | 已执行，有限覆盖；[linux-token-kinds](../validation/full-review-20261114/linux-token-kinds.json)、[asan-token-kinds](../validation/full-review-20261114/asan-token-kinds.json)、[linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json) |
| S13 | 公开 SQL 子集及成功版本补充对照 | 引号、转义、Unicode、分号、NUL 数据、SQL 数值词法和显式 AS 名称作用域 | 已执行，有限覆盖；[linux-sql-literals](../validation/full-review-20261114/linux-sql-literals.json)、[linux-sql-numeric-forms](../validation/full-review-20261114/linux-sql-numeric-forms.json)、[linux-order-aliases](../validation/full-review-20261114/linux-order-aliases.json) |
| S14 | 赛题 §2.2 未指定 ORDER BY | 不遗漏或复制结果行；无排序时允许任意集合顺序 | 已执行，有限覆盖；[linux-invariants](../validation/full-review-20261114/linux-invariants.json)、[linux-reference-differential](../validation/full-review-20261114/linux-reference-differential.json) |
| V01 | 赛题 §2.1 向量类型 | 固定维度，按有限 float32 存储；拒绝 NaN、Infinity、非数值、维度错误、转换溢出 | 已执行，有限覆盖；[linux-invariants](../validation/full-review-20261114/linux-invariants.json)、[linux-vector-edges](../validation/full-review-20261114/linux-vector-edges.json)、[linux-sql-numeric-forms](../validation/full-review-20261114/linux-sql-numeric-forms.json) |
| V02 | 赛题 §2.2 L2_DISTANCE | 按已存 float32 计算欧氏距离，返回有限数值并满足公开误差容限 | 已执行，有限覆盖；[linux-official](../validation/full-review-20261114/linux-official.json)、[linux-vector-edges](../validation/full-review-20261114/linux-vector-edges.json)、[linux-certified-screen](../validation/full-review-20261114/linux-certified-screen.json) |
| V03 | 赛题 §2.2、§3.2 TopK | 先过滤再 TopK；不足 K、空集、并列主键和无重复 ID 均正确 | 已执行，有限覆盖；[linux-benchmark](../validation/full-review-20261114/linux-benchmark.json)、[linux-published-requirements](../validation/full-review-20261114/linux-published-requirements.json)、[linux-cache-column-sets](../validation/full-review-20261114/linux-cache-column-sets.json) |
| V04 | 赛题 §2.2 索引及模式 | 默认 exact；建立向量索引后 exact 仍精确，ann 满足当前值、过滤和召回率规则 | 已执行，有限覆盖；[linux-benchmark](../validation/full-review-20261114/linux-benchmark.json)、[linux-reference-boundaries](../validation/full-review-20261114/linux-reference-boundaries.json)、[linux-published-requirements](../validation/full-review-20261114/linux-published-requirements.json) |
| V05 | 赛题 §2.2、§2.3 索引维护 | CREATE VECTOR INDEX；插入、更新、删除、事务、回滚和重启后检索与当前可见记录一致 | 已执行，有限覆盖；[linux-official](../validation/full-review-20261114/linux-official.json)、[linux-invariants](../validation/full-review-20261114/linux-invariants.json)、[linux-published-requirements](../validation/full-review-20261114/linux-published-requirements.json)、[linux-cache-column-sets](../validation/full-review-20261114/linux-cache-column-sets.json) |
| D01 | 赛题 §2.3 持久化 | 表定义、多表记录与索引定义在同目录正常重启后保持 | 已执行，有限覆盖；[linux-official](../validation/full-review-20261114/linux-official.json)、[linux-reference-differential](../validation/full-review-20261114/linux-reference-differential.json) |
| D02 | 赛题 §2.3 自动提交 | 未 BEGIN 时每条成功修改自动提交；失败的多行语句不部分生效 | 已执行，有限覆盖；[linux-official](../validation/full-review-20261114/linux-official.json)、[linux-insert-batches](../validation/full-review-20261114/linux-insert-batches.json)、[linux-invariants](../validation/full-review-20261114/linux-invariants.json) |
| D03 | 赛题 §2.3 显式事务 | BEGIN、COMMIT、ROLLBACK；普通查询与向量查询可读到自身修改 | 已执行，有限覆盖；[linux-invariants](../validation/full-review-20261114/linux-invariants.json)、[linux-published-requirements](../validation/full-review-20261114/linux-published-requirements.json) |
| D04 | 赛题 §2.3 语句失败 | 失败语句回退本语句；事务之前的成功语句仍可继续提交或回滚 | 已执行，有限覆盖；[linux-insert-batches](../validation/full-review-20261114/linux-insert-batches.json)、[linux-token-kinds](../validation/full-review-20261114/linux-token-kinds.json)、[linux-published-requirements](../validation/full-review-20261114/linux-published-requirements.json) |
| D05 | 赛题 §2.3 ROLLBACK | 回滚恢复记录、可见集合及向量结果；补充验证事务内 DDL 回滚 | 已执行，有限覆盖；[linux-invariants](../validation/full-review-20261114/linux-invariants.json)、[linux-published-requirements](../validation/full-review-20261114/linux-published-requirements.json)、[linux-reference-boundaries](../validation/full-review-20261114/linux-reference-boundaries.json) |
| D06 | 赛题 §2.3、§3.2 确认提交 | 成功响应前完成文件同步；COMMIT 与自动提交的已确认数据在进程强杀后保留 | 已执行，有限覆盖；[linux-commit-publish](../validation/full-review-20261114/linux-commit-publish.json)、[linux-crash-commit](../validation/full-review-20261114/linux-crash-commit.json)、[linux-capacity-2g](../validation/full-review-20261114/linux-capacity-2g.json) |
| D07 | 赛题 §3.2 提交边界 | 未请求提交的事务不生效；提交请求后未确认的事务允许全有或全无，不能部分生效 | 已执行，有限覆盖；[linux-crash-commit](../validation/full-review-20261114/linux-crash-commit.json)、[linux-commit-publish](../validation/full-review-20261114/linux-commit-publish.json)、[linux-published-requirements](../validation/full-review-20261114/linux-published-requirements.json) |
| D08 | 赛题 §2.3、§3.2 恢复 | 恢复途中再次终止仍可恢复；必要恢复和有界索引准备在 ready 前完成 | 已执行，有限覆盖；[linux-recovery-boundaries](../validation/full-review-20261114/linux-recovery-boundaries.json)、[linux-crash-commit](../validation/full-review-20261114/linux-crash-commit.json)、[linux-recovery-allocation](../validation/full-review-20261114/linux-recovery-allocation.json) |
| D09 | 赛题 §2.3、STDIO_PROTOCOL §4 关闭 | 正常关闭回滚未提交事务；复用目录保留提交数据，独立用例目录不串数据 | 已执行，有限覆盖；[linux-official](../validation/full-review-20261114/linux-official.json)、[linux-protocol-contract](../validation/full-review-20261114/linux-protocol-contract.json) |
| D10 | 赛题 §2.3、§3.2 资源 | 支持超过内存预算的数据；有界缓存、外部排序、磁盘查询与强杀恢复 | 已执行，有限覆盖；[linux-capacity](../validation/full-review-20261114/linux-capacity.json)、[linux-capacity-2g](../validation/full-review-20261114/linux-capacity-2g.json)、[linux-invariants](../validation/full-review-20261114/linux-invariants.json) |
| D11 | 赛题 §2.3 存储与资源；补充故障验证 | 多次更新回收历史；原子替换和响应暂存失败不破坏已确认数据 | 已执行，有限覆盖；[linux-space-reuse](../validation/full-review-20261114/linux-space-reuse.json)、[linux-compaction-crash](../validation/full-review-20261114/linux-compaction-crash.json)、[linux-response-failure](../validation/full-review-20261114/linux-response-failure.json) |
| E01 | 赛题 §2.1、§3.1 实现范围 | 单机、单进程、单客户端；自有存储、执行、索引和事务，无现成数据库或向量引擎代执行 | 已查源码/材料；[linux-submission-contract](../validation/full-review-20261114/linux-submission-contract.json)、[linux-official](../validation/full-review-20261114/linux-official.json)、`src/main.cpp`、`src/storage.cpp`、`src/btree.cpp`、`src/engine.cpp`、`src/query.cpp`、`src/vector_index.cpp`、`Makefile`、`third_party/DEPENDENCIES.json` |
| E02 | 赛题 §3.1、§4.2.1 容器 | Linux x86_64，从源码离线构建，默认前台 JSONL，无网络监听，运行无需联网 | 已执行，有限覆盖；[linux-official](../validation/full-review-20261114/linux-official.json)、[linux-submission-contract](../validation/full-review-20261114/linux-submission-contract.json) |
| E03 | 赛题 §2.3、§3.3.2 存储路径 | 数据库、恢复、排序、响应暂存和回收文件均位于 /data | 已查源码/材料；[linux-official](../validation/full-review-20261114/linux-official.json)、[linux-capacity-2g](../validation/full-review-20261114/linux-capacity-2g.json)、`src/main.cpp`、`src/storage.cpp`、`src/compaction.cpp`、`src/query.cpp`、`src/result.cpp` |
| E04 | protocol_check/config.json 公开资源 | 默认入口在只读根、断网、2 CPU、2 GiB、128 PID 下通过；握手 120s、请求 60s、用例 600s | 已执行，有限覆盖；[linux-official](../validation/full-review-20261114/linux-official.json)、[linux-capacity-2g](../validation/full-review-20261114/linux-capacity-2g.json) |
| M01 | 赛题 §4.1、§4.2.1 构建交付 | 完整源码、Dockerfile、锁定依赖、所需运行库和基础镜像，无未交付本地二进制依赖 | 已执行，有限覆盖；[linux-submission-contract](../validation/full-review-20261114/linux-submission-contract.json)、[linux-official](../validation/full-review-20261114/linux-official.json) |
| M02 | 赛题 §4.2 运行说明 | README 说明语言版本、构建、入口、/data、SQL、错误处理，并给出写读、TopK、提交、回滚、重启例子 | 已查源码/材料；[linux-published-requirements](../validation/full-review-20261114/linux-published-requirements.json)、`README.md`、`Dockerfile`、`Makefile` |
| M03 | 赛题 §4.3 设计与验证 | 说明记录布局、索引、事务、同步、恢复、已实现功能及已知限制，保留版本化验证证据 | 已查源码/材料；`README.md`、`docs/ALL_REQUIREMENTS_20261114.md`、`docs/REFERENCE_BOUNDARIES_20261114.md` |
| M04 | 赛题 §4.4 JSON 材料格式 | agent/conversation.json 是 UTF-8 JSON 对象、至多 128 KiB，无重复键或非标准数值；根目录副本相同 | 已执行，有限覆盖；[linux-submission-contract](../validation/full-review-20261114/linux-submission-contract.json) |
| M05 | 赛题 §3.4、§4.4 材料真实性 | 保留真实用户与 Agent 来源、实际结果和缺失范围，不编造学生理解或交互，不提交隐藏推理或凭据 | 已查源码/材料；`agent/actual_conversation.json`、`scripts/build_material.py`、`agent/conversation.json` |
| M06 | 赛题 §3.5、§4.1 版本锁定 | 规定分支、可获取 HTTPS 仓库和完整 40 位 SHA；源码、镜像构建与材料来自该次提交 | 发布时单独核对；`submission manifest: verified local/remote branch, archive and material hashes` |
| X01 | 赛题 §2.4、§3.2 正式准入 | 正式 29 个 MySQL case 全部断言通过，包含未公开的 9 个用例 | 待平台确认；只可在正式平台证明；本轮未发起正式提测。20 个公开样例不能代表 29 个正式用例。 |
| X02 | 赛题 §3.3 正式性能与排名 | 三轮平台 T/B/M、专用设备 I/O、全容器峰值内存、正式正确性及独立排名 | 待平台确认；开发 benchmark 覆盖已公布形状和召回，但没有平台隔离设备、冻结基线、正式随机种子或排名；不能推算赛事分数。 |
| X03 | 赛题 §3.4 正式材料评分 | 冻结评审配置下三轮评审、证据引用校验、必要复核及有效原始分与排名 | 待平台确认；格式与来源可自查，学习证据效力和原始分由平台评审；Agent 自动完成不保证学生贡献分。 |
| X04 | 赛题 §3.1、§3.3 硬性限制冻结 | 正式磁盘容量、I/O 带宽、索引准备时间和隐藏规模等全部硬性边界 | 待平台确认；公开 config 未给出这些全部数值；只证明公布的 CPU/内存/超时及已测容量，不能声称覆盖未取得的限制。 |

## 尚不能确认的部分

X01–X04 没有伪造通过状态：隐藏用例、正式三轮 T/B/M 与排名、材料三轮评审，以及公开配置未给出全部数值的硬限制，均需要平台或组织方信息。有限对拍与开发 CI 不能证明任意输入、任意资源规模必然通过。

16 MiB 字节证据是 10000×128 投影的 14330893 字节回归，全部 128 万个 float32 分量按位往返；不声称任意大结果都能装入 16 MiB。容量证据为 2 GiB cgroup 中 2621440000 字节业务负载，不能替代未取得的正式磁盘与带宽限额。恢复保证针对题目明确的进程强杀，未扩展为机器断电。

正式评测仍按用户要求暂缓。可确认的是这些公布要求已有相应实现、有限测试或明确的源码核对；不能保证全体隐藏输入一定通过或承诺赛事分数。
