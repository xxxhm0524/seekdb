# 缓存内存分配失败排查

## 已复现问题

上一候选 `8514212bbaf3966eef030c90a2ff6505a52be0b0` 的缓存命中路径先删除 LRU 节点，再分配新节点。后一次分配抛出 `bad_alloc` 时，页映射仍保留被删除节点的迭代器。随后正常 SELECT 再次访问该页，发生非法访问。

Windows 原生探针先提交一行，在显式事务中插入两行既有修改，对 UPDATE 的第 21 个普通分配位置注入一次失败。UPDATE 返回失败后，SELECT 在 `PageCache::touch` 的 `std::list::erase` 崩溃。GDB 栈确认路径为 `snapshot → execute → exec_select → for_each_visible → PageCache::get → touch`。不是通过篡改磁盘或模拟断电触发。

另一个异常安全缺口是缓存插入：LRU 节点已经入链后，映射节点或桶分配失败会留下无映射的链表节点。

这些路径在旧正式提交 `6da81bf4` 中也存在，但正式失败 SQL 未公开，不能据此认定它就是平台失败根因。

## 修改

- 缓存命中使用 `list::splice` 移动既有节点，保持迭代器有效且不分配内存。
- 将三条缓存插入路径统一为内部 `insert`；映射插入失败时回退刚加入的 LRU 节点。
- 不改变 WAL、文件格式、SQL 结果或索引算法。

## 回归入口

`python tests/transaction_allocation_failure.py --output protocol-results/transaction-allocation-failure.json`

覆盖 480 个 DML 分配位置，验证失败语句之前的事务工作、失败语句回退、后续提交和正常重启。另覆盖 80 个缓存分配位置，验证命中无分配、插入失败后的再次插入、淘汰和重读。脚本支持 Linux ASAN/UBSAN；实际结果保存为 JSON，不能将工作流中尚未执行的步骤视为通过。

本地初次修复后的矩阵结果：DML 实际注入 77 次、缓存实际注入 2 次，560 个位置零违规。完整原生回归及 Linux CI 结果以对应运行产物为准。

## 限制

这是单次分配失败模型，不代表持续 OOM 时仍能保证无限进展。它不能代替正式隐藏用例，也不产生平台分数。未发起新的 OceanBase 提测。
