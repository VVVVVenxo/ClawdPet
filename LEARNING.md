# ClawdPet 项目知识地图

> 一个把 Claude Code CLI 的实时状态同步到 M5Stack StickS3 屏幕上的「电子宠物」。
> 这份文档不讲业务，而是把整个项目用到的知识点拆开，方便从零建立完整认知。

项目跨了三个领域：**嵌入式固件（C++/Arduino）**、**电脑端桥接服务（Python）**、**Claude Code 集成（hooks）**。下面按「这块在干什么 → 用到哪些知识点 → 在哪学」的结构展开。

---

## 0. 全局架构：先看懂数据怎么流动

```
                    [你在 VSCode 里用 Claude Code]
                              │  触发 hook 事件 (UserPromptSubmit / Stop / ...)
                              ▼
   clawdpet_hook.py  ── stdin JSON(session_id) ──►  TCP:8787 (localhost)
   (Claude 每次事件启动一个轻量进程)                    │
                              │ 守护没起就自动拉起        ▼
                    clawdpet_daemon.py  (常驻，独占串口)
                              │  复用 TaskStateManager 聚合多窗口状态
                              │  累计今日完成数、token 统计、检测成就、检测节日
                              ▼
                    串口 COM (115200, USB-CDC)
                              │  发送文本协议: working / #work a|b / #today D=5 / #tok IN=...
                              ▼
                    StickS3 固件 (main.cpp)
                              │  解析协议 → 切 GIF 动画 + 重绘 UI 叠加层
                              ▼
                    135x240 屏幕显示宠物 + 状态 (三页面切换)
```

**核心设计思想**（贯穿全项目，值得记住）：
- **进程职责单一**：hook 只「上报」不碰硬件；daemon 独占硬件做「聚合 + 下发」；固件只「显示」。
- **解耦 + 回调注入**：`TaskStateManager` 完全不知道串口的存在，状态变化时调用注入的 `emit` 回调，因此能脱离硬件单元测试。这是依赖倒置（DIP）的典型实践。
- **协议向后兼容**：串口协议是纯文本行，新增命令（`#achv`/`#festival`/`#today`/`#sess`/`#hist`/`#tok`）不破坏旧命令。

---

## 1. 嵌入式固件层（`src/main.cpp` + `platformio.ini`）

### 1.1 硬件 / SoC 基础
| 知识点 | 在本项目的体现 |
|---|---|
| **ESP32-S3 SoC** | 双核 Xtensa LX7，240MHz，本项目 SoC 是 ESP32-S3-PICO-1-N8R8 |
| **Flash vs PSRAM** | N8R8 = 8MB Flash（存固件/资源）+ 8MB PSRAM（存大缓冲，如离屏画布）。两者寻址、速度、用途都不同 |
| **QIO / OPI（Octal）** | Flash 用 QIO（4 线），PSRAM 用 OPI（8 线）。`board_build.psram_type=opi` `memory_type=qio_opi` 必须和实际颗粒对齐，否则跑不起来 |
| **GPIO 复用** | 屏幕/I2C/音频各占固定引脚。做裸 GPIO 实验要避开，否则屏幕变黑 |
| **USB-CDC / USB-Serial-JTAG** | S3 内置 USB 控制器，`Serial` 直接走板载 USB（无需外置 CH340）。PID 恒为 `303A:1001` |

📚 学：乐鑫 [ESP32-S3 技术参考手册]、Arduino-ESP32 文档的 USB CDC 章节。

### 1.2 PlatformIO 构建系统
`platformio.ini` 是整个固件的「项目配置 + 依赖管理 + 烧录配置」三合一文件。要看懂的概念：
- **platform / board / framework** 三层：platform=espressif32（工具链），board=硬件定义，framework=arduino（API 层）。
- **`build_flags`**：编译期宏定义。本项目用 `-DARDUINO_USB_CDC_ON_BOOT=1` 让 Serial 走 USB。
- **`board_build.embed_files`**：把 GIF 文件直接嵌进固件二进制，编译器生成 `_binary_xxx_start/_end` 符号——这就是 main.cpp 里那些 `extern const uint8_t xxx_gif_start[]` 的来源（链接器符号 + asm 重命名）。当前嵌入 16 个 GIF 文件。
- **`lib_deps`**：声明式拉库（M5Unified、AnimatedGIF）。
- **分区表** `default_8MB.csv`：Flash 怎么切（bootloader / app / 文件系统）。

