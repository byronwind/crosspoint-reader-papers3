# AnkiEInk 集成与实现方案

> 基于 CrossPoint PaperS3 Port 项目，参考 crossmux App 架构，面向 M5Stack PaperS3 硬件的 Anki 墨水屏复习终端集成设计。
>
> 关联文档：[AnkiProd.md](../AnkiProd.md)（PRD V1.1）、[PORTING_PLAN_M5PaperS3.md](../PORTING_PLAN_M5PaperS3.md)（移植计划）

---

## 目录

1. [框架分析与借鉴 (crossmux 参考)](#1-框架分析与借鉴-crossmux-参考)
2. [整体架构集成设计](#2-整体架构集成设计)
3. [核心模块技术实现方案](#3-核心模块技术实现方案)
4. [构建、依赖与分区规划](#4-构建依赖与分区规划)
5. [开发路线图](#5-开发路线图)
6. [风险与缓解策略](#6-风险与缓解策略)
7. [实施 Checklist](#7-实施-checklist)

---

## 1. 框架分析与借鉴 (crossmux 参考)

### 1.1 crossmux 项目架构概览

crossmux 是一个成熟的多功能 E-Ink 设备固件，基于同一套 freeink-sdk，其 App 架构设计可直接借鉴。

**目录结构**

```
crossmux/src/
├── activities/              # 所有 UI 屏幕（类比 Android Activity）
│   ├── Activity.h           # 基类：onEnter/onExit/loop/render 生命周期
│   ├── ActivityManager.h    # 栈式管理器：push/pop/replace + 渲染互斥
│   ├── ActivityResult.h     # 子 Activity 回传结果
│   ├── MainTab.h            # 主 Tab 枚举 + 导航逻辑
│   ├── RenderLock.h         # 渲染锁（FreeRTOS Mutex 保护帧缓冲）
│   ├── boot_sleep/          # 启动/休眠
│   ├── home/                # 主菜单、文件浏览、最近书籍
│   ├── reader/              # EPUB/TXT/XTC 阅读器
│   ├── settings/            # 设置页面集合
│   ├── network/             # Web 服务器、WiFi 选择
│   ├── browser/             # OPDS 浏览器
│   ├── apps/                # 小游戏/工具（2048、数独、推箱子等）
│   └── util/                # 确认框、全屏消息等通用 Activity
├── components/themes/       # 多主题（Lyra/Inx/RoundedRaff）
├── network/                 # Web 服务器、OTA、WebDAV
├── util/                    # 工具类
├── main.cpp                 # 入口：setup()/loop() + 全局对象
├── CrossPointState.h        # 全局状态（PersistableStore 模式）
├── CrossPointSettings.h     # 设置持久化
└── MappedInputManager.h     # 输入抽象层
```

### 1.2 关键设计模式与迁移映射

| 模式 | crossmux 实现 | AnkiEInk 借鉴方式 |
|------|-------------|-----------------|
| **Activity 生命周期** | `Activity` 基类：`onEnter()/onExit()/loop()/render(RenderLock&&)` | 直接复用，新增 `ReviewActivity`、`DeckListActivity` 等 |
| **栈式导航** | `ActivityManager` 管理栈，`replaceActivity/pushActivity/popActivity` | 直接复用，Anki 各页面作为 Activity 入栈 |
| **渲染互斥** | `RenderLock` RAII 锁 + `requestUpdateAndWait()` 阻塞等待 | 直接复用，确保 FSRS 计算与 UI 渲染不冲突 |
| **全局单例 Store** | `PersistableStore<T>` 基类，JSON 序列化到 SD 卡 | 新增 `AnkiState`（复习会话）、`FsrsConfig`（FSRS 参数） |
| **MainTab 导航** | `MainTab` 枚举 + `MainTabFocus` 切换焦点 | 扩展为 `{Recent, Library, Anki, Apps, Settings}` |
| **输入映射** | `MappedInputManager` 映射为 `Button::{Up,Down,Left,Right,Select,Back,Power}` | 直接复用，复习评分按钮映射为触摸区域 |
| **Web 服务器** | `CrossPointWebServer` 基于 `WebServer` + `WebSocketsServer` | 扩展 RESTful API（`/api/decks`、`/api/settings/fsrs` 等） |
| **多 App 插件** | `apps/` 目录每个 App 独立子目录 | Anki 模块作为 `apps/anki/` 或 `activities/anki/` 加入 |

### 1.3 迁移适配策略

1. **Activity 框架**：从 crossmux 复制 `Activity.h`/`ActivityManager.h`/`ActivityResult.h`/`MainTab.h`/`RenderLock.h` 到当前项目 `src/activities/`，无需修改——它们不依赖特定设备。
2. **Store 模式**：`PersistableStore<T>` 在 `lib/Serialization/` 中，AnkiEInk 的 `FsrsConfig`、`AnkiState`、`DeckStore` 均继承此模板。
3. **Web 服务器**：在现有 `CrossPointWebServer` 基础上追加 RESTful 路由，或新建独立 `AnkiWebServer` 类。
4. **主题/组件**：复用现有 `BaseTheme` 接口，新增 Anki 专用 UI 元素（评分按钮、进度条、统计图表）。

---

## 2. 整体架构集成设计

### 2.1 分层架构图

```
┌─────────────────────────────────────────────────────────────────────┐
│                    应用层 (AnkiEInk Activities)                      │
│  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐ ┌────────────┐ │
│  │ReviewActivity│ │DeckListAct.  │ │WebAdminAct.  │ │StatsAct.   │ │
│  └──────┬───────┘ └──────┬───────┘ └──────┬───────┘ └─────┬──────┘ │
├─────────┼────────────────┼────────────────┼───────────────┼────────┤
│         │         业务逻辑层 (Business Logic)               │        │
│  ┌──────┴───────────────────────────────────────────────┐          │
│  │  FSRS 调度引擎 (lib/fsrs/)                           │          │
│  │  FsrsScheduler / FsrsOptimizer / FsrsQueue           │          │
│  └──────────────────────────────────────────────────────┘          │
│  ┌──────────────────────────────────────────────────────┐          │
│  │  牌组管理模块 (lib/anki/)                            │          │
│  │  ApkgImporter / CardRenderer / DeckStore / HtmlToEpd │          │
│  └──────────────────────────────────────────────────────┘          │
├─────────────────────────────────────────────────────────────────────┤
│  服务层: HTTP Server / WebSocket / WiFi Mgr / NTP / Power Mgr      │
├─────────────────────────────────────────────────────────────────────┤
│  数据层: SQLite (SD card, rollback journal) / NVS (settings) / SD (media)      │
├─────────────────────────────────────────────────────────────────────┤
│  HAL (freeink-sdk): HalDisplay / HalGPIO / HalPower / HalStorage   │
├─────────────────────────────────────────────────────────────────────┤
│  平台层: ESP-IDF / FreeRTOS / Arduino / PSRAM                      │
└─────────────────────────────────────────────────────────────────────┘
```

### 2.2 新增模块目录规划

```
lib/
├── fsrs/                    # FSRS V5 算法引擎
│   ├── include/
│   │   ├── FsrsScheduler.h  # 评分→间隔计算
│   │   ├── FsrsOptimizer.h  # 参数优化（梯度下降）
│   │   ├── FsrsQueue.h      # 每日复习队列生成
│   │   └── FsrsTypes.h      # CardState, Rating 等类型
│   └── src/
├── anki/                    # APKG 解析与牌组管理
│   ├── include/
│   │   ├── ApkgImporter.h   # ZIP 流式解析 + SQLite 导入
│   │   ├── CardRenderer.h   # HTML→E-Ink 位图渲染
│   │   ├── DeckStore.h      # SQLite DAO 封装
│   │   ├── AnkiDbSchema.h   # 建表 SQL
│   │   └── HtmlToEpd.h      # 简化 HTML 解析器
│   └── src/

src/activities/anki/         # Anki UI Activities
├── ReviewActivity.h/cpp     # 复习主界面
├── DeckListActivity.h/cpp   # 牌组列表/选择
├── DeckImportActivity.h/cpp # APKG 导入进度
├── StatsActivity.h/cpp      # 复习统计
├── FsrsSettingsActivity.h/cpp # FSRS 参数设置
└── WebAdminActivity.h/cpp   # Web 管理入口
```

### 2.3 FreeRTOS 多任务分配

| 任务名称 | 绑定核心 | 优先级 | 栈大小 | 实现方式 | 说明 |
|----------|----------|--------|--------|---------|------|
| `main_loop_task` | Core 1 | 1 | 8192 | 现有 `loop()` | 主事件循环 |
| `gui_task` | Core 1 | 3 | 16384 | `ActivityManager::renderTaskTrampoline` | E-Ink 界面渲染 |
| `httpd_task` | Core 0 | 5 | 8192 | `esp_httpd` 自带 | HTTP 服务器 |
| `ws_server_task` | Core 0 | 4 | 4096 | `xTaskCreatePinnedToCore` | WebSocket 推送 |
| `wifi_event_task` | Core 0 | 6 | 4096 | ESP-IDF 内部 | WiFi 事件 |
| `fsrs_task` | Core 1 | 2 | 8192 | 按需创建/销毁 | FSRS 计算 + 参数优化 |
| `db_task` | Core 1 | 2 | 8192 | 按需创建/销毁 | SQLite 读写 |
| `touch_task` | Core 0 | 4 | 2048 | `InputManager` 内部 | GT911 I2C |
| `power_task` | Core 0 | 1 | 2048 | `PowerManager` 内部 | 电源监测 |

**设计要点**：GUI 渲染优先级最高(3)，FSRS/DB 为后台(2)不阻塞 UI。FSRS 和 DB 按需创建任务，完成后 `vTaskDelete(NULL)` 释放内存。

---

## 3. 核心模块技术实现方案

### 3.1 数据存储：SQLite 集成

**库选择**：[esp32_arduino_sqlite3_lib](https://github.com/siara-cc/esp32_arduino_sqlite3_lib)（ESP32 优化，自带 ESP32 VFS 与 shox96/unishox 压缩函数）。

**编译配置**（`m5papers3-anki` 环境 `build_flags`，经 Sqlite3Esp32 的 `config_ext.h` 生效）：

```
-DSQLITE_OMIT_UTF16          # 去掉 UTF-16 支持
-DSQLITE_OMIT_LOAD_EXTENSION # 禁用扩展加载
-DSQLITE_OMIT_COMPLETE       # 禁用 sqlite3_complete
```

> 注意：不要定义 `SQLITE_OMIT_VIRTUALTABLE` —— sqlite3.c 会把它映射为
> `SQLITE_OMIT_ALTERTABLE`（L14040-14043），parser 规则因此引用未定义符号，链接失败。

> 注意：该库以 `SQLITE_OMIT_WAL=1` 编译（`config_ext.h`），`PRAGMA journal_mode=WAL` 无法启用；
> 且其自带 ESP32 VFS 基于 `fopen`，无法访问 SdFat 挂载的 SD 卡。因此本项目实现了
> `lib/anki/src/SdFatVfs.cpp` —— 将 `sqlite3_vfs` 桥接到 `SDCardManager` 的 FsVolume。
> 数据库使用 rollback journal 模式（低频复习写入场景下足够），WAL 留待 Phase 2 优化。

**存储位置**：SD 卡 `/AnkiEInk/anki.db`（SdFat 路径从卷根开始，无 `/sd` 挂载前缀）。

**打开方式**：

```c
// DeckStore::init 内部自动注册 SdFat VFS 后直接按路径打开
registerSdFatVfs();                       // 幂等，SdFat VFS 设为默认
sqlite3_open("/AnkiEInk/anki.db", &db);  // 文件 I/O 全部经 SdFat FsFile
sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", ...);
```

**掉电保护**：rollback journal + `synchronous=NORMAL` 保证事务原子性；每日凌晨自动备份到 SD 卡 `/AnkiEInk/backup/`；启动时 `PRAGMA integrity_check` 检测损坏。

**SD 卡媒体分桶**：Phase 0 平铺存储到 `/AnkiEInk/media/`，分桶（`media/{xx}/filename`）留待 Phase 3 媒体管理。

### 3.2 APKG 解析

**现有基础**：`lib/ZipFile/`（ZIP 中央目录 + 流式解压，`readFileToStream`/`readFileToMemory`）。

**解析流程**（`lib/anki/src/ApkgImporter.cpp` 已实现）：

```
.apkg (ZIP)
├── collection.anki2  → 流式解压到 /AnkiEInk/tmp/import.anki2 → SQLite 只读打开
├── media             → JSON 映射 {序号: 文件名}
└── 0, 1, 2, ...      → 媒体文件 → SD 卡 /AnkiEInk/media/
```

**Anki Schema → 本地 Schema 映射**：

| Anki 源表 | 本地目标 | 映射逻辑 |
|-----------|---------|---------|
| `cards` (id,nid,did,queue,type,due,reps,lapses) | `cards` + `card_states` | 拆为内容 + FSRS 状态；非新卡继承 ivl/reps/lapses，due ≈ now + ivl 天 |
| `notes` (id,mid,flds,tags) | `cards.front_html, back_html` | `flds` 按 `\x1f` 分隔：字段 0 → 正面，字段 1 → 背面（模板渲染留待 Phase 1） |
| `col` (conf JSON) | NVS `fsrs` 命名空间 | 提取 w 向量、request_retention（Phase 1） |
| 媒体文件 | `media` 表 + SD 卡 | 拷贝到 `/AnkiEInk/media/`，文件名防路径穿越 |

**分批导入**：每批 100 张卡片，事务提交，PSRAM 占用有界。

### 3.3 FSRS 算法移植

**核心**：13 维参数向量 `w[0..12]`，计算初始稳定性、难度、稳定性更新、间隔。

**FPU 加速**：全部使用 `float`（非 `double`），ESP32-S3 Xtensa LX7 硬件单精度 FPU 直接执行，单次计算 < 50ms（实测 ~5ms）。

**参数优化器**：在 Core 1 后台任务执行梯度下降，最小化 MSE（log-loss），需 > 1000 条复习记录。迭代 100 轮或收敛后通过 callback 返回结果。

**参考实现**：[fsrs-rs](https://github.com/open-spaced-repetition/fsrs-rs)（Rust，逻辑清晰可翻译为 C++）。

### 3.4 E-Ink UI 与渲染

**渲染管线**：

```
卡片 HTML → HtmlToEpd（简化 DOM）→ 布局引擎 → GfxRenderer 绘制命令 → 帧缓冲 → HalDisplay → E-Ink
```

**HtmlToEpd**：基于 `lib/expat/` 的简化 HTML 解析器，支持 `b/i/u/br/p/div/span/img/table/ul/ol/li`，忽略 `script/style`，图片通过 `MediaResolver` 从 SD 卡加载并抖动转 16 灰阶。

**刷新模式策略**：

| 场景 | 模式 | 实现 |
|------|------|------|
| 菜单切换 / 卡片正面 | GC16 | `displayBuffer(FULL_REFRESH)` |
| 卡片翻转（门帘） | DU4/GL16 | 逐条带差分刷新 |
| 评分按钮按下 | A2 | `displayBufferAsync(FAST_REFRESH)` |
| 每 5 次 A2 后 | GC16 | 计数器触发全刷 |

**门帘翻页动效**：从中间向上下展开，每次刷新 32px 高条带，使用 DU4 差分模式，`delay(20)` 控制速度，约 30 帧完成全页。利用现有 `GfxRenderer::writeGrayscalePlaneStrip()` + `displayGrayscaleBase()` 机制。

### 3.5 Web 管理后台

**HTTP 服务器**：复用 `esp_http_server`，新增 RESTful 路由（完整 API 列表见 PRD §9.2）。

**前端打包**：复用 `scripts/build_html.py` 模式，将 SPA 打包为单个 gzip 文件存储于 `web_res` FAT 分区（1MB），运行时 `Content-Encoding: gzip` 直接发送。前端技术栈：Tailwind CSS (JIT 精简) + Chart.js (精简) + 原生 JS，gzip 后 < 200KB。

**WebSocket 事件**：复用 `WebSocketsServer`，新增 `sync_progress`、`device_status`、`import_complete` 等事件类型。

---

## 4. 构建、依赖与分区规划

### 4.1 platformio.ini 新增配置

```ini
[env:m5papers3-anki]
extends = env:m5papers3
board_build.partitions = partitions_ankieink.csv

build_flags =
  ${env:m5papers3.build_flags}
  -DANKIEINK=1
  ; Sqlite3Esp32 以 SQLITE_OMIT_WAL / SQLITE_TEMP_STORE=1 编译（config_ext.h），
  ; SD 卡存储采用 rollback journal，无需 WAL 相关宏
  -DSQLITE_OMIT_UTF16
  -DSQLITE_OMIT_LOAD_EXTENSION
  ; 不要定义 SQLITE_OMIT_VIRTUALTABLE：sqlite3.c 会间接 OMIT ALTERTABLE，
  ; 导致 parser 符号未定义（链接失败）
  -DSQLITE_OMIT_COMPLETE
  -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES=1

lib_deps =
  ${env:m5papers3.lib_deps}
  https://github.com/siara-cc/esp32_arduino_sqlite3_lib.git
```

> 数据库文件 I/O 不经过该库自带的 ESP32 VFS（基于 `fopen`，无法访问 SdFat 卷），
> 而是由 `lib/anki/src/SdFatVfs.cpp` 注册的自定义 `sqlite3_vfs`（"sdfat"）桥接到
> `SDCardManager` 的 FsFile，注册为默认 VFS 后 `sqlite3_open("/AnkiEInk/anki.db")` 直接可用。

### 4.2 分区表 (partitions_ankieink.csv)

```csv
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x5000,
otadata,  data, ota,     0xe000,   0x2000,
app0,     app,  ota_0,   0x10000,  0x640000,  # 6.25MB OTA slot 0
app1,     app,  ota_1,   0x650000, 0x640000,  # 6.25MB OTA slot 1
web_res,  data, fat,     0xc90000, 0x100000,  # 1MB Web 前端 (FAT)
coredump, data, coredump, 0xf20000, 0x10000,   # 64KB
nvskey,   data, nvs_keys, 0xf30000, 0x4000,    # 16KB
```

**容量评估**：app0/app1 各 6.25MB（与原始 partitions.csv 一致，OTA 空间充足）；web_res 1MB（gzip SPA < 200KB，余量充足）；SQLite DB 存于 SD 卡，无 Flash 容量限制。

### 4.3 新增依赖清单

| 依赖 | 版本 | 用途 | 状态 |
|------|------|------|------|
| `esp32_arduino_sqlite3_lib` | GitHub latest | SQLite 数据库引擎（SdFatVfs 桥接） | 已集成 |
| `ZipFile` | 内置 `lib/ZipFile/` | APKG (ZIP) 容器解析 | 已有 |
| `miniz` | 内置 `lib/miniz/` | ZIP 流式解压（ZipFile 内部） | 已有 |
| `ArduinoJson` | 7.4.2 | JSON 序列化 | 已有 |
| `WebSockets` | 2.7.3 | WebSocket 推送 | 已有 |
| `expat` | 内置 `lib/expat/` | HTML/XML 解析 | 已有 |
| `PNGdec` | 1.1.6 | PNG 解码 | 已有 |
| `JPEGDEC` | git 指定 | JPEG 解码 | 已有 |

---

## 5. 开发路线图

### Phase 0: 环境搭建 (Week 1)

| 里程碑 | 交付物 | 验收标准 |
|--------|--------|---------|
| M0-T1 | `[env:m5papers3-anki]` 构建环境 | `pio run` 零错误 |
| M0-T2 | `partitions_ankieink.csv` 烧录 | FAT 分区挂载正常 |
| M0-T3 | SQLite CRUD + journal 模式验证 | 经 SdFat VFS 读写 `/AnkiEInk/anki.db` 正常（rollback journal） |
| M0-T4 | FSRS V5 C++ 移植 + 单元测试 | 47 个 gtest 用例全部通过（与 fsrs-rs 参考输出一致） |
| M0-T5 | APKG 解析原型 | 解压 .apkg → 读取 collection.anki2 → 导入 cards/notes/media |

### Phase 1: 核心 MVP (Week 2-4)

| 里程碑 | 交付物 | 依赖 |
|--------|--------|------|
| M1 | Activity 框架引入 | M0 |
| M2 | ReviewActivity（硬编码测试卡片） | M1 |
| M3 | DeckStore SQLite DAO | M0-T3 |
| M4 | FsrsScheduler 集成 | M0-T4, M3 |
| M5 | ApkgImporter 从 SD 卡导入 | M0-T5, M3 |
| M6 | FsrsQueue 每日调度 | M4 |
| M7 | 门帘翻页动效 | M2 |
| M8 | 复习撤回功能 | M2 |
| M9 | 基础设置 E-Ink 页面 | M1 |
| M10 | 集成测试 | M1-M9 |

### Phase 2: Web 管理后台 (Week 5-7)

| 里程碑 | 交付物 | 依赖 |
|--------|--------|------|
| M11 | AnkiWebServer 框架 + Basic Auth | M0 |
| M12 | 牌组管理 API + 前端 | M11, M5 |
| M13 | FSRS 参数 API + 前端 | M11, M4 |
| M14 | WebSocket 事件推送 | M11 |
| M15 | 仪表盘 + 统计页面 | M11 |
| M16 | OTA 更新 | M11 |
| M17 | 设备设置页面 | M11 |
| M18 | 响应式适配 + 暗色主题 | M15 |
| M19 | Web + 设备协同测试 | M11-M18 |

### Phase 3: 优化与功能补全 (Week 8-10)

| 里程碑 | 交付物 |
|--------|--------|
| M20 | 功耗优化（深睡 < 500uA，定时 WiFi） |
| M21 | E-Ink 波形优化（残影控制） |
| M22 | 启动速度 < 3 秒 |
| M23 | 卡片渲染美化 |
| M24 | 触摸响应 < 50ms |
| M25 | FSRS 参数优化算法 |
| M26 | CSV 统计导出 |
| M27 | 媒体文件管理 |
| M28 | 多语言框架 (i18n) |

### Phase 4: 测试与发布 (Week 11-12)

| 里程碑 | 交付物 |
|--------|--------|
| M29 | 全面测试（单元/集成/性能/功耗/兼容性） |
| M30 | Bug 修复 |
| M31 | 用户文档 + 开发者文档 |
| M32 | V1.0 发布 |

---

## 6. 风险与缓解策略

| 风险 | 等级 | 缓解策略 |
|------|------|---------|
| **PSRAM 内存泄漏** | 高 | 每次评分后监控 `ESP.getFreePsram()`；APKG 流式分批导入（100 张/批）；`FontCacheManager` 设 PSRAM 上限 |
| **E-Ink 残影** | 高 | 严格"5 次 A2 → 1 次 GC16"策略；门帘动效用 DU4 掩盖过渡残影；Phase 3 专项 LUT 调优 |
| **APKG 兼容性** | 高 | V1.0 先支持基础格式（纯文本+图片），覆盖 95% 牌组；不支持标签降级纯文本；建立 10+ 主流牌组测试集 |
| **SQLite 掉电损坏** | 中 | rollback journal + `synchronous=NORMAL`（WAL 被 SQLITE_OMIT_WAL 排除，留待 Phase 2 VFS v2 + shm）；每日 SD 备份；启动 integrity_check |
| **Flash 空间不足** | 中 | Web 前端极致压缩（< 200KB gzip）；OTA 分区可略小于 factory；大牌组媒体存 SD 卡 |
| **FSRS 浮点精度** | 低 | 全部 `float` + FPU 硬件加速；与 Python 参考实现交叉验证；参数边界检查 |
| **IT8951 驱动不稳定** | 中 | Phase 0 充分验证 M5GFX Panel_EPD；刷新失败自动软复位 IT8951；fallback 到 GC16 全刷 |

---

## 7. 实施 Checklist

### Phase 0: 环境搭建 (Week 1)

- [x] 新建 `[env:m5papers3-anki]` 构建环境，集成 sqlite3，编译通过（Flash 84.6%）
- [x] 创建 `partitions_ankieink.csv`（无 DB 分区，DB 存 SD 卡；web_res FAT 挂载验证随 Phase 2）
- [x] SQLite CRUD + rollback journal 经 SdFatVfs 在 SD 卡路径实现（`/AnkiEInk/anki.db`），编译通过，运行时验证待首次烧录
- [x] FSRS V5 核心公式 C++ 移植完成，`test/fsrs_scheduler` 47 个 gtest 用例全部通过
- [x] APKG 解析原型：`ApkgImporter` 解压 .apkg → 读取 collection.anki2 → 分批导入 cards/notes + 媒体文件

### Phase 1: 核心 MVP (Week 2-4)

- [ ] 从 crossmux 引入 Activity 框架（Activity.h / ActivityManager.h / RenderLock.h / MainTab.h）
- [ ] MainTab 枚举扩展，新增 Anki Tab
- [ ] ReviewActivity 实现：硬编码测试卡片，完整复习流程（正面 → 翻转 → 评分 → 下一张）
- [ ] DeckStore (SQLite DAO)：cards / card_states / review_logs 表 CRUD
- [ ] FsrsScheduler 集成：评分触发 FSRS 计算，更新 card_states，计算下次到期
- [ ] ApkgImporter 完善：从 SD 卡导入 .apkg，卡片内容 + 媒体文件全链路
- [ ] FsrsQueue 每日调度：到期卡片排序、新卡片/复习上限控制
- [ ] 门帘翻页动效（DU4/GL16 条带刷新，从中间向上下展开）
- [ ] 复习撤回功能（操作快照 + 回滚，3 秒内可撤回）
- [ ] Phase 1 集成测试：导入 → 复习 → 统计 全链路通过

### Phase 2: Web 管理后台 (Week 5-7)

- [ ] AnkiWebServer 框架搭建，RESTful 路由注册，Basic Auth 鉴权
- [ ] 牌组管理 API：上传 APKG（分片）、列表、详情、删除
- [ ] 牌组管理前端页面（上传进度 + WebSocket 推送）
- [ ] FSRS 参数 API：读取/更新参数向量、触发优化、恢复默认
- [ ] FSRS 参数前端页面（滑块调参 + 参数向量编辑）
- [ ] WebSocket 事件推送：sync_progress / device_status / import_complete
- [ ] 仪表盘页面：电量、存储、今日进度、30 天趋势图
- [ ] 统计页面：每日复习量柱状图、评分分布、CSV 导出
- [ ] OTA 更新：Web 上传 .bin + URL 下载 + 进度显示
- [ ] 设备设置页面：WiFi / 时区 / 电源 / 复习限制
- [ ] 前端响应式适配（320px ~ 1920px）+ 暗色主题
- [ ] Phase 2 集成测试：Web + 设备协同操作无冲突

### Phase 3: 优化与功能补全 (Week 8-10)

- [ ] 深度休眠优化：电流 < 500uA，触摸唤醒恢复原 UI
- [ ] 定时 WiFi 策略：凌晨自动同步，15 分钟无操作关闭
- [ ] E-Ink 波形调优：残影控制、刷新策略参数调优
- [ ] 启动速度优化：冷启动 < 3 秒到主菜单
- [ ] 卡片渲染美化：排版、字体回退策略、图片抖动算法
- [ ] 触摸响应优化：延迟 < 50ms
- [ ] FSRS 参数优化算法：后台梯度下降，需 > 1000 条记录
- [ ] CSV 统计报告导出
- [ ] 媒体文件管理：清理孤立文件、预览

### Phase 4: 测试与发布 (Week 11-12)

- [ ] 全面测试：单元测试 / 集成测试 / 性能基准 / 功耗基准 / APKG 兼容性
- [ ] Bug 修复（目标：P0 全部关闭，P1 < 5 个遗留）
- [ ] 用户文档（使用说明书）+ 开发者文档（API + 架构）
- [ ] V1.0 发布固件构建 + GitHub Release

---

## 附录 A：与现有 CrossPoint 功能的共存

AnkiEInk 作为 CrossPoint 电子书阅读器的**并行功能模块**：

- **Activity 共存**：`MainTab` 扩展 `{Recent, Library, Anki, Apps, Settings}`
- **存储共存**：CrossPoint 使用 `/.crosspoint/`，AnkiEInk 使用 `/AnkiEInk/`
- **Flash 分区共享**：`app0/app1` 包含两个 OTA 固件槽，SQLite DB 存储在 SD 卡（无专用 Flash 分区）
- **构建切换**：`-DANKIEINK=1` 控制编译开关；Anki 源码（`lib/anki/`、`src/activities/anki/`）整体以 `#ifdef ANKIEINK` 守护，非 Anki 构建编译为空翻译单元，避免 LDF 拉入 sqlite3 依赖

## 附录 B：关键参考链接

- FSRS V5 算法：https://github.com/open-spaced-repetition/fsrs4anki
- FSRS C++ 参考：https://github.com/open-spaced-repetition/fsrs-rs
- Anki APKG 格式：https://github.com/ankidroid/Anki-Android/wiki/Database-Structure
- [esp32_arduino_sqlite3_lib](https://github.com/siara-cc/esp32_arduino_sqlite3_lib)
- M5Stack PaperS3 文档：https://docs.m5stack.com/en/core/PaperS3
- PortableAnki 参考：https://github.com/zykkkl/PortableAnki
