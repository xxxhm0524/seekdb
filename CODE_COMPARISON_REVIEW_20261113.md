公开代码对照与独立反例

本轮实际读取了两份公开实现的相关代码，未把它们视为正式参考执行器，也未复制其实现：

- AtoFuture/seekdb-round2-review，提交 fc1fef56b37036db40d216f591a6f0b1a46e90ac：SQL binder/parser、JSONL、距离核及 SQL/数值/向量字面量测试。
  https://github.com/AtoFuture/seekdb-round2-review/blob/fc1fef56b37036db40d216f591a6f0b1a46e90ac/tests/sql_tests.cpp
- wzhaihqy/seekdb，加赛分支：core.py、sqlparser.py、engine.py、store.py、server.py、fastpath.py。
  https://github.com/wzhaihqy/seekdb/tree/2026-competition-round2/minivectordb

实际对照了条件字面量绑定、Unicode 字符数、向量 float32 转换、距离及并列排序、事务暂存/提交/撤销，以及词法错误处理。代码差异不自动等于本程序缺陷：例如不同类型之间是否允许转换，不能仅凭另一份实现的选择修改语义。

新发现：本程序把未闭合的块注释默默视为输入结束。

    CREATE TABLE t(id BIGINT PRIMARY KEY);
    INSERT INTO t VALUES(1) /* unfinished
    SELECT id FROM t ORDER BY id;

1696c7f7 的第二条请求返回 ok:true、affected_rows:1，第三条返回 [[1]]。这不是仅看代码推测；已通过真实 JSONL 复现。公开实现的 SQL 测试包含对未闭合注释的拒绝检查，这提供了审查线索。

修复：词法器搜索不到 */ 时抛出 syntax_error，在解析与语句执行前拒绝完整请求。保留闭合块注释、行注释及引号内注释字符的行为。没有修改持久化布局、WAL、索引或距离计算。

新增 tests/sql_comment_boundaries.py：旧版前两条请求即失败；修复后 241 次请求通过，114 条未闭合注释均被拒绝，覆盖 INSERT/UPDATE/DELETE/SELECT、DDL、COMMIT/ROLLBACK、先前事务修改、合法及引号内注释、强杀重启。完整原生回归和独立 Linux/Docker 流水线分别保存实际结果，不能把本文件当作未运行检查的通过证明。

原候选 1696c7f7 与其归档保留。本修复在独立分支 fix/reject-unclosed-comments-20261113；未发起平台正式提测。这个反例证明旧版存在语法错误处理缺陷，尚不能证明它就是正式任务 47251869 中未公开请求的失败原因。
