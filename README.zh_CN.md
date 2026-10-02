简体中文 · [English](README.md)

# AI Passport AI 额度看板

面向 FoloToy AI Passport（ESP32-C3、240 × 320 小屏）的 AI 订阅额度看板，配套本地电脑网页管理账户和设备设置。支持最多八个独立的 ChatGPT/Codex、Claude 订阅账户和 DeepSeek API 账户，展示 Logo、订阅额度或 API 余额，支持刷新间隔与离线缓存。

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
4. 添加 DeepSeek 时填写[官方平台](https://platform.deepseek.com/api_keys)生成的 API Key。名称是本地备注，余额接口不提供已验证邮箱。密钥只保存在当前用户可访问的本地档案，不发送到设备。页面显示[官方余额接口](https://api-docs.deepseek.com/api/get-user-balance/)返回的人民币总余额、赠送余额、充值余额、可用状态和采集时间。首版不展示美元；缺失人民币数据时保持未知。不会发送模型请求。
5. 刷新设置可选 1、5、15、30 分钟；自动息屏可选 30 秒、1/2/5/10 分钟或“永不”，默认 2 分钟。

## 连接小屏

先刷入经过验证的固件，再连接 USB、打开小屏物理配对窗口。已配置设备长按 OK 进入设置，用上下键选择配对并按 OK。窗口持续 120 秒。

在网页设备配置中选择电脑私有 IPv4 地址，填写 **2.4 GHz Wi-Fi** 信息，点击连接并配置，选择 ESP32-C3 USB Serial/JTAG 设备并等待确认。Wi-Fi 信息直接从浏览器内存发送到 USB。配对后小屏连接所选电脑 **4318** 端口的固定证书 HTTPS 服务；网页 **4317** 端口只供本机使用。电脑 IP 改变后重新配对；停止同步会撤销令牌。

小屏操作：上下键切换账户，短按 OK 刷新或确认，长按 OK 打开设置或返回。按设置的时间自动息屏；息屏后的第一次功能键操作只亮屏；亮屏时长按 DOWN 息屏。独立电源键保留官方长按关机方式。配对窗口内不自动息屏，息屏后继续后台同步。

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

## 其他 Agent 从这里接着开发

先读 [AGENTS.md](AGENTS.md)、本 README 和[应用及协议说明](docs/applications/ai-quota-monitor.zh_CN.md)。后续以此仓库为准，停止在此前分散的工作目录继续修改。

| 范围 | 文件 |
| --- | --- |
| 固件界面、状态和输入 | `main/main.c`、`main/quota_ui.c`、`main/quota_logic.c` |
| Wi-Fi、固定证书 HTTPS、USB、NVS | `main/quota_service.c` |
| 显示及硬件驱动 | `components/bsp/` |
| React 网页与 USB 通信 | `companion/src/App.jsx`、`companion/src/serial.mjs`、`companion/src/styles.css` |
| 官方登录与额度采集 | `companion/server/accounts.mjs`、`clients.mjs`、`claude-feed.mjs`、`claude-session.mjs`、`deepseek.mjs` |
| 本机 API、配对与存储 | `companion/server/index.mjs`、`pairing.mjs`、`protocol.mjs`、`storage.mjs` |
| 主机测试 | `tests/test_quota_logic.c`、`tests/test_quota_fonts.py`、`tests/test_quota_refresh_runtime.py`、`tests/test_quota_storage_runtime.py`、`companion/test/*.test.mjs` |

电脑端验证：**47/47 项测试通过**，正式构建通过。网页检查确认人民币余额、正确的额度填充比例、正式服务的设备状态保持未知、息屏时间保存。文档截图使用隔离示例。

**2026-10-02 固件验证状态**：完整检查通过，包括 ESP-IDF 构建、合并镜像/调试归档核验、全部主机检查、刷新状态测试、NVS 兼容测试和 135 个非 ASCII 字形覆盖。新增息屏、显示状态和 DeepSeek 的固件**尚未刷入或真机验收**。DeepSeek 尚未使用真实密钥及响应测试；余额测试使用模拟响应。

当前调试归档为 `build/firmware/07225fb61fd6e6264f36c88be46fdf64fbe0b8120ca74692c166465e0b3d1050/`，ELF SHA-256 为 `c93feedbdc8378ebadc4c6a38ff063abd4b0588667b35073ebcff0be94c21ede`。该产物由提交前工作区（`c2bb775-dirty`）构建。分区表与此前实际刷入镜像一致，可按核验后的分段升级保留 NVS。刷机前确认当前设备及具体写入范围。

此前真机观察确认初始配对页中文清晰、两分钟窗口关闭正常、启动未见崩溃。一个真实 Codex 账户已提供周额度，其数据源没有提供 5 小时窗口。

上述真机观察使用整理仓库之前的完整镜像 `690229c2dca7ec2ee0a794d1c4f8625468e4feedab54507b7c3cae0448b1af7c`，匹配 ELF SHA-256 为 `15a33663935734a7df19fd553db87e461f53d1a22e2984973342ca8df3bc0858`。重新编译此仓库会产生新的产物身份，本次新构建尚未刷机。分析崩溃前必须匹配实际刷入镜像的 ELF。

**接下来先验收 USB 配对**：用户报告过配对窗口内仍出现 15 秒超时。网页已改为打开后立即接收、有限等待启动、4096 字节数据流缓冲和具体错误提示；主机检查通过，实际重试尚未确认。原生串口发送短帧和实际配置长度的安全无效帧均得到匹配编号的确认。打开串口监视器可能重启设备，避免同时占用串口，区分重启与配对超时。

本次增加可设置的自动息屏、完整唤醒按键抑制、设备真实 Wi-Fi/电池图标、本次启动校时后显示的时钟、订阅名称展示首字母大写、黑白 OpenAI Logo 和 DeepSeek 余额。后台读取缓存不再提示正在刷新。DeepSeek 需要本次新固件，旧固件会拒绝该平台。

网页低额度/危险额度预览曾把整块数字背景着色，现已修正并在浏览器检查。保持数字背景透明，进度条只填充剩余比例；不要把模拟数据流检查或网页预览当成真机验收。

配对成功后继续验证 Wi-Fi/HTTPS 同步、Claude 授权与真实额度、全部页面/按键/Logo、设置双向同步、断网及重启缓存、自动息屏/唤醒、DeepSeek 余额、TLS 动态内存。未知或重置过期窗口保持未知，保留来源时间，不伪造新额度。

凭证和档案在仓库外的 `~/.local/share/ai-passport-quota/`（或 `AIQ_STATE_DIR`）。禁止提交认证文件、Wi-Fi 信息、令牌、私钥、设备标识或原始日志；截图用示例账户。LVGL 线程之外的固件界面操作需要 BSP 锁，网络及存储不能阻塞按键回调。按改动运行测试，固件交付通过完整检查；刷机需要另外授权。

## 来源与许可

保留 MIT 许可的 [FoloToy AI Passport](https://gitee.com/FoloToy/ai-passport) 固件历史，基于 `0b9e4c81ee4421c0bac39ca3561d65a8285acd4a`。本地 `upstream` 指向上游，`origin` 指向本项目。许可与资源归属见 [LICENSE](LICENSE)、[字体与固件资源](assets/README.zh_CN.md)、[电脑端资源](companion/ASSETS.zh_CN.md)。
