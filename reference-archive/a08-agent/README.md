# MiniVectorDB 开发与学习材料

入口 `agent/conversation.json` 与根目录镜像字节相同，UTF-8 JSON 对象，100,184 字节，低于 128 KiB。包含 70 条用户记录、41 条实验、16 条 Agent 观察，以及当前代码模块映射。基础记录、当前结果、原文引用及限制均内嵌在 JSON，外部文档只便于人工复现。

当前运行源码固定为 `a24d2cd764027adec7371f268f99ccdca2307032`；材料只选 a24 形成之前的 `u01-u70` / `e01-e126` / `a01-a50`，a24 之后的 J11/centered 与 `u71+` 实验不作为当前源码证据。最新用户反馈：a24 正式 MySQL 通过，E=3615.639045，Prompt 仍为合并无效提示；历史 a24 反馈为 E=3977.995012。新增解释明确归 Agent，不改写为用户理解，不自填分数或模拟三轮评审。

`material-digest.json` 为本地完整性清单；`scripts/check-submission-contract.py --check-internal-references` 检查包装与引文，`scripts/check-learning-material.py` 核验历史记录、归档、摘要和来源（`selection_scope=a24-subset` 时按精确前缀校验）。旧 `build-prompt-rubric.py` 只构建早期版本，不用于覆盖当前材料。完整说明见 `docs/submission-evidence-explanation.txt`。
