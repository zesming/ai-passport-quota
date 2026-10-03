简体中文 · [English](README.md)

# AI Passport AI 额度看板

面向 FoloToy AI Passport（ESP32-C3、240 × 320 小屏）的 AI 订阅额度看板，配套本地电脑网页管理账户和设备设置。支持最多八个独立的 ChatGPT/Codex、Claude 订阅账户和 DeepSeek API 账户，展示 Logo、订阅额度或 API 余额，支持刷新间隔与离线缓存。

小屏状态栏显示 Wi-Fi 连接图标、无数字百分比的绿色电池进度，以及校时后的当前时间。深色屏幕使用黑白 OpenAI Logo。支持设置息屏时间和功能键唤醒。息屏后暂停设备轮询与刷新请求，亮屏后立即静默读取电脑端最新缓存，并保留下一次账户刷新时间。手动刷新或已启用的自动刷新到点时才显示刷新提示。电脑端保留独立的定时刷新。电池填充始终反映实测电量，读取不可用时显示斜线。绿色是电池图标的配色，充电状态请看绿色指示灯。

## 页面截图

以下为运行中的电脑端应用截图，使用隔离的**文档示例账户**。邮箱、额度、余额及设备状态图标都是示例数据，不含个人账户信息。左侧是网页设备预览，不是真机照片。预览时钟使用电脑本地时间；电脑端目前不接收真机 Wi-Fi/电池状态。

![账户管理与设备预览](docs/screenshots/dashboard.jpg)

![刷新设置](docs/screenshots/settings.jpg)

![DeepSeek 余额与设备预览](docs/screenshots/deepseek.jpg)

![USB 设备配置](docs/screenshots/device-setup.jpg)

## 启动电脑端

需要 Node.js 22+、npm、OpenSSL，以及要连接平台的官方 Codex/Claude CLI。USB 配置使用桌面版 Chrome 或 Edge 和数据 USB 线。固件使用 ESP-IDF **5.5.3**，目标 ESP32-C3、8 MB Flash、无 PSRAM。

```bash
git clone https://github.com/zesming/ai-passport-quota.git
cd ai-passport-quota/companion
npm ci
npm run build
npm start
```

打开 **http://127.0.0.1:4317/**。Mac 可双击 `companion/start-dashboard.command` 启动已编译版本。同步期间保持电脑与服务运行。应用自动寻找官方客户端；可通过 `AIQ_CODEX_BIN`、`AIQ_CLAUDE_BIN` 指定位置，`AIQ_STATE_DIR` 指定本地数据目录。

