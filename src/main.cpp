// ============================================================================
// ClawdPet - Claude Code CLI 状态电子宠物
// M5Unified + 竖屏 135x240 + USB Serial(115200) + 全屏 GIF 动画 + UI 叠加层
//
// 4 个状态各一段 GIF (240x240, 像素风), 嵌入固件, AnimatedGIF 实时解码到
// PSRAM 上的 135x240 离屏画布。每帧解码后在画布上叠加 UI, 再整帧推屏:
//   y:0-20    状态栏  (左:24h 时间 HH:MM   右:电池图标+百分比)
//   y:20-84   计数区  (TOTAL / WORK / DONE 三行, 不透明字底防糊)
//   y:84-150  气泡区  (随状态变色圆角气泡 + 短语, 尾巴指向宠物)
//   全屏      GIF 宠物背景 (UI 之外区域可见)
//
// 状态: IDLE / WORKING / CHOOSING / DONE / ALL_DONE / ERROR  (默认 IDLE)
// 串口协议 (整行, \n 结束):
//   <state>            idle|working|choosing|done|all_done|error -> 切气泡/动画
//   #stat T=3 W=1 D=2  刷新计数 (不动动画)
//   #work a|b|c        当前正在工作的终端名列表 (| 分隔, 空=无人工作)
//   #wait a|b|c        当前在等用户选择/输入的终端名列表 (choosing 气泡轮播)
//   #time HH:MM        校时(24h); 设备无 RTC, 由 daemon 下发
//   ping               -> pong
// ============================================================================

#include <M5Unified.h>
#include <AnimatedGIF.h>
#include <Preferences.h>

enum class PetState { IDLE, WORKING, CHOOSING, DONE, ALL_DONE, ERROR };
static const int STATE_COUNT = 6;

static PetState g_state = PetState::IDLE;
static char     g_lineBuf[202];
static int      g_lineLen = 0;

// --- 屏幕 / GIF 几何 ---
static const int SCR_W   = 135;
static const int SCR_H   = 240;
static const int GIF_W   = 240;                  // GIF 原始宽
static const int CROP_X0 = (GIF_W - SCR_W) / 2;  // 居中裁切左偏移 = 52

// GIF 用绿幕 (0,255,0) 做透明/背景色键, 在屏上替换为该背景色。
static const uint16_t CHROMA_GREEN = 0xE007;     // RGB565 BE of (0,255,0) read as LE uint16_t
static const uint16_t BG_COLOR     = TFT_BLACK;  // 背景色 (绿幕替换为此色)
static const int      PET_Y_OFFSET = 10;         // 宠物整体下移像素 (底部超出部分裁掉)

// --- UI 叠加层布局 ---
static const int STATUS_Y  = 0;
static const int STATUS_H  = 20;
static const int COUNT_Y   = 22;
static const int BUBBLE_Y  = 101;   // 气泡下移 7
static const int BUBBLE_H  = 150 - 84;   // = 66

// 气泡短语 + 短语文字色 (按状态; 气泡不填充底色, 仅描边)
struct StateStyle { const char* name; const char* phrase;
                    uint8_t tr, tg, tb; };
static const StateStyle kStyles[STATE_COUNT] = {
  // name        phrase          text(tr,tg,tb)
  { "IDLE",     "waiting.",   255, 255, 255 },  // 白
  { "WORKING",  "thinking.",  230, 170,   0 },  // 同 WORK 琥珀
  { "CHOOSING", "YOUR TURN",    0, 200, 255 },  // 亮青(无等待名时的回落文案)
  { "DONE",     "one done",      0, 200,  90 },  // 同 DONE 绿
  { "ALL DONE", "complete",      0, 200,  90 },  // 绿
  { "ERROR",    "break",255, 220,  60 }, // 黄(可读)
};

// 计数 (由 #stat 更新)
static int g_total = 0, g_work = 0, g_done = 0;

// 正在工作的终端名 (由 #work 更新)
static const int      MAX_WORK_NAMES = 8;
static String         g_workNames[MAX_WORK_NAMES];
static int            g_workCount    = 0;

// 主界面名字轮播
static int            g_infoCycle    = 0;
static uint32_t       g_lastCycleMs  = 0;
static const uint32_t INFO_CYCLE_MS  = 2000;

// 正在等用户选择/输入的终端名 (由 #wait 更新); choosing 气泡内轮播, 每 1s 切一个
static String         g_waitNames[MAX_WORK_NAMES];
static int            g_waitCount       = 0;
static int            g_waitCycle       = 0;
static uint32_t       g_lastWaitCycleMs = 0;
static const uint32_t WAIT_CYCLE_MS     = 1000;

// 时间 (由 #time 校准; 设备无 RTC, 用 millis() 自走分钟)
static bool     g_timeValid    = false;
static int      g_baseMinOfDay = 0;     // 上次校时对应的当日分钟数 0..1439
static uint32_t g_baseMillis   = 0;

// 电量 (固件本地轮询)
static int      g_battLevel  = -1;
static bool     g_charging   = false;
static uint32_t g_lastBattMs = 0;
static const uint32_t BATT_POLL_MS = 5000;

// --- 页面切换 (BtnB 循环: MAIN -> DETAIL -> TOKEN -> MAIN) ---
enum class Page { MAIN, DETAIL, TOKEN };
static Page     g_page = Page::MAIN;
static bool     g_detailDirty = true;
static int      g_detailScroll = 0;

// --- Token 用量 (由 #tok 更新) ---
static int32_t  g_tokIn    = -1;   // -1 = 未收到
static int32_t  g_tokOut   = -1;
static int32_t  g_tokCache = -1;   // cache_creation + cache_read

static const int MAX_SESSIONS = 8;
struct SessionInfo {
  String   name;
  char     state;      // 'R','W','D','P','E'
  uint32_t durSec;
  uint32_t recvMs;     // millis() 收到时刻, 用于本地自增
  uint16_t color;      // peacock 主题色 (RGB565), hasColor=false 时用默认白
  bool     hasColor;
};
static SessionInfo g_sessions[MAX_SESSIONS];
static int         g_sessionCount = 0;

static const int MAX_HIST = 8;
struct HistEntry {
  String name;
  String info;
};
static HistEntry g_history[MAX_HIST];
static int       g_histCount = 0;

// --- 嵌入的 GIF 数据 (platformio.ini board_build.embed_files) ---
extern const uint8_t choosing_gif_start[] asm("_binary_src_anim_choosing_gif_start");
extern const uint8_t choosing_gif_end[]   asm("_binary_src_anim_choosing_gif_end");
extern const uint8_t error_gif_start[]    asm("_binary_src_anim_error_gif_start");
extern const uint8_t error_gif_end[]      asm("_binary_src_anim_error_gif_end");

extern const uint8_t coffee_gif_start[]    asm("_binary_src_anim_coffee_gif_start");
extern const uint8_t coffee_gif_end[]      asm("_binary_src_anim_coffee_gif_end");
extern const uint8_t reading_gif_start[]   asm("_binary_src_anim_reading_gif_start");
extern const uint8_t reading_gif_end[]     asm("_binary_src_anim_reading_gif_end");
extern const uint8_t watering_gif_start[]  asm("_binary_src_anim_watering_gif_start");
extern const uint8_t watering_gif_end[]    asm("_binary_src_anim_watering_gif_end");