📚 学：PlatformIO 官方文档「Project Configuration File」。

### 1.3 显示与图形渲染（本项目最硬核的部分）
这是「全屏 GIF 动画 + UI 叠加 + 防闪烁」的完整渲染管线：

| 知识点 | 体现 |
|---|---|
| **M5Unified / M5GFX** | M5 的统一硬件抽象库，运行时自动识别板型（屏幕/IMU/电源），`M5.Display` 即屏幕对象 |
| **离屏画布 / 双缓冲** | `M5Canvas g_canvas`（Sprite）在 PSRAM 上开 135x240 缓冲，所有绘制先画到它，最后 `pushSprite` 一次性推屏 → **消除闪烁** |
| **RGB565 色彩** | 16 位色（5R6G5B），`color565(r,g,b)` 转换。纯绿 `(0,255,0)` 的 RGB565 值是 `0x07E0` |
| **RGB565 字节序（大坑）** | AnimatedGIF 库用 `begin(GIF_PALETTE_RGB565_BE)` 输出 Big Endian 像素；但 ESP32 是小端 CPU，读 uint16_t 时字节反转。所以色键比较值 `CHROMA_GREEN` 必须写成 `0xE007`（BE 的 `0x07E0` 被 LE CPU 读出来的值）。如果用了 `_LE` 模式，颜色在 M5GFX 上会 R/B 互换（橙→蓝，褐→粉）；如果用了 `_BE` 模式但色键仍写 `0x07E0`，绿幕就抠不掉 |
| **色键（Chroma Key）抠图** | GIF 用绿幕 `(0,255,0)` 做背景，解码时遇到绿色像素就替换成黑底 → 实现「透明背景宠物」 |
| **GIF 解码** | AnimatedGIF 库逐帧解码，`GIF_DRAW_COOKED` 模式直接吐 RGB565 整行，回调 `gifDraw` 把像素写进行缓冲再 `pushImage` 整行推 |
| **GIF 编码要求** | 嵌入的 GIF 必须统一：`disposal_method=2`（restore to background）+ 设置 `transparency` 索引指向绿色 `(0,255,0)`。如果 disposal=1（leave in place）且无 transparency，AnimatedGIF COOKED 模式帧合成会出错导致颜色错乱 |
| **行缓冲优化** | `g_rowBuf[SCR_W]` 预存一行像素，用 `pushImage` 一次推行，比逐像素 `drawPixel` 快数倍 |
| **裁切 + 偏移** | GIF 是 240 宽，屏幕 135 宽，`CROP_X0=52` 居中裁切；`PET_Y_OFFSET=10` 垂直下移 |
| **帧率控制** | 用 `millis()` 做非阻塞计时，按 GIF 自带的 delay 播放，播完 `reset()` 循环 |
| **动画池随机选取** | 每个状态有多个 GIF 候选（`kIdleGifs[4]`、`kWorkingGifs[4]` 等），切换时 `esp_random()` 选一个 |
| **图形基元绘制** | `drawString / fillRect / drawRoundRect / fillCircle / drawLine` 画状态栏、气泡、电池图标 |
| **文字基准点（datum）** | `top_left / top_right / middle_center` 控制文字对齐 |
| **中文字体** | 详情页用 `fonts::efontCN_12`，UI 叠加层用默认字体（`setFont(nullptr)` 显式重置） |

渲染主循环的核心节奏（`loop()` MAIN 页面）：
```
解一帧 GIF → 行缓冲 pushImage → 画布 → 叠加 UI（状态栏/名字列表/气泡）→ pushSprite 推屏
```

📚 学：M5GFX/LovyanGFX 文档、AnimatedGIF 库 README、RGB565 与双缓冲概念。

