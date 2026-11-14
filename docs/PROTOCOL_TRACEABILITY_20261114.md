# 协议逐条验收矩阵

依据未修改的 `STDIO_PROTOCOL.md`、公开驱动配置和赛题事务范围。旧解析器的输出不作为规范真值。每项都映射到本轮实际通过的报告，原始提交、CI URL 和报告 SHA256 见 `validation/edge-review-20261114/protocol-traceability.json`。

| 编号 | 规范条款 | 验收要求 | 实际证据 |
| --- | --- | --- | --- |
| P01 | STDIO_PROTOCOL §1.1–2 | UTF-8、完整单行 JSON 对象和物理 LF | `linux-protocol-contract.json`、`asan-protocol-contract.json` |
| P02 | STDIO_PROTOCOL §1.3、§9 | 每条响应及时刷新，交互式等待而非预写全部输入 | `linux-official.json`、`linux-protocol-contract.json` |
| P03 | STDIO_PROTOCOL §1.4、§1.6 | stdout 没有日志、空行、提示符或额外状态行 | `linux-official.json`、`linux-protocol-contract.json` |
| P04 | STDIO_PROTOCOL §1.5 | 每个请求恰好一个响应 | `linux-official.json`、`linux-protocol-contract.json` |
| P05 | STDIO_PROTOCOL §1.5、§3 id | 所有响应路径按值和 JSON 类型保留 id，包括超 64 位整数与嵌套编号 | `linux-protocol-ids.json`、`asan-protocol-ids.json` |
| P06 | STDIO_PROTOCOL §1.7 | 响应没有 NaN、Infinity、重复对象键 | `linux-protocol-contract.json`、`linux-vector-response-budget.json` |
| P07 | STDIO_PROTOCOL §2 | 输入前主动发出准确 ready；恢复完成后才就绪 | `linux-official.json`、`linux-crash-commit.json`、`linux-recovery-boundaries.json` |
| P08 | STDIO_PROTOCOL §3 请求字段 | 校验 execute、sql、search_mode 的值和类型 | `linux-protocol-ids.json`、`linux-protocol-contract.json` |
| P09 | STDIO_PROTOCOL §3 完整解析 | 先验证整帧再执行；分号和换行在 SQL 字符串内保持原值 | `linux-protocol-contract.json`、`linux-complete-input.json` |
| P10 | STDIO_PROTOCOL §3 查询成功 | columns 与 rows 顺序一致，二维数组，空集为 [] | `linux-official.json`、`linux-protocol-contract.json` |
| P11 | STDIO_PROTOCOL §3 修改成功 | DML affected_rows 是实际整数；DDL/TX 可省略可选字段 | `linux-protocol-contract.json`、`linux-reference-boundaries.json` |
| P12 | STDIO_PROTOCOL §3 执行失败 | ok 是布尔值，error.code 非空；预期 SQL 错误后进程可继续 | `linux-protocol-contract.json`、`linux-invariants.json` |
| P13 | STDIO_PROTOCOL §3 数据类型 | BIGINT 不经浮点或字符串，向量数组和距离都是有限数值 | `linux-protocol-contract.json`、`linux-sql-numeric-forms.json`、`linux-vector-response-budget.json` |
| P14 | STDIO_PROTOCOL §4 | shutdown 回滚未提交事务，原编号确认，flush 后退出 0，无尾随输出 | `linux-official.json`、`linux-protocol-contract.json`、`linux-protocol-ids.json` |
| P15 | STDIO_PROTOCOL §4 持久性 | 确认提交不依赖 shutdown，异常重启保留已确认数据 | `linux-crash-commit.json`、`linux-commit-publish.json`、`linux-capacity-2g.json` |
| P16 | STDIO_PROTOCOL §6 | 驱动 restart/mode 指令正确包装，每个用例独立目录，重启复用目录 | `linux-official.json`、`linux-protocol-contract.json` |
| P17 | STDIO_PROTOCOL §7、§9 | 默认 Docker JSONL 入口、只读根、断网、2 CPU/2 GiB 和权限限制 | `linux-official.json`、`linux-capacity-2g.json` |
| P18 | protocol_check/config.json max_response_bytes | 给定 1 万行×128维向量回归中，结果在 16 MiB 内且全部 float32 位模式往返不变 | `linux-vector-response-budget.json` |
| P19 | 赛题关闭与事务语义；补充 EOF 验证 | EOF 回滚未提交数据且没有额外输出 | `linux-protocol-contract.json`、`asan-protocol-contract.json` |
| P20 | STDIO_PROTOCOL §1.5、§3 成功/失败 | 响应暂存失败只返回一条完整错误，编号类型仍保留 | `linux-response-failure.json` |

本轮 [完整 Linux CI](https://github.com/xxxhm0524/seekdb/actions/runs/43598504397) 的 47 份报告全部通过，包括正常构建和 sanitizer。矩阵不是额外增加的 20 个独立测试分数，而是条款与已执行证据的对应关系。

新增交互场景先等待准确 ready，再分段发送一个 UTF-8 字符中途截断的请求；完整 JSON 尚无 LF 时确认没有提前响应。测试还覆盖转义换行/NUL/引号、SQL 字符串内分号、BIGINT 极值及类型、十种无效帧不执行其有效 SQL 前缀、错误后继续、正常关闭和 EOF 回滚、独立用例目录。

编号的独立 oracle 使用 Python 任意精度 JSON 和类型比较，覆盖 3001 位整数、复合编号、300009 字节字符串及成功/SQL 错误/请求错误/提交/关闭/大响应路径。大向量输出同时核对真实报文字节和全部 float32 位模式。

条件范围：赛题明确不要求 SQL NULL，因此未实现的 NULL 语法返回错误；`id:null` 已在 P05 验证。P18 的字节证据是给定 10000 行×128维回归，不代表任意规模结果都能装入 16 MiB。P15 的故障模型是进程终止而非机器断电。

此前的漏洞来自验收方法：将新解析器与旧有界数字 DOM 对拍，无法检出双方共享的大整数编号失真。现在以规范、独立值/类型真值和实际报文为依据，并用固定通过版本补充对照。正式提测仍然暂缓。