extern const uint8_t coding_gif_start[]    asm("_binary_src_anim_coding_gif_start");
extern const uint8_t coding_gif_end[]      asm("_binary_src_anim_coding_gif_end");
extern const uint8_t painting_gif_start[]  asm("_binary_src_anim_painting_gif_start");
extern const uint8_t painting_gif_end[]    asm("_binary_src_anim_painting_gif_end");
extern const uint8_t guitar_gif_start[]    asm("_binary_src_anim_guitar_gif_start");
extern const uint8_t guitar_gif_end[]      asm("_binary_src_anim_guitar_gif_end");

extern const uint8_t photo_gif_start[]     asm("_binary_src_anim_photo_gif_start");
extern const uint8_t photo_gif_end[]       asm("_binary_src_anim_photo_gif_end");
extern const uint8_t eating_gif_start[]    asm("_binary_src_anim_eating_gif_start");
extern const uint8_t eating_gif_end[]      asm("_binary_src_anim_eating_gif_end");

extern const uint8_t dragon_boat_gif_start[] asm("_binary_src_anim_dragon_boat_gif_start");
extern const uint8_t dragon_boat_gif_end[]   asm("_binary_src_anim_dragon_boat_gif_end");
extern const uint8_t exercise_gif_start[]  asm("_binary_src_anim_exercise_gif_start");
extern const uint8_t exercise_gif_end[]    asm("_binary_src_anim_exercise_gif_end");

extern const uint8_t nap_gif_start[]       asm("_binary_src_anim_nap_gif_start");
extern const uint8_t nap_gif_end[]         asm("_binary_src_anim_nap_gif_end");
extern const uint8_t hammer_gif_start[]    asm("_binary_src_anim_hammer_gif_start");
extern const uint8_t hammer_gif_end[]      asm("_binary_src_anim_hammer_gif_end");
extern const uint8_t cheer_gif_start[]     asm("_binary_src_anim_cheer_gif_start");
extern const uint8_t cheer_gif_end[]       asm("_binary_src_anim_cheer_gif_end");
extern const uint8_t poked_gif_start[]     asm("_binary_src_anim_poked_gif_start");
extern const uint8_t poked_gif_end[]       asm("_binary_src_anim_poked_gif_end");

struct GifEntry { const uint8_t* start; const uint8_t* end; };

static const GifEntry kIdleGifs[] = {
  { coffee_gif_start, coffee_gif_end },
  { reading_gif_start, reading_gif_end },
  { watering_gif_start, watering_gif_end },
  { nap_gif_start, nap_gif_end },
};
static const GifEntry kWorkingGifs[] = {
  { coding_gif_start, coding_gif_end },
  { painting_gif_start, painting_gif_end },
  { guitar_gif_start, guitar_gif_end },
  { hammer_gif_start, hammer_gif_end },
};
static const GifEntry kDoneGifs[] = {
  { photo_gif_start, photo_gif_end },
  { eating_gif_start, eating_gif_end },
  { cheer_gif_start, cheer_gif_end },
};
static const GifEntry kAllDoneGifs[] = {
  { dragon_boat_gif_start, dragon_boat_gif_end },
  { exercise_gif_start, exercise_gif_end },
};
static const GifEntry kChoosingGifs[] = {
  { choosing_gif_start, choosing_gif_end },
};
static const GifEntry kErrorGifs[] = {
  { error_gif_start, error_gif_end },
};

struct GifPool { const GifEntry* entries; int count; };
static const GifPool kPools[STATE_COUNT] = {
  { kIdleGifs,     4 },  // IDLE
  { kWorkingGifs,  4 },  // WORKING
  { kChoosingGifs, 1 },  // CHOOSING
  { kDoneGifs,     3 },  // DONE
  { kAllDoneGifs,  2 },  // ALL_DONE
  { kErrorGifs,    1 },  // ERROR
};

// --- "戳一戳"互动: 随机反应动画 + 鼓励短语 ---
static const GifEntry kPokeGifs[] = {
  { eating_gif_start, eating_gif_end },
  { exercise_gif_start, exercise_gif_end },
  { watering_gif_start, watering_gif_end },
  { coffee_gif_start, coffee_gif_end },
  { photo_gif_start, photo_gif_end },
  { poked_gif_start, poked_gif_end },
};
static const int POKE_GIF_COUNT = 6;

static const char* kPokePhrases[] = {
  "hydrate!", "stretch!", "great!",
  "break?", "go go go!", "awesome!",
  "snack?", "breathe~", "nice!",
};
static const int POKE_PHRASE_COUNT = 9;

static bool     g_pokeActive   = false;
static uint32_t g_pokeStartMs  = 0;
static const uint32_t POKE_DURATION_MS = 3000;
static const char* g_pokePhrase = nullptr;

// --- 摇一摇互动 ---
static int      g_shakeCount   = 0;
static bool     g_shakeActive  = false;
static uint32_t g_shakeStartMs = 0;
static const float SHAKE_THRESHOLD = 2.0f;
static const int   SHAKE_FRAMES   = 3;
static const uint32_t SHAKE_DURATION_MS = 3000;
static const char* g_shakePhrase = nullptr;
static const char* kShakePhrases[] = { "dizzy!", "woah!", "hey!!", "stooop!" };
static const int SHAKE_PHRASE_COUNT = 4;

// --- 长按喂食 ---
static bool     g_feedActive   = false;
static uint32_t g_feedStartMs  = 0;
static const uint32_t FEED_DURATION_MS = 4000;
static const char* g_feedPhrase = nullptr;
static const char* kFeedPhrases[] = { "yummy!", "nom nom!", "thanks!", "more!" };
static const int FEED_PHRASE_COUNT = 4;

// --- 翻转待机 (已禁用) ---

// --- 互动状态统一判断 ---
static bool isInteracting() {
  return g_pokeActive || g_shakeActive || g_feedActive;
}

// --- 今日统计 ---
static int      g_todayDone    = 0;

// --- 宠物心情值 ---
static int      g_mood = 70;
static uint32_t g_lastMoodDecay = 0;

// --- NVS 持久化 ---
static Preferences g_prefs;
static bool     g_saveDirty    = false;
static uint32_t g_saveDirtyMs  = 0;
static const uint32_t SAVE_DEBOUNCE_MS = 5000;

static void markDirty() {
  if (!g_saveDirty) {
    g_saveDirty   = true;
    g_saveDirtyMs = millis();
  }
}

static void saveState() {
  g_prefs.putInt("mood", g_mood);
  g_prefs.putInt("todayDone", g_todayDone);
  g_prefs.putInt("state", (int)g_state);
  g_prefs.putInt("workCount", g_workCount);
  for (int i = 0; i < g_workCount; i++) {
    char key[8]; snprintf(key, sizeof(key), "work%d", i);
    g_prefs.putString(key, g_workNames[i]);
  }
  g_prefs.putInt("waitCount", g_waitCount);
  for (int i = 0; i < g_waitCount; i++) {
    char key[8]; snprintf(key, sizeof(key), "wait%d", i);
    g_prefs.putString(key, g_waitNames[i]);
  }
  g_prefs.putInt("sessCount", g_sessionCount);
  for (int i = 0; i < g_sessionCount; i++) {
    char key[8]; snprintf(key, sizeof(key), "sess%d", i);
    String packed = g_sessions[i].name + ":" + String((char)g_sessions[i].state) + ":" + String(g_sessions[i].durSec);
    g_prefs.putString(key, packed);
  }
  g_saveDirty = false;
  Serial.println("[nvs] saved");
}

