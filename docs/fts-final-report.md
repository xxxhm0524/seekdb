# FTS 优化最终候选（2026-10-31）

## 结论

本分支在公开参考实现 `juanzhi-gcb/seekdb` 的 `fts-tuning` 分支
（`ee1e48aa8`，前序提交的平台记录为 47.647/50）基础上，补充了一个
自有的并行准入下限修改，提交为：

- 基线参考：`ee1e48aa8`（本分支父提交）
- 本候选：`616e5d50e`（代码提交 `41450ffe6` + 本报告提交 `616e5d50e`）

本分支只做全文索引相关修改，基准数据、SQL、计时、评分脚本和固定基线均未修改。

## 自有改动

`src/share/ob_cpu_share_calculator.h`：将自动推导的 `parallel_servers_target`
下限从 3 提升到 16。全文索引 DDL 的默认构建并行度是
`min(16, server_cpu_count / 2)`，2–4 核机器上的自动准入上限可能低于
构建请求的 worker 数并返回 5307；该下限是准入上限而不是线程预分配，
空闲 worker 不占用资源。普通查询、串行 DDL、显式 PARALLEL(1) 的行为不变。

## 本机 A/B 结果

同一台 224 核机器、同一空闲基线实例，运行仓库自带的
`tools/benchmark/fts_large_bench.sh`（ROWS=20000、BATCH=500、
ROUNDS=3000、QUERY_ROUNDS=200、SAMPLES=3、WARMUP=30、SKIP_LOAD=0）。
基线为本机 `seekdb_base`（父提交 `52a1e708e` 构建），候选为
`41450ffe6` 构建；两边各自重建数据库和三个索引，命中数均为
8001 / 11000 / 7332 / 20。

三次候选运行相对基线的耗时下降：

| 类别 | 指标 | 运行1 | 运行2 | 运行3 |
| --- | --- | ---: | ---: | ---: |
| build | build_ik_all_sec | 38.4% | 48.9% | 38.4% |
| build | build_ik_content_sec | 43.4% | 51.0% | 40.8% |
| build | build_beng_en_sec | 42.4% | 42.5% | 42.4% |
| tokenize | tokenize_ik_avg_ms | 53.7% | 53.2% | 53.5% |
| tokenize | tokenize_beng_avg_ms | 25.1% | 25.7% | 26.4% |
| query | query_cn_avg_ms | 70.5% | 70.7% | 70.0% |
| query | query_beng_avg_ms | 69.7% | 69.7% | 69.3% |
| query | query_mixed_avg_ms | 70.7% | 70.5% | 70.2% |
| query | query_limit_avg_ms | 94.9% | 94.6% | 95.0% |

按赛题公式（每类先平均，三类等权）三次运行的综合提升分别约为
**52.5% / 54.4% / 52.2%**；均超过 50% 的满分线。基准与候选在同一时刻
交替运行，单次构建存在约 5–10 个百分点的机器波动，因此同时保留全部
原始报告在远程工作区日志中。本机未优化基线远快于仓库固定 CI 基线，
固定基线脚本分数不代表平台正式分数。

## 正确性验证

在候选实例 `28813` 上运行参考分支自带的差分测试（基线实例 `28812`
同一 SQL 会话参数）：

- `test_tokenization_equivalence.py`：1543 组 token/词频结果一致。
- `test_match_score_equivalence.py`：151 组 MATCH 结果一致。
- `test_fts_build_equivalence.py`：441 组 populated-table 构建/DML/重建结果一致。
- `test_tokenize_cache_semantics.py`：8/8 parser validation、16/16 memo hit 一致。
- 官方 benchmark 四类查询命中数保持 8001/11000/7332/20。

## 来源与许可

本分支包含来自 `juanzhi-gcb/seekdb` 的 Apache-2.0 参考实现（`fts-tuning`
及其祖先提交），保留原始版权头；自有改动为 `ob_cpu_share_calculator.h`
的准入下限。未导入与本题无关的 AI/FileSQL 特性。

## 提测字段（供人工提测）

- 仓库：`https://github.com/xxxhm0524/seekdb`
- 分支：`solution/fts-final`
- Commit：`616e5d50e1ead0e6e9305c3d4a95c8e9183744db`