> **踩坑记录：GIF 颜色全错乱事件**
>
> 症状：主界面 GIF 动画颜色全部错误——橙色变蓝，褐色变粉；部分 GIF 绿幕没被抠掉。
>
> 根因（两层问题叠加）：
> 1. **字节序错配**：`g_gif.begin(GIF_PALETTE_RGB565_LE)` 输出 LE 像素，但 M5GFX canvas 的 `pushImage` 期望 BE → R/B 通道互换。改为 `GIF_PALETTE_RGB565_BE` 后颜色恢复。
> 2. **色键值未跟着改**：改 BE 后，ESP32（小端 CPU）从内存读 uint16_t 时会把 BE 字节交换，绿色 `0x07E0` 读出来是 `0xE007`，但 `CHROMA_GREEN` 仍是 `0x07E0` → 比较失败 → 绿幕没抠掉。改 `CHROMA_GREEN = 0xE007` 后修复。
> 3. **GIF 编码格式不统一**：用 clawd-emotes-skill 生成的 4 个新 GIF（nap/hammer/cheer/poked）使用了 `disposal=1` 且未设 transparency index，与原始 GIF（`disposal=2` + `transparency=255` 指向绿色）不一致，导致 AnimatedGIF COOKED 模式帧合成异常。用 Python Pillow 重新编码统一格式后修复。
>
> 教训：添加新 GIF 素材时必须确保 disposal_method=2 + transparency 指向绿色；修改字节序参数后必须同步修改色键常量。

### 1.4 三页面 UI 系统

BtnB 循环切换：`MAIN → DETAIL → TOKEN → MAIN`

**MAIN 页面**（有 GIF 动画）：
```
┌─────────────────┐
│ 14:30       87% │  状态栏: 时间(左) + 电池图标+百分比(右)
│ W:2  D:5        │  小字: 工作中数量(琥珀) + 今日完成数(绿)
│ ClawdPet         │  大字 (textSize 2): work+wait 合并列表
│ my-task    ●    │  wait 名闪烁青色(500ms 周期)
│ another         │  超过 3 行时每 2s 轮播
│   🐾 宠物 GIF   │  全屏 GIF 动画 (背景, 绿幕→黑底)
│ ┌─────────────┐ │
│ │ thinking.   │ │  状态气泡 (描边, 不填底色)
│ └─────────────┘ │
└─────────────────┘
```

**DETAIL 页面**（纯静态，每秒刷新时长）：
- session 列表（彩色圆点 + 名称 + 状态标签 + 时长自增）
- 分页翻页（BtnA）
- 底部：今日完成数 + 心情条

**TOKEN 页面**（纯静态，dirty 时刷新）：
- 总量大字（IN+OUT，金黄色，textSize 3）
- IN / OUT / CACHE 明细行（各带颜色标识）
- 数值自动 K/M 缩写

页面切换时：
- 离开 MAIN → `closeGif()` 关闭 GIF 解码器（不释放 buffer）
- 回到 MAIN → `openGifForState()` 重开，`setFrameBuf` 复用同一块 PSRAM

### 1.5 嵌入式编程范式
- **非阻塞状态机 + 协作式调度**：没有 RTOS 任务，全靠 `loop()` 里用 `millis()` 比较时间戳来「轮询多个周期任务」（电量轮询、帧播放、名字轮播、串口读取、心情衰减）。这是单片机里最常用的「超级循环（superloop）」模式。
- **无 RTC 的软时钟**：设备没有实时时钟芯片，靠 daemon 每分钟下发 `#time`，固件用 `millis()` 自走分钟（`currentMinOfDay()`）。
- **手写命令解析**：`handleWork/handleToday/handleAchv/handleSess/handleHist/handleTok` 手动逐字符解析串口文本（嵌入式里常避开重量级解析库省内存）。
- **固定行缓冲**：`pollSerial` 用 `char g_lineBuf[202]` 固定数组累积字符到 `\n`，比 Arduino String 更可控、无碎片化。
- **switch/case 页面分发**：`loop()` 用 `switch(g_page)` 分发到不同页面逻辑，结构清晰。

### 1.6 GIF 内存管理（踩坑与优化）

GIF 解码需要一块 frame buffer（`240*(240+3)` ≈ 58KB），存放合成后的整帧像素。

**原始做法（有坑）**：每次切 GIF 都 `freeFrameBuf()` + `allocFrameBuf()`，反复在 PSRAM 上 malloc/free。
- **PSRAM 碎片化**：频繁分配释放导致碎片，最终 `ps_malloc` 返回 NULL
- **空指针崩溃**：`allocFrameBuf` 失败后未检查返回值，`playFrame` 用 NULL buffer 直接重启
- **戳一戳连按崩溃**：快速 BtnA 连按触发多次 close→open→alloc 循环，加速碎片化