1. 在账户管理中添加 Codex 或 Claude，在官方页面完成授权。每个账户使用独立档案，不导入已有 CLI 登录信息。
2. Codex 通过官方 app-server 采集额度。它展示 **Codex 使用窗口**，不代表普通 ChatGPT 的全部消息限制；数据源缺少的窗口显示未知。
3. Claude 登录后复制页面提供的账户会话启动命令，正常使用该独立账户。正常模型回复后的状态栏回调提供额度；看板刷新不会发送付费模型请求。
4. 添加 DeepSeek 时填写[官方平台](https://platform.deepseek.com/api_keys)生成的 API Key。名称是本地备注，余额接口不提供已验证邮箱。密钥只保存在当前用户可访问的本地档案，不发送到设备。页面只显示[官方余额接口](https://api-docs.deepseek.com/api/get-user-balance/)返回的一项人民币可用余额（`total_balance`，包含赠送和充值部分），以及可用状态和采集时间。不展开赠送/充值明细，也不展示美元；缺失人民币数据时保持未知。不会发送模型请求。
5. 刷新设置可选 1、5、15、30 分钟；自动息屏可选 30 秒、1/2/5/10 分钟或“永不”，默认 2 分钟。

DeepSeek 当前只展示可用余额，不支持账户累计/区间消费、请求次数及历史 Tokens 总量。[模型响应](https://api-docs.deepseek.com/api/create-chat-completion/)中的用量只对应单次请求，不是其他客户端产生的官网近 30 天统计。

## 连接小屏

先刷入经过验证的固件，再连接 USB、打开小屏物理配对窗口。已配置设备长按 OK 进入设置，用上下键选择配对并按 OK。窗口持续 120 秒。

在网页设备配置中选择电脑私有 IPv4 地址，填写 **2.4 GHz Wi-Fi** 信息，点击连接并配置，选择 ESP32-C3 USB Serial/JTAG 设备并等待确认。Wi-Fi 信息直接从浏览器内存发送到 USB。配对后小屏连接所选电脑 **4318** 端口的固定证书 HTTPS 服务；网页 **4317** 端口只供本机使用。电脑 IP 改变后重新配对；停止同步会撤销令牌。

小屏操作：上下键切换账户，短按 OK 刷新或确认，长按 OK 打开设置或返回。按设置的时间自动息屏；息屏后的第一次功能键操作只亮屏；亮屏时长按 DOWN 息屏。独立电源键保留官方长按关机方式。配对窗口内不自动息屏。息屏暂停设备快照轮询、来源刷新、设置 HTTP 请求和主动 Wi-Fi 重连；唤醒后立即静默读取一次缓存，即使关闭自动刷新也执行；已有的限时请求先结束，断网时等连接恢复。唤醒不会单独触发账户刷新或推迟其时间；已启用的自动刷新若在息屏期间到点，则在唤醒读取缓存后执行。账户缓存与配对信息保留。

USB 配置失败时刷新网页，重新打开小屏窗口并填写 Wi-Fi 信息，关闭其他串口工具或配对网页。页面会区分未收到信息、USB 中断、确认编号不匹配和设备明确拒绝。

## 编译与验证

```bash
# 电脑端：从仓库根目录开始
cd companion
npm ci
npm test
npm run build
cd ..

# 固件：先激活本机 ESP-IDF 5.5.3
source <esp-idf-v5.5.3>/export.sh
python3 tools/install_passport_skills.py --install
./tools/validate.sh --static
./tools/validate.sh --firmware
# 完整交付检查是 ./tools/validate.sh
```

固件检查生成 `build/FoloToy-AI-Passport-full.bin` 和 `build/firmware/<sha256>/` 下匹配的调试归档。用 `python3 tools/archive_firmware.py verify <归档目录>` 核验。刷写需要明确同意具体设备、产物及数据影响。保留设置时，先确认分区兼容，再按记录的偏移写入核验后的引导程序、分区表和应用。完整镜像从 `0x0` 写入也会覆盖 NVS 间隙，可能重置设置。编译产物不提交 Git。详见[环境配置](docs/development/engineering/environment-setup.zh_CN.md)与[编译及刷写说明](docs/development/engineering/build-and-test.zh_CN.md)。

开发网页时保持电脑 API 运行，再开另一个终端：

```bash
cd companion
AIQ_DEV_ORIGIN=http://127.0.0.1:5173 npm start
# 另一个终端，同一目录
npm run dev
```

开发时打开 http://127.0.0.1:5173/ ，与上面允许的来源地址保持一致。

重新生成文档截图可用 `cd companion && npm run preview:readme`，打开 http://127.0.0.1:4327/ 。该示例服务使用临时设置和示例账户，与正式账户及服务隔离。

## 开发入口

开发时先读 [AGENTS.md](AGENTS.md) 和[应用及协议说明](docs/applications/ai-quota-monitor.zh_CN.md)。按日期记录的变更、验证及真机验收见[更新记录](docs/CHANGELOG.zh_CN.md)。

| 范围 | 文件 |
| --- | --- |
| 固件界面、状态和输入 | `main/main.c`、`main/quota_ui.c`、`main/quota_logic.c` |
| Wi-Fi、固定证书 HTTPS、USB、NVS | `main/quota_service.c` |
| 显示及硬件驱动 | `components/bsp/` |
| React 网页与 USB 通信 | `companion/src/App.jsx`、`companion/src/serial.mjs`、`companion/src/styles.css` |
| 官方登录与额度采集 | `companion/server/accounts.mjs`、`clients.mjs`、`claude-feed.mjs`、`claude-session.mjs`、`deepseek.mjs` |
| 本机 API、配对与存储 | `companion/server/index.mjs`、`pairing.mjs`、`protocol.mjs`、`storage.mjs` |
| 主机测试 | `tests/test_quota_logic.c`、`tests/test_quota_fonts.py`、`tests/test_quota_http_runtime.py`、`tests/test_quota_refresh_runtime.py`、`tests/test_quota_storage_runtime.py`、`companion/test/*.test.mjs` |

凭证和档案在仓库外的 `~/.local/share/ai-passport-quota/`（或 `AIQ_STATE_DIR`）。禁止提交认证文件、Wi-Fi 信息、令牌、私钥、设备标识或原始日志；截图用示例账户。LVGL 线程之外的固件界面操作需要 BSP 锁，网络及存储不能阻塞按键回调。按改动运行测试，固件交付通过完整检查；刷机需要另外授权。

## 来源与许可

保留 MIT 许可的 [FoloToy AI Passport](https://gitee.com/FoloToy/ai-passport) 固件历史，基于 `0b9e4c81ee4421c0bac39ca3561d65a8285acd4a`。本地 `upstream` 指向上游，`origin` 指向本项目。许可与资源归属见 [LICENSE](LICENSE)、[字体与固件资源](assets/README.zh_CN.md)、[电脑端资源](companion/ASSETS.zh_CN.md)。
