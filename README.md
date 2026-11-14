# MiniVectorDB — clean rewrite

这是从官方 `2026-competition-round2` 空白题包重新实现的 C++20 数据库。原先几个目录的数据库源码没有进入本实现。SQL、磁盘 B+ 树、事务、恢复、查询执行、外部排序和向量索引均自行编写；通用 JSON 依赖为 nlohmann/json 3.11.3，以及按完整 commit 固定的 RapidJSON。通过版本的聚集和整数筛选设计经改写后接入，来源与差异见 [进一步优化](docs/FASTER_REFERENCE_20261114.md)。

赛题：[完成一个轻量向量数据库](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/lg8enxlmwg69unt6)。官方题包基点：`f4d4e26653af823a7601668802f80e374ad6643c`。`STDIO_PROTOCOL.md`、`mysqltest/` 和 `protocol_check/` 保持官方内容。

## 构建与运行

正式目标为 Linux x86_64。两层基础镜像均按 digest 固定，JSON 依赖已随源码交付；编译阶段不联网，运行阶段无需网络。标准库静态链接，运行镜像不安装额外软件包。

```bash
docker build --platform linux/amd64 -t minivectordb-clean .
mkdir -p data
docker run --rm -i --read-only --network=none \
  --cpus=2 --memory=2g --memory-swap=2g --pids-limit=128 \
  -v "$PWD/data:/data" minivectordb-clean
```

容器以前台 JSONL 模式启动，主动输出 `{"type":"ready"}`。不开放端口、不使用 TTY。每条响应换行并立即 flush，stdout 不包含日志。完整格式见 [STDIO_PROTOCOL.md](STDIO_PROTOCOL.md)。

```json
{"id":1,"op":"execute","sql":"CREATE TABLE docs(id BIGINT PRIMARY KEY,tag VARCHAR(8),v VECTOR(2));"}
{"id":2,"op":"execute","sql":"INSERT INTO docs VALUES(1,'中文','[3,4]'),(2,'x','[0,0]');"}
{"id":3,"op":"execute","sql":"SELECT id,tag FROM docs ORDER BY id;"}
{"id":4,"op":"execute","sql":"SELECT id,L2_DISTANCE(v,'[0,0]') AS distance FROM docs ORDER BY distance LIMIT 2;","search_mode":"exact"}
{"id":5,"op":"execute","sql":"BEGIN;"}
{"id":6,"op":"execute","sql":"UPDATE docs SET v='[1,0]' WHERE id=1;"}
{"id":7,"op":"execute","sql":"ROLLBACK;"}
{"id":8,"op":"execute","sql":"BEGIN;"}
{"id":9,"op":"execute","sql":"DELETE FROM docs WHERE id=2;"}
{"id":10,"op":"execute","sql":"COMMIT;"}
{"id":11,"op":"shutdown"}
```

再次使用相同挂载目录启动，`SELECT * FROM docs;` 应只返回主键 1，向量为 `[3,4]`。测试中也直接终止数据库进程，以检查已经确认提交的数据不依赖 shutdown。

本地编译需要支持 C++20 的 g++ 与 make，无需 CMake 或包管理器：

```bash
make -j2
./minivectordb --data-dir ./data
```

Windows/MSYS2 使用 `mingw32-make`，生成 `minivectordb.exe`；正式验收仍使用 Linux 镜像。

## 模块与职责

| 文件 | 职责 |
| --- | --- |
| `src/main.cpp` | JSONL 校验、请求分派、握手与进程退出 |
| `src/request.*` | 有严格校验和原解析器回退的请求解码 |
| `src/sql.*` | SQL 词法分析与语法树；解析完整语句后才执行 |
| `src/model.*` | BIGINT、Unicode、float32 校验，二进制记录编码 |
| `src/storage.*`、`src/btree.cpp` | 文件 I/O、有界页缓存、磁盘 B+ 树、提交根 |
| `src/compaction.cpp`、`src/record_reader.cpp` | 历史页回收、按需读取记录列 |
| `src/engine.*` | 自动提交、显式事务、语句原子性与 DDL |
| `src/query.cpp` | 条件绑定、投影、TopK、外部归并排序 |
| `src/vector_index.*` | 有界向量快照、KD 树与精确下界剪枝 |
| `src/exact_batch.hpp`、`src/vector_literal.*` | 分块精确距离、CPU 分派与严格向量数字绑定 |
| `src/quantized_screen.hpp` | 有误差上界的整数候选筛选与精算证书 |
| `src/result.*` | 大响应在数据目录暂存，完整生成后才输出 |