static void loadState() {
  g_prefs.begin("clawdpet", false);
  g_mood      = g_prefs.getInt("mood", 70);
  g_todayDone = g_prefs.getInt("todayDone", 0);
  g_state     = (PetState)g_prefs.getInt("state", 0);
  g_workCount = g_prefs.getInt("workCount", 0);
  for (int i = 0; i < g_workCount && i < MAX_WORK_NAMES; i++) {
    char key[8]; snprintf(key, sizeof(key), "work%d", i);
    g_workNames[i] = g_prefs.getString(key, "");
  }
  g_waitCount = g_prefs.getInt("waitCount", 0);
  for (int i = 0; i < g_waitCount && i < MAX_WORK_NAMES; i++) {
    char key[8]; snprintf(key, sizeof(key), "wait%d", i);
    g_waitNames[i] = g_prefs.getString(key, "");
  }
  g_sessionCount = g_prefs.getInt("sessCount", 0);
  uint32_t now = millis();
  for (int i = 0; i < g_sessionCount && i < MAX_SESSIONS; i++) {
    char key[8]; snprintf(key, sizeof(key), "sess%d", i);
    String packed = g_prefs.getString(key, "");
    int c1 = packed.indexOf(':');
    int c2 = (c1 >= 0) ? packed.indexOf(':', c1 + 1) : -1;
    if (c1 > 0 && c2 > c1) {
      g_sessions[i].name   = packed.substring(0, c1);
      g_sessions[i].state  = packed.charAt(c1 + 1);
      g_sessions[i].durSec = packed.substring(c2 + 1).toInt();
      g_sessions[i].recvMs = now;
    }
  }
  Serial.printf("[nvs] loaded: mood=%d todayDone=%d state=%d work=%d sess=%d\n",
                g_mood, g_todayDone, (int)g_state, g_workCount, g_sessionCount);
}

static void pollSave() {
  if (g_saveDirty && (millis() - g_saveDirtyMs) >= SAVE_DEBOUNCE_MS) {
    saveState();
  }
}

static const uint8_t* gifDataForState(PetState s, size_t* outSize) {
  const GifPool& pool = kPools[(int)s];
  int idx = (pool.count > 1) ? (esp_random() % pool.count) : 0;
  const GifEntry& e = pool.entries[idx];
  *outSize = e.end - e.start;
  return e.start;
}

static AnimatedGIF g_gif;
static M5Canvas    g_canvas(&M5.Display);   // 135x240 离屏画布 (PSRAM)
static bool        g_gifOpen      = false;
static uint32_t    g_lastFrameMs  = 0;
static int         g_frameDelayMs = 50;

// GIF frame buffer: 分配一次, 所有 GIF 复用 (都是 240x240, 大小一样)
static uint8_t*    g_frameBuf     = nullptr;
static const int   FRAME_BUF_SIZE = GIF_W * (GIF_W + 3);  // 240*(240+3)

// --- 解码回调: cooked 模式下 pPixels 已是合成好的 RGB565 整行 ---
static uint16_t g_rowBuf[SCR_W];
static uint16_t g_gifBgColor565 = CHROMA_GREEN;  // GIF 文件声明的背景色 (RGB565)

static void gifDraw(GIFDRAW* p) {
  int y = p->iY + p->y + PET_Y_OFFSET;     // 整体下移
  if (y < 0 || y >= SCR_H) return;
  if (p->pPalette) g_gifBgColor565 = p->pPalette[p->ucBackground];
  const uint16_t* src = (const uint16_t*)p->pPixels;
  int srcStart = max(0, CROP_X0 - p->iX);
  int srcEnd   = min(p->iWidth, CROP_X0 + SCR_W - p->iX);
  if (srcStart >= srcEnd) return;
  int dstX = p->iX + srcStart - CROP_X0;
  int w = srcEnd - srcStart;
  for (int i = 0; i < w; i++) {
    uint16_t c = src[srcStart + i];
    g_rowBuf[i] = (c == CHROMA_GREEN || c == g_gifBgColor565) ? BG_COLOR : c;
  }
  g_canvas.pushImage(dstX, y, w, 1, g_rowBuf);
}

// ---------------------------------------------------------------------------
// 时间: 当前应显示的当日分钟数, 未校准返回 -1
// ---------------------------------------------------------------------------
static int currentMinOfDay() {
  if (!g_timeValid) return -1;
  uint32_t elapsedMin = (millis() - g_baseMillis) / 60000UL;
  return (g_baseMinOfDay + (int)elapsedMin) % 1440;
}

// ---------------------------------------------------------------------------
// UI 叠加层: 全部画进 g_canvas (推屏前调用)。文字用不透明字底保证清晰。
// ---------------------------------------------------------------------------
static void drawStatusBarOverlay() {
  // 状态栏底条 (半高暗条, 提升可读性)
  g_canvas.fillRect(0, STATUS_Y, SCR_W, STATUS_H, TFT_BLACK);
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);

  // 左: 时间
  char ts[6];
  int m = currentMinOfDay();
  if (m < 0) strcpy(ts, "--:--");
  else       snprintf(ts, sizeof(ts), "%02d:%02d", m / 60, m % 60);
  g_canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  g_canvas.setTextDatum(top_left);
  g_canvas.drawString(ts, 4, 6);

  // 中: 心情指示 (小圆点, 颜色随心情变化)
  uint16_t moodCol = (g_mood > 70) ? g_canvas.color565(0, 200, 90)
                   : (g_mood > 30) ? g_canvas.color565(230, 200, 0)
                   : g_canvas.color565(150, 60, 60);
  g_canvas.fillCircle(44, 9, 3, moodCol);

  // 右: 电池图标 + 百分比
  int pct = g_battLevel < 0 ? 0 : g_battLevel;
  uint16_t col = TFT_WHITE;
  if (g_charging)     col = g_canvas.color565(0, 200, 80);
  else if (pct <= 20) col = g_canvas.color565(220, 40, 40);

  char ps[6];
  snprintf(ps, sizeof(ps), "%d%%", pct);
  int bx = SCR_W - 4 - 18;
  int by = 5;
  g_canvas.setTextColor(col, TFT_BLACK);
  g_canvas.setTextDatum(top_right);
  g_canvas.drawString(ps, bx - 3, 6);

  g_canvas.drawRect(bx, by, 18, 9, col);
  g_canvas.fillRect(bx + 18, by + 2, 2, 5, col);   // 正极凸点
  int fillW = (16 * pct) / 100;
  if (fillW > 0) g_canvas.fillRect(bx + 1, by + 1, fillW, 7, col);
}

static void drawCountsOverlay() {
  int y = COUNT_Y;

  // 第一行小字: W:n  D:n (默认字体)
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  // W 部分琥珀色
  g_canvas.setTextColor(g_canvas.color565(230, 170, 0), TFT_BLACK);
  g_canvas.setTextDatum(top_left);
  char wPart[8];
  snprintf(wPart, sizeof(wPart), "W:%d", g_workCount);
  g_canvas.drawString(wPart, 4, y);
  // D 部分绿色
  int wWidth = g_canvas.textWidth(wPart);
  g_canvas.setTextColor(g_canvas.color565(0, 200, 90), TFT_BLACK);
  char dPart[8];
  snprintf(dPart, sizeof(dPart), "D:%d", g_todayDone);
  g_canvas.drawString(dPart, 4 + wWidth + 12, y);
  y += 14;

  // 合并 work + wait 列表, 按颜色区分
  int totalNames = g_workCount + g_waitCount;
  if (totalNames > 0) {
    g_canvas.setTextSize(2);
    g_canvas.setTextDatum(top_left);

    int maxShow = 3;
    int startIdx = 0;
    if (totalNames > maxShow) {
      int pages = (totalNames + maxShow - 1) / maxShow;
      startIdx = (g_infoCycle % pages) * maxShow;
    }
    int endIdx = startIdx + maxShow;
    if (endIdx > totalNames) endIdx = totalNames;

    // 清除名字区域防止短名字残留长名字尾巴
    int nameAreaH = (endIdx - startIdx) * 20;
    g_canvas.fillRect(0, y, SCR_W, nameAreaH, TFT_BLACK);

    uint16_t cyanCol = g_canvas.color565(0, 200, 255);
    bool blink = ((millis() / 500) % 2) == 0;

    for (int idx = startIdx; idx < endIdx; idx++) {
      bool isWait = (idx >= g_workCount);
      const String& nm = isWait ? g_waitNames[idx - g_workCount]
                                : g_workNames[idx];
      String display = nm.substring(0, 10);
      if (isWait) {
        g_canvas.setTextColor(blink ? cyanCol : TFT_DARKGREY, TFT_BLACK);
      } else {
        g_canvas.setTextColor(TFT_WHITE, TFT_BLACK);
      }
      g_canvas.drawString(display, 4, y);
      y += 20;
    }
  }
}

