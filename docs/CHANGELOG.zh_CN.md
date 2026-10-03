<p align="right">
  <strong>简体中文</strong> · <a href="CHANGELOG.md">English</a>
</p>

# Changelog

> 本 Fork 在此记录功能变更、验证和真机验收。根 README 只阐述当前功能、用法及限制。下方保留继承的上游条目；`Unreleased` 不代表已经发布版本。

## Unreleased

### 2026-10-03 — 按息屏状态控制同步

设备息屏时暂停快照轮询、来源刷新、设置 HTTP 请求和主动 Wi-Fi 重试及配置应用，保留缓存、配对信息及功能键检测。唤醒后立即排队一次合并的刷新与快照同步，即使关闭自动刷新也执行；先结束已接纳的限时请求，离线唤醒保留到连接恢复。随后从唤醒同步周期恢复正常轮询和已启用的自动刷新。电脑端保留独立的定时刷新。已接纳的 Wi-Fi 初始化允许完成；HTTP 请求保持限时，显示状态切换代数用于阻止息屏或快速息屏/唤醒后的旧结果和后续请求。

充电状态检查：现有 CW2017/BSP 提供电量和电压，板级引脚表没有充电状态输入，也没有随项目提供的原理图。[官方充电说明](https://ai-passport.folotoy.cn/en/guides/getting-started/)使用绿色指示灯。真实充电动画仍需要有明确资料、芯片可读的 CHG/STAT 信号；未用 USB 连接、电量上涨或电压变化替代真实充电状态。

用户要求本轮**只开发验证、不刷机**。本轮不连接设备、不写入固件；之前的真机观察不能用于验证这次新增的息屏同步行为。

#### 验证

构建：**通过** — ESP-IDF 5.5.3 下完整 `./tools/validate.sh` 通过，包括合并镜像和调试归档。主机测试：**通过** — 仓库/工作流检查和全部主机测试通过，四项运行时测试直接执行实际 HTTP 准入、快照发布、刷新/设置函数及网络循环。覆盖息屏暂停、关闭自动刷新后唤醒、离线恢复、快速断线/重连、保留被取消的 GET、过期代次、设置延后、来源时间、缺失窗口、按完成时间恢复周期及 135 个非 ASCII 字形。新增取快照前重连的回归测试在修复前失败、修复后通过；最终独立复查没有剩余可操作问题。代码提交 `f557281` 的 GitHub 静态检查和固件检查均通过。本地工作区固件在该代码提交之前构建。电脑端源码未变，之前的验证记录见下文。

核验后的本地归档为 `build/firmware/2a795a6280ca8f8b74bfa02966a03d4c8bb72e0c782f5ebdfb1789e49af261ac/`，合并镜像 SHA-256 为 `2a795a6280ca8f8b74bfa02966a03d4c8bb72e0c782f5ebdfb1789e49af261ac`，ELF SHA-256 为 `c420668eea7f7f393ea267fb20c0aa65bd687046b9f3fea4dda681d3db038f8c`。应用大小 1,496,576 字节，合并镜像 1,562,112 字节；工作区构建报告版本 `3c823a3-dirty`。固件和原始日志仅留本地，未提交。

真机测试：**未运行** — 未访问硬件或串口，未刷机。上次刷入的仍为 `c0eb0a89…` 归档。未验证：新固件真机上的息屏/唤醒流量暂停、离线恢复、刷新计时、动态内存和耗电，以及真实硬件充电状态接口。充电动画需求仍等待该接口，尚未实现。

### 2026-10-02 — AI Passport Quota

将固件和本地电脑端应用整理到用户的私有仓库，加入隔离示例截图，说明账户连接、USB 配置及开发入口。新增息屏控制和状态图标，修正刷新状态及额度预览，接入 DeepSeek 并只显示一项人民币可用余额。

核对 DeepSeek 官方 API 文档：[余额接口](https://api-docs.deepseek.com/api/get-user-balance/)支持可用余额，未找到公开文档支持的账户累计/区间消费、请求次数或历史 Tokens 总量接口。[模型响应中的用量](https://api-docs.deepseek.com/api/create-chat-completion/)只对应单次请求。官网统计等待验证数据来源后接入。取消两端赠送/充值明细展示，保留来源数据兼容性。

#### 验证与真机验收

电脑端验证：**47/47 项测试通过**，正式构建通过。网页检查确认只显示一项人民币可用余额、不展开赠送/充值明细，正确的额度填充比例、正式服务的设备状态保持未知、息屏时间保存。文档截图使用隔离示例。

代码提交 `e0b56c8` 的 GitHub 静态检查和固件检查均通过。

**2026-10-02 固件验证状态**：完整检查通过，包括 ESP-IDF 构建、合并镜像/调试归档核验、全部主机检查、刷新状态测试、NVS 兼容测试和 135 个非 ASCII 字形覆盖。新增息屏、显示状态和 DeepSeek 单一人民币余额的固件**已刷入并检查启动**，用户确认主页、长按下键息屏和功能键唤醒均正常。完整功能验收仍待完成。DeepSeek 尚未使用真实密钥及响应测试；余额测试使用模拟响应。

当前已刷入的调试归档为 `build/firmware/c0eb0a8988a819ae43a2cb4b3e388747e14e79c6a667d7916a2fb73a04123de4/`，ELF SHA-256 为 `4a87f07e1535d9ea084a811aeff055dbcf8fdc4ea3e5fb4ae2cee8ff42e18a8f`。该产物由工作区（`5ef51a1-dirty`）构建，随后代码提交为 `e0b56c8`。2026-10-02 核对实机分区表与归档一致，按 `0x0` / `0x8000` / `0x10000` 分段写入并校验引导程序、分区表和应用，保留 NVS 与 PHY 区域，没有全片擦除。30 秒启动观察匹配应用版本和启动日志中的 ELF 前缀，确认显示初始化、应用就绪、额度缓存恢复，未发现 panic/看门狗重启。随后电脑端收到已认证的 HTTPS 同步请求。初次 Wi-Fi 连接有一条正在连接提示，之后同步成功。原始日志仅本地留存，串口已释放。

此前真机观察确认初始配对页中文清晰、两分钟窗口关闭正常、启动未见崩溃。一个真实 Codex 账户已提供周额度，其数据源没有提供 5 小时窗口。

上述早期真机观察使用整理仓库之前的完整镜像 `690229c2dca7ec2ee0a794d1c4f8625468e4feedab54507b7c3cae0448b1af7c`，匹配 ELF SHA-256 为 `15a33663935734a7df19fd553db87e461f53d1a22e2984973342ca8df3bc0858`。本次升级观察对应上面记录的新归档。分析崩溃前必须匹配实际刷入镜像的 ELF。

**仍需验收 USB 配对**：用户报告过配对窗口内仍出现 15 秒超时。网页已改为打开后立即接收、有限等待启动、4096 字节数据流缓冲和具体错误提示；主机检查通过，实际重试尚未确认。本次升级恢复已有配对配置，不能证明浏览器重新配对可用。原生串口发送短帧和实际配置长度的安全无效帧均得到匹配编号的确认。打开串口监视器可能重启设备，避免同时占用串口，区分重启与配对超时。

本次增加可设置的自动息屏、完整唤醒按键抑制、设备真实 Wi-Fi/电池图标、本次启动校时后显示的时钟、订阅名称展示首字母大写、黑白 OpenAI Logo 和 DeepSeek 余额。后台读取缓存不再提示正在刷新。DeepSeek 需要本次新固件，旧固件会拒绝该平台。

网页低额度/危险额度预览曾把整块数字背景着色，现已修正并在浏览器检查。保持数字背景透明，进度条只填充剩余比例；不要把模拟数据流检查或网页预览当成真机验收。

本次升级已确认初次 Wi-Fi/HTTPS 通信、额度缓存恢复、主页显示、手动息屏和功能键唤醒正常。后续验证浏览器重新配对、Claude 授权与真实额度、其余页面/按键/Logo、设置双向同步、持续断网及重启缓存行为、按时间自动息屏、DeepSeek 余额、TLS 动态内存。未知或重置过期窗口保持未知，保留来源时间，不伪造新额度。

### 继承的上游条目

- 加入厂家为优特利 520mAh 电芯生成的 80 字节 CW2017 profile，并实现内容与更新标志检查、写入后校验、规定的重启时序以及有上限的 SOC 就绪等待。

- 扩充环境引导文档：新增乐鑫 Git 服务镜像（`git.espressif.com.cn`）作为中国大陆首选线路，覆盖 ESP-IDF v5.5.3 及其子模块；补充子模块长等待/超时处理、原地修复，以及 `esp32-wifi-lib` 等大仓的按钉死 commit 浅取；提示按仓库残留的 Jihulab `insteadOf` 旧配置；并把官方离线 release 压缩包加入兜底方案（经验来自 `esp-mosaico/esp-mosaico-vibe`）。

- 按功能域整理文档并采用双入口：根目录 `AGENTS.md` 变为薄路由（只保留硬约束与任务路由），详细的 AI 开发工作流下沉到 `docs/development/ai-guide.md`，`agent-guide.md` 并入其中。为 `docs/development/` 增加二级分区（`engineering/`、`ci/`、`release/`），把 `plays/` 应用档案与 `experiences/` 移入带专属 README 的 `docs/reference/` 参考区；删除 `docs/software-design/`（空脚手架）；把 `assets/{fonts,images,music}/README` 三个叶子 README 并入 `assets/` README；把 `project-completion` 的六个子文档压平为单文件；并把每个目录统一为单一 README，消除所有 `INDEX` 文件与一处重复经验索引。所有交叉引用与文献链接已更新；未丢弃任何内容。

- 删除位于 `0x700000` 的旧 app/test 分区，以及相关的 bootloader、校验和
  文档要求；固定的 `cardid` 保护分区及其 CI 校验保持不变。
- 规定多应用发布的 Release 标题约定：tag 按 `v<版本>-<应用名>`（如 `v0.1.0-voice-keychain`）命名，让 Release 标题同时带版本与应用名；发布成功后核对标题，保证一眼扫 Release 列表就能区分是哪个应用。
- 新增发布后收尾流程：`issue-suggestions` skill 用于把用户反馈作为 issue 提交到上游项目；`experience-pr` skill 用于把可复用的开发经验作为文档 PR 提交；新增 `docs/experiences/` 目录保存单条经验文件；并配套 `project-completion`、`file-issues` 与经验索引文档。
- 精简仓库根目录：将 GitHub 可识别的社区治理文档迁入 `.github/`，将变更记录迁入 `docs/`，同步全部引用，并在仓库检查中加入根目录文档白名单。
- 全仓库文档语言规范：所有维护中的 Markdown 默认 `.md` 文件使用英文，简体中文使用配对的 `.zh_CN.md`，双方提供语言切换；静态检查会阻止缺失配对、缺失切换链接或英文默认页混入中文正文。
- AI 开发流程一期：精简按任务加载的上下文入口，统一本地/CI 验证脚本，新增 PR 自动构建与模板，并提交依赖锁文件以提高构建可复现性。
- PR 审查修复：GitHub Actions 固定到完整 commit SHA，构建与发布 job 按最小权限拆分，同步 checkout 关闭凭证持久化；补充 Feature Request / Usage Question issue 表单；启用并修正私密安全报告兜底说明；清理 README 路径、CI 触发条件与历史分支描述漂移。
- 语言规范变更：commit 标题、PR 标题与 body 由"默认中文"改为**使用英文**（`docs/contribution/commit-and-pr.md` 更新）；中文写作规范（全角标点）适用范围剔除 PR/MR 描述（`doc-conventions.md` 更新）。
- CI 构建改造：`build-firmware.yml` 显式传入 `SDKCONFIG_DEFAULTS=sdkconfig.defaults` 再 `idf.py build`，由 defaults 启用自定义分区表（`CONFIG_PARTITION_TABLE_CUSTOM=y`，文件名为 `partitions.csv`）；`CONFIG_ESPTOOLPY_HEADER_FLASHSIZE_UPDATE` 改为 `n`，再用 `idf.py merge-bin -o build/FoloToy-AI-Passport-full.bin` 合并可直刷完整固件；产物精简为仅 full.bin；`actions/cache` 升级到 v5 以消除 GitHub Actions Node.js 20 弃用警告；CI 文档同步更新。
- 合并上游 PR #6（wireless-low-power-demos）以解决 PR #4 冲突：引入无线/低功耗 demo（`main/demo_wifi.c`、`demo_ble.c`、`demo_radio.c`、`demo_low_power.c`）、`partitions.csv`（NVS/PHY/3 MB factory-app 分区）、`main/CMakeLists.txt`/`main.c`/`demo.h`/`sdkconfig.defaults` 更新；同步硬件指南的 Wi-Fi/BLE/低功耗章节；README 能力契约表补充 Wi-Fi/Bluetooth LE/Low power 三项（中英双语）。
- 提交规范补充：`docs/contribution/commit-and-pr.md` 明确 PR 标题与 commit 标题使用相同的 Conventional Commit 格式和英文祈使句，不用名词短语当标题。
- CI 与文档清理：`sync-main.yml` 移除 `test_mode` 残留模板注释；`docs/development/coding-conventions.md` 将「Redis TTL」条目泛化为「缓存组件」条目（当前固件无 TTL 约束需求，消除从模板带入的无关约定）。
- 补充通用规范（借鉴 Shinku）：`docs/contribution/doc-conventions.md` 新增中文全角标点规范（正文 `，`；`（`）`，代码/命令/路径保留英文原样）、凭证不入仓规范（token/密钥/私钥绝不入仓，提交前 git diff 扫描敏感前缀）、文件删除安全规范（删除走系统回收站，不用 rm -rf/git clean -fd）。
- 代码注释规范强化：`docs/development/coding-conventions.md` 补充完善注释要求——函数说明（用途/参数/返回值/副作用/线程上下文/内存所有权/初始化顺序）、变量说明（语义/取值范围/生命周期/同步要求）、逻辑注释（状态机/时序/寄存器/魔数依据），覆盖范围宁多勿少，中文注释保留英文技术术语。
- 文档去 AI 化：`docs/README.md` / `docs/README.zh_CN.md` 移除 AI 专属章节（Entry point、Source-of-truth、提需求格式、BSP 边界、Runtime invariants、验收交付格式、构建命令），README 只保留给人看的项目介绍、硬件能力契约、demo 案例与项目结构；构建命令章节删除（与 `docs/development/build-and-test.md` 重复）。
- 新增 `docs/development/agent-guide.md`：集中承载"AI 如何在本仓库工作"（上下文建立顺序、事实来源优先级、提需求格式、BSP 边界、运行时规则、交付格式），并链接 build-and-test 与硬件指南，不重复构建命令与验收矩阵。
- 同步更新索引：`AGENTS.md` 规则索引新增 agent-guide 条目；`docs/INDEX.md` 与 `docs/development/README.md` 新增 agent-guide 索引行。
- 文档补充：`docs/fork-guide.md` 说明「为什么根目录不放置 README」——根目录 README 预留给 fork 开发者自行放置（上游留空），fork 后可将自己的内容写入根目录 `README.md` 介绍 fork 后的项目；GitHub 显示优先级（根 README > docs/README.md）契合该预留意图。
- 分支合并：创建 `main-update` 分支（基于与上游一致的 main），将 `feature/repo-structure`、`ci/build-firmware`、`ci/sync-main` 三个分支合并进来，统一 docs 结构（CI 文档归入 `docs/development/`，workflow 文件随 ci 分支引入 `.github/workflows/`）；解决 development/software-design README 的 add/add 冲突。
- 合并后审查修复：`docs/INDEX.md` 补充 CI 文档索引；`docs/fork-guide.md` 修正 workflow 引用为 `.github/workflows/sync-main.yml`；`docs/README` 双语项目结构块补充 `.github/workflows/` 与 CI 文档说明。
- ci 分支 CI 文档路径调整：`ci/build-firmware` 的 `docs/software-design/CI-build-and-release.md` 与 `ci/sync-main` 的 `docs/software-design/CI-sync-main.md` 均移入各分支的 `docs/development/`（CI 属工程规范）；`docs/software-design/README.md` 保留为软件设计索引；feature 分支的 software-design 索引同步更新引用。
- fork 补充文档目录迁移：`assets/docs/` 移至 `docs/assets/`（文档素材归入 docs/ 更合理），新增 `docs/assets/.gitkeep` 空目录占位；同步更新 AGENTS.md / INDEX / doc-conventions / fork-guide 的路径引用。
- 文档结构调整：根目录不再放 README——上游英文 README 移入 `docs/README.md`、中文移入 `docs/README.zh_CN.md`（GitHub 从 docs/ 识别主 README）；原 `docs/README.md` 根总索引更名为 `docs/INDEX.md`；同步更新 AGENTS.md / CONTRIBUTING / SUPPORT / fork-guide / doc-conventions 的路径引用。
- 初始化项目文档：新增 `AGENTS.md`、`CLAUDE.md` 和 `CHANGELOG.md`。
- 仓库结构规整：上游英文 `README.md` 更名为 `README.en_US.md`，保留 `README.zh_CN.md`。
- 新增目录骨架：`docs/`（software-design / hardware-design）、`assets/`（fonts / images / music，各含 `README.md`）、`skills/`。
- 将上游硬件开发指南归位到 `docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md`。
- 文档规范：子目录 readme 统一为大写 `README.md`；补充 fork 用户约定（main 只动根 README）。
- 扩展 fork 用户约定：`main` 分支允许修改根目录 `README.md` 和 `assets/docs/`（README 不足以说明项目时存放补充文档与素材）。
- 新增 `assets/docs/` 目录约定：上游 main 只保留空目录 `.gitkeep`，内容文件仅存在于 fork；使用方法规范写入 AGENTS.md「给 fork 用户」约定。
- CI 文档迁移：`docs/software-design/CI.md` 从本分支移除，迁至 `ci/build-firmware` 分支并改名为 `docs/software-design/CI-build-and-release.md`。
- 补充 `main` 分支策略说明：解释 `main` 保持干净的两大原因（与上游同步无冲突 + 多小项目按分支整理）；例外——执意 main 开发需停用 CI 自动同步；提醒 fork 用户默认 action 关闭需手动启用（此条为整个 CI 的通用要求，统一写入 AGENTS.md）。
- 文档拆分：将 `AGENTS.md` 按主题拆为公共文档——新增 `docs/contribution/`（doc-conventions.md、commit-and-pr.md）与 `docs/development/`（build-and-test.md、coding-conventions.md），新增 `docs/fork-guide.md`；`AGENTS.md` 精简为简介 + 项目概述 + 必读文档索引。
- 同步更新索引：`docs/software-design/README.md`、`README.en_US.md` / `README.zh_CN.md` 的 `docs/` 目录说明。
- 参考 cindy 仓库文档组织完善索引：新增 `docs/README.md` 根总索引；AGENTS.md 规则索引按触发场景改写（附触发条件）；`docs/contribution/` 与 `docs/development/` 的 README 补充收录标准。
- 引入社区治理文档（参照 cindy 改写，放仓库根目录）：新增 `CONTRIBUTING.md` / `.zh_CN.md`（贡献指南，针对 ESP-IDF/AI agent/fork 场景改写）、`CODE_OF_CONDUCT.md` / `.zh_CN.md`（贡献者公约）、`SECURITY.md` / `.zh_CN.md`（安全报告流程）、`SUPPORT.md` / `.zh_CN.md`（支持渠道）；AGENTS.md 与 docs/README.md 同步引用。
