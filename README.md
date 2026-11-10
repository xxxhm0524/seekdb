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
- `CREATE VECTOR INDEX name ON table(vector_column)` 可用；本实现使用自研页式堆和精确扫描，
  索引元数据会持久化，索引创建后增删改查始终以当前可见记录为准（不依赖过期的索引副本）。
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
- 请求中的 `search_mode`（`exact` / `ann`）按请求生效，本实现两种模式均使用精确扫描保证召回。
- `shutdown` 先回滚未提交事务、刷盘并清理，再返回 `{"id":N,"ok":true}` 并以 0 退出。
- 进程被强杀后，已提交事务由 WAL 恢复；未提交改动不会部分生效。

`--query-threads=N` 控制向量扫描线程数（默认 2），`--cache-mb=N` 控制页缓存上限（默认 64 MiB），
`--data-dir=/data`（或环境变量 `VECTORDB_DATA_DIR`）指定数据目录。

## 4. 设计与验证

### 4.1 存储布局

- 固定 64 KiB 页；每页 16 字节头（magic、nrows、page_no、CRC 保留位），页内为定长记录槽。
- 每张表一个 `t_<id>.dat` 文件；记录包含：有效位、8 字节主键副本、各列定长槽（VARCHAR 槽为 4 字节长度 + 最大字节空间，VECTOR 槽为 float32 数组）。
- 主键到槽位的映射在内存中维护，启动/恢复时扫描页头与有效记录重建。
- 页缓存按 LRU 管理，超过 `--cache-mb` 时淘汰干净页，脏页刷盘；扫描不会把整表读入内存，支持大于内存预算的数据集。

### 4.2 事务与语句原子性

- 显式事务维护每表的 `pk -> RowState` 叠加层；读取先查叠加层，再查基线，因此事务内可读到自己的修改。
- 每条语句先在叠加层完成校验；语句失败时丢弃本条语句的改动（显式事务恢复到语句前快照），不留部分修改。
- `ROLLBACK` 丢弃叠加层；`COMMIT` 先把提交涉及的页生成完整页镜像写入 WAL，`fsync` 后才安装到页缓存并返回成功。
- 未提交事务从不写数据页；崩溃后 WAL 中没有 COMMIT 记录的页镜像会被忽略。

### 4.3 WAL 与崩溃恢复

- `wal.log` 记录两类记录：页镜像（表 id、页号、64 KiB 页内容）和 COMMIT（txid）。
- 每次提交：写页镜像 → 写 COMMIT → `fsync(wal)` → 返回成功；随后页缓存可延迟刷盘。
- 启动恢复两遍扫描：第一遍收集有 COMMIT 的 txid，第二遍按 WAL 顺序重放这些页镜像（幂等），
  然后 `fsync` 数据文件、截断 WAL。恢复途中再次被 kill 也安全。
- 运行期 WAL 超过 32 MiB 时自动 checkpoint：先刷全部脏页并 `fsync` 数据文件，再截断 WAL；
  长时间写入不会让日志无限增长，重启时的恢复内存也有上界。
- 正常关闭（stdin EOF 或 SIGINT/SIGTERM）：回滚未提交事务、刷脏页、`fsync`、截断 WAL。

### 4.4 向量检索

- 精确扫描：过滤谓词先判定，命中记录才计算距离；维护大小为 K 的最大堆，排序键为 `(平方距离, 主键)`。
- AVX2/FMA 距离内核带运行期 CPU 检测，不支持 AVX2 时回退到标量实现；不会因 `-march=native` 在未知评测机报非法指令。
- OpenMP 按页切分并行扫描，线程数默认 2（可用 `--query-threads` 调整），结果合并后按 `(距离, 主键)` 输出。
- `CREATE VECTOR INDEX` 持久化索引元数据；当前检索路径以精确扫描保证召回，索引维护不会放宽过滤与事务正确性。

## 5. 验证记录

以下结果来自本机 Windows MSYS2（GCC 15.2，2 线程）和仓库内测试脚本；正式成绩以平台锁定版本为准。

| 验证项 | 方法 | 结果 |
| --- | --- | --- |
| JSONL 握手/关闭 | `python3 protocol_check/run.py --handshake-only`（官方检查器，先等 ready 再发 shutdown） | PASS |
| 公开用例（官方协议） | `python3 protocol_check/run.py`（`mysqltest/` 20 个用例/95 条 SQL，逐请求 JSONL） | 20/20 PASS |
| 公开用例（本地驱动） | `python tests/local_driver.py`（同一题包，逐语句比对 columns/rows/affected_rows/error.code） | 20/20 PASS |
| 正式要求逐项覆盖 | `python tests/formal_coverage.py`：模式/类型边界、全部比较运算、ORDER BY/LIMIT/OFFSET、事务原子性、非法向量、恢复 | ALL PASS |
| 随机模型压力 | `python tests/stress_random.py`：400 主键随机增删改、exact/ann、过滤/不过滤、重启与强杀 | PASS（多随机种子） |
| 事务/回滚/持久化 | `python tests/regression.py` | PASS |
| 崩溃恢复 | `tests/regression.py`：显式事务中 kill；已提交保留、未提交不残留、重启可继续写 | PASS |
| 恢复中重复强杀 | `python tests/crash_recovery_test.py`：50,000 行 × 32 维，WAL 重放期间再 kill 3 次 | PASS |
| WAL 有界与 checkpoint | 30,000 行 × 128 维写入过程中 32 MiB 自动刷盘截断；重启行数与首尾主键核对 | PASS（WAL 峰值约 3.3 MiB） |
| 向量 TopK | `tests/regression.py`：500 行 4 维随机数据，20 个过滤查询对暴力真值；索引后增删改 | PASS |
| 大规模 TopK | `python tests/bench.py`：10,000 行 × 128 维，1,000 个 K=10 查询（一半过滤），NumPy 暴力核对 | PASS（Recall=1.0） |
| 本地性能 | 同上，本机 2 线程 | 插入 10,000 行约 1.74 s；1,000 查询约 0.30 s（约 0.30 ms/查询） |
| 极端 float32 距离 | ±3.4e38 合法向量：float 快速路径溢出时 double 重算，输出有限距离并按距离排序 | PASS |
| Linux 构建与全量回归 | GitHub Actions `ubuntu-24.04`：`make` + 官方 `protocol_check` + 正式覆盖/压力/恢复测试 + Benchmark | PASS |
| Docker 镜像与容器运行 | GitHub Actions `ubuntu-24.04`：`docker build` + 容器内跑官方 `protocol_check` 全部公开用例 | PASS（`all cases passed`） |
| 超过缓存的扫描 | `--cache-mb=1` 插入 20,000 行 × 32 维，多次 TopK 与暴力结果核对 | PASS |

`protocol_check` 是官方题包自带的交互式检查器：它按正式顺序等待启动 ready、逐请求发送、检查响应
id 类型与布尔 ok，并支持 `--restart` 和 `--mode ann`。本机没有 Docker，因此 Docker 边界由
GitHub Actions 的 Ubuntu 24.04 runner 实际构建并运行验证。

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
3. 当前 ANN 模式与 exact 模式共用精确扫描（保证 Recall=1.0）；未实现 HNSW/IVF 近似加速。
4. 单个显式事务的叠加层保存在内存；超大事务受内存限制（基准负载为每 100 行提交，不属于此情形）。
