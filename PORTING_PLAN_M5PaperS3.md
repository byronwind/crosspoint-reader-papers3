# Crosspoint Reader → M5PaperS3 移植计划（优化版）

> 本文档是对 `PORTING_PLAN.md` 评估后的**优化与修订版**，基于对代码库（`lib/hal/`、`src/`、`freeink-sdk/`）与 M5PaperS3 官方硬件的实际交叉分析，而非复述原草案。

---

## 0. 结论先行（Executive Summary）

### 整体可行性：**高（可行，风险可控）**

移植的技术风险主要不在"显示驱动"（这是原草案最大的错误假设），而在**输入适配、电源管理时序与 Cherry-Pick 冲突纪律**。原因如下：

1. **freeink-sdk（原 open-x4-sdk）已经内置了 ED047TC1 并行 EPD 的驱动能力**。`LgfxEpdDriver` 封装 LovyanGFX 的 `Panel_EPD`/`Bus_EPD`，`LgfxEpdConfig` 提供 8 位并行总线 pin + 电源钩子 + LUT 表，且已有 **LilyGo T5 S3**（同一块 ED047TC1 面板）的参考移植。M5PaperS3 与 LilyGo T5 S3 是**同一类 raw 960×540 16 级灰度并行 EPD**，仅 pin 映射不同。
2. **无需引入 epdiy**。原草案（`PORTING_PLAN.md` §6）把 `epdiy` 与 `M5GFX` 并列加入依赖，但这会引入第二套并行 EPD 驱动栈，与 freeink-sdk 的 `LgfxEpdDriver` 冲突。**正确路径是扩展现有 freeink-sdk，添加一个 `M5PaperS3` BoardProfile**，复用 `LgfxEpdDriver` 的 M5GFX-backed 路径。
3. **HAL 接口（seam）设计良好**。`HalDisplay` 封装 `EInkDisplay`，应用层只通过 `display.getBufferSize()/getFrameBuffer()/DISPLAY_*` 取几何与缓冲，`GfxRenderer` 从 HalDisplay **动态**取宽高。因此分辨率 800×480 → 960×540 的适配是**局部性的**，不涉及活动层重写。
4. **HalGPIO.cpp 已含有 `#if defined(FREEINK_DEVICE_M5PAPER)` 分支**，说明 freeink-sdk 作者已预留 M5Paper 家族的接入点，移植是"填空"而非"重写"。

### 推荐路径

**扩展现有 freeink-sdk，新增 `M5PaperS3` BoardProfile（FREEINK_DEVICE_M5PAPERS3），复用其 LgfxEpdDriver / InputManager / PowerManager / Rtc / BatteryMonitor；不采用"用 epdiy 替换 EInkDisplay"的旁路方案。**

| 维度 | 原草案建议 | 本计划建议 |
|---|---|---|
| 显示驱动 | M5GFX **+ epdiy** 双栈 | **单栈**：`LgfxEpdDriver`（M5GFX Panel_EPD），新增 M5PaperS3 BoardProfile |
| SDK 关系 | 移除 open-x4-sdk | **保留并扩展** freeink-sdk（改名后的同一 SDK） |
| 输入 | 新建 TouchInputManager | 复用 freeink-sdk `InputManager`（已支持 GT911），仅补 M5PaperS3 pin/旋转 |
| 电源/RTC | M5Unified Power + 手写 RTC | 复用 `PowerManager`/`Rtc`/`BatteryMonitor`（BM8563/BAT_ADC） |

### 工作量估算

| 阶段 | 工作量 | 说明 |
|---|---|---|
| 构建基础设施（sdkconfig/flags/依赖） | 0.5~1 天 | 主要在 platformio.ini / sdkconfig.defaults |
| HAL 适配（显示/存储/系统/GPIO） | 2~3 天 | 显示已由 freeink-sdk 承担大半 |
| 渲染适配（GfxRenderer/主题/DPI） | 1~2 天 | 分辨率/方向常量 + 触摸定制位置 |
| 输入适配（MappedInputManager/触摸分区） | 2~3 天 | 触摸防抖、手势、旋转映射是重点 |
| 电源管理（睡眠/唤醒/RTC/电池） | 1~2 天 | 时序与唤醒源 |
| Cherry-Pick 同步纪律 | 贯穿 | 建议 1~2 周跟进一次 upstream |
| **合计** | **约 2~3 周（单人）** | 不含回归测试与 UI 打磨 |