**优化后做法**：
- `setup()` 中一次性 `ps_malloc(FRAME_BUF_SIZE)` 分配 frame buffer
- 所有 GIF 切换用 `setFrameBuf(g_frameBuf)` 复用同一块内存，零运行时分配
- `closeGif()` 只关闭 GIF 解码器，不释放 buffer
- 开 GIF 前检查 `g_frameBuf != nullptr`，防止 PSRAM 分配失败时空指针

**教训**：嵌入式环境中应尽量避免运行时动态分配；同尺寸资源优先复用固定 buffer。

---

## 2. 电脑端桥接层（`tools/*.py`）

### 2.1 进程间通信与并发
| 知识点 | 体现 |
|---|---|
| **TCP socket 服务** | daemon 用 `socket` 监听 `127.0.0.1:8787`，hook 当客户端连进来发 JSON。这是最经典的本地 IPC 方式 |
| **多线程** | 每个连接开一个 `threading.Thread` 处理；另有校时线程、重连线程。用 `RLock`/`Lock` 保护共享状态和串口写 |
| **串口写互斥** | `ser_lock` 保证多线程不会同时写串口 |
| **定时器** | `threading.Timer` 实现「done 脉冲停留 2s」「all_done 后 360s 回落」的延时回落 |
| **守护进程** | hook 用 `subprocess.Popen` + `DETACHED_PROCESS` 标志把 daemon 脱离终端后台拉起 |

### 2.2 串口编程（pyserial）
- `serial.Serial(port, 115200)` 打开，`list_ports.comports()` 按 USB VID（`0x303A` 乐鑫）自动探测端口。
- **Windows 串口独占**：同一 COM 口同时只能一个进程打开 → 这是「必须用单一 daemon」的根本原因。
- **自动重连**：写失败（设备拔出/RESET）捕获 `SerialException`，关句柄、起后台线程每秒轮询重开，重开后 `_resync()` 重发当前状态。这是健壮 IO 的标准模式。

### 2.3 状态聚合的核心算法（`task_state_manager.py`）
这是整个项目的「大脑」，是一个**纯逻辑、可单测**的状态机：
- 每个 Claude session 是一个 task，状态 `pending/running/waiting/done/error`。
- 用**优先级聚合**算出设备该显示什么：`error > choosing > working > all_done > idle`。
- **瞬时脉冲 vs 稳态**：`done` 是瞬时提示（force 发送，定时后 `settle()` 回稳态），区别于持续状态。
- **去抖（debounce）**：状态没变就不重复 emit，避免刷屏。
- **emit 回调注入**：构造时传入 `emit`，逻辑层和串口完全解耦 → 文件底部一大段 `assert` 就是不接硬件的单元测试。

📚 学：状态机设计、依赖注入、Python `unittest`/断言式自测。

### 2.4 扩展功能（daemon 侧）
| 功能 | 实现方式 |
|---|---|
| **今日完成数** | `daily_done` 计数，跨天清零，通过 `#today D=N` 下发 |
| **Token 统计** | 累计 IN/OUT/CACHE 用量，通过 `#tok IN=N OUT=N CACHE=N` 下发 |
| **成就系统** | `Stop` 事件后检查条件（首次完成/日完成数/连续天数/单次 2h），命中发 `#achv NAME`，持久化到 `achievements.json` |
| **节日彩蛋** | `time_loop` 中每分钟检查日期，命中节日表发一次 `#festival name` |
| **session 详情** | 周期性 `#sess` 下发各 session 状态/时长，供详情页渲染 |
| **会话超时清理** | `time_loop` 每分钟扫描 running/waiting session，超 90 秒无事件自动回落 idle |

### 2.5 环境变量向后兼容
hook 读取终端名时按优先级依次查找 `$CLAWDPET_NAME` → `$NOAPET_NAME` → `$NONOPET_NAME`，兼容历史版本的环境变量配置，用户无需修改 shell profile。

---

## 3. Claude Code 集成层（hooks）

这是把「Claude Code 的行为」变成「事件流」的关键，体现了对 Claude Code 扩展机制的理解：

