# FTS 优化最终候选（2026-10-31）

## 结论

本分支在公开参考实现 `juanzhi-gcb/seekdb` 的 `fts-tuning` 分支
（`ee1e48aa8`，前序提交的平台记录为 47.647/50）基础上，补充了
并行准入下限和 v4 的构建排序优化，代码提交为：

- 基线参考：`ee1e48aa8`（本分支父提交）
- 自有/移植增量：`41450ffe6`（并行准入下限）、`cc0503e24`（Datum 字节判等短路 + 排序 dump 边界预扩展）
- 本报告提交为该分支最新 commit

本分支只做全文索引相关修改，基准数据、SQL、计时、评分脚本和固定基线均未修改。

## 增量内容

1. `src/share/ob_cpu_share_calculator.h`：将自动推导的
   `parallel_servers_target` 下限从 3 提升到 16。全文索引 DDL 的默认构建
   并行度是 `min(16, server_cpu_count / 2)`，2–4 核机器上的自动准入上限可能
   低于构建请求的 worker 数并返回 5307；该下限是准入上限而不是线程预分配。
2. `src/share/datum/ob_datum_cmp_func_def.h`：等长且字节完全相同的字符串
   在排序比较中直接判定相等，跳过 collation 解码；全文构建的 doc-word 排序
   存在大量重复词。
3. `src/sql/engine/sort/ob_sort_op_impl.h`：`check_need_dump()` 先按下一次
   add 的路径扩展自动内存边界，只有 dump 真正不可避免时才切换策略。
   全文辅助表构建已由参考实现使用 `enable_newsort` 编码排序。

## 本机 A/B 结果

同一台 224 核机器、同一空闲基线实例，运行仓库自带的
`tools/benchmark/fts_large_bench.sh`（ROWS=20000、BATCH=500、
ROUNDS=3000、QUERY_ROUNDS=200、SAMPLES=3、WARMUP=30、SKIP_LOAD=0）。
基线为本机 `seekdb_base`（父提交 `52a1e708e` 构建），候选为本分支构建；
两边各自重建数据库和三个索引，命中数均为 8001 / 11000 / 7332 / 20。

最终候选三次运行相对基线的耗时下降：

| 类别 | 指标 | 运行1 | 运行2 | 运行3 |
| --- | --- | ---: | ---: | ---: |
| build | build_ik_all_sec | 44.5% | 44.5% | 57.0% |
| build | build_ik_content_sec | 48.4% | 48.4% | 56.1% |
| build | build_beng_en_sec | 47.1% | 47.1% | 47.0% |
| tokenize | tokenize_ik_avg_ms | 54.3% | 54.4% | 47.0% |
| tokenize | tokenize_beng_avg_ms | 26.4% | 19.9% | 13.8% |
| query | query_cn_avg_ms | 70.4% | 70.0% | 69.2% |
| query | query_beng_avg_ms | 69.3% | 69.3% | 69.3% |
| query | query_mixed_avg_ms | 69.9% | 69.1% | 70.2% |
| query | query_limit_avg_ms | 95.0% | 94.6% | 94.7% |

按赛题公式（每类先平均，三类等权）三次运行的综合提升分别约为
**54.4% / 53.2% / 53.2%**；均明显超过 50% 的满分线。基准与候选在同一
时刻交替运行，单次构建/分词存在机器波动，因此保留全部原始报告在远程
工作区日志中。本机未优化基线远快于仓库固定 CI 基线，固定基线脚本分数
不代表平台正式分数。

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
及其祖先提交），保留原始版权头；构建排序的两处增量移植自团队
`solution/fts-performance-plus`（`7046ee6ce`）。未导入与本题无关的
AI/FileSQL 特性。

## 提测字段（供人工提测）

- 仓库：`https://github.com/xxxhm0524/seekdb`
- 分支：`solution/fts-final`
- Commit：分支最新 HEAD（见推送结果；代码提交为 `cc0503e24`）