---

## 1. 对现有 PORTING_PLAN.md 的评估

### 1.1 遗漏的模块

- **`src/util/ButtonNavigator.h`、`util/ScreenshotUtil.h`**：ScreenshotUtil 依赖 `BTN_POWER`+`BTN_DOWN` 组合键截图，M5PaperS3 无物理组合键，需改为触摸手势（如三指下滑）或移除。
- **`src/SdCardFontSystem`**：SD 卡字体系统依赖 `SdFat`/`FsHelpers`，M5PaperS3 的 SD 走 M5Unified SPI（GPIO47 CS），需确认 `HalStorage` 上层接口兼容性。
- **`SilentRestart.h`（RTC_NOINIT）**：ESP32-S3 同样支持 `RTC_NOINIT_ATTR`，但需验证在 S3 布局上跨轻睡/深睡保持。
- **`lib/epdiy` 的 LUT / 波形**：原草案未提，freeink-sdk `LgfxEpdConfig` 已含 LUT 表钩子，M5PaperS3 需复用 ED047TC1 的波形。
- **`FontCacheManager` / `FontDecompressor`**：内存优化层，S3 有 PSRAM 后可放宽，但需确认 `FontCacheManager` 在 **8MB PSRAM** 下的分配策略（内部还是 PSRAM）。
- **`build_html.py` / `gen_i18n.py` 等工具链**：不影响移植，但 `platformio.ini` 的 `extra_scripts` 若引用 X3/X4 专属符号需排查。

### 1.2 潜在风险与错误假设

| # | 原草案假设 | 实际 / 影响 |
|---|---|---|
| 1 | "epdiy 与 M5GFX 并用" | **错误**。会造成双 EPD 驱动栈冲突。应只走 M5GFX（LgfxEpdDriver）。 |
| 2 | "移除 open-x4-sdk" | **过时命名**。SDK 已改名 freeink-sdk，且其能力远超原草案认知，应**保留扩展**。 |
| 3 | M5PaperS3 `PWR: GPIO46` | 官方为 **`PWR: G45`**（`OE: G45` 冲突！）。原草案 §2 同时把 `PWR=GPIO46` 与 `OE=GPIO45`，需核对 M5GFX 源码。 |
| 4 | §8 引用"1.3.0 sync"的 `CROSSPOINT_PAPERS3` 回归 | **当前代码库中 `CROSSPOINT_PAPERS3` 仅存在于 PORTING_PLAN.md**，`SleepActivity.cpp`/`BaseTheme.cpp`/`LyraTheme.cpp`/`JpegToBmpConverter.cpp` 均无该块。§8 是**前瞻性/假设性**描述，应据实修正为"预期冲突点"而非"已发生回归"。 |
| 5 | "grayscale 用 epdiy 16 级" | M5GFX `Panel_EPD` 已支持 16 级灰度（PSRAM 8-bit canvas），无需 epdiy。 |
| 6 | "触摸用 M5Unified touch API" | freeink-sdk `InputManager` 已封装 GT911，应复用而非重写。 |
| 7 | "`GfxRenderer` 需更新分辨率常量" | 实际 `GfxRenderer` 从 HalDisplay **动态**取几何，常量主要在 `BoardProfile`/`EInkDisplay`。 |
| 8 | S1 SPI 显示 → S3 并行 EPD 的缓冲格式 | 1-bit 位打包（BW）帧缓冲 → 8-bit 灰度 canvas。**GfxRenderer 的 `GRAYSCALE_LSB/MSB` 与条带模式需验证**，见 §5 风险。 |