| 知识点 | 体现 |
|---|---|
| **Hooks 机制** | 在 `~/.claude/settings.json` 配 `UserPromptSubmit/Stop/SessionStart/SessionEnd/Notification/PostToolUse`，Claude 在对应时机调用脚本，stdin 传 JSON |
| **hook 必须快速返回** | clawdpet_hook.py 任何异常都 `sys.exit(0)`，绝不阻塞 Claude |
| **事件 → 状态映射** | UserPromptSubmit→working、Stop→done/all_done、Notification(idle_prompt)→idle、choosing→choosing |
| **ESC 中断的坑** | 按 ESC 不触发任何 hook，导致卡 working。恢复机制：(1) `idle_prompt` 通知（60s）(2) daemon 会话超时清理（90s）→ 双重兜底 |
| **session_id 关联** | hook 从 stdin 拿 session_id，daemon 用它区分多窗口 |
| **终端命名链路** | PowerShell `claude-pass <名字>` → `$env:CLAWDPET_NAME` → hook payload → daemon `names` 字典 → `#work` 下发 |

📚 学：Claude Code 官方 hooks 文档（事件类型、matcher、stdin schema）。

---

## 4. 宠物系统设计

### 4.1 心情值
- 范围 0-100，初始 70
- **回升**：工作中每分钟 +2，完成任务 +10，戳一戳 +5
- **衰减**：空闲 30 分钟后开始，每 30 分钟 -10，最低 10
- **状态切换重置计时器**：`applyState()` 中重置 `g_lastMoodDecay`，确保切状态后立即正确计时
- **主界面心情指示**：状态栏时间右侧有彩色圆点（绿 >70 / 黄 30-70 / 红 <30），一眼看到心情
- **表现**：
  - 心情 < 70：IDLE 动画变慢 1.5x
  - 心情 < 30：动画变慢 2x，气泡变灰显示 "bored..." / "miss u" / "lonely"（按心情档位选一次，不每帧闪烁）
- **持久化**：心情值存入 NVS，重启不归零

### 4.2 终端命名
- 名字优先级: `rename` 命令 > `$CLAWDPET_NAME` 环境变量 > cwd 目录名
- rename 通过 `clawdpet_send.py rename "name"` 触发
- daemon 标记被 rename 的 session，后续 hook 事件不会覆盖名字
- SessionEnd 时清理标记

### 4.3 成就系统
| 成就 | 条件 |
|---|---|
| FIRST! | 首次完成任务 |
| HAT TRICK | 单日完成 3 个 |
| ON FIRE | 单日完成 10 个 |
| STREAK x3 | 连续 3 天有工作 |
| MARATHON | 单次连续工作 2h+ |

成就持久化在 `tools/achievements.json`，触发时发 `#achv NAME` 显示金色气泡。

### 4.4 节日彩蛋
daemon 每分钟检查日期，命中节日表（1/1, 2/14, 10/31, 12/25）发一次 `#festival name`，固件弹出节日祝福气泡，持续 5 秒（`FESTIVAL_DURATION_MS`）。

### 4.5 戳一戳互动
BtnA 短按触发：
- 从 6 个反应动画池中随机选一个（eating/exercise/watering/coffee/photo/poked）
- 从 9 个鼓励短语中随机选一个
- 金黄色气泡显示短语，持续 3 秒后自动恢复状态动画
- 心情 +5
- 播放中再按可切换新动画 + 重置计时

### 4.6 NVS 持久化（掉电保存）
设备重启后所有状态保留，用 ESP32 Preferences (NVS) 存储：
- **保存内容**：心情值、今日完成数、当前状态、工作/等待名单、session 详情
- **防抖写入**：数据变化后标记 dirty，5 秒后统一写入 Flash（防止频繁写导致 Flash 磨损）
- **首次脏触发**：计时器从第一次变化开始，后续更新不重置（解决 daemon 持续发数据导致永远存不上的问题）
- **启动恢复**：`setup()` 中 `loadState()` 读回所有数据，`applyState()` 恢复到上次的状态和动画

📚 学：ESP32 Preferences/NVS API、Flash 磨损均衡、防抖 vs 限流的区别。

