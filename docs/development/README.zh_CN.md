简体中文 · [English](README.md)

# 开发、校验与刷写

## 结构

| 范围 | 入口 |
| --- | --- |
| 固件输入与显示生命周期 | `main/main.c` |
| 纯状态、解析与时序 | `main/quota_logic.c`、`main/quota_logic.h` |
| 网络/USB 执行者与帧解析 | `main/quota_service.c`、`main/quota_usb.c` |
| 账户目录、控制器、授权/查询与存储 | `main/quota_catalog.c`、`main/quota_portable_service.c`、`main/quota_direct*.c`、`main/quota_store.c` |
| 热点/USB 共用设置页 | `main/quota_portal.c`、`main/portable_setup.html`、`main/portable_setup.mjs`、`main/portable_serial.mjs` |
| 看板与显示资源 | `main/quota_ui.c`、`main/quota_brand_assets.c`、`assets/` |
| 板卡驱动 | `components/bsp/include/`、`components/bsp/src/` |
| 电脑 UI 与 USB | `companion/src/` |
| 官方来源与本地服务 | `companion/server/` |
| 电脑端共享设置 | `companion/shared/contract.mjs` |
| 主机检查与固件打包 | `tests/`、`companion/test/`、`tools/` |

状态和协议计算应能脱离 ESP-IDF/LVGL 测试。固件按键回调投递事件，应用与网络工作任务处理慢操作。非 LVGL 任务访问 UI 须持有 BSP 锁。修改协议、来源数据、刷新或持久化前阅读[应用契约](../applications/ai-quota-monitor.zh_CN.md)；修改驱动或硬件配置前阅读[硬件参考](../hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md)。

## 构建与检查

电脑端需要 Node.js 22+、npm、OpenSSL 和官方服务客户端。固件针对 ESP32-C3，使用 ESP-IDF **5.5.3**。激活合适的已有安装；缺少时按照[乐鑫安装指南](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/get-started/index.html)安装该版本。保留其他工具链和用户配置。

```bash
# 仓库根目录；电脑端行为与前端检查
(cd companion && npm ci && npm test && npm run build)

# 先激活 ESP-IDF 5.5.3
source <esp-idf-v5.5.3>/export.sh
idf.py --version
./tools/validate.sh --static    # 仓库/workflow 检查和固件主机测试
./tools/validate.sh --firmware  # 隔离构建和镜像/归档检查
./tools/validate.sh             # 两项固件门禁
```

迭代时运行聚焦检查。固件交付使用完整门禁，电脑端行为变更运行对应检查。CI 调用同一门禁，不另维护竞争的构建命令序列。仅文档修改需要文档检查，无需刷写无关固件。

受版本管理的默认值来自 `sdkconfig.defaults`、`partitions.csv` 和 `dependencies.lock`。LVGL 固定为 9.5.0，以 `CONFIG_LV_BUILD_EXAMPLES=n` 和 `CONFIG_LV_BUILD_DEMOS=n` 关闭未使用的 examples/demos。依赖锁变化须与组件 manifest 一起审阅。固件门禁在临时目录生成独立 `sdkconfig` 并构建，被忽略的本地设置不会进入该产物。需要本地变体时明确解决配置差异。已有 ccache 时可用 `IDF_CCACHE_ENABLE=1` 复用编译结果。

成功门禁生成 `build/FoloToy-AI-Passport-full.bin`，并将匹配产物存于 `build/firmware/<full-image-sha256>/`。归档包含合并镜像、应用 ELF/MAP/镜像、bootloader、分区表、`flash_args` 及大小/哈希 manifest。校验命令：

```bash
python3 tools/archive_firmware.py verify <bundle-directory>
```

分析崩溃时使用匹配 ELF，后续重构建可能具有不同身份。失败运行可能保留旧输出，必须报告确切成功归档路径和镜像哈希。生成固件和调试归档不提交，也不自动上传。额外自定义分区载荷不会单独保存于此归档。

设备内嵌设置页可用 `node tools/preview_portable.mjs` 预览同一生产 HTML 和示例数据，打开启动输出的地址。`http://127.0.0.1:4328/__preview/manual` 可验证手动连接，示例密钥为连续 43 个 `s`。这是手动开发预览，不是采集服务。修改设备授权前阅读[便携设计](portable-connectivity.zh_CN.md)。

前端迭代时在 `companion/` 运行 `AIQ_DEV_ORIGIN=http://127.0.0.1:5173 npm start`，另开终端执行 `npm run dev`，打开这个确切 origin。文档截图使用 `npm run preview:readme`，在 `http://127.0.0.1:4327/` 提供隔离合成账号。

## 刷写与 NVS

默认 8 MB 布局：NVS 为 `0x9000`/`0x6000`，PHY data 为 `0xF000`/`0x1000`，factory app 为 `0x10000`/`0x7B0000`，独立 portable NVS 为 `0x7C0000`/`0x40000`；分区表通常位于 `0x8000`。校验器检查配置偏移、边界、不重叠、分区 MD5 和镜像一致性。允许合法自定义布局，以实际产物记录的偏移为准。

写入前识别目标设备和确切已验证产物，确认分区兼容性并说明数据影响。获得适用于该设备、产物和写入范围的授权。其他构建的旧授权不适用于新产物；重构建会改变待交付文件。不要求先读回原固件。

合并 `full.bin` 从 `0x0` 写入时包含填充间隙，可能重置 NVS/PHY 数据，适用于空设备或明确的完整刷新。保留设置时，先确认分区表兼容且写入不覆盖存储数据，再按记录偏移仅写已验证组件镜像。需要保留的数据应使用应用支持的方法保存。不得将 app-only 镜像写在 `0x0`，未经明确授权不得擦除全片。产物校验本身不保证用户数据保留。

## 验收与报告

授权全程保持原生 USB 串口连接，避免改变调制解调器控制线。重新连接日志工具可能复位 C3，使该轮验收失效。

分开报告 `Build`、`Host tests`、`Device tests` 和 `Unverified`，列出实际完成的检查。浏览器截图和主机模拟不能证明板卡显示、物理 USB 配对或服务授权。相关检查包括独立/缺失窗口、真实 0%、过期等待、按条件显示 Codex 扩展项，以及重启后恢复来源绑定匹配的扩展缓存；未观测到的扩展在同步前保持隐藏。真机验收还包括字体/按键、配对、证书拒绝、Wi-Fi 停止/重连、LCD Sleep In/out、缓存静默唤醒、保留刷新截止时间及重启持久化。注明 USB/电池条件并比较亮屏/息屏实测电流，配置和主机测试不能证明电流降低。真实 DeepSeek 凭证与扩展/离线时序仍需要各自验收证据；已发生结果见[变更日志](../CHANGELOG.zh_CN.md)，不能把旧验收自动移用于新构建。
