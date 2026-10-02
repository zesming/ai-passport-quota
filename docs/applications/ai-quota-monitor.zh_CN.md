<p align="right"><strong>简体中文</strong> · <a href="ai-quota-monitor.md">English</a></p>

# AI 订阅额度看板

应用把演示菜单替换为 AI Passport 240 × 320 竖屏额度看板。电脑端伴侣管理最多八个独立的 Codex、Claude 订阅账户；设备只接收邮箱、订阅名称、额度窗口、设置及采集时间。

## 电脑端使用

仓库内 `companion/` 目录包含 Node.js 服务和 React 设置页。使用 Node.js 22 或以上，在该目录依次运行 `npm install`、`npm run build`、`npm start`，打开 `http://127.0.0.1:4317/`。设备同步时需保持电脑和服务运行。连接账户与配对前安装官方客户端和 OpenSSL。

在账户管理中添加账户，完成官方授权，等待显示已验证邮箱。每个账户使用 `~/.local/share/ai-passport-quota/profiles/<id>` 下的独立配置，不导入已有 Codex/Claude 登录。可通过 `AIQ_CODEX_BIN`、`AIQ_CLAUDE_BIN` 指定官方客户端，通过 `AIQ_STATE_DIR` 更换本地存储位置。文件权限仅开放给当前系统用户。移除账户会立即从应用中解除关联并尝试官方退出登录，保留配置文件。

Codex 使用官方 app-server 的设备验证码登录及 `account/rateLimits/read`。展示的是 Codex 使用窗口，不代表 ChatGPT 普通聊天的全部限制。窗口按时长匹配；缺失的 5 小时或周额度显示未知。

Claude 使用官方订阅登录和状态栏回调。授权后，在网页复制会话启动命令，正常使用该独立账户；首次正常模型回复后可能出现额度。启动助手清除环境中其他服务的认证信息，并将回调绑定到已验证账户。看板不会为获取额度发送模型请求。重复回调、定时刷新不会伪造新的采集时间。

## USB 配对

使用桌面版 Chrome 或 Edge，用支持数据传输的 USB 线连接设备，并在小屏打开配对页。电脑「设备配置」选择本机私有 IPv4 地址，输入 Wi-Fi 信息，点击连接并配置，选择串口，等待结果。Wi-Fi 信息仅从浏览器内存直接发送到 USB，不经过电脑 API。设备仅在物理打开的 120 秒配对窗口内接受配置；已配置设备启动时关闭该窗口。

设备同步地址为 `https://<所选私有IP>:4318`。配对生成随机访问令牌和包含 IP 主体备用名称的 ECDSA 证书，固件验证证书及地址。设置网页仅监听本机回环地址；局域网服务在用户配对时启用，或恢复此前已授权的配置。电脑 IP 改变后需重新配对；停止同步会撤销旧令牌。

## 显示与保存

主页显示账户 Logo、已验证邮箱、5 小时和 7 天剩余百分比。短按 OK 请求刷新，上下键切换账户，长按 OK 进入设置或返回。设置支持账户选择、刷新间隔、自动刷新及配对。网络和存储操作在工作任务执行，不阻塞按钮回调或 LVGL 锁。

两分钟无按键操作后，背光调暗至 15%；任意按键恢复亮度。配对窗口开启时保持亮屏，调暗后继续同步额度。亮度变化、Wi-Fi/TLS 工作时的可用内存与最大连续块、耗电均需真机验证。

未知数据显示横线。重置时间已到时等待数据源提供新窗口，不推算恢复到 100%。旧数据、离线和登录过期均有状态提示。服务端设置为准，设备收到确认后保存。设备在 NVS 保存配对信息及脱敏额度缓存；快照写入间隔至少十五分钟。

## 验证

电脑端运行 `npm test` 和 `npm run build`。固件交付前必须激活 ESP-IDF 5.5.3 并通过完整 `./tools/validate.sh`。本工作区在 macOS 使用临时编译器包装脚本适配 Darwin 链接器及校验过的 actionlint，固件仍使用仓库默认配置。真机需检查中文字形、USB 配对、Wi-Fi 重连、错误证书拒绝、按钮、窗口重置和重启保存；编译通过不代表这些检查通过。

## 协议

设备请求携带 `Authorization: Bearer <配对令牌>`，不跟随重定向。`GET /v1/snapshot` 返回版本 1、服务时间、修订号、设置及已授权账户。`POST /v1/refresh` 合并重复刷新请求。`PATCH /v1/settings` 接受 60、300、900、1800 秒间隔及布尔自动刷新开关。快照最多 8192 字节，最多八个账户。

USB 帧以 `@AIQ:` 开头、换行结尾，最多 4096 字节。版本 1 的 `configure` 帧包含八位十六进制请求编号、Wi-Fi 字段、私有 HTTPS 地址、令牌、证书和初始可信时间。`result` 确认匹配请求编号，不回显凭证。

数据源约定：[Codex 认证](https://learn.chatgpt.com/codex/auth)、[Codex app-server](https://learn.chatgpt.com/codex/app-server)、[Claude 状态栏](https://code.claude.com/docs/en/statusline)、[Claude 认证](https://code.claude.com/docs/en/authentication)。
