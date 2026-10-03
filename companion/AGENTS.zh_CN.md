简体中文 · [English](AGENTS.md)

# 电脑端约束

阅读根[仓库说明](../AGENTS.zh_CN.md)与[应用契约](../docs/applications/ai-quota-monitor.zh_CN.md)。

- 设置 API 只监听本机，私有 profile 保持隔离，秘密不得进入公开状态、快照、USB 响应或日志。不导入已有服务凭证。
- 使用官方 Codex app-server、Claude 授权/statusline 和 DeepSeek 余额来源。保留来源时间、未知窗口、十进制字符串和独立币种；当前视图只选择 CNY。DeepSeek 标签不代表验证邮箱，恢复方式为余额重试或更换密钥。
- Wi-Fi 输入不得到达 API。保留 USB request ID 确认匹配和有界的传输失败处理。
- 设备预览为 240 × 320，额度轨道为 216 × 8。百分比文本和填充条分别应用低额/临界样式，数字容器保持透明。正式预览不伪造板卡遥测。
- 公开截图使用合成账号和临时状态（`npm run preview:readme`）。行为变更运行 `npm test` 和 `npm run build`，UI 变化在浏览器检查。主机/浏览器结果与真实账号、设备验收分开报告。
