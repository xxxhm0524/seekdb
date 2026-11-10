# MiniVectorDB

2026 OceanBase 数据库大赛 · 加赛 · 赛题一「完成一个轻量向量数据库」的从零实现。

本实现不依赖 SQLite、RocksDB、Faiss、VSAG、HNSWlib 等现成数据库或向量索引引擎；
SQL 词法/语法、页式存储、WAL、事务、恢复和距离计算均为自行实现。

## 1. 语言与构建

| 项目 | 值 |
| --- | --- |
| 语言 | C++20 |
| 编译器 | GCC 13+（Ubuntu 24.04 的 `build-essential`） |
| 构建 | GNU make；`make -j` |
| 第三方依赖 | 无（仅 libc/libm/libstdc++/libgomp） |
| 运行环境 | Linux x86_64 容器；本地 Windows/MSYS2 也可编译测试 |

```bash
make -j
./minivectordb --data-dir=/tmp/mvd-data
```

### Docker

```bash
docker build -t minivectordb:latest .
docker run --rm -i -v minivectordb-data:/data --cpus=2 --memory=2g minivectordb:latest
```

容器以前台方式运行，只通过 stdin 接收 JSONL 请求、stdout 返回 JSONL 响应；日志只写 stderr，不监听网络端口。
所有持久化文件位于 `/data`。启动完成后先主动输出一行 `{"type":"ready"}`，随后逐行处理
`execute` / `shutdown` 请求（见第 3 节）。

## 2. SQL 支持范围

### 2.1 表与类型

```sql
CREATE TABLE docs (
    id BIGINT PRIMARY KEY,
    category VARCHAR(32),
    content VARCHAR(1024),
    embedding VECTOR(128)
);
```

- `BIGINT`、`VARCHAR(n)`、`VECTOR(n)`；一个 `BIGINT` 主键（列内 `PRIMARY KEY` 或表级 `PRIMARY KEY (id)`）。
- 每表最多一个 `VECTOR` 列。
- `VARCHAR(n)` 的 `n` 按 Unicode 码点数计算；字符串比较按 UTF-8 字节序（等价于 Unicode 码点序）。
- `VECTOR(n)` 固定维度 float32；拒绝 NaN、Infinity、维度不符和溢出。

### 2.2 数据操作与查询

```sql
INSERT INTO docs VALUES (1, 'db', '事务', '[1,0,0]'), (2, 'db', '索引', '[0,1,0]');
INSERT INTO docs (id, category, content, embedding) VALUES (3, 'travel', '攻略', '[0,0,1]');
SELECT * FROM docs WHERE category = 'db' ORDER BY id DESC LIMIT 10;
SELECT id, content, L2_DISTANCE(embedding, '[0.9,0.1,0]') AS distance
FROM docs WHERE category = 'db' ORDER BY distance LIMIT 2;
UPDATE docs SET content = '新内容' WHERE id = 1;
DELETE FROM docs WHERE id >= 10;

BEGIN;
UPDATE docs SET embedding = '[0.8,0.2,0]' WHERE id = 1;
DELETE FROM docs WHERE id = 2;
ROLLBACK;
```

- 支持 `=`、`<`、`<=`、`>`、`>=`、`AND`、`ORDER BY`（多列、升降序）、`LIMIT` / `OFFSET`。
- `L2_DISTANCE` 返回欧氏距离（不是平方距离）。精确 TopK 在满足 `WHERE` 的记录集合内计算；
  距离相同按主键升序；结果不足 K 条时返回全部。
- `CREATE VECTOR INDEX name ON table(vector_column)` 构建自研 HNSW 与紧凑向量/属性缓存。
  exact 模式始终精确；ann 模式根据规模、过滤条件和预算选择图搜索或精确索引路径。
  提交后维护索引，回滚不修改已提交索引，重启从恢复后的记录重建。
- 未显式 `BEGIN` 时，成功的修改语句自动提交。
- JSON 协议中查询结果里的 VECTOR 列以 JSON 数组输出（如 `[1.0,2.0]`）。

### 2.3 不支持项