没有数据库引擎或向量检索引擎依赖。依赖版本、校验和及许可证位于 `third_party/`。

## 存储与恢复

持久文件为 `/data/database.mvd`。前两个 4096 字节页是交替使用的提交根，包含版本、递增代号、catalog 位置、有效文件末尾和 FNV-1a 64 位校验。之后是追加的记录、对齐的 B+ 树页和 catalog；BIGINT 与 float32 使用明确的小端编码，字符串按字节保存 UTF-8。

B+ 树叶页保存有符号 BIGINT 主键到记录位置的映射。树页按需读取；不在内存维护全表主键目录。更新采用写时复制：只允许修改当前 SQL 语句自己创建的树页，已提交页及先前成功语句的页保持不可变。删除保留合法的路由边界，空叶页允许后续重插。

每条修改语句先复制小型 catalog，记录文件回退位置，再执行到新的树根。失败时丢弃本语句新页并截断到回退位置；事务中此前成功的语句仍然可读、可提交。显式 ROLLBACK 恢复上次提交根并回收未提交尾部。未显式 BEGIN 时，一条修改语句就是一个自动提交事务。

提交顺序为：完成记录、catalog 和脏树页的全部 positional write → 写另一份提交根 → `fdatasync` 整个文件 → 返回成功。与参考版本相同，使用赛题规定的进程崩溃模型：进程被终止时，已完成的内核写入仍对恢复可见，成功响应前的同步负责持久化确认。机器断电时的写入排序需要额外的前置屏障，不属于这些进程强杀测试已建立的保证。提交期间的 I/O 异常会结束进程，不在提交结果可能已生效时反向回滚。Windows 自测使用 `_commit`。

多行 INSERT 将不超过 64 KiB 的记录字节合并追加，位置条目最多 1024 个；大行直接追加。普通插入、替换和查找可直接处理有界树页，只有分裂、删除或扫描才需要完整解码。写时复制页归属和整条语句回退保持不变。

恢复选择代号最新且校验有效的提交根，读取 catalog，截断未提交尾部，清理本实现的临时文件；有界向量索引的恢复准备在 ready 前完成。恢复不回写已提交页，因此重复终止恢复不会把已提交页改坏。崩溃测试针对进程强杀；不声称覆盖外部篡改或硬盘损坏。

无法识别的非空文件会拒绝打开并保留原字节；首次空库初始化中断只按特定格式白名单重试。恢复按代号从新到旧验证候选根，一旦校验确定根，解码的资源或结构异常会中止启动，不能当作“旧版本更可用”而截断已确认数据。

文件超过 64 MiB 且明显大于当前有效数据和树页估算时，在事务之间回收已提交历史：流式复制当前可见记录到 `/data/.compact-work`，完整提交并同步新库，再原子替换主文件。替换前后的精确强杀已验证；未完成的临时库在恢复时清理。`live_bytes` 计数随语句一起提交和回滚，早期本重写格式缺少计数时，可从叶页引用恢复。

## 查询与内存

支持 CREATE TABLE、CREATE VECTOR INDEX、INSERT（含多行和完整列名重排）、SELECT、UPDATE、DELETE、BEGIN、COMMIT、ROLLBACK。支持 BIGINT、VARCHAR(n)、VECTOR(n)，每表一个 BIGINT 主键、最多一个向量列；支持比较运算、AND、多列 ORDER BY、LIMIT、列别名和 L2_DISTANCE。距离表达式未写 AS 时返回列名 `l2_distance`。不支持的 JOIN、聚合、子查询、ALTER、NULL、主键更新及嵌套事务返回错误。