### 4.7 会话超时清理
- **问题**：按 ESC 暂停 Claude Code 不触发任何 hook，设备卡在 working
- **方案**：daemon 的 `time_loop` 每分钟扫描，session 处于 running/waiting 超过 90 秒无新事件 → 自动标记 idle
- 与 `idle_prompt` 通知（60 秒）互为兜底，谁先触发谁生效

### 4.8 IMU 物理互动
设备内置 IMU（加速度计），启用两种物理交互：

**摇一摇**：
- `M5.Imu.getAccel()` 每帧读取加速度，计算合加速度向量模 `sqrt(x²+y²+z²)`
- 超过 2G 连续 3 帧 → 判定为摇晃
- 触发：随机 GIF + 红色气泡 "dizzy!" / "woah!" / "hey!!" / "stooop!"
- 心情 -5（别摇人家！），3 秒后恢复
- 与 poke/feed 互斥

**长按喂食**：
- `M5.BtnA.wasHold()` 长按超 500ms 触发（`setHoldThresh` 可调）
- 触发：eating.gif + 粉色气泡 "yummy!" / "nom nom!" / "thanks!" / "more!"
- 心情 +20，4 秒后恢复
- 短按仍是戳一戳（`wasClicked` 和 `wasHold` 是独立事件）

📚 学：M5Unified IMU API（`Imu.update()` + `getAccel()`）、按钮状态机（click vs hold）。

---

## 5. 工具链与工作流知识

- **Bash 脚本**（`flash.sh`）：`trap` 清理、后台进程 `&`、子 shell。
- **烧录流程**：原生 USB-CDC 自动复位常超时，需手动进下载模式（按住 BOOT + 点 RESET）。esptool 在 `303A:1001` 模式可直接烧。
- **Git 资源管理**：`.bin`/`.gif` 等大资源、`.pio` 构建产物用 `.gitignore` 排除。
- **跨平台路径**：Windows 下用 bash，注意 `/dev/null`、正斜杠。
- **一键打包**：`pack.bat` 用 Python `zipfile` 模块将必要文件打包为 `ClawdPet.zip`（排除构建产物、IDE 配置、备份文件、日志等）。

---

## 6. 推荐学习路径（从这个项目出发往外扩）

1. **嵌入式入门**：ESP32 + Arduino 框架 → PlatformIO → GPIO/串口 → 显示驱动（M5GFX/LovyanGFX）。
2. **图形渲染**：双缓冲、RGB565、色键、帧动画、行缓冲优化——可迁移到任何小屏 GUI。
3. **Python 服务端**：socket + threading + 锁 → 串口 IO + 自动重连 → 状态机与单元测试。
4. **系统集成思维**：单一职责、解耦/回调注入、协议向后兼容、健壮性（重连/去抖/超时）——这些是工程素养，比具体 API 更值钱。
5. **Claude Code 扩展**：hooks 事件模型，把外部工具接入 AI 工作流。

---

## 7. 一句话总结每个文件

| 文件 | 角色 |
|---|---|
| `platformio.ini` | 固件构建/依赖/烧录配置，嵌入 16 个 GIF 资源 |
| `src/main.cpp` | 固件：GIF 动画池渲染 + 三页面 UI + 串口协议 + 心情系统 + NVS 持久化 + 戳一戳 |
| `src/anim/*.gif` | 16 个嵌入动画（6 状态池 + 戳一戳池） |
| `src/anim/emotes/` | Clawd 像素蟹 emote 源文件 |
| `src/anim/custom/` | 自定义动画 |
| `tools/task_state_manager.py` | 纯逻辑状态聚合大脑（可单测，无硬件依赖） |
| `tools/clawdpet_daemon.py` | 常驻守护：独占串口 + TCP 服务 + 多线程 + 自动重连 + 成就/节日/token/统计 + 会话超时清理 |
| `tools/clawdpet_hook.py` | 轻量桥接：Claude 事件 → TCP 上报，自动拉起 daemon |
| `tools/clawdpet_send.py` | 手动发状态/改名/调试 CLI |
| `tools/debug_notification.py` | 调试 notification 事件的辅助工具 |
| `tools/flash.sh` | 一键烧录（烧录期清场 daemon 释放串口） |
| `tools/download_emotes.py` | 下载 GIF 动画素材 |
| `tools/achievements.json` | 成就持久化数据 |
| `pack.bat` | 一键打包发布用 zip（排除构建产物/日志/备份） |