不要求且未实现：NULL、JOIN、聚合、子查询、ALTER TABLE、外键、嵌套事务、主键更新、`OR`、`IN`、`LIKE`。
不支持的语句返回失败响应 `{"ok":false,"error":{"code":"<semantic_name>",...}}`，不会静默改变语义。

## 3. stdin/stdout 协议（JSON Lines）

正式判题器通过进程 stdin/stdout 逐行交换 JSON，不使用文本 SQL 终端或表格输出。协议细节见
`STDIO_PROTOCOL.md`。

1. 启动：完成数据目录打开与恢复后，主动输出一行并 flush：

```json
{"type":"ready"}
```

2. 请求：每行一个 JSON 对象。

```json
{"id":1,"op":"execute","sql":"SELECT id FROM docs ORDER BY id;","search_mode":"exact"}
{"id":2,"op":"shutdown"}
```

3. 响应：原样回传请求 `id`（保持 JSON 类型），`ok` 为布尔值。

```json
{"id":1,"ok":true,"columns":["id"],"rows":[[1],[2]]}
{"id":2,"ok":true,"affected_rows":1}
{"id":3,"ok":true}
{"id":4,"ok":false,"error":{"code":"duplicate_key","message":"duplicate primary key"}}
```

- 查询返回 `columns` + `rows`（空结果也返回列名）；BIGINT 为 JSON 整数，VECTOR 为 JSON 数组。
- INSERT/UPDATE/DELETE 返回实际 `affected_rows`；DDL 与事务控制返回仅 `id` + `ok:true`。
- 每条响应一行并立即 flush；stdout 不输出日志、提示符或空行，调试信息只写 stderr。
- 请求中的 `search_mode`（`exact` / `ann`）按请求生效；ann 支持 HNSW，事务内查询使用当前事务可见记录精确计算。
- `shutdown` 先回滚未提交事务、刷盘并清理，再返回 `{"id":N,"ok":true}` 并以 0 退出。
- 进程被强杀后，已提交事务由 WAL 恢复；未提交改动不会部分生效。

`--query-threads=N` 控制向量扫描线程数（默认 2），`--cache-mb=N` 控制页缓存上限（默认 64 MiB），
`--data-dir=/data`（或环境变量 `VECTORDB_DATA_DIR`）指定数据目录。
`--tx-memory-mb=N` 控制事务行数据与更新值的驻留预算（默认 8 MiB）；
`--ann-min-rows=N` 控制图搜索启用规模（默认 40,000，亦需维度至少 32）。

## 4. 设计与验证

### 4.1 存储布局

- 固定 64 KiB 页；每页 16 字节头（magic、nrows、page_no、CRC 保留位），页内为定长记录槽。
- 每张表一个 `t_<id>.dat` 文件；记录包含：有效位、8 字节主键副本、各列定长槽（VARCHAR 槽为 4 字节长度 + 最大字节空间，VECTOR 槽为 float32 数组）。
- 主键到槽位的映射在内存中维护，启动/恢复时扫描页头与有效记录重建。
- 页缓存按 LRU 管理，超过 `--cache-mb` 时淘汰干净页，脏页刷盘；扫描不会把整表读入内存，支持大于内存预算的数据集。
- 对齐页缓冲复用，额外闲置池最多保留一个 I/O 批次（4 MiB），避免扫描、事务节点与提交镜像反复分配时造成堆碎片增长。
- 提交镜像按页号排序；相邻脏页合并为至多 4 MiB 的写入，降低大修改与 checkpoint 的同步 I/O 次数。
  Linux 使用 `pwritev` 直接写对齐页，省去合并写暂存的分配和复制；其他平台保留合并缓冲路径。
- 顺序扫描和主键索引恢复使用至多 4 MiB 的批量读取；缓存较小时缩小批次，预读不会覆盖较新的脏缓存页，读写暂存缓冲各至多 4 MiB。

### 4.2 事务与语句原子性

- 显式事务维护每表的 `pk -> RowState` 叠加层；读取先查叠加层，再查基线，因此事务内可读到自己的修改。
- 已有记录的 UPDATE 保存共享的列修改，DELETE 保存删除标记，避免为每条宽记录复制完整定长槽。
- 无过滤 UPDATE/DELETE 直接合并主键索引和事务行定位，避免为枚举主键读取整表；提交完整覆盖的原始行或墓碑页时省去旧页读取。
- 无过滤且只投影、排序主键的 SELECT 也从当前可见的主键定位中取得值，保留通用排序及 LIMIT/OFFSET 语义。
  投影其他列、有过滤或按其他列排序时仍使用实际行数据。