VARCHAR 长度按 Unicode 码点计算；合法 UTF-8 的字节字典序与码点顺序一致。BIGINT 不经浮点中转。向量和查询向量均先转换成有限 float32，再以 double 按维度顺序累加平方距离，返回欧氏距离；编译禁用 FMA 收缩，未启用 fast-math。有限 float32 的转换临界值按舍入后的结果判断，而非误用 `FLT_MAX` 作为十进制输入上界。

过滤先于 TopK。距离和普通排序出现并列时，按主键升序确定次序。磁盘路径仅读取投影、过滤和排序所需列，跳过无关大字段。小 LIMIT 使用有界堆；大 LIMIT 或无 LIMIT 的排序分批写运行段，最多同时归并 32 个文件。查询响应超过 256 KiB 后在 `/data` 暂存；先完成暂存文件收尾，再输出 JSON 前缀，避免最终 flush 失败时混入半条成功响应。

页缓存默认 4 MiB。向量缓存仍共用 16 MiB 预算，只保存向量、主键及过滤/排序工作列，未缓存的最终投影按赢家位置读取。物化前检查预算；缓存拒绝以列集合记录，避免大字段阻碍后续小字段查询。超过预算时使用磁盘精确扫描，不加载整表。

低维使用 float32 包围盒的精确 KD 下界；高维使用每组 32 候选的分块平面索引，根据 CPU 能力选择 scalar、AVX2 或 AVX-512。各候选仍按原维度顺序以 double 累加，禁用 FMA 收缩。工作列可按普通过滤列聚集，二分确定范围，未覆盖的条件仍逐条校验。

预算允许时另建 int8 辅助编码，原始 float32 向量保留。仅在保守误差界能证明所有未入选记录都更差时，才接受不超过 32 个候选的完整 double 精算；证明失败即使用完整精算路径。沿用通过版本的布局选择，证书候选使用行式连续源向量，完整扫描时一次性恢复分块布局。便携整数、AVX2 和 VNNI 内核不改变返回距离或并列顺序。`exact` 和 `ann` 都提供精确结果，任何修改或事务控制都会清除派生缓存。

向量文字采用严格数字绑定和完整输入检查。请求解码对整帧原始 UTF-8、完整 JSON、全部层级的重复键和转义执行校验；原始 NUL 不能提前终止帧解析，合法转义的 VARCHAR NUL 数据仍受支持。常规整数 ID 请求使用 RapidJSON 的带内存填充的整段复制路径；浮点或超 64 位数字、解析异常均交回原来的严格 nlohmann 解析器，以保留数值和错误语义。两个库均不代执行数据库核心。

自测可通过 `MVD_PAGE_CACHE_MIB`（0–64；0 表示最小 8 页）和 `MVD_VECTOR_CACHE_MIB`（0–64；0 关闭快照）降低缓存。正式默认值不随测试输入改变。

catalog、单条 SQL 输入和单条记录仍需能放入内存；分页和外部排序用于处理总数据量超过内存的数据库。回收发生在事务之间，需要同时保存旧库和新库；长事务的未提交尾部仍需额外磁盘空间。不兼容原先单体实现的数据格式。

## 验证