static void drawBubbleOverlay() {
  const StateStyle& st = kStyles[(int)g_state];

  int pad = 8;
  int bx = pad, by = BUBBLE_Y + 4;
  int bw = SCR_W - pad * 2;
  int bh = BUBBLE_H - 18;
  int r  = 10;

  // 摇一摇模式: 特殊气泡
  if (g_shakeActive && g_shakePhrase) {
    uint16_t col = g_canvas.color565(255, 100, 100);  // 红
    g_canvas.drawRoundRect(bx, by, bw, bh, r, col);
    int tipX = SCR_W / 2;
    int tailTop = by + bh;
    g_canvas.drawLine(tipX - 8, tailTop, tipX, tailTop + 9, col);
    g_canvas.drawLine(tipX + 8, tailTop, tipX, tailTop + 9, col);
    g_canvas.setTextSize(2);
    g_canvas.setTextColor(col);
    g_canvas.setTextDatum(middle_center);
    g_canvas.drawString(g_shakePhrase, SCR_W / 2, by + bh / 2);
    return;
  }

  // 喂食模式: 特殊气泡
  if (g_feedActive && g_feedPhrase) {
    uint16_t col = g_canvas.color565(255, 150, 200);  // 粉
    g_canvas.drawRoundRect(bx, by, bw, bh, r, col);
    int tipX = SCR_W / 2;
    int tailTop = by + bh;
    g_canvas.drawLine(tipX - 8, tailTop, tipX, tailTop + 9, col);
    g_canvas.drawLine(tipX + 8, tailTop, tipX, tailTop + 9, col);
    g_canvas.setTextSize(2);
    g_canvas.setTextColor(col);
    g_canvas.setTextDatum(middle_center);
    g_canvas.drawString(g_feedPhrase, SCR_W / 2, by + bh / 2);
    return;
  }

  // 戳一戳模式: 特殊气泡
  if (g_pokeActive && g_pokePhrase) {
    uint16_t col = g_canvas.color565(255, 200, 50);  // 金黄
    g_canvas.drawRoundRect(bx, by, bw, bh, r, col);
    int tipX = SCR_W / 2;
    int tailTop = by + bh;
    g_canvas.drawLine(tipX - 8, tailTop, tipX, tailTop + 9, col);
    g_canvas.drawLine(tipX + 8, tailTop, tipX, tailTop + 9, col);
    g_canvas.setTextSize(2);
    g_canvas.setTextColor(col);
    g_canvas.setTextDatum(middle_center);
    g_canvas.drawString(g_pokePhrase, SCR_W / 2, by + bh / 2);
    return;
  }

  // CHOOSING: 边框闪烁(亮青/暗灰每 ~0.4s 切换) + 气泡内显示等待终端名(轮播)
  if (g_state == PetState::CHOOSING) {
    bool on = ((millis() / 400) % 2) == 0;
    uint16_t border = on ? g_canvas.color565(0, 200, 255)
                         : g_canvas.color565(40, 60, 70);
    g_canvas.drawRoundRect(bx, by, bw, bh, r, border);
    int tipX = SCR_W / 2;
    int tailTop = by + bh;
    g_canvas.drawLine(tipX - 8, tailTop, tipX, tailTop + 9, border);
    g_canvas.drawLine(tipX + 8, tailTop, tipX, tailTop + 9, border);

    // 文字: 有等待名则显示当前轮播到的那个, 否则回落 phrase("YOUR TURN")
    const char* txt = (g_waitCount > 0)
        ? g_waitNames[g_waitCycle % g_waitCount].c_str()
        : st.phrase;
    g_canvas.setTextSize(2);
    g_canvas.setTextColor(g_canvas.color565(0, 200, 255));
    g_canvas.setTextDatum(middle_center);
    g_canvas.drawString(txt, SCR_W / 2, by + bh / 2);
    return;
  }

  // 不填充底色: 仅描白边, 让 GIF 宠物透过气泡可见
  g_canvas.drawRoundRect(bx, by, bw, bh, r, TFT_WHITE);

  // 尾巴: 向下三角指向宠物 (描边轮廓, 不填充)
  int tipX = SCR_W / 2;
  int tailTop = by + bh;
  g_canvas.drawLine(tipX - 8, tailTop, tipX, tailTop + 9, TFT_WHITE);
  g_canvas.drawLine(tipX + 8, tailTop, tipX, tailTop + 9, TFT_WHITE);

  // 短语: 透明背景, 仅描文字 (叠在 GIF 上)
  g_canvas.setTextSize(2);
  const char* phrase = st.phrase;
  uint16_t txtCol = g_canvas.color565(st.tr, st.tg, st.tb);
  // 心情低时 idle 气泡变灰 (按心情档位选一次, 避免每帧闪烁)
  if (g_state == PetState::IDLE && g_mood < 30) {
    static const char* sadPhrases[] = {"bored...", "miss u", "lonely"};
    static int sadIdx = 0;
    static int lastMoodBand = -1;
    int curBand = g_mood / 10;
    if (lastMoodBand != curBand) { sadIdx = esp_random() % 3; lastMoodBand = curBand; }
    phrase = sadPhrases[sadIdx];
    txtCol = g_canvas.color565(120, 120, 120);
  }
  g_canvas.setTextColor(txtCol);
  g_canvas.setTextDatum(middle_center);
  g_canvas.drawString(phrase, SCR_W / 2, by + bh / 2);
}

static void drawOverlay() {
  drawStatusBarOverlay();
  drawCountsOverlay();
  drawBubbleOverlay();
}

// 解码好的画布 + UI 叠加 -> 推屏
static void presentCanvas() {
  drawOverlay();
  g_canvas.pushSprite(0, 0);
}

// ---------------------------------------------------------------------------
// GIF 生命周期
// ---------------------------------------------------------------------------
static void closeGif() {
  if (g_gifOpen) {
    g_gif.close();
    // 不释放 frame buffer, 复用
    g_gifOpen = false;
  }
}

static void openGifForState() {
  closeGif();
  size_t sz = 0;
  const uint8_t* data = gifDataForState(g_state, &sz);
  if (!data || sz == 0) {                       // 无 GIF: 纯色占位 + UI
    g_canvas.fillScreen(M5.Display.color565(120, 20, 20));
    presentCanvas();
    return;
  }
  if (g_frameBuf && g_gif.open((uint8_t*)data, (int)sz, gifDraw)) {
    g_gif.setDrawType(GIF_DRAW_COOKED);
    g_gif.setFrameBuf(g_frameBuf);
    g_gifOpen = true;
    g_frameDelayMs = 50;
    g_lastFrameMs  = 0;
    g_canvas.fillScreen(BG_COLOR);
  } else {
    Serial.printf("[gif] open failed, err=%d\n", g_gif.getLastError());
    g_canvas.fillScreen(BG_COLOR);
    presentCanvas();
  }
}