### 1.3 关键修正点

- **命名**：全文 `open-x4-sdk` → `freeink-sdk`。
- **依赖**：去掉 `epdiy`，保留 `M5Unified`/`M5GFX`（M5GFX 由 freeink-sdk 的 `LgfxEpdDriver` 使用）。
- **§8 冲突纪律**：保留（纪律原则正确），但将"已发生回归"改为"预期冲突点"，并补充 `CROSSPOINT_PAPERS3` gating 的落地方式。

---

## 2. 硬件差异核对（C3 → S3）

| 维度 | Xteink X4 (源) | M5PaperS3 (目标) | 影响与处置 |
|---|---|---|---|
| MCU | ESP32-C3 RISC-V 单核 | ESP32-S3 Xtensa LX7 双核 240MHz | `HalSystem` 的 RISC-V panic handler 需改 Xtensa；S3 中断/定时器差异 |
| PSRAM | 无 | 8MB OPI-PSRAM | `sdkconfig` 开 `CONFIG_SPIRAM` + octal；内存优化可放宽 |
| 显示 | 800×480 SPI SSD1677 | 960×540 并行 EPD (ED047TC1) | freeink-sdk `LgfxEpdDriver` 已覆盖 |
| 触摸 | 无 | GT911 I2C | freeink-sdk `InputManager` 已覆盖 |
| 输入 | 7 物理按键 | 电源键 + 触摸 | `MappedInputManager` 改为触摸分区 |
| SD | SPI 共享总线 | SPI GPIO47 CS | `HalStorage` 走 M5Unified/SdFat 二选一 |
| 电池 | ADC GPIO0 | M5Unified Power | 复用 `BatteryMonitor` |
| RTC | 无 | BM8563 I2C | 复用 `Rtc` |
| 电源 | GPIO3 wakeup | 电源键 + RTC wakeup | `PowerManager` 适配 S3 唤醒源 |

### 2.1 M5PaperS3 官方 Pin 映射（与计划的差异）

| 信号 | 原计划 | 官方/实测 | 结论 |
|---|---|---|---|
| EPD Data[0-7] | G6,14,7,12,9,11,8,10 | 同 | ✅ 一致 |
| PWR | **G46** | **G45** | ⚠️ **修正**：见 §1.2 #3，需以 M5GFX 源码为准 |
| SPV | G17 | G17 | ✅ |
| CKV | G18 | G18 | ✅ |
| SPH/XSTL | G13 | G13 | ✅ 功能对应 |
| OE? | G45 | G45(PWR) | ⚠️ 需确认 OE 是否复用/独立 |
| LE/XLE | G15 | G15 | ✅ |
| CL | G16 | G16 | ✅ |
| GT911 SDA/SCL/INT | 41 / 42 / 48 | 同 | ✅ |
| SD CS | G47 | G47 | ✅ |
| PWROFF_PULSE | G44 | G44 | ✅ |
| 旋转 | offset_rotation=3 | 面板 960×540 | 触摸 offset_rotation=1 |

**给实施者的硬性要求**：在写 `LgfxEpdConfig` 前，先读 M5PaperS3 的 M5GFX `M5PaperS3` panel 定义（或 M5Stack 官方原理图），**以源码 pin 为准**，不要照抄本计划表。

---

## 3. REPLACE / ADAPT / KEEP 清单（修正版）

### 3.1 KEEP UNCHANGED（应用逻辑，约 90% 代码）

