简体中文 · [English](README.md)

# AI Passport Quota

FoloToy AI Passport 的额度看板：ESP32-C3、240 × 320 屏幕、8 MB Flash、无 PSRAM。展示 Codex/Claude 订阅额度、DeepSeek 人民币余额、重置倒计时，以及来源提供的可用重置和剩余额度。缺失的额度窗口隐藏，真实 0% 保留。

## 选择联网方式

| 方式 | 账户 | 需要保持运行 |
| --- | --- | --- |
| 设备直连 | 实验性 Codex 授权、DeepSeek API 余额 | 普通 2.4 GHz Wi-Fi 或手机热点 |
| 电脑同步 | 本地服务采集 Codex、Claude、DeepSeek | 同一网络内手动启动的电脑服务 |

直连方式由设备保存账户和设置，最多八个账户、三个网络。电脑账户与直连账户分开保存，切换方式保留双方配置。已配对设备初始保留电脑同步；在手机设置切换为**设备直连**，重新添加账户。不会导入已有电脑凭据。

Codex 直连采用官方客户端的设备码流程和当前公开客户端 ID，属于实验性兼容实现，并非本项目已注册的 OAuth 集成或稳定公开额度 API。账户、工作区限制和服务变更可能阻止授权或查询。展示的是 **Codex 用量**，不代表全部 ChatGPT 消息限制。Claude 订阅登录仍通过电脑同步；设备直连不提供 Claude 登录。

## 手机配置

先按[构建与刷机指南](docs/development/README.zh_CN.md)安装固件。

1. 设备长按 OK，进入**网络与配网 → 手机配网**。临时密码热点开放十分钟。
2. 扫描第一步二维码连接设备热点，也可手动输入屏幕显示的网络名称和密码。手机提示“无互联网”时仍保持连接。
3. 短按 OK，扫描第二步二维码打开本地设置页。二维码包含临时设置授权；仅手动输入裸地址不能获得修改权限。
4. 选择**设备直连**，填写 Wi-Fi 或 2.4 GHz 手机热点，再添加 Codex 或 DeepSeek API 密钥。网络和密钥提交后处于待验证状态；结束设置后设备联网验证。
5. Codex 开始授权时会关闭设备热点。手机恢复有网连接或开启已配置的个人热点，再扫描**设备屏幕**上的官方授权二维码、输入验证码。设备完成授权并保存自己的令牌，授权最长十五分钟。

网页由设备本地提供，不需要云端账户服务，也不依赖网页蓝牙。实际扫码和切网行为仍取决于手机。企业证书网络与蓝牙网络中转尚未实现；普通 Wi-Fi 不可用时使用兼容的手机热点。完整交互与后续网络方案见[便携设计](docs/development/portable-connectivity.zh_CN.md)。

DeepSeek 只显示人民币总可用余额，包含赠送和充值部分。别名不是认证邮箱。余额 API 不提供消费历史、请求次数或累计 Tokens。更换密钥时先验证候选密钥，成功后替换原密钥和余额。

设备凭据位于独立 NVS 分区，不进入 Git。本工程固件**未加密 Flash 中的凭据**，不抵御物理读取。设置 API 不返回密钥、令牌或账户密码；账户密码仅输入官方登录页。

## 设备使用

上/下键切换账户；短按 OK 刷新或确认；长按 OK 打开设置或返回。长按下键息屏，首次功能键操作只亮屏。独立电源键保持硬件长按关机。

自动刷新支持 1/5/15/30 分钟；自动息屏支持从不、30 秒、1/2/5/10 分钟。息屏停止 Wi-Fi、手机设置和新网络请求；已发出的请求可能完成，收到的新令牌仍保存。授权在息屏时暂停，有效期继续计时。亮屏先显示缓存，再恢复联网。设备直连仅在手动请求或启用的刷新周期到点时查询来源；电脑同步还会静默读取电脑缓存。亮屏不会推迟来源刷新。

状态栏展示 Wi-Fi、校时后的 UTC+8 时间和按实际比例填充的绿色电池。绿色仅为样式，不表示已验证的充电状态。续航和息屏电流尚未测量。

## 可选电脑服务

需要 Node.js 22+、npm、OpenSSL；Codex/Claude 需要官方客户端。USB 配对使用支持 Web Serial 的桌面 Chrome/Edge。

```bash
git clone https://github.com/zesming/ai-passport-quota.git
cd ai-passport-quota/companion
npm ci
npm run build
npm start
```

打开 **http://127.0.0.1:4317/** 添加独立账户。Claude 使用网页给出的会话命令，正常使用后的官方 statusline 回调提供额度；刷新不会发送付费模型提示。

设备进入**电脑配对**，USB 窗口开放 120 秒。在设备配置页填写电脑内网 IPv4 和 Wi-Fi，连接 USB 并等待确认。设备通过 **4318** 端口的固定证书 HTTPS 同步；电脑地址变化时重新配对。服务终端需保持打开，不安装自启动服务。账户资料位于 `~/.local/share/ai-passport-quota/` 或 `AIQ_STATE_DIR`。

## 截图与开发

截图使用隔离的示例账户；网页预览不证明硬件实时状态。

![手机设置页](docs/screenshots/phone-setup.png)
![ChatGPT Pro 额度](docs/screenshots/quota-pro.jpg)
![Claude 额度](docs/screenshots/claude.jpg)
![DeepSeek 余额](docs/screenshots/deepseek.jpg)

继续开发先读 [AGENTS](AGENTS.zh_CN.md)、[开发指南](docs/development/README.zh_CN.md)和[应用契约](docs/applications/ai-quota-monitor.zh_CN.md)。验证与真机验收记录放在[变更日志](docs/CHANGELOG.zh_CN.md)。

基于 MIT 许可的 [FoloToy AI Passport](https://gitee.com/FoloToy/ai-passport)，提交 `0b9e4c81ee4421c0bac39ca3561d65a8285acd4a`。见 [LICENSE](LICENSE)和[素材许可](assets/README.zh_CN.md)。
