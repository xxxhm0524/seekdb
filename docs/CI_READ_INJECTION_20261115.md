# 新版 libc 下的读取故障注入兼容

4da36e6 的完整 GitHub CI 43720343049 编译成功，70项中67项通过，core_unindexed_snapshot、core_typed_scan、core_cache_admission 失败。原日志保留，没有将该完整 CI 说成成功。

先在同一 Ubuntu 24.04 / GCC 13.3 环境编译未经运行逻辑修改的 a08 参考及候选。两版三项均失败，诊断为 caught=0、failed_reads=0；库符号实际引用 __pread_chk，而旧测试仅 --wrap=pread。这表明测试没有向这些受保护读取注入错误，不是已观测到的候选数据库语义回归。

诊断提交只增加测试适配：合法长度的 __pread_chk 转到各测试原有的 __wrap_pread；越界长度仍调用真实 libc 检查入口。原断言保留，不关闭安全检查，不移除测试。两版随后均3/3通过，且各自默认数据库程序的哈希与修复前相同。对应诊断运行分别为43721883246、43722443343，原始结果独立保存。

主线将同一适配用于五个使用 pread 包装的测试目标：unindexed_snapshot、typed_scan、text_scan、certified_block、cache_admission。兼容函数仅链接测试程序；生产源码、生产目标编译选项、存储格式和提交顺序不变。musl 不编译 glibc 专有入口。

新快照728c945bbca3ee9f34f9cb947ac8d56944cf67c9 在 A6000 从源重新构建，70/70通过；默认程序仍为 SHA-256 6fd307974aafc6cd51fb33469fdaa4a882cd6e8bacea6ad55a59feae6b9e8ade。旧性能/边界报告保持原核心 c2 的归属，另用二进制完全相同的报告关联新测试快照，不倒填历史测试身份。

最终提交仍需其完整 GitHub CI。诊断任务收集成功、三项修复成功和某次本地70/70不能单独当作最终完整 CI 或正式评测通过。

51974955 的完整 CI 43723851914 已70/70通过，随后公开测试全部在 mount 阶段退出，日志为“/data: mount point does not exist”；没有运行到数据库。后续 CI 仅在新建的临时 GitHub runner 上补建空挂载点，已有 /data 不修改权限或内容；通用原生验证器增加前置检查，不在用户服务器上自动创建或改动数据目录。该次校准失败记录保留，未解释为 SQL 失败。