- `src/activities/*`、`src/components/*`：全部 UI 活动/组件（仅主题按钮提示位置需 ADAPT）
- `src/CrossPointSettings.*`、`CrossPointState.*`、`JsonSettingsIO.*`、`RecentBooksStore.*`、`WifiCredentialStore.*`、`OpdsServerStore.*`
- `src/fontIds.h`、`src/images/*`、`src/network/*`、`src/util/*`（除 ScreenshotUtil 的按键组合）
- `lib/Epub/`、`lib/EpdFont/`、`lib/FsHelpers/`、`lib/I18n/`、`lib/InflateReader/`、`lib/JpegToBmpConverter/`、`lib/PngToBmpConverter/`、`lib/KOReaderSync/`、`lib/Logging/`、`lib/OpdsParser/`、`lib/Serialization/`、`lib/Txt/`、`lib/Utf8/`、`lib/Xtc/`、`lib/ZipFile/`、`lib/expat/`、`lib/picojpeg/`、`lib/uzlib/`
- `lib/GfxRenderer/`：**核心逻辑 KEEP**，仅几何常量/方向由 BoardProfile 提供

### 3.2 ADAPT

| 模块 | 改动 |
|---|---|
| `src/MappedInputManager.*` | 物理按键 → 触摸分区＋电源键；保留逻辑按钮接口（seam 不变） |
| `src/main.cpp` | `M5.begin()` 顺序、睡眠/唤醒流程、Screenshot 触发方式 |
| `src/util/ScreenshotUtil.h` | 去掉 `BTN_POWER`+`BTN_DOWN` 组合，改触摸手势 |
| `src/components/themes/*`(`BaseTheme`/`LyraTheme`) | 540-px 宽触摸友好按钮提示位置 |
| `platformio.ini` | 新增 `[env:m5papers3]`，S3 板型、PSRAM、依赖 |
| `lib/hal/HalSystem.*` | RISC-V panic handler → Xtensa `XtExcFrame` |
| freeink-sdk `BoardConfig` | 新增 `FREEINK_DEVICE_M5PAPERS3` BoardProfile（几何/显示/触摸/电源） |

### 3.3 REPLACE（最小化，大部分已由 freeink-sdk 承担）

| 模块 | 处置 |
|---|---|
| `lib/hal/HalDisplay.*` | 复用 freeink-sdk `EInkDisplay`→`LgfxEpdDriver`（M5PaperS3 BoardProfile），**接口不变** |
| `lib/hal/HalGPIO.*` | 复用 freeink-sdk `InputManager`（GT911）＋电源键；保留 `FREEINK_DEVICE_M5PAPER` 分支能力 |
| `lib/hal/HalPowerManager.*` | freeink-sdk `PowerManager`：S3 深睡 + G44 关断脉冲 + RTC(BM8563) 唤醒 |
| `lib/hal/HalStorage.*` | SD 走 M5Unified SPI（G47），SdFat 兼容性见 §5 |
| `lib/hal/HalSystem.*` | 仅 panic handler 架构差异（REPLACE 内的一小段） |

---

## 4. 分阶段实施路线图

### Phase 0 — 决策与分支（前置）
- **输入**：确认选择"扩展 freeink-sdk"路径（本计划默认）。
- **输出**：`docs/` 记录选型 ADR；在 `freeink-sdk` 建 `M5PaperS3` 参考 env。
- **验证**：与 upstream 分支策略书面化（见 §6）。

### Phase 1 — 构建基础设施
- **输入**：`platformio.ini`、`sdkconfig.defaults`
- **动作**：
  1. 新增 `[env:m5papers3]`：`esp32-s3-devkitc-1` (或官方 M5PaperS3 板型)，16MB flash，8MB **OPI-PSRAM (quad/octal)**。
  2. `sdkconfig.defaults`：`CONFIG_SPIRAM=1`、`CONFIG_SPIRAM_MODE_OCT`、`CONFIG_SPIRAM_SPEED_80M`、`CONFIG_LWIP` 等。
  3. `lib_deps`：`m5stack/M5Unified`、`m5stack/M5GFX`（freeink-sdk 的 `LgfxEpdDriver` 依赖）、`bblanchon/ArduinoJson @ 7.x`、`ricmoo/QRCode`、`bitbank2/PNGdec`、`bitbank2/JPEGDEC`、`links2004/WebSockets`。**去掉 epdiy**。
  4. 增加 `FREEINK_DRIVER_LGFX_EPD`、`FREEINK_DEVICE_M5PAPERS3` 构建宏。