static void applyState(PetState s) {
  g_state = s;
  g_lastMoodDecay = millis();
  markDirty();
  Serial.printf("[state] -> %s\n", kStyles[(int)s].name);
  if (s == PetState::DONE || s == PetState::ALL_DONE) {
    g_mood = min(100, g_mood + 10);
  }
  openGifForState();
}

// ---------------------------------------------------------------------------
// 命令解析
// ---------------------------------------------------------------------------
static void handleStat(const String& cmd) {
  // #stat T=3 W=1 D=2  (大小写不敏感, 顺序任意)
  int t = g_total, w = g_work, d = g_done;
  int n = cmd.length();
  for (int i = 0; i < n; ++i) {
    char c = cmd[i];
    if ((c == 't' || c == 'w' || c == 'd') && i + 1 < n && cmd[i + 1] == '=') {
      int v = 0, j = i + 2; bool got = false;
      while (j < n && cmd[j] >= '0' && cmd[j] <= '9') {
        v = v * 10 + (cmd[j] - '0'); j++; got = true;
      }
      if (got) { if (c == 't') t = v; else if (c == 'w') w = v; else d = v; }
    }
  }
  g_total = t; g_work = w; g_done = d;
}

static void handleWork(const String& raw) {
  // #work a|b|c  (原始大小写, '|' 分隔; 取 "#work " 之后的部分)
  int sp = raw.indexOf(' ');
  String body = (sp < 0) ? String("") : raw.substring(sp + 1);
  body.trim();

  g_workCount = 0;
  g_infoCycle = 0;
  int start = 0;
  while (start <= (int)body.length() && g_workCount < MAX_WORK_NAMES) {
    int bar = body.indexOf('|', start);
    String tok = (bar < 0) ? body.substring(start) : body.substring(start, bar);
    tok.trim();
    if (tok.length() > 0) g_workNames[g_workCount++] = tok;
    if (bar < 0) break;
    start = bar + 1;
  }
  markDirty();
}

static void handleWait(const String& raw) {
  // #wait a|b|c  (原始大小写, '|' 分隔; 取 "#wait " 之后的部分)
  int sp = raw.indexOf(' ');
  String body = (sp < 0) ? String("") : raw.substring(sp + 1);
  body.trim();
  g_waitCount = 0;
  g_waitCycle = 0;
  int start = 0;
  while (start <= (int)body.length() && g_waitCount < MAX_WORK_NAMES) {
    int bar = body.indexOf('|', start);
    String tok = (bar < 0) ? body.substring(start) : body.substring(start, bar);
    tok.trim();
    if (tok.length() > 0) g_waitNames[g_waitCount++] = tok;
    if (bar < 0) break;
    start = bar + 1;
  }
  markDirty();
}

static void handleTime(const String& cmd) {
  // #time 14:32
  int colon = cmd.indexOf(':');
  if (colon < 0) return;
  int i = colon - 1, h = 0, hd = 1; bool hg = false;
  while (i >= 0 && cmd[i] >= '0' && cmd[i] <= '9') {
    h += (cmd[i] - '0') * hd; hd *= 10; i--; hg = true;
  }
  int j = colon + 1, m = 0; bool mg = false;
  while (j < (int)cmd.length() && cmd[j] >= '0' && cmd[j] <= '9') {
    m = m * 10 + (cmd[j] - '0'); j++; mg = true;
  }
  if (!hg || !mg || h > 23 || m > 59) return;
  g_baseMinOfDay = h * 60 + m;
  g_baseMillis   = millis();
  g_timeValid    = true;
}

// 把 6 位 hex "rrggbb" 转 RGB565; 非法返回 0 (表示无颜色)。
static uint16_t parseHexColor(const String& hex) {
  String h = hex;
  h.trim();
  if (h.startsWith("#")) h = h.substring(1);
  if (h.length() != 6) return 0;
  auto nv = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  int r = nv(h[0]) * 16 + nv(h[1]);
  int g = nv(h[2]) * 16 + nv(h[3]);
  int b = nv(h[4]) * 16 + nv(h[5]);
  if (r < 0 || g < 0 || b < 0) return 0;
  return g_canvas.color565(r, g, b);
}

// --- #sess name:state:dur|name:state:dur|... ---
static void handleSess(const String& raw) {
  int sp = raw.indexOf(' ');
  String body = (sp < 0) ? String("") : raw.substring(sp + 1);
  body.trim();
  g_sessionCount = 0;
  if (body.length() == 0) { g_detailDirty = true; return; }
  int start = 0;
  uint32_t now = millis();
  while (start <= (int)body.length() && g_sessionCount < MAX_SESSIONS) {
    int bar = body.indexOf('|', start);
    String tok = (bar < 0) ? body.substring(start) : body.substring(start, bar);
    // parse "name:state:dur[:rrggbb]"
    int c1 = tok.indexOf(':');
    int c2 = (c1 >= 0) ? tok.indexOf(':', c1 + 1) : -1;
    int c3 = (c2 >= 0) ? tok.indexOf(':', c2 + 1) : -1;
    if (c1 > 0 && c2 > c1) {
      SessionInfo& s = g_sessions[g_sessionCount];
      s.name   = tok.substring(0, c1);
      s.state  = tok.charAt(c1 + 1);
      String durStr = (c3 > c2) ? tok.substring(c2 + 1, c3) : tok.substring(c2 + 1);
      s.durSec = durStr.toInt();
      s.recvMs = now;
      s.hasColor = false;
      s.color    = 0;
      if (c3 > c2) {                            // 可选第 4 段: peacock 颜色
        uint16_t col = parseHexColor(tok.substring(c3 + 1));
        if (col) { s.color = col; s.hasColor = true; }
      }
      g_sessionCount++;
    }
    if (bar < 0) break;
    start = bar + 1;
  }
  g_detailDirty = true;
  markDirty();
}

// --- #hist name:info|name:info|... ---
static void handleHist(const String& raw) {
  int sp = raw.indexOf(' ');
  String body = (sp < 0) ? String("") : raw.substring(sp + 1);
  body.trim();
  g_histCount = 0;
  if (body.length() == 0) { g_detailDirty = true; return; }
  int start = 0;
  while (start <= (int)body.length() && g_histCount < MAX_HIST) {
    int bar = body.indexOf('|', start);
    String tok = (bar < 0) ? body.substring(start) : body.substring(start, bar);
    int c1 = tok.indexOf(':');
    if (c1 > 0) {
      g_history[g_histCount].name = tok.substring(0, c1);
      g_history[g_histCount].info = tok.substring(c1 + 1);
      g_histCount++;
    }
    if (bar < 0) break;
    start = bar + 1;
  }
  g_detailDirty = true;
}

// --- #today D=5 ---
static void handleToday(const String& cmd) {
  String low = cmd; low.toLowerCase();
  int di = low.indexOf("d=");
  if (di >= 0) {
    int v = 0, j = di + 2;
    while (j < (int)low.length() && low[j] >= '0' && low[j] <= '9') {
      v = v * 10 + (low[j] - '0'); j++;
    }
    g_todayDone = v;
  }
  g_detailDirty = true;
  markDirty();
}

