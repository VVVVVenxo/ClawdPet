# ClawdPet 社交媒体发布文案

---

## 小红书文案

### 标题
给 AI 编程搭档做了个像素宠物🦀 它能感知你在不在写代码！

### 正文
程序员的桌面小摆件，但它是活的👀

用 M5StickS3 做了一只像素小螃蟹 Clawd，
通过 USB 连接电脑，自动感知你在用 Claude Code 写代码的状态：

🖥️ 你在写代码 → 它在敲键盘/弹吉他/画画（随机！）
⏳ 等你回复 → 它着急闪烁提醒你
✅ 任务完成 → 它开心庆祝
😴 你摸鱼太久 → 它会无聊变灰... "miss u"

还有：
• 24 种像素表情动画（节日彩蛋也有！）
• 心情系统：写代码它开心，摸鱼它难过
• 成就系统：日完成 10 个解锁 "ON FIRE" 🔥
• 按按钮戳它，它会说 "hydrate!" "stretch!"
• 多窗口聚合，支持同时跟踪多个终端

硬件就一个 M5StickS3（¥100左右），烧录好固件 USB 一插就行。
开源了，链接见评论区 👇

### 话题标签
#程序员桌面 #电子宠物 #像素风 #ClaudeCode #M5Stack #开源项目 #程序员的浪漫 #桌面好物 #ESP32 #AI编程

---

## M5Stack 社区文案（英文）

### Title
ClawdPet — A Pixel Desk Pet That Reacts to Your AI Coding Sessions

### Description
ClawdPet is an open-source pixel-art desk pet running on M5StickS3 that syncs with Claude Code (Anthropic's AI coding CLI) in real time.

**What it does:**
- Displays animated pixel crab that reacts to your coding state (idle/working/done/error)
- 16 unique GIF animations with random selection per state
- 3-page UI: Pet view / Session details / Token usage dashboard
- Mood system, achievements, festival surprises, poke interaction
- Multi-window aggregation for multiple Claude Code terminals

**Tech stack:**
- M5Unified + AnimatedGIF library
- Chroma-key compositing (green screen → transparent background)
- USB-CDC serial protocol, 115200 baud
- Python daemon for state aggregation + auto-reconnect
- Claude Code hooks integration

**Hardware:** M5StickS3 (ESP32-S3, 135x240 TFT, 8MB PSRAM)

GitHub: https://github.com/VVVVVenxo/ClawdPet

### Tags
M5StickS3, ESP32-S3, pixel-art, desk-pet, Claude-Code, open-source, AnimatedGIF, USB-CDC

---

## 拍摄建议（实拍素材）

### 必拍镜头：
1. **桌面全景** — 键盘旁边放设备，屏幕上 Claude Code 在跑，设备同步显示 "thinking."
2. **状态切换** — 录一段从 idle → working → done 的完整流程（约 15 秒）
3. **戳一戳** — 手指按按钮，宠物切换动画 + 弹出鼓励气泡
4. **翻页** — 快速按 BtnB 展示三个页面
5. **表情特写** — 设备近景，展示像素动画细节

### 拍摄技巧：
- 背光关掉，靠屏幕自发光最好看（暗环境下像素风超出片）
- 手机竖屏录，适配小红书/抖音
- 开慢动作录按钮交互，后期再加速到 1.5x
- 桌面放点键帽/手办/绿植做氛围装饰
