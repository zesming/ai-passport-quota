简体中文 · [English](README.md)

# AI Passport 本地额度伴侣

与 AI Passport 额度看板固件配套的本地账户和设置应用。支持独立登录 Codex、Claude 订阅账户，展示数据源提供的 5 小时和周剩余额度，调整刷新间隔，并通过 USB 配置设备。不使用演示账户或模拟额度。

需要 Node.js 22 或以上，依次执行 `npm install`、`npm run build`、`npm start`，打开 http://127.0.0.1:4317/ 。服务依赖官方 Codex、Claude 客户端并自动寻找其安装路径；可通过 `AIQ_CODEX_BIN`、`AIQ_CLAUDE_BIN` 手动指定。浏览器设备配对需使用支持 Web Serial 的桌面版 Chrome 或 Edge。

添加账户后完成官方授权。Codex 展示 Codex 使用窗口，不代表 ChatGPT 的全部消息限制。Claude 需复制账户会话启动命令并正常使用，状态栏在正常模型响应后提供额度。自动刷新不会发送付费模型请求，也不会把旧数据伪装成新数据。

账户配置与认证信息使用私有权限保存在 `~/.local/share/ai-passport-quota`。Wi-Fi 信息从浏览器内存直接发往 USB。设置网页仅监听本机回环地址。设备 HTTPS 同步在配对后监听所选私有地址，或恢复此前已授权配置。同步时需保持电脑和服务运行。停止同步撤销旧令牌，恢复需重新配对。

`npm test` 验证额度转换、账户隔离及生命周期、旧数据时间、请求校验及证书校验；`npm run build` 编译网页。真实账户授权、浏览器交互和硬件仍需用户验证；自动测试使用模拟数据源及临时目录。

完整双语使用与协议文档：[AI 订阅额度看板](../docs/applications/ai-quota-monitor.zh_CN.md)。

USB 打开后立即接收设备消息，短暂等待启动，并按请求编号确认配置结果。失败提示会区分未收到 USB 信息、通信中断、回复编号不匹配，以及设备明确拒绝。应用更新后刷新网页，重新输入 Wi-Fi 信息并打开小屏配对窗口，再重试。串口选择仅显示 ESP32-C3 原生 USB Serial/JTAG 设备；其他串口工具或配对网页需要先释放设备。

Mac 可双击 `start-dashboard.command` 再次启动已配置的应用。同步时保持打开的运行窗口，网页地址不变；启动文件不包含账户凭证。