// --- #achv ACHIEVEMENT_NAME ---
static void handleAchv(const String& raw) {
  int sp = raw.indexOf(' ');
  if (sp < 0) return;
  String name = raw.substring(sp + 1);
  name.trim();
  if (name.length() == 0) return;
  // 触发金色成就气泡
  g_pokeActive  = true;
  g_pokeStartMs = millis();
  g_pokePhrase  = "ACHIEVED!";
}

// --- #festival name ---
static bool     g_festivalActive = false;
static uint32_t g_festivalStartMs = 0;
static const uint32_t FESTIVAL_DURATION_MS = 5000;

static void handleFestival(const String& raw) {
  int sp = raw.indexOf(' ');
  if (sp < 0) return;
  String name = raw.substring(sp + 1);
  name.trim();
  if (name.length() == 0) return;
  // 显示节日气泡 5 秒
  g_festivalActive  = true;
  g_festivalStartMs = millis();
  g_pokeActive  = true;
  g_pokeStartMs = millis();
  // 根据节日名设不同短语
  if (name == "christmas")       g_pokePhrase = "Merry Xmas!";
  else if (name == "halloween")  g_pokePhrase = "Boo! Happy Halloween!";
  else if (name == "valentine")  g_pokePhrase = "Happy Valentine!";
  else if (name == "new-year")   g_pokePhrase = "Happy New Year!";
  else                           g_pokePhrase = "Celebrate!";
}

// --- #tok IN=820000 OUT=380000 CACHE=2100000 ---
static void handleTok(const String& cmd) {
  int n = cmd.length();
  for (int i = 0; i < n; ++i) {
    char c = cmd[i];
    bool isIn = (c == 'i' && i + 2 < n && cmd[i+1] == 'n' && cmd[i+2] == '=');
    bool isOut = (c == 'o' && i + 3 < n && cmd[i+1] == 'u' && cmd[i+2] == 't' && cmd[i+3] == '=');
    bool isCache = (c == 'c' && i + 5 < n && cmd.substring(i, i+6) == "cache=");
    int vStart = -1;
    if (isIn) vStart = i + 3;
    else if (isOut) vStart = i + 4;
    else if (isCache) vStart = i + 6;
    else continue;
    int32_t v = 0; int j = vStart; bool got = false;
    while (j < n && cmd[j] >= '0' && cmd[j] <= '9') {
      v = v * 10 + (cmd[j] - '0'); j++; got = true;
    }
    if (got) {
      if (isIn) g_tokIn = v;
      else if (isOut) g_tokOut = v;
      else g_tokCache = v;
    }
    i = j - 1;
  }
  g_detailDirty = true;
}

// --- Token 格式化: K/M 缩写 ---
static void fmtTok(int32_t val, char* buf, int bufSz) {
  if (val < 0) { snprintf(buf, bufSz, "--"); return; }
  if (val < 1000) { snprintf(buf, bufSz, "%d", (int)val); return; }
  if (val < 1000000) { snprintf(buf, bufSz, "%dK", (int)(val / 1000)); return; }
  int whole = val / 1000000;
  int frac = (val % 1000000) / 100000;
  if (frac > 0) snprintf(buf, bufSz, "%d.%dM", whole, frac);
  else snprintf(buf, bufSz, "%dM", whole);
}

// --- Token 页渲染 ---
static void drawTokView() {
  int y = 30;

  // 标题
  g_canvas.setTextSize(1);
  g_canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  g_canvas.setTextDatum(middle_center);
  g_canvas.drawString("TODAY TOKENS", SCR_W / 2, y);
  y += 24;

  // 总量大字 (IN + OUT)
  char totalBuf[12];
  int32_t total = (g_tokIn >= 0 && g_tokOut >= 0) ? g_tokIn + g_tokOut : -1;
  fmtTok(total, totalBuf, sizeof(totalBuf));
  g_canvas.setTextSize(3);
  g_canvas.setTextColor(g_canvas.color565(255, 200, 50), TFT_BLACK);
  g_canvas.setTextDatum(middle_center);
  g_canvas.drawString(totalBuf, SCR_W / 2, y + 10);
  y += 44;

  // 明细行
  g_canvas.setTextSize(2);
  g_canvas.setTextDatum(top_left);
  char valBuf[12];

  // IN
  g_canvas.setTextColor(g_canvas.color565(230, 170, 0), TFT_BLACK);
  g_canvas.drawString("IN", 8, y);
  fmtTok(g_tokIn, valBuf, sizeof(valBuf));
  g_canvas.setTextDatum(top_right);
  g_canvas.drawString(valBuf, SCR_W - 8, y);
  y += 24;

  // OUT
  g_canvas.setTextDatum(top_left);
  g_canvas.setTextColor(g_canvas.color565(0, 200, 90), TFT_BLACK);
  g_canvas.drawString("OUT", 8, y);
  fmtTok(g_tokOut, valBuf, sizeof(valBuf));
  g_canvas.setTextDatum(top_right);
  g_canvas.drawString(valBuf, SCR_W - 8, y);
  y += 24;

  // CACHE
  g_canvas.setTextDatum(top_left);
  g_canvas.setTextColor(g_canvas.color565(0, 200, 255), TFT_BLACK);
  g_canvas.drawString("CACHE", 8, y);
  fmtTok(g_tokCache, valBuf, sizeof(valBuf));
  g_canvas.setTextDatum(top_right);
  g_canvas.drawString(valBuf, SCR_W - 8, y);

}

// --- 详情页渲染 ---
static String formatDuration(uint32_t sec) {
  if (sec < 60) return String(sec) + "s";
  if (sec < 3600) return String(sec / 60) + "m";
  return String(sec / 3600) + "h";
}

static uint16_t stateColor(char s) {
  switch (s) {
    case 'R': return g_canvas.color565(230, 170, 0);    // amber
    case 'W': return g_canvas.color565(0, 200, 255);    // cyan
    case 'D': return g_canvas.color565(0, 200, 90);     // green
    case 'E': return g_canvas.color565(220, 40, 40);    // red
    default:  return TFT_DARKGREY;
  }
}

static const char* stateLabel(char s) {
  switch (s) {
    case 'R': return "RUN";
    case 'W': return "WAIT";
    case 'D': return "DONE";
    case 'P': return "PEND";
    case 'E': return "ERR";
    default:  return "?";
  }
}

