# logs/ — AI Coding 日志目录

本目录存放 D13x 移植过程中与 AI 工具的真实对话日志，并与作品代码一并提交。

`lladlam/` 中包含 MiMo Code（按组委会确认作为 OpenCode 记录）和 Codex
CLI 日志。历史补录只按完整会话排除空白会话、自动标题请求、归档压缩和
账号解锁等无关会话；已选开发会话内部不删改事件。

## 目录结构

```text
logs/
└── <github_login>/              # 你的 GitHub 用户名，一人一目录
    ├── manifest.json            # 会话清单
    └── <date>/                  # 日期 YYYY-MM-DD
        └── <tool>__<sid>.jsonl  # 一个会话一个文件（工具名与 session id 用 __ 连接）
```

- `<tool>`：`claude-code` / `opencode` / `codex` / `kiro`
- 每个 `.jsonl` 每行一个事件，符合组委会事件 schema；`manifest.json`
  记录会话来源、事件数量、完整性状态和脱敏计数。

导出与提交的完整步骤、字段定义见[《AI Coding 日志归集与提交手册》](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/ai_coding_log_guide.md)。