- 每条语句先校验，使用只记录本语句修改行的撤销日志恢复失败语句；不再为每条语句复制整个事务。
- 新增行及独立更新值超出预算后写入 `/data/txn.tmp`；相同列的旧补丁会被裁剪，避免保留无效历史。
  失败语句回退溢写文件的有效范围；提交、回滚和正常关闭释放该文件，异常重启丢弃未提交溢写数据。
  连续读取溢写行时使用至多 1 MiB 预读，随机访问只读取所需行；回退文件时清空该缓存，避免复用偏移后读到旧内容。
- 新增槽位的分配进度只在 COMMIT 后安装，失败语句与 ROLLBACK 不消耗基线槽位。
- `ROLLBACK` 丢弃叠加层；`COMMIT` 先把提交涉及的页生成完整页镜像写入 WAL，`fsync` 后才安装到页缓存并返回成功。
- 提交逐页生成镜像，只保留首 1 MiB 的镜像和后续 WAL 偏移；大提交不需要把全部修改页同时放进内存。
- 未提交事务从不写数据页；崩溃后 WAL 中没有 COMMIT 记录的页镜像会被忽略。

### 4.3 WAL 与崩溃恢复

- `wal.log` 记录两类记录：页镜像（表 id、页号、64 KiB 页内容）和 COMMIT（txid）。
- 每次提交：写页镜像 → 写 COMMIT → `fsync(wal)` → 返回成功；随后页缓存可延迟刷盘。
- 启动恢复两遍扫描：第一遍收集有 COMMIT 的 txid，第二遍按 WAL 顺序重放这些页镜像（幂等），
  然后 `fsync` 数据文件、截断 WAL。两遍均按记录读取，不把整个 WAL 读入内存。
- 每次提交后检查 32 MiB checkpoint 阈值：先刷全部脏页并 `fsync` 数据文件，再截断 WAL。
  单个大事务的 WAL 可以超过阈值，其页镜像仍逐页生成、安装和恢复。
- 大事务每写入约 8 MiB WAL 同步一次并释放已干净的 Linux 文件缓存；最终 COMMIT 仍单独同步后才安装数据页。
  分段持久化不代表事务提交，缺少 COMMIT 的前缀仍会在恢复时忽略。
- 正常关闭（JSON shutdown 或 stdin EOF）：回滚未提交事务、刷脏页、`fsync`、截断 WAL。
  恢复未成功时保留 WAL；同步失败会返回错误，不继续截断恢复日志。

### 4.4 向量检索

- 精确扫描：过滤谓词先判定，命中记录才计算距离；维护大小为 K 的最大堆，排序键为 `(平方距离, 主键)`。
- float32 存储、double 差值与平方累加，避免极小合法向量的距离下溢为 0；最大有限 float32 向量的距离也可表示。
- 十进制向量分量使用 `from_chars` 直接转换，特殊形式和范围边界回退 `strtof`；解析仍先完整校验语法。
- AVX2 距离内核带运行期 CPU 检测，不支持 AVX2 时回退到 double 标量实现；不会因 `-march=native` 在未知评测机报非法指令。
- 候选距离落入保守 ULP 误差带时，用补偿 long double 累加重新比较，防止等距向量因分量顺序不同排错；普通排序路径也使用补偿距离作为排序键。
- OpenMP 按页切分并行扫描，线程数默认 2（可用 `--query-threads` 调整），结果合并后按 `(距离, 主键)` 输出。
- 自研 HNSW 使用分层图和邻居多样性筛选（M=32、构建 ef=256、搜索 ef 至少 384）。
  图内使用 float32 快速候选距离，返回前按存储后的 float32 向量重新精确计算和排序。
  反向邻接保留有界余量后批量筛选（第 0 层最多 80 条、上层最多 40 条），减少重复构建工作。