static void drawDetailContent() {
  uint32_t now = millis();
  int y = 22;

  // Header: SESSIONS
  g_canvas.setFont(&fonts::efontCN_12);
  g_canvas.setTextSize(1);
  g_canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  g_canvas.setTextDatum(top_left);
  char hdr[24];
  snprintf(hdr, sizeof(hdr), "SESSIONS (%d)", g_sessionCount);
  g_canvas.drawString(hdr, 4, y);
  y += 14;

  // Session rows
  if (g_sessionCount == 0) {
    g_canvas.setTextColor(TFT_DARKGREY);
    g_canvas.drawString("(none active)", 4, y);
  } else {
    int maxShow = 9;
    int pages = (g_sessionCount + maxShow - 1) / maxShow;
    if (g_detailScroll >= pages) g_detailScroll = 0;
    int startIdx = g_detailScroll * maxShow;
    int endIdx = startIdx + maxShow;
    if (endIdx > g_sessionCount) endIdx = g_sessionCount;
    for (int i = startIdx; i < endIdx; i++) {
      SessionInfo& s = g_sessions[i];
      uint16_t col = stateColor(s.state);
      g_canvas.fillCircle(6, y + 5, 3, col);
      String nm = s.name.substring(0, 9);
      // 名字用 peacock 主题色 (无色回落白), 状态点保留状态色
      uint16_t nameCol = s.hasColor ? s.color : TFT_WHITE;
      g_canvas.setTextColor(nameCol, TFT_BLACK);
      g_canvas.setTextDatum(top_left);
      g_canvas.drawString(nm, 14, y);
      g_canvas.setTextColor(col, TFT_BLACK);
      g_canvas.setTextDatum(top_left);
      g_canvas.drawString(stateLabel(s.state), 75, y);
      uint32_t elapsed = s.durSec + (now - s.recvMs) / 1000;
      String dur = formatDuration(elapsed);
      g_canvas.setTextDatum(top_right);
      g_canvas.drawString(dur, SCR_W - 4, y);
      y += 15;
    }
    if (pages > 1) {
      char pi[8];
      snprintf(pi, sizeof(pi), "%d/%d", (g_detailScroll % pages) + 1, pages);
      g_canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
      g_canvas.setTextDatum(top_right);
      g_canvas.drawString(pi, SCR_W - 4, 22);
    }
  }

  // === 底部统计区 ===
  const int todayY = 170;
  g_canvas.drawFastHLine(4, todayY - 4, SCR_W - 8, TFT_DARKGREY);

  // "TODAY" 标题 + 完成数
  g_canvas.setTextSize(1);
  g_canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  g_canvas.setTextDatum(top_left);
  g_canvas.drawString("TODAY", 4, todayY);

  // 完成数大字
  int doneY = todayY + 16;
  g_canvas.setTextSize(2);
  char doneStr[8];
  snprintf(doneStr, sizeof(doneStr), "%d", g_todayDone);
  g_canvas.setTextColor(g_canvas.color565(0, 200, 90), TFT_BLACK);
  g_canvas.setTextDatum(top_left);
  g_canvas.drawString(doneStr, 4, doneY);
  // "done" 标签
  g_canvas.setTextSize(1);
  g_canvas.setTextColor(g_canvas.color565(150, 150, 150), TFT_BLACK);
  g_canvas.setTextDatum(top_left);
  int labelX = 4 + (g_todayDone >= 10 ? 26 : 14);
  g_canvas.drawString("done", labelX, doneY + 6);

  // 心情条 (右侧)
  int moodBarX = 70, moodBarW = SCR_W - 74, moodBarH = 6;
  int moodBarY = doneY + 4;
  uint16_t moodCol = (g_mood > 70) ? g_canvas.color565(0, 200, 90)
                   : (g_mood > 30) ? g_canvas.color565(230, 200, 0)
                   : g_canvas.color565(150, 150, 150);
  g_canvas.drawRect(moodBarX, moodBarY, moodBarW, moodBarH, TFT_DARKGREY);
  int moodFill = (int)(moodBarW * g_mood / 100);
  if (moodFill >= 3) {
    g_canvas.fillRect(moodBarX + 1, moodBarY + 1, moodFill - 2, moodBarH - 2, moodCol);
  }
  // mood 标签
  char moodLabel[12];
  snprintf(moodLabel, sizeof(moodLabel), "mood %d", g_mood);
  g_canvas.setTextColor(moodCol, TFT_BLACK);
  g_canvas.setTextDatum(top_right);
  g_canvas.drawString(moodLabel, SCR_W - 4, moodBarY + moodBarH + 2);
}

static void handleCommand(String cmd) {
  cmd.trim();
  if (cmd.isEmpty()) return;
  Serial.printf("[recv] '%s'\n", cmd.c_str());

  String low = cmd; low.toLowerCase();
  if (low.startsWith("#stat")) { handleStat(low); return; }
  if (low.startsWith("#work")) { handleWork(cmd); return; }  // 名字保留原始大小写
  if (low.startsWith("#wait")) { handleWait(cmd); return; }  // 名字保留原始大小写
  if (low.startsWith("#time")) { handleTime(low); return; }
  if (low.startsWith("#sess")) { handleSess(cmd); return; }
  if (low.startsWith("#hist")) { handleHist(cmd); return; }
  if (low.startsWith("#today")) { handleToday(low); return; }
  if (low.startsWith("#achv")) { handleAchv(cmd); return; }
  if (low.startsWith("#festival")) { handleFestival(cmd); return; }
  if (low.startsWith("#tok")) { handleTok(low); return; }

  if      (low == "idle")     applyState(PetState::IDLE);
  else if (low == "working")  applyState(PetState::WORKING);
  else if (low == "choosing") applyState(PetState::CHOOSING);
  else if (low == "done")     applyState(PetState::DONE);
  else if (low == "all_done" || low == "alldone") applyState(PetState::ALL_DONE);
  else if (low == "error")    applyState(PetState::ERROR);
  else if (low == "ping")     Serial.println("pong");
  else    Serial.printf("[warn] unknown command: '%s'\n", low.c_str());
}

static void pollSerial() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (g_lineLen > 0) {
        g_lineBuf[g_lineLen] = '\0';
        handleCommand(String(g_lineBuf));
        g_lineLen = 0;
      }
    } else if (g_lineLen < 200) {
      g_lineBuf[g_lineLen++] = c;
    } else {
      g_lineLen = 0;
    }
  }
}

// 电量轮询: 仅更新全局, 由叠加层每帧重绘
static void pollBattery() {
  uint32_t now = millis();
  if (now - g_lastBattMs < BATT_POLL_MS && g_battLevel >= 0) return;
  g_lastBattMs = now;
  g_battLevel = M5.Power.getBatteryLevel();
  g_charging  = M5.Power.isCharging();
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 1500) { delay(10); }

  M5.Display.setRotation(0);          // 竖屏 135x240
  M5.Display.setBrightness(128);
  M5.Display.fillScreen(BG_COLOR);

  g_canvas.setPsram(true);
  g_canvas.setColorDepth(16);
  g_canvas.createSprite(SCR_W, SCR_H);
  g_canvas.fillScreen(BG_COLOR);

  g_gif.begin(GIF_PALETTE_RGB565_BE);

  g_frameBuf = (uint8_t*)ps_malloc(FRAME_BUF_SIZE);
  if (!g_frameBuf) Serial.println("[gif] FATAL: frame buffer alloc failed");

  loadState();

  Serial.println();
  Serial.println("[ClawdPet] boot ok");
  Serial.printf("[ClawdPet] display=%dx%d psram=%u imu=%d\n",
                M5.Display.width(), M5.Display.height(),
                (unsigned)ESP.getPsramSize(), M5.Imu.getType());
  Serial.println("[ClawdPet] cmds: idle|working|choosing|done|all_done|error | "
                 "#stat T=.. W=.. D=.. | #work a|b | #wait a|b | #time HH:MM | ping");

  pollBattery();
  g_lastMoodDecay = millis();
  applyState(g_state);
}