- **输出**：可在 S3 上编译的最小固件（空显示）。
- **验证**：`pio run -e m5papers3` 通过；烧录后串口正常。
- **验收标准**：构建零错误、PSRAM 开启、无 RISC-V 残留编译错误。

### Phase 2 — HAL 适配
- **输入**：freeink-sdk 的 `LgfxEpdConfig`/`BoardConfig`/`InputManager` 模板；M5PaperS3 官方 pin 表
- **动作**：
  1. **显示**：新增 `M5PaperS3` BoardProfile → `LgfxEpdConfig`（dataPins[8]/SPV/CKV/LE/CL/PWR、busHz、offset_rotation、LUT、电源钩子）。`HalDisplay::begin()/displayBufferAsync()/灰度` 映射到 `LgfxEpdDriver`。
  2. **存储**：`HalStorage` 接 M5Unified SD（G47 CS）；评估 SdFat SPI 兼容。
  3. **系统**：`HalSystem` panic handler 改 Xtensa；`RvExcFrame`→`XtExcFrame`。
  4. **GPIO/电源**：电源键、G44 关断、RTC、电池复用 freeink-sdk。
- **输出**：能点亮 ED047TC1、显示测试图案、SD 可读、时间可读。
- **验证**：单元/实机：显示几何、刷新、睡眠唤醒回路。
- **验收标准**：面板亮起、灰度正常、SD 挂载、RTC 时间正确。

### Phase 3 — 渲染适配
- **输入**：BoardProfile 几何（960×540）
- **动作**：
  1. 确认 `GfxRenderer` 动态几何生效；`DISPLAY_WIDTH/HEIGHT` 由 BoardProfile 提供。
  2. 验证 `GRAYSCALE_LSB/MSB` 与条带灰度在 8-bit canvas 下的映射。
  3. 边距/行距/字体缩放（960×540 比 800×480 各维约 +20%）。
- **输出**：阅读器正文、列表、弹窗正常渲染。
- **验证**：实机截图对比；`ScreenshotUtil` 输出检查。
- **验收标准**：无撕裂、无错位、灰度/刷新模式正确。

### Phase 4 — 输入适配
- **输入**：freeink-sdk `InputManager`（GT911）
- **动作**：
  1. `MappedInputManager` 保留逻辑按钮接口，底层由触摸分区驱动（§4 触摸区设计沿用原草案但要修正：原草案将 960×540 当 portrait 540×960，需与 `offset_rotation` 一致）。
  2. 触摸防抖（见 §5）、swipe/long-press/tap 手势、旋转方向映射。
  3. 移除物理按键组合（Screenshot 改手势）。
- **输出**：全活动可纯触摸操作。
- **验证**：逐活动回归（Home/Reader/Library/Settings/网络）。
- **验收标准**：无幽灵输入、无漏判、旋转下方向正确。

### Phase 5 — 电源与睡眠
- **输入**：freeink-sdk `PowerManager`/`Rtc`/`BatteryMonitor`
- **动作**：
  1. 深睡：`esp_deep_sleep_start()` + 电源键/RTC 唤醒；G44 关断脉冲。
  2. `/sleep` 封面、quick-resume 帧缓冲（`SLEEP_FRAME_FILE` 的 960×540 缓冲写入 SD）。
  3. 电池电量显示（M5Unified Power）。
- **输出**：锁屏/睡眠/唤醒/关机全链路。
- **验证**：实测功耗、唤醒源、RTC 定时唤醒。
- **验收标准**：睡眠电流达标、唤醒恢复原 UI、无掉电丢状态。

### Phase 6 — 回归与 Cherry-Pick 同步（贯穿）
- **输入**：upstream 新提交
- **动作**：按 §6 纪律逐个 cherry-pick；`CROSSPOINT_PAPERS3` gating 统一落地。
- **输出**：同步后的稳定分支。
- **验收**：`git diff <pre-sync>..HEAD --stat` 审计无丢失。

