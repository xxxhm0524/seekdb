# MiniVectorDB

一个面向单进程、单客户端场景的磁盘型向量数据库。实现采用 C++20 与 POSIX 文件接口，运行时不依赖任何第三方库。在 Linux x86_64 上，程序以前台方式运行：SQL 从 stdin 读入，结果经 stdout 返回，所有数据落在 `/data`。

## 构建与重启

以下操作均在提交分支的仓库根目录执行。构建由根目录的 Dockerfile 完成：源码取自 `extra/`，编译器锁定为固定摘要的 GCC 13.3.0，产出的镜像内只含静态链接的程序与数据目录。镜像默认使用官方 JSONL 协议；下面这条手动 SQL 演示则显式切换到逐行文本模式。

~~~sh
docker build --platform linux/amd64 -t minivectordb .
mkdir -p extra/.demo-data
docker run --rm -i --network none -v "$PWD/extra/.demo-data:/data" minivectordb --input=line <<'SQL'
CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(16), embedding VECTOR(2));
INSERT INTO docs VALUES (1,'db','[1,0]'),(2,'db','[0,1]'),(3,'other','[0,0]');
SELECT id, category FROM docs WHERE id >= 2 ORDER BY id;
CREATE VECTOR INDEX docs_vec ON docs(embedding);
SELECT id, L2_DISTANCE(embedding,'[0,0]') AS d FROM docs WHERE category='db' ORDER BY d LIMIT 2;
BEGIN;
UPDATE docs SET embedding='[0,0]' WHERE id=2;
SELECT id FROM docs WHERE category='db' ORDER BY L2_DISTANCE(embedding,'[0,0]') LIMIT 1;
ROLLBACK;
BEGIN;
DELETE FROM docs WHERE id=3;
COMMIT;
QUIT
SQL
docker run --rm -i --network none -v "$PWD/extra/.demo-data:/data" minivectordb --input=line <<'SQL'
SELECT * FROM docs ORDER BY id;
QUIT
SQL
~~~

事务里最后一次更新落在 `id=2` 上：ROLLBACK 使其回到 `[0,1]`；已提交的删除在重启后依旧生效，因此最终只剩 `id=1` 与 `id=2` 两行。

## SQL 与本地交互

- `CREATE TABLE`：每表一个 `BIGINT` 主键、若干 `VARCHAR(n)`，至多一个 `VECTOR(n)`；主键既可写在列定义里，也可以用表级 `PRIMARY KEY(id)` 指定。
- `INSERT`（单行与多行）、`UPDATE`、`DELETE` 均可用；带列清单的 `INSERT` 必须给全所有列。主键全局唯一且不允许被更新。
- 查询支持 `SELECT *` 与列投影，`WHERE` 提供 `= < <= > >=` 和 `AND`，另有 `ORDER BY` 与 `LIMIT`。排序键相同的行按主键升序排列；未写排序时按主键升序输出。
- `VARCHAR(n)` 的长度与比较都按 Unicode 码点处理。向量常量形如 `'[1,2]'`，入库时转成可表示的 float32。`L2_DISTANCE` 基于存储后的向量求欧氏距离，TopK 只在通过过滤条件的行中取。
- 事务接口为 `BEGIN`、`COMMIT`、`ROLLBACK`。写语句默认自动提交；显式事务里已成功的修改立刻可见，某条语句失败只撤销这一条本身。