void loop() {
  M5.update();

  pollSerial();
  pollBattery();
  pollSave();

  // BtnB: 三页循环切换 MAIN -> DETAIL -> TOKEN -> MAIN
  if (M5.BtnB.wasPressed()) {
    Page prev = g_page;
    g_page = (Page)(((int)g_page + 1) % 3);
    if (g_page == Page::MAIN) {
      openGifForState();
    } else {
      if (prev == Page::MAIN) closeGif();
      g_detailDirty = true;
    }
  }

  switch (g_page) {
  case Page::DETAIL: {
    // 详情页模式: 无 GIF, 仅 dirty 时重绘
    static uint32_t lastDetailRefresh = 0;
    uint32_t tnow = millis();
    if (tnow - lastDetailRefresh >= 1000) {
      lastDetailRefresh = tnow;
      g_detailDirty = true;
    }
    if (g_detailDirty) {
      g_canvas.fillScreen(TFT_BLACK);
      drawStatusBarOverlay();
      drawDetailContent();
      g_canvas.pushSprite(0, 0);
      g_detailDirty = false;
    }
    // BtnA 在详情页: 翻页
    if (M5.BtnA.wasPressed()) {
      int maxShow = 9;
      int pages = (g_sessionCount + maxShow - 1) / maxShow;
      if (pages > 1) {
        g_detailScroll = (g_detailScroll + 1) % pages;
        g_detailDirty = true;
      }
    }
    break;
  }

  case Page::TOKEN: {
    // Token 页: 无 GIF, dirty 时重绘
    if (g_detailDirty) {
      g_canvas.fillScreen(TFT_BLACK);
      drawStatusBarOverlay();
      drawTokView();
      g_canvas.pushSprite(0, 0);
      g_detailDirty = false;
    }
    break;
  }

  case Page::MAIN:
  default: {

    // --- IMU 读取 + 摇一摇 / 翻转检测 ---
    if (M5.Imu.isEnabled()) {
      M5.Imu.update();
      float ax, ay, az;
      M5.Imu.getAccel(&ax, &ay, &az);

      // 摇一摇检测: 合加速度 > 阈值
      if (!isInteracting()) {
        float mag = sqrtf(ax * ax + ay * ay + az * az);
        if (mag > SHAKE_THRESHOLD) {
          g_shakeCount++;
          if (g_shakeCount >= SHAKE_FRAMES) {
            g_shakeCount = 0;
            g_shakeActive  = true;
            g_shakeStartMs = millis();
            g_shakePhrase  = kShakePhrases[esp_random() % SHAKE_PHRASE_COUNT];
            g_mood = max(0, g_mood - 5);
            closeGif();
            const GifEntry& pe = kPokeGifs[esp_random() % POKE_GIF_COUNT];
            size_t sz = pe.end - pe.start;
            if (g_frameBuf && g_gif.open((uint8_t*)pe.start, (int)sz, gifDraw)) {
              g_gif.setDrawType(GIF_DRAW_COOKED);
              g_gif.setFrameBuf(g_frameBuf);
              g_gifOpen = true;
              g_frameDelayMs = 50;
              g_lastFrameMs  = 0;
              g_canvas.fillScreen(BG_COLOR);
            }
            markDirty();
          }
        } else {
          g_shakeCount = 0;
        }
      }
    }

    // 摇一摇超时恢复
    if (g_shakeActive && (millis() - g_shakeStartMs) >= SHAKE_DURATION_MS) {
      g_shakeActive = false;
      g_shakePhrase = nullptr;
      openGifForState();
    }

    // 心情衰减: idle 30 分钟后开始, 每 30 分钟 -10, 最低 10
    if (g_state == PetState::IDLE && (millis() - g_lastMoodDecay) >= 1800000UL) {
      g_lastMoodDecay = millis();
      if (g_mood > 10) { g_mood = max(10, g_mood - 10); markDirty(); }
    }
    // 工作中心情自动回升: 每分钟 +2
    if (g_state == PetState::WORKING && (millis() - g_lastMoodDecay) >= 60000UL) {
      g_lastMoodDecay = millis();
      if (g_mood < 100) { g_mood = min(100, g_mood + 2); markDirty(); }
    }
    // 名字轮播: work+wait 超过 3 条时每 2s 翻页
    uint32_t tnow = millis();
    int totalNames = g_workCount + g_waitCount;
    if (totalNames > 3 && (tnow - g_lastCycleMs) >= INFO_CYCLE_MS) {
      g_lastCycleMs = tnow;
      int pages = (totalNames + 2) / 3;
      g_infoCycle = (g_infoCycle + 1) % pages;
    }

    // choosing 气泡: 多个等待终端时, 每 1s 轮播到下一个名字
    if (g_state == PetState::CHOOSING && g_waitCount > 1 &&
        (tnow - g_lastWaitCycleMs) >= WAIT_CYCLE_MS) {
      g_lastWaitCycleMs = tnow;
      g_waitCycle = (g_waitCycle + 1) % g_waitCount;
    }

    uint32_t now = millis();
    if (g_gifOpen && (now - g_lastFrameMs) >= (uint32_t)g_frameDelayMs) {
      g_lastFrameMs = now;
      int delayMs = 0;
      int rc = g_gif.playFrame(false, &delayMs);
      presentCanvas();
      g_frameDelayMs = (delayMs > 0) ? delayMs : 50;
      // 心情低时动画变慢
      if (g_state == PetState::IDLE && g_mood < 70 && g_mood >= 30) {
        g_frameDelayMs = g_frameDelayMs * 3 / 2;
      } else if (g_state == PetState::IDLE && g_mood < 30) {
        g_frameDelayMs = g_frameDelayMs * 2;
      }
      if (rc <= 0) {
        g_gif.reset();
        g_lastFrameMs = 0;
      }
    }

    // BtnA 长按=喂食
    if (M5.BtnA.wasHold() && !g_shakeActive && !g_feedActive) {
      g_feedActive  = true;
      g_feedStartMs = millis();
      g_feedPhrase  = kFeedPhrases[esp_random() % FEED_PHRASE_COUNT];
      g_mood = min(100, g_mood + 20);
      markDirty();
      closeGif();
      // 播放 eating GIF
      size_t sz = eating_gif_end - eating_gif_start;
      if (g_frameBuf && g_gif.open((uint8_t*)eating_gif_start, (int)sz, gifDraw)) {
        g_gif.setDrawType(GIF_DRAW_COOKED);
        g_gif.setFrameBuf(g_frameBuf);
        g_gifOpen = true;
        g_frameDelayMs = 50;
        g_lastFrameMs  = 0;
        g_canvas.fillScreen(BG_COLOR);
      }
    }

    // BtnA 短按=戳一戳 (不在喂食/摇晃中才触发)
    if (M5.BtnA.wasClicked() && !g_shakeActive && !g_feedActive) {
      g_mood = min(100, g_mood + 5);
      markDirty();
      g_pokeActive  = true;
      g_pokeStartMs = millis();
      g_pokePhrase  = kPokePhrases[esp_random() % POKE_PHRASE_COUNT];
      closeGif();
      const GifEntry& pe = kPokeGifs[esp_random() % POKE_GIF_COUNT];
      size_t sz = pe.end - pe.start;
      if (g_frameBuf && g_gif.open((uint8_t*)pe.start, (int)sz, gifDraw)) {
        g_gif.setDrawType(GIF_DRAW_COOKED);
        g_gif.setFrameBuf(g_frameBuf);
        g_gifOpen = true;
        g_frameDelayMs = 50;
        g_lastFrameMs  = 0;
        g_canvas.fillScreen(BG_COLOR);
      }
    }

    // 喂食超时恢复
    if (g_feedActive && (millis() - g_feedStartMs) >= FEED_DURATION_MS) {
      g_feedActive = false;
      g_feedPhrase = nullptr;
      openGifForState();
    }

    // 戳一戳/节日超时恢复
    uint32_t pokeDur = g_festivalActive ? FESTIVAL_DURATION_MS : POKE_DURATION_MS;
    if (g_pokeActive && (millis() - g_pokeStartMs) >= pokeDur) {
      g_pokeActive = false;
      g_pokePhrase = nullptr;
      g_festivalActive = false;
      openGifForState();
    }
    break;
  } // case MAIN
  } // switch

  delay(2);
}