- 小集合、低维、过滤查询使用紧凑向量/属性缓存精确计算；不适合缓存的过滤或超预算集合使用页式扫描。
  已缓存的投影直接返回，避免逐条回读宽记录。密集等距结果回退精确索引，保持主键并列规则。
- 重复过滤条件可复用满足条件的行编号；每次查询仍重新计算距离和 TopK，提交增删改后清空候选缓存。
  该缓存计入索引预算，最多 64 组条件且不超过每个节点 8 字节、单索引 4 MiB 的较小值。
- 索引总预算按保守估计限制为 128 MiB；提交维护当前值，删除标记累计后重建，恢复重建完成后才发送 ready。
  大批量提交使派生索引失效，下一次检索或重启重建；事务内读取自身修改不依赖旧图。
- 算法参考：[HNSW 原论文](https://arxiv.org/abs/1603.09320)，未调用任何现成向量索引库。

## 5. 验证记录

2026-11-10 重新审计并修复公开样例之外的边界。以下是 Windows MSYS2 的实际本地结果，
不是正式 29 个用例的通过证明；Linux、sanitizer 与实际 Docker 镜像以对应 SHA 的 `minivectordb-ci` 记录为准。

| 验证项 | 方法 | 结果 |
| --- | --- | --- |
| JSONL 握手、关闭、公开用例 | `protocol_check/run.py`，实际目标参数由下方审计入口生成 | 握手及 20/20 公开用例 PASS |
| 公开要求的本地覆盖 | `tests/formal_coverage.py`，类型、比较、排序、事务与恢复 | PASS；不等同正式用例 |
| 边界回归 | `tests/hidden_boundaries.py`，极小距离、别名、空结果错误、维度和行宽回绕、非法向量、回滚槽位、UPDATE 原子性、Unicode 错误响应、数值等距、重复列、DDL 孤立文件、float32 最大值回传、请求 id 类型 | 19 组 PASS |
| 浮点解析差分 | `tests/float_parse_model.py`，与原 `strtof` 行为逐位比较，包含随机有限 float32、十进制、极值、非法符号及下溢 | 本地约 60 万次转换 PASS |
| 过滤候选模型 | `tests/filter_cache_model.py`，106 组谓词、多查询向量、缓存淘汰、提交/回滚/失败/重启 | 本地 PASS；主键与排序严格核对 |
| 学习材料完整性 | `tests/validate_agent_material.py`，UTF-8、128 KiB、重复键、非有限数、凭据模式、镜像一致与 RFC6901 引用 | 只验证格式、来源角色和引用，不计算学习分数 |
| 预读和脏页一致性 | `tests/cache_coherence.py`，1 MiB 缓存下交错更新、过滤 TopK、提交/回滚及强杀重启 | PASS |
| 宽记录修改 I/O | `tests/mutation_io.py`，按进程读取字节计数验证无过滤修改、全页删除，并检查事务叠加和部分页保留 | 本地 PASS；不放宽容器的 60 秒请求限制 |
| 主键投影 I/O | `tests/primary_key_projection.py`，主键在最后一列，覆盖别名、极值、OFFSET、事务可见性与其他列回退 | 本地 PASS；记录进程读取字节，非官方物理 I/O 指标 |
| 溢写读取放大 | `tests/spool_read_boundaries.py`，多表事务越过溢写阈值，验证随机偏移不会触发固定大窗口读取 | 查看对应 SHA 的审计和 Actions |
| 批量预读分配 | `tests/page_pool_churn.py`，8 MiB 页缓存反复扫描宽表，约束分配器内存增长 | 查看对应 SHA 的审计和 Actions |
| 长事务溢写 | `tests/transaction_spill.py`，1 MiB 行数据预算，包含 INSERT/UPDATE、覆盖历史裁剪、失败语句、提交/回滚/强杀 | 本地 PASS；12,000 条宽记录峰值约 15 MiB |
| HNSW 生命周期 | `tests/ann_lifecycle.py`，显式启用图路径，验证精确/近似切换、过滤、增删改、事务与崩溃重建 | 本地 PASS |
| 随机模型 | `tests/stress_random.py`，种子 7、42、211，各 100 轮 | PASS |
| 原回归和扩展 | `tests/regression.py`、`tests/run_extended_tsv.py`；后者名称保留，实际走 JSONL | PASS |
| 恢复中重复强杀 | `tests/crash_recovery_test.py 50000`，合法未提交增删改，核对所有 ID | PASS；本地观测到 2 次恢复写页后、ready 前强杀 |
| 多表提交途中强杀 | `tests/commit_crash_boundaries.py`，包含未发 COMMIT、不同延迟、已确认 COMMIT | PASS；完整旧状态或完整新状态 |
| 大于内存的数据文件 | `tests/resource_boundaries.py --rows 260000 --max-mb 128`，1 MiB 页缓存，2321.44 MiB 数据文件 | PASS；排序/事务 TopK 约 22.4 MiB，更新/删除峰值约 78.8 MiB（进程指标） |
| exact 基准 | 10k×128d，1000 查询，种子 20261110；强杀后逐条核对持久化内容及查询距离 | 两组 Recall@10 均 1.0；写入约 1.35 s，查询约 0.272 s |
| ann 基准 | 同规模，种子 20261111；该规模选择紧凑精确索引 | 两组 Recall@10 均 1.0；写入约 1.23 s，查询约 0.260 s |
| Linux 和 sanitizer | `.github/workflows/minivectordb.yml`，GCC/UBSan 和回归 | 查看对应 SHA 的 Actions |
| 实际容器资源边界 | `tests/docker_resource_boundaries.py`，256 MiB 容器、超过 2 GiB 数据文件、全表更新/删除及重启 | 查看对应 SHA 的 Actions |

重跑本地审计（输出目录必须是新目录；需已构建程序与 NumPy）：

```bash
python tests/audit_suite.py --output protocol-results/audit-20261110 --large-resource
```

审计保留逐项日志和 `summary.json`。基准计时包含 JSONL 请求/响应与提交持久化，排除 SQL 文本生成及事后验证；
不据此宣称平台综合性能或排名。Windows 传输层增加了带超时的管道读写，Linux 保留原 selectors 路径，
公开断言规则未放宽。本机无 Docker，容器边界由 Ubuntu 24.04 Actions 构建并运行。

## 6. 目录

```
src/common.h      文件、CRC32、UTF-8、数值解析
src/sql.h/.cpp    SQL 词法/递归下降解析器
src/engine.h/.cpp 页存储、页缓存、WAL、事务、恢复、向量扫描
src/main.cpp      JSONL stdin/stdout 服务
tests/            本地协议驱动、回归、Benchmark
mysqltest/        官方公开用例与 manifest
protocol_check/   官方 JSON 协议检查器
agent/conversation.json  Agent Prompt 学习材料
Dockerfile
```

## 7. 已知限制

1. 行必须能放入 64 KiB 页；超宽 VARCHAR/VECTOR 建表会返回错误。
2. 不支持 NULL、默认值、JOIN、聚合、子查询、ALTER TABLE、主键更新。
3. ANN 会近似检索，也会选择精确缓存或扫描；其召回率须由实际数据验证，不能据此宣称平台排名。
4. 行数据和更新值可溢写，但主键索引、事务行定位/撤销元数据仍与记录数有关；单条巨大 SQL 的解析和无 LIMIT 的巨大响应仍受内存限制。
5. 本地测试和 CI 覆盖公开要求的已知边界，不能证明未获得输入的正式 29 个用例全部通过。未经用户授权不做平台提测。

## 8. 学习材料与版本

默认评测材料为 `agent/conversation.json`，根目录 `conversation.json` 是相同内容的兼容副本。
材料选录可核对的用户原话、助手可见回复和真实日志，明确区分角色；未给学生自行赋分，
未把助手设计、执行的实验改写成学生独立完成。尚未收到的学生预测、解释和复盘会明确标明。

本地可用独立取回的消息导出进一步核对原话，导出文件不作为评测材料替代品：

```bash
python tests/validate_agent_material.py --path agent/conversation.json \
  --mirror conversation.json --require-evidence --source-corpus <可见消息导出.json>
```

`tests/docker_score_compare.py` 提供三轮新旧代码 T/B/M 对照，B/M 包含准备过程和最终写回，
内容校验使用另一容器。其参考版本与比较指标仅用于工程判断，不代表官方基线、成绩或名次。