默认的 JSONL 模式在完成恢复后发出 `{"type":"ready"}`。每个请求占一行，是带 `id` 与 `op` 字段的 JSON 对象；执行类请求额外携带 `sql`，并用 `search_mode` 在 exact 与 ann 之间选择。响应沿用请求 id 的类型与取值，给出布尔 `ok` 以及对应的查询结果、影响行数或错误信息。BIGINT 以 JSON 整数输出，向量输出为数组。收到 shutdown 后，先撤销未提交事务、回一次成功响应，再退出进程。每条响应独立成行并立即刷新，诊断输出走 stderr。[官方 stdin/stdout 协议](https://github.com/oceanbase/seekdb/blob/f4d4e26653af823a7601668802f80e374ad6643c/STDIO_PROTOCOL.md)

本地调试可改用 `--input=line`，一行一条完整 SQL。进程就绪后先输出 `READY`；写入成功回 `OK <影响行数>`，出错回 `ERROR <代码> <消息>`，查询回 `RESULT <列数> <行数>`、制表符分隔的列名与数据行，最后是 `END`。`MODE exact` 与 `MODE ann` 用于切换检索模式，`QUIT` 结束会话并回 `BYE`。`--input=sql` 则面向多行脚本：只要分号出现在字符串、向量常量、反引号标识符或注释之外，即视为请求结束。启动参数同时支持 `--data-dir /data` 与 `--data-dir=/data` 两种写法。无论哪种输入，遇到 EOF、关闭信号或 stdout 读端关闭，都会结束会话并撤销未提交的事务。

错误码与公开题包对齐：重复主键、不支持的 SQL、向量字面量维度不符分别对应公开题包使用的 `duplicate_key`、`unsupported_sql`、`dimension_mismatch`；格式错误、非有限数值与 float32 溢出仍归入 `VALUE`。其余代码覆盖：语法（`SYNTAX`）、表结构（`SCHEMA`）、表或列不存在（`UNKNOWN_TABLE`、`UNKNOWN_COLUMN`）、命名冲突（`TABLE_EXISTS`、`INDEX_EXISTS`）以及事务状态（`TRANSACTION`）。一旦存储层故障，进程即刻停止并把诊断写入 stderr，下次启动再依据日志恢复。

## 设计

- **记录与读取**。表定义、数据变更与提交帧全部进入一个带版本号的二进制日志。新增的 `MINIVDB5` 格式沿用了 `MINIVDB3` 的紧凑插入帧。若一个向量能用更短的记录逐位还原，就把它写成 8、10、12 或 16 位有符号整数的二进制定点，步长取 2 的整数次幂；低 16 位全为零的向量仍可只存高 16 位。其余向量尝试以 1 到 3 字节有符号整数编码各分量，十进制步长取 `10^-2`、`10^-4` 或 `10^-6`——只有当还原后每个 float32 分量的位模式与原值完全一致、且确实更短时才采纳。剩下的可压缩向量沿用每八个分量记录符号与指数差的布局；若无节省，则退回原始 float32。`MINIVDB1` 至 `MINIVDB4` 的既有日志按原格式照常读写。主键目录额外记录帧头位置与向量前缀长度，按标量列过滤时即可整段跳过向量。查询与建索引可挂最多 256 MiB 的只读文件映射；存在地址空间上限时，映射规模不超过其四分之一。映射不可用就退回按范围读盘，任何修改与事务操作前先解除映射。
- **事务与恢复**。每一次行变更都记下其上一版本的位置，帧尾长度字段支撑逆向撤销。更新若未改变行内容，则直接复用原记录，但影响行数仍按匹配行数计。语句级与事务级起点分别登记；出现语义错误时，只回退当前语句并截断相应日志。提交流程是：先写入完整提交帧并调用 `fdatasync`，然后才返回成功。启动时扫描日志，读到完整提交帧才重放对应变更；启动缓冲至多缓存 128 个帧头，超长事务的剩余帧按需回读。日志尾部若无提交帧，一律截断。崩溃模型仅为进程被终止。
- **检索**。容量为 K 的堆只接纳通过过滤的候选。在具备 AVX2 的机器上，事务之外的中高维精确 TopK 可借助至多 2 MiB 的共享候选缓存；每个 SIMD 向量一次性筛过八行的 8 位量化码。剪枝依据三角不等式：把全表最大量化误差与本次查询的误差并入当前第 K 名的半径，整数距离只要严格超过向上取整后的界限即跳过该行；留下的候选再读磁盘上的原向量。一旦发生修改或回滚，缓存即失效；预算不足、查询落在量化范围之外或处于事务中，都直接走扫描。对 32 至 4096 维的升序 LIMIT 查询，另有基于 float32 平方和的保守下界，临近边界或可能溢出时改用 double；若两条 double 距离的误差区间交叠，则改用存储的 float32 值对平方和做精确比较，同距再按主键分先后。对外返回的距离取自 double 平方和。`CREATE VECTOR INDEX` 为当前可见向量建立按第一坐标排序的投影索引；ann 模式自最接近查询点的位置向两侧扩展，只有当未访问坐标的距离下界严格大于当前第 K 名时才停止。投影索引与量化缓存都引用主键目录中的行位置，任何修改或回滚前先行失效。恢复阶段会在对外就绪前把投影索引备好。投影预算 4 MiB，超限或分配失败时退回磁盘精确扫描。
- **输入解析**。不带转义的 SQL 请求与字面量直接引用输入缓冲区，生命周期限于本次执行；含转义的字符串则解码到独立缓冲区。在 x86-64 上，连续的 ASCII 段可按 64 字节成批校验，只有多字节字符才逐个验证。形如一位整数加至多十八位小数的普通向量字面量，先合并十进制数字再转 float32；逼近 float32 舍入中点的长小数走标准转换。其余形式同样沿用标准转换，次正规数、溢出与非法字面量的检查全部保留。同一向量字面量在查询表达式中重复出现时，解析结果可复用。
- **资源范围**。为长字符串排序只保留候选键的日志位置与长度，比较时再分块读入内容。主键目录和排序元数据的体积随记录数增长；日志保留全部已提交历史，长期更新或删除会推高磁盘占用。实现范围限定在题目的 SQL 子集，以及单客户端、单事务的执行模型。

精确 TopK 若带单列等值过滤，可在缓存预算内为该列建立至多 4,096 个连续分组：先统计每个值的行数，再按主键顺序为每行定分组位置，量化码与行引用就地移动。首次等值查询能直接复用现成的量化码。BIGINT 按完整值分组；VARCHAR 退而按指纹分组，并对候选逐行复核原谓词。分组查询还可叠加主键范围。一旦超出分组上限、碰到宽行或预算吃紧，就沿用未分组扫描。步长 128 字节的量化查询用四路 32 位累加，VNNI 与 AVX2 点积在运行时择一。建立向量索引时，先把已提交的日志前缀映射进来，再落盘索引定义，以减少乱序主键带来的零散读取。

小 K 查询若过滤资格可直接判定、且不受主键范围约束，VNNI 扫描能暂存至多 512 个量化候选；已有紧凑几何分组的查询继续走前缀候选。取合格候选中第 K 小的量化距离，加两倍总误差，即得候选量化距离的保守上界；上界收紧后再读原向量。若缓冲区内候选仍然密集，先精算留存候选，其余沿用逐块扫描。其余 128 字节步长的小 K 查询，可用前 256 行的量化距离选种，再以第 K 小的精确距离立阈值。当量化剪枝成块保留候选时，继续用 float32 平方和的保守下界预筛，随后只对需要的行算 double 距离。量化同距与贴着阈值的行，仍保留进入最终精排的资格。

当 128 维数据的各坐标波动范围远小于其绝对值时，量化可用各坐标的中点充当公共原点，从而收窄步长。若样本统计与全表范围出入明显，可在核验全部记录后，仅对主体数据收窄量化步长；落在范围外的少量记录逐条精算。含极端记录的分组一律给零几何下界，保住它们进入 TopK 的可能。

若已分组量化码的组间距足够明显，可从每组取一行当中心，用全组记录算出覆盖半径。无过滤检索按组距离下界由近及远推进；某组下界一旦严格超过计入量化误差的现行阈值，其余各组即可整体跳过。原向量复核与主键并列规则与逐行检索完全一致。

分组数较少时，只按主键过滤的大区间可逐组定位其中的行，各组之间共用同一个精确距离阈值；区间较小时，直接扫描目标记录即可。

## 验证

以下步骤在 `extra` 目录内执行。

~~~sh
mkdir -p .build
TMPDIR="$PWD/.build" g++ -std=c++20 -O3 -flto -static -Wall -Wextra -Wpedantic \
  sql.cpp log.cpp db.cpp main.cpp -o .build/minivectordb
python3 verify.py .build/minivectordb --benchmark
~~~

验证范围覆盖 Unicode 与 float32 的边界、带过滤的 TopK、极端数值下的距离排序、索引维护、语句原子性、提交与回滚，以及强杀后的重复恢复。两组剪枝测试以 Python 整数精确平方和为排名基准，覆盖 31 至 4097 维、溢出、下溢、并列与量化范围外的查询，并核对更新、失败写入、回滚和重启后的缓存失效。输出阻塞用例验证：stdout 写满或读端关闭时，进程仍能退出，且撤销已写入日志的未提交修改。限内存用例在 14 MiB 进程地址空间内恢复 100,000 条带索引记录，并核对 exact 与 ann 两种查询。另一组在 18 MiB 地址空间里重启并读入超过 36 MiB 的乱序日志，检验 TopK、长字符串排序、排序并列和事务内变更。TopK 的进程读取量借助 Linux `/proc/<pid>/io` 的 `rchar` 度量。可选基准写入 10,000 条 128 维记录、执行 1,000 个 Top10 查询，逐条校验返回距离，并以独立穷举抽查两组排名。

等值过滤回归另查：Unicode 分组、713 个过滤取值、主键范围内查无记录的情形，以及改写过滤列或回滚之后旧分组必须失效。

存储回归取含负零、次正规数与大指数差的 129 维向量，对五种日志版本逐位核对，并覆盖十进制定点的三种整数宽度与二进制定点的四种整数宽度；两种编码都检验向量尾部、索引、回滚与重启。截断回归覆盖旧版紧凑帧与宽帧的各处边界。检索回归另含：量化同距而原距离不同、等值分组内主键范围首尾错位、组尾离群点、跨组精确并列，以及量化平移、查询超出量化范围、事务回滚和重启后的排名一致性。优先精算回归覆盖未对齐的 VARCHAR 分组、K 的切换边界，以及更新、回滚与重启后的一致性。

追加 `--stress` 后另跑两组日志超出进程地址空间上限的恢复测试：一是在 26 MiB 地址空间内恢复 600,000 条窄行，核对主键范围与两种检索模式；二是在 9 MiB 地址空间内两次重启，确认含 400,000 条未提交记录的日志尾部已截断、已提交数据完好。常规自测还包括多行输入、批量删除引发的分组合并与跨分组回滚。

可以从官方快照提取 `mysqltest/`，在同一目录复跑 20 组、95 条公开 SQL。

~~~sh
git init --bare .build/official.git
git --git-dir=.build/official.git fetch --depth=1 --no-tags \
  https://github.com/oceanbase/seekdb.git f4d4e26653af823a7601668802f80e374ad6643c
git --git-dir=.build/official.git archive FETCH_HEAD mysqltest protocol_check STDIO_PROTOCOL.md | tar -x -C .build
python3 verify.py .build/minivectordb --public-suite .build/mysqltest
TMPDIR="$PWD/.build" python3 .build/protocol_check/run.py --image minivectordb \
  --output .build/official-protocol-results
~~~

自测为每组用例建立独立数据目录：`--restart` 复用该目录，`--mode ann` 则切换检索模式。断言项包括成功状态、错误语义名、影响行数、列名与结果行；早期那种清单不带文件路径的题包同样支持。无序结果按多重集合比较，浮点距离允许 `1e-5` 的绝对与相对容差。官方检查器走镜像默认的 JSONL 协议，输出目录必须是全新目录。[官方公开用例说明](https://github.com/oceanbase/seekdb/tree/f4d4e26653af823a7601668802f80e374ad6643c/mysqltest)

仓库根目录的 Dockerfile 是提交的构建入口，`agent/conversation.json` 为学习记录。
