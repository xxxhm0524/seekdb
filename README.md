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

容器以前台方式运行，只通过 stdin 接收 SQL、stdout 返回响应；日志只写 stderr，不监听网络端口。
所有持久化文件位于 `/data`。可用环境变量 `MVD_OUTPUT` 切换响应格式（见第 3 节）。

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
- `SELECT *` 中的 VECTOR 列输出为 `[v0,v1,...]` 文本。

### 2.3 不支持项

不要求且未实现：NULL、JOIN、聚合、子查询、ALTER TABLE、外键、嵌套事务、主键更新、`OR`、`IN`、`LIKE`。
不支持的语句返回 `ERROR` 响应（JSON 模式下为 `{"error":"...","message":"..."}`），不会静默改变语义。

## 3. stdin/stdout 协议

程序启动后保持运行，逐条接收 SQL。语句以 `;` 结束（支持多行语句和 `--`/`#`/`/* */` 注释）。

响应格式可通过 `--output=` 或环境变量 `MVD_OUTPUT` 选择：

| 模式 | 格式 |
| --- | --- |
| `json` | 每条语句输出一行 JSON：`{}`、`{"affected_rows":N}`、`{"columns":[...],"rows":[[...]]}`；错误为 `{"error":"CODE","message":"..."}` |
| `mysql` | 查询输出 `列名\t列名` 表头 + 制表符分隔行；DML 输出 `OK N`；DDL/事务输出 `OK`；错误为 `ERROR CODE: message` |
| `tsv` | 查询只输出制表符分隔行；DML 输出 `OK N`；DDL/事务输出 `OK` |
| `miniob` | 查询输出 ` | ` 分隔的表头和行；成功修改输出 `SUCCESS`；错误输出 `FAILURE` |

`--ready-marker=TOKEN` 会在启动完成后输出一次 `TOKEN`，并在每次响应后追加该行（便于驱动分帧）。
`--query-threads=N` 控制向量扫描线程数（默认 2），`--cache-mb=N` 控制页缓存上限（默认 64 MiB）。

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
| 公开用例 | `python tests/local_driver.py`（5 个官方公开用例，逐语句比对 columns/rows/affected_rows） | 5/5 PASS |
| 事务/回滚/持久化 | `python tests/regression.py` | PASS |
| 崩溃恢复 | `tests/regression.py`：显式事务中 kill；已提交保留、未提交不残留、重启可继续写 | PASS |
| 向量 TopK | `tests/regression.py`：500 行 4 维随机数据，20 个过滤查询对暴力真值；索引后增删改 | PASS |
| 大规模 TopK | `python tests/bench.py`：10,000 行 × 128 维，1,000 个 K=10 查询（一半过滤），NumPy 暴力核对 | PASS（Recall=1.0） |
| 本地性能 | 同上，本机 2 线程 | 插入 10,000 行约 1.36 s；1,000 查询约 0.26 s（约 0.26 ms/查询） |
| Linux 构建与全量回归 | GitHub Actions `ubuntu-24.04`：`make` + 公开用例 + 回归 + Benchmark | PASS（插入约 0.65–1.00 s；1,000 查询约 0.20–0.30 s，Recall=1.0） |
| Docker镜像与容器运行 | GitHub Actions `ubuntu-24.04`：`docker build` + 容器内跑公开用例 | PASS（`all cases passed`） |
| 超过缓存的扫描 | `--cache-mb=1` 插入 20,000 行 × 32 维，多次 TopK 与暴力结果核对 | PASS |

本开发机没有 Docker/WSL，但 `Dockerfile` 已在 GitHub Actions 的 Ubuntu 24.04 runner 上实际 `docker build`，
并在容器内通过全部公开用例；`make` 构建链、回归与 Benchmark 也在同一 CI 环境通过。

## 6. 目录

```
src/common.h      文件、CRC32、UTF-8、数值解析
src/sql.h/.cpp    SQL 词法/递归下降解析器
src/engine.h/.cpp 页存储、页缓存、WAL、事务、恢复、向量扫描
src/main.cpp      stdin/stdout 服务与响应格式
tests/            公开用例、回归、Benchmark
agent/conversation.json  Agent Prompt 学习材料
Dockerfile
```

## 7. 已知限制

1. 行必须能放入 64 KiB 页；超宽 VARCHAR/VECTOR 建表会返回错误。
2. 不支持 NULL、默认值、JOIN、聚合、子查询、ALTER TABLE、主键更新。
3. 当前 ANN 模式与 exact 模式共用精确扫描（保证 Recall=1.0）；未实现 HNSW/IVF 近似加速。
4. 单个显式事务的叠加层保存在内存；超大事务受内存限制（基准负载为每 100 行提交，不属于此情形）。