Linux 容器 CI：[本轮完整回归 ac217b1](https://github.com/xxxhm0524/seekdb/actions/runs/43592647314)。37 份回归和 4 份专题报告及其 commit/CI URL 见 `validation/faster-review-20261114/`。数值内核复用报告的相关源码哈希已核对保持一致。此前记录保留在其他 `validation/` 子目录，发现的问题见 [审查记录](docs/AUDIT_20261114.md)，本轮源码迁移和三方同机对照见 [进一步优化](docs/FASTER_REFERENCE_20261114.md)。这些不能替代平台的 29 个正式用例或正式得分。

* 未修改的官方驱动和公开断言：20/20。
* 500 步独立随机状态机，覆盖查询、失败语句、提交、回滚和强杀。
* 60000 个打乱的主键：多层分裂、极小页缓存、全删回滚、删除与重插、跨缓存的晚期失败。
* 外部排序和约 8 MB 大响应；Unicode、BIGINT 极值、非法向量和 float32 临界舍入。
* KD 路径及关闭向量缓存的磁盘路径：共 1008 次极端 float32 查询，与独立顺序累加 oracle 比较。
* 14 次提交中断，核对双表原子性、确认提交持久性和未确认提交的全有或全无；另有 15 次启动中断。
* 在恢复真正执行尾部截断前、后各精确中断两次，连续恢复没有就绪间隔，已提交数据保留，未提交修改回滚。
* 3 类未知非空文件拒绝打开且字节不变；13 种首次初始化残片可重试；最新 catalog 解码分配失败时保留文件，解除故障后 20 行已确认数据完整。
* 96 MiB cgroup 容器中写入 163840000 字节业务负载，数据库约 170 MB，完成查询、提交、回滚和强杀恢复。
* 正式 2 GiB 内存预算下写入 2621440000 字节业务文本；120 种列排列、响应 ENOSPC 和原子替换故障注入通过。
* ASan、UBSan 和标准库边界断言未报错。
* 混合过滤列、缓存拒绝、赢家回查、事务回滚和重启通过；生产编译器下距离内核 18432 次逐位比对、17246 个数字解析兼容与边界样本通过。
* 61680 次整数筛选界/可用内核比对，1085 次完整检索对拍，覆盖近等距与裁剪查询的证书回退、行式候选和完整扫描布局恢复；528 个 SQL 文字与 48279 个完整 JSON 输入对拍通过。
* 64 KiB 两侧、大行与小行混合、超 1024 条元数据、多次刷新后的晚期错误，核对失败后文件字节不变；四个真实提交根/同步系统调用边界精确强杀保留双表原子性。5000 行单条 INSERT 的实际数据文件写调用为 66 次，确认事务完成一次文件同步。
* 与有公开正式通过记录的固定版本完成 1600 步 / 1629 请求对拍，并由独立状态模型核对。
* 10000 行 × 128 维，1000 次 TopK10，两组 Recall@10 均为 1.0。同机三轮、16/1024 字节正文的查询中位数约 0.2653/0.2643 秒；对照约 0.2572/0.3110 秒。1 KiB 正文写入/查询约快 6.9%/15.0%，短正文仍慢约 4.0%/3.1%，内存也未全面领先。输入生成在计时外，时间含驱动开销，不推算正式性能分或排名。

```bash
python3 protocol_check/run.py --image minivectordb-clean --output results/official
python3 tests/client.py --output results/local-public
python3 tests/invariants.py --output results/local-invariants
# 下列 oracle 测试需要 NumPy；CI 单独使用锁定的 2.2.6，不进入运行镜像
python3 tests/benchmark.py --output results/local-benchmark
python3 tests/vector_edges.py --output results/local-vector-edges
MVD_DOCKER_IMAGE=minivectordb-clean python3 tests/capacity.py --output results/capacity
python3 tests/crash_commit.py --output results/crash-commit
python3 tests/contract.py
```

## 提交材料

正式提交使用已登记仓库的 `2026-competition-round2` 分支和完整 40 位 SHA。根目录 `conversation.json` 与 `agent/conversation.json` 内容相同，均为 UTF-8 JSON 对象，低于 128 KiB。材料保留本次重写的真实用户发言、明确归属 Agent 的实现说明和实际测试输出，不把 Agent 的工作伪装为学生自己的解释。

对照使用 [juanzhi-gcb/seekdb 的固定成功版本 dfee55b8](https://github.com/juanzhi-gcb/seekdb/tree/dfee55b84284f4c39d2df45951cdbcb1d1baf1a2)，39 个源码文件按哈希固定，其执行器、工作列缓存、整数筛选和证书实现是本轮直接源码对照与迁移的依据。作者公开记录称该版正式 MySQL 通过、Prompt 原始分 6.5 有效；来源及无法独立认证对方平台账户的边界均在审查记录中说明。双方反斜杠转义约定的差异单独保留，对拍覆盖题目明确的公共语义。未复制对方学习材料。