---

## 5. 风险与应对策略

| 风险 | 等级 | 应对 |
|---|---|---|
| **显示缓冲格式**（1-bit 位打包 → 8-bit 灰度 canvas） | 高 | 先验证 `GfxRenderer` 的写缓冲接口与 `LgfxEpdDriver` 的 `LGFX_Sprite`(PSRAM) 对齐；必要时在 HalDisplay 加一层格式转换 adapter。 |
| **灰度支持**（LSB/MSB 双缓冲 → 16 级） | 中 | 复用 `LgfxEpdDriver` 的 16 级灰度；确认 `preconditionGrayscale/displayGrayscaleBase/writeGrayscalePlaneStrip/supportsStripGrayscale` 在 Lgfx 路径的语义。 |
| **触摸防抖**（e-ink 慢刷新导致幽灵输入） | 高 | freeink-sdk `InputManager` 的 debounce；加触摸坐标过滤 + 阈值；`MappedInputManager` 保持 `getHeldTime()` 语义。 |
| **SdFat 兼容性**（跨 `FsHelpers`/SD 字体） | 中 | 优先保留 SdFat 并为其配置 SPI(G47)；若走 M5Unified SD，需在 `HalStorage` 提供 SdFat-compatible 封装层。 |
| **内存优化**（C3 无 PSRAM 的 workaround） | 低 | S3 有 8MB PSRAM，可放宽；但 `FontCacheManager/FontDecompressor` 需显式分配 PSRAM 以省内部 SRAM。 |
| **M5PaperS3 pin 表不一致**（PWR=G45 vs G46） | 中 | 以 M5GFX 源码/M5Stack 原理图为准，写进 BoardProfile 的 `LgfxEpdConfig`。 |
| **Cherry-Pick 冲突纪律** | 高 | 见 §6：禁用 `--theirs`，手工合并 `CROSSPOINT_PAPERS3` 块，翻译 YAML 例外。 |
| **HalSystem Xtensa panic handler** | 中 | 参考 ESP-IDF 的 Xtensa backtrace 接口重写；保留可移植的分界。 |

---

## 6. Cherry-Pick 冲突纪律（沿用并修正）

沿用原草案 §8 的正确原则，并做两处修正：

1. **明确 gating 落地**：设备特有改动统一用 `#if defined(FREEINK_DEVICE_M5PAPERS3)`（而非 `CROSSPOINT_PAPERS3`，后者当前未在任何代码中出现）。建议收敛到 **BoardProfile + 少数共享文件**，减少散落 `#if`。
2. **将"已发生回归"改为"预期冲突点"**：原 §8 列出的 `SleepActivity.cpp`/`BaseTheme.cpp`/`LyraTheme.cpp`/`JpegToBmpConverter.cpp` 是**潜在冲突点**，同步时应重点检查这些文件。

**纪律**：
- 逐个 `git cherry-pick -x <hash>`，commit message 记 `port upstream <hash>`。
- **禁止 `git checkout --theirs <file>`**（会静默丢弃 M5PaperS3 特有块）。
- 冲突先 `git diff --diff-filter=U` 检视，人工合并；`--theirs` 仅限翻译 YAML；`--ours` 仅限有意分歧。
- 合并后 `git diff <pre-sync>..HEAD --stat | grep -E "src/.*\.cpp|lib/.*\.cpp"` 审计。

---

## 7. 验收里程碑总览

| 里程碑 | 判定条件 |
|---|---|
| M1 构建基础设施 | S3 编译通过、PSRAM 开启、无 RISC-V 残留 |
| M2 HAL 点亮 | ED047TC1 显示正常、灰度正确、SD/RTC 可用 |
| M3 渲染适配 | 全活动渲染无错位、刷新模式正确 |
| M4 输入适配 | 纯触摸可操作全活动、无幽灵输入 |
| M5 电源链路 | 睡眠/唤醒/关机/电池全链路达标 |
| M6 同步稳定 | 一次 upstream sync 后无损回归 |