// ============================================================================
// ClawdPet - Multi-Agent (Claude / Codex / Pi) 赛博潮玩桌面宠物 v3
// M5Unified + 竖屏 135x240 + USB Serial(115200) + 21段GIF动画 + 粒子与物理引擎
//
// 新增趣味特性 (全静音办公室友好版):
//   1. 舞台像素粒子 + 昼夜动态星空 (夜间闪烁繁星+像素弯月, 敲代码冒0/1矩阵与火花, 喂食飘爱心)
//   2. RPG 等级养成系统 (Lv.1..99 + EXP 经验条, 算力与任务自动转化为经验, NVS 永久保存)
//   3. 21 段像素 GIF 全激活 (新增 gaming/singing/sleeping/listening/shower, 按昼夜与心情智能切换)
//   4. 陀螺仪重力倾斜物理 (左右倾斜机身, 宠物、气泡、粒子随之滑移互动)
//   5. 翻转桌面开启 25 分钟专注番茄钟 (扣放桌面再翻回自动开关 Pomodoro 倒计时)
//   6. 工具调用 Nx COMBO 连击徽章 + 底部实时算力心电图波形 (Activity ECG Sparkline)
// ============================================================================

#include <M5Unified.h>
#include <AnimatedGIF.h>
#include <Preferences.h>

enum class PetState { IDLE, WORKING, CHOOSING, DONE, ALL_DONE, ERROR };
static const int STATE_COUNT = 6;

static PetState g_state = PetState::IDLE;
static char     g_lineBuf[320];
static int      g_lineLen = 0;

// --- 屏幕 / GIF 几何 ---
static const int SCR_W   = 135;
static const int SCR_H   = 240;
static const int GIF_W   = 240;                  // GIF 原始宽
static const int CROP_X0 = (GIF_W - SCR_W) / 2;  // 居中裁切左偏移 = 52

// GIF 用绿幕 (0,255,0) 做透明/背景色键, 在屏上替换为该背景色。
static const uint16_t CHROMA_GREEN = 0xE007;     // RGB565 BE of (0,255,0) read as LE uint16_t
static const uint16_t BG_COLOR     = TFT_BLACK;  // 背景色 (绿幕替换为此色)
static const int      PET_Y_OFFSET = -18;        // 宠物整体上移 18px, 双脚踩在 y=202 底座舞台线上

// --- 气泡短语 + 主题色 ---
struct StateStyle {
  const char* name;
  const char* phrase;
  uint8_t tr, tg, tb;
};
static const StateStyle kStyles[STATE_COUNT] = {
  { "IDLE",     "ready! ~",    220, 230, 245 },  // 柔和亮白
  { "WORKING",  "thinking..",  255, 190,  40 },  // 琥珀金
  { "CHOOSING", "YOUR TURN!",    0, 220, 255 },  // 赛博亮青
  { "DONE",     "task done!",    0, 220, 115 },  // 翡翠绿
  { "ALL DONE", "all clear!",    0, 220, 115 },  // 翡翠绿
  { "ERROR",    "oops! err",   255,  90,  90 },  // 珊瑚红
};

// 计数 (由 #stat 更新)
static int g_total = 0, g_work = 0, g_done = 0;
static int g_agentWorkC = -1, g_agentWorkX = -1, g_agentWorkP = -1;

// 正在工作的终端名 (由 #work 更新, 兜底用)
static const int      MAX_WORK_NAMES = 8;
static String         g_workNames[MAX_WORK_NAMES];
static int            g_workCount    = 0;

// 主界面名字轮播
static int            g_infoCycle    = 0;
static uint32_t       g_lastCycleMs  = 0;
static const uint32_t INFO_CYCLE_MS  = 2400;

// 正在等用户选择/输入的终端名 (由 #wait 更新)
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

// --- Token 用量 (由 #tok 更新, 使用 int64_t 防止海量 Cache 溢出) ---
static int64_t  g_tokIn     = -1;
static int64_t  g_tokOut    = -1;
static int64_t  g_tokCache  = -1;
static int64_t  g_tokClaude = 0;
static int64_t  g_tokCodex  = 0;
static int64_t  g_tokPi     = 0;

// --- RPG 养成系统 (Level & EXP) ---
static int      g_level = 1;
static int      g_exp   = 0;   // 0..99, 满 100 升一级

// --- 工具连击 (Combo) & 实时心电图波形 (Sparkline) ---
static int      g_combo         = 0;
static uint32_t g_lastComboMs   = 0;
static const uint32_t COMBO_SHOW_MS = 18000;

static const int SPARK_LEN = 22;
static uint8_t   g_spark[SPARK_LEN] = {0};
static uint32_t  g_lastSparkStepMs  = 0;
static uint8_t   g_pendingPulse     = 0;

static void recordPulse(uint8_t strength) {
  if (strength > g_pendingPulse) g_pendingPulse = strength;
}

static void stepSparkline() {
  uint32_t now = millis();
  if (now - g_lastSparkStepMs < 600) return;
  g_lastSparkStepMs = now;
  for (int i = 0; i < SPARK_LEN - 1; i++) {
    g_spark[i] = g_spark[i + 1];
  }
  uint8_t base = (g_state == PetState::WORKING) ? (2 + (esp_random() % 3))
               : (g_state == PetState::CHOOSING) ? 5
               : 1;
  uint8_t val = max(base, g_pendingPulse);
  g_spark[SPARK_LEN - 1] = min((uint8_t)10, val);
  if (g_pendingPulse > 0) g_pendingPulse /= 2;
}

// --- 陀螺仪重力倾斜 & 翻转番茄钟 ---
static float    g_tiltSmooth     = 0.0f;
static int      g_tiltOffsetPx   = 0;      // -15 .. +15 像素水平偏移
static int      g_prevTiltPx     = 0;
static uint32_t g_tiltReactMs    = 0;

static bool     g_faceDown       = false;
static uint32_t g_faceDownStart  = 0;
static bool     g_pomoActive     = false;
static uint32_t g_pomoStartMs    = 0;
static const uint32_t POMO_TOTAL_MS = 25UL * 60UL * 1000UL; // 25 分钟

static const int MAX_SESSIONS = 8;
struct SessionInfo {
  String   name;
  char     state;      // 'R','W','D','P','E'
  uint32_t durSec;
  uint32_t recvMs;     // millis() 收到时刻, 用于本地自增
  uint16_t color;      // peacock 主题色 (RGB565)
  bool     hasColor;
  char     agent;      // 'C'=Claude, 'X'=Codex, 'P'=Pi
  String   tool;       // 当前工具名: bash/edit/read/code/think...
};
static SessionInfo g_sessions[MAX_SESSIONS];
static int         g_sessionCount = 0;

static const int MAX_HIST = 8;
struct HistEntry {
  char   agent;
  String name;
  String info;
};
static HistEntry g_history[MAX_HIST];
static int       g_histCount = 0;

// --- 嵌入的 21 段 GIF 数据 (platformio.ini board_build.embed_files) ---
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

// 新增解锁的 5 段动画
extern const uint8_t gaming_gif_start[]    asm("_binary_src_anim_gaming_gif_start");
extern const uint8_t gaming_gif_end[]      asm("_binary_src_anim_gaming_gif_end");
extern const uint8_t singing_gif_start[]   asm("_binary_src_anim_singing_gif_start");
extern const uint8_t singing_gif_end[]     asm("_binary_src_anim_singing_gif_end");
extern const uint8_t sleeping_gif_start[]  asm("_binary_src_anim_sleeping_gif_start");
extern const uint8_t sleeping_gif_end[]    asm("_binary_src_anim_sleeping_gif_end");
extern const uint8_t listening_gif_start[] asm("_binary_src_anim_listening_gif_start");
extern const uint8_t listening_gif_end[]   asm("_binary_src_anim_listening_gif_end");
extern const uint8_t shower_gif_start[]    asm("_binary_src_anim_shower_gif_start");
extern const uint8_t shower_gif_end[]      asm("_binary_src_anim_shower_gif_end");

struct GifEntry { const uint8_t* start; const uint8_t* end; };

static const GifEntry kIdleDayGifs[] = {
  { coffee_gif_start,    coffee_gif_end },
  { reading_gif_start,   reading_gif_end },
  { watering_gif_start,  watering_gif_end },
  { listening_gif_start, listening_gif_end },
  { gaming_gif_start,    gaming_gif_end },
  { singing_gif_start,   singing_gif_end },
  { shower_gif_start,    shower_gif_end },
  { nap_gif_start,       nap_gif_end },
};
static const GifEntry kIdleNightGifs[] = {
  { sleeping_gif_start,  sleeping_gif_end },
  { nap_gif_start,       nap_gif_end },
  { listening_gif_start, listening_gif_end },
  { reading_gif_start,   reading_gif_end },
};
static const GifEntry kWorkingGifs[] = {
  { coding_gif_start,    coding_gif_end },
  { painting_gif_start,  painting_gif_end },
  { guitar_gif_start,    guitar_gif_end },
  { hammer_gif_start,    hammer_gif_end },
  { listening_gif_start, listening_gif_end },
};
static const GifEntry kDoneGifs[] = {
  { photo_gif_start,   photo_gif_end },
  { eating_gif_start,  eating_gif_end },
  { cheer_gif_start,   cheer_gif_end },
  { singing_gif_start, singing_gif_end },
};
static const GifEntry kAllDoneGifs[] = {
  { dragon_boat_gif_start, dragon_boat_gif_end },
  { exercise_gif_start,    exercise_gif_end },
  { gaming_gif_start,      gaming_gif_end },
  { cheer_gif_start,       cheer_gif_end },
};
static const GifEntry kChoosingGifs[] = {
  { choosing_gif_start, choosing_gif_end },
};
static const GifEntry kErrorGifs[] = {
  { error_gif_start, error_gif_end },
};

// --- "戳一戳"互动池 (含新增的打游戏/唱歌/洗澡/听歌) ---
static const GifEntry kPokeGifs[] = {
  { eating_gif_start,    eating_gif_end },
  { exercise_gif_start,  exercise_gif_end },
  { watering_gif_start,  watering_gif_end },
  { coffee_gif_start,    coffee_gif_end },
  { photo_gif_start,     photo_gif_end },
  { poked_gif_start,     poked_gif_end },
  { gaming_gif_start,    gaming_gif_end },
  { singing_gif_start,   singing_gif_end },
  { listening_gif_start, listening_gif_end },
  { shower_gif_start,    shower_gif_end },
};
static const int POKE_GIF_COUNT = 10;

static const char* kPokePhrases[] = {
  "hydrate! ~", "stretch!", "you rock!",
  "coffee?", "go go go!", "awesome!",
  "snack?", "breathe~", "nice code!",
};
static const int POKE_PHRASE_COUNT = 9;

static bool     g_pokeActive   = false;
static uint32_t g_pokeStartMs  = 0;
static const uint32_t POKE_DURATION_MS = 3200;
static const char* g_pokePhrase = nullptr;
static char     g_customBubbleBuf[28] = {0};

// --- 摇一摇互动 ---
static int      g_shakeCount   = 0;
static bool     g_shakeActive  = false;
static uint32_t g_shakeStartMs = 0;
static const float SHAKE_THRESHOLD = 2.0f;
static const int   SHAKE_FRAMES   = 3;
static const uint32_t SHAKE_DURATION_MS = 3000;
static const char* g_shakePhrase = nullptr;
static const char* kShakePhrases[] = { "dizzy @_@", "woah!!", "hey!!", "stooop~" };
static const int SHAKE_PHRASE_COUNT = 4;

// --- 长按喂食 ---
static bool     g_feedActive   = false;
static uint32_t g_feedStartMs  = 0;
static const uint32_t FEED_DURATION_MS = 4000;
static const char* g_feedPhrase = nullptr;
static const char* kFeedPhrases[] = { "yummy! +20", "nom nom~", "thanks! <3", "so good!" };
static const int FEED_PHRASE_COUNT = 4;

// --- 节日问候 ---
static bool     g_festivalActive = false;
static uint32_t g_festivalStartMs = 0;
static const uint32_t FESTIVAL_DURATION_MS = 5000;

static bool isInteracting() {
  return g_pokeActive || g_shakeActive || g_feedActive;
}

// --- 今日统计 & 连胜 ---
static int      g_todayDone  = 0;
static int      g_streakDays = 1;

// --- 宠物心情值 ---
static int      g_mood = 75;
static uint32_t g_lastMoodDecay = 0;

// --- NVS 持久化 ---
static Preferences g_prefs;
static bool     g_saveDirty    = false;
static uint32_t g_saveDirtyMs  = 0;
static const uint32_t SAVE_DEBOUNCE_MS = 5000;

static AnimatedGIF g_gif;
static M5Canvas    g_canvas(&M5.Display);   // 135x240 离屏画布 (PSRAM)
static bool        g_gifOpen      = false;
static uint32_t    g_lastFrameMs  = 0;
static int         g_frameDelayMs = 50;

// GIF frame buffer: 分配一次, 所有 GIF 复用 (都是 240x240, 大小一样)
static uint8_t*    g_frameBuf     = nullptr;
static const int   FRAME_BUF_SIZE = GIF_W * (GIF_W + 3);  // 240*(240+3)

// ---------------------------------------------------------------------------
// 色彩辅助函数 (赛博潮玩 HUD 调色盘)
// ---------------------------------------------------------------------------
static uint16_t colSurface()    { return g_canvas.color565(18, 23, 35); }
static uint16_t colSurfaceAlt() { return g_canvas.color565(13, 17, 26); }
static uint16_t colBorder()     { return g_canvas.color565(42, 54, 78); }
static uint16_t colMuted()      { return g_canvas.color565(125, 140, 165); }
static uint16_t colClaude()     { return g_canvas.color565(255, 138, 61); }  // 琥珀橙
static uint16_t colCodex()      { return g_canvas.color565(0, 212, 255); }   // 极客青
static uint16_t colPi()         { return g_canvas.color565(188, 110, 255); } // 霓虹紫
static uint16_t colEmerald()    { return g_canvas.color565(0, 220, 115); }   // 翡翠绿
static uint16_t colAmber()      { return g_canvas.color565(255, 190, 40); }  // 活力金
static uint16_t colCoral()      { return g_canvas.color565(255, 85, 95); }   // 珊瑚红

static uint16_t agentColor(char ag) {
  switch (ag) {
    case 'X': case 'x': return colCodex();
    case 'P': case 'p': return colPi();
    default:            return colClaude();
  }
}

static uint16_t agentDimColor(char ag) {
  switch (ag) {
    case 'X': case 'x': return g_canvas.color565(18, 65, 82);
    case 'P': case 'p': return g_canvas.color565(58, 36, 84);
    default:            return g_canvas.color565(82, 46, 22);
  }
}

static uint16_t stateColor(char s) {
  switch (s) {
    case 'R': return colAmber();
    case 'W': return colCodex();
    case 'D': return colEmerald();
    case 'E': return colCoral();
    default:  return colMuted();
  }
}

static const char* stateLabel(char s) {
  switch (s) {
    case 'R': return "RUN";
    case 'W': return "WAIT";
    case 'D': return "DONE";
    case 'P': return "IDLE";
    case 'E': return "ERR";
    default:  return "?";
  }
}

static const char* levelTitle(int lv) {
  if (lv >= 30) return "CyberGod";
  if (lv >= 20) return "Wizard";
  if (lv >= 12) return "Architect";
  if (lv >= 6)  return "Hacker";
  if (lv >= 3)  return "Coder";
  return "Novice";
}

// ---------------------------------------------------------------------------
// 舞台像素粒子系统 (Particle Engine)
// ---------------------------------------------------------------------------
enum ParticleType : uint8_t {
  PT_NONE = 0,
  PT_BIT0,     // '0' 矩阵数字
  PT_BIT1,     // '1' 矩阵数字
  PT_SPARK,    // '+' 金色打铁/写代码火花
  PT_HEART,    // 像素小红心
  PT_ZZZ,      // 'z' 睡眠气泡
  PT_STAR,     // 星点粒子
};

struct Particle {
  float    x, y;
  float    vx, vy;
  uint8_t  type;
  uint8_t  life;
  uint8_t  maxLife;
  uint16_t col;
};

static const int MAX_PARTICLES = 14;
static Particle  g_particles[MAX_PARTICLES];
static uint32_t  g_lastPtSpawnMs = 0;

static void spawnParticle(uint8_t type, float x, float y, float vx, float vy, uint8_t maxLife, uint16_t col) {
  for (int i = 0; i < MAX_PARTICLES; i++) {
    if (g_particles[i].life == 0) {
      g_particles[i] = { x, y, vx, vy, type, maxLife, maxLife, col };
      return;
    }
  }
}

static void burstParticles(uint8_t type, uint16_t col, int count) {
  for (int i = 0; i < count; i++) {
    float rx = 30.0f + (esp_random() % 75);
    float ry = 140.0f + (esp_random() % 45);
    float vx = ((int)(esp_random() % 21) - 10) * 0.08f;
    float vy = -0.45f - (esp_random() % 10) * 0.06f;
    spawnParticle(type, rx, ry, vx, vy, 22 + (esp_random() % 15), col);
  }
}

// ---------------------------------------------------------------------------
// NVS 持久化 & EXP 升级逻辑
// ---------------------------------------------------------------------------
static void markDirty() {
  if (!g_saveDirty) {
    g_saveDirty   = true;
    g_saveDirtyMs = millis();
  }
}

static void addExp(int amount) {
  if (amount <= 0) return;
  g_exp += amount;
  bool leveledUp = false;
  while (g_exp >= 100) {
    g_exp -= 100;
    if (g_level < 99) {
      g_level++;
      leveledUp = true;
    }
  }
  if (leveledUp) {
    snprintf(g_customBubbleBuf, sizeof(g_customBubbleBuf), "LEVEL UP! Lv.%d", g_level);
    g_pokeActive  = true;
    g_pokeStartMs = millis();
    g_pokePhrase  = g_customBubbleBuf;
    burstParticles(PT_HEART, colAmber(), 6);
  }
  markDirty();
}

static void saveState() {
  g_prefs.putInt("mood", g_mood);
  g_prefs.putInt("todayDone", g_todayDone);
  g_prefs.putInt("streak", g_streakDays);
  g_prefs.putInt("level", g_level);
  g_prefs.putInt("exp", g_exp);
  if (g_timeValid) {
    g_prefs.putInt("lastMin", currentMinOfDay());
  }
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
    String packed = g_sessions[i].name + ":" + String((char)g_sessions[i].state)
                  + ":" + String(g_sessions[i].durSec) + "::"
                  + String((char)g_sessions[i].agent) + ":" + g_sessions[i].tool;
    g_prefs.putString(key, packed);
  }
  g_saveDirty = false;
  Serial.println("[nvs] saved");
}

static void loadState() {
  g_prefs.begin("clawdpet", false);
  g_mood       = g_prefs.getInt("mood", 75);
  g_todayDone  = g_prefs.getInt("todayDone", 0);
  g_streakDays = max(1, g_prefs.getInt("streak", 1));
  g_level      = max(1, g_prefs.getInt("level", 1 + g_streakDays));
  g_exp        = constrain(g_prefs.getInt("exp", (g_todayDone * 25) % 100), 0, 99);
  int savedMin = g_prefs.getInt("lastMin", -1);
  if (savedMin >= 0 && savedMin < 1440) {
    g_baseMinOfDay = savedMin;
    g_baseMillis   = millis();
    g_timeValid    = true;
  }
  g_state      = (PetState)g_prefs.getInt("state", 0);
  g_workCount  = g_prefs.getInt("workCount", 0);
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
      g_sessions[i].name     = packed.substring(0, c1);
      g_sessions[i].state    = packed.charAt(c1 + 1);
      g_sessions[i].durSec   = packed.substring(c2 + 1).toInt();
      g_sessions[i].recvMs   = now;
      g_sessions[i].hasColor = false;
      g_sessions[i].color    = 0;
      g_sessions[i].agent    = 'C';
      g_sessions[i].tool     = "";
    }
  }
  Serial.printf("[nvs] loaded: lv=%d exp=%d mood=%d todayDone=%d\n",
                g_level, g_exp, g_mood, g_todayDone);
}

static void pollSave() {
  if (g_saveDirty && (millis() - g_saveDirtyMs) >= SAVE_DEBOUNCE_MS) {
    saveState();
  }
}

// ---------------------------------------------------------------------------
// 时间与格式化辅助
// ---------------------------------------------------------------------------
static int currentMinOfDay() {
  if (!g_timeValid) return -1;
  uint32_t elapsedMin = (millis() - g_baseMillis) / 60000UL;
  return (g_baseMinOfDay + (int)elapsedMin) % 1440;
}

static bool isNightTime() {
  int m = currentMinOfDay();
  if (m < 0) return false;
  int h = m / 60;
  return (h >= 21 || h < 6);
}

static const uint8_t* gifDataForState(PetState s, size_t* outSize) {
  const GifEntry* pool = kIdleDayGifs;
  int count = 8;

  switch (s) {
    case PetState::IDLE: {
      int m = currentMinOfDay();
      int h = (m >= 0) ? (m / 60) : 14;
      if (h >= 23 || h < 6) {
        pool = kIdleNightGifs;
        count = 4;
      } else {
        pool = kIdleDayGifs;
        count = 8;
      }
      break;
    }
    case PetState::WORKING:  pool = kWorkingGifs;  count = 5; break;
    case PetState::CHOOSING: pool = kChoosingGifs; count = 1; break;
    case PetState::DONE:     pool = kDoneGifs;     count = 4; break;
    case PetState::ALL_DONE: pool = kAllDoneGifs;  count = 4; break;
    case PetState::ERROR:    pool = kErrorGifs;    count = 1; break;
  }

  int idx = (count > 1) ? (esp_random() % count) : 0;
  const GifEntry& e = pool[idx];
  *outSize = e.end - e.start;
  return e.start;
}

// --- 解码回调: 支持重力倾斜偏移 g_tiltOffsetPx ---
static uint16_t g_rowBuf[SCR_W];
static uint16_t g_gifBgColor565 = CHROMA_GREEN;

static void gifDraw(GIFDRAW* p) {
  int y = p->iY + p->y + PET_Y_OFFSET;
  if (y < 0 || y >= SCR_H) return;
  if (p->pPalette) g_gifBgColor565 = p->pPalette[p->ucBackground];
  const uint16_t* src = (const uint16_t*)p->pPixels;
  int cropX = constrain(CROP_X0 - g_tiltOffsetPx, 10, GIF_W - SCR_W - 10);
  int srcStart = max(0, cropX - p->iX);
  int srcEnd   = min(p->iWidth, cropX + SCR_W - p->iX);
  if (srcStart >= srcEnd) return;
  int dstX = p->iX + srcStart - cropX;
  int w = srcEnd - srcStart;
  for (int i = 0; i < w; i++) {
    uint16_t c = src[srcStart + i];
    g_rowBuf[i] = (c == CHROMA_GREEN || c == g_gifBgColor565) ? BG_COLOR : c;
  }
  g_canvas.pushImage(dstX, y, w, 1, g_rowBuf);
}

static void fmtTok(int64_t val, char* buf, int bufSz) {
  if (val < 0) { snprintf(buf, bufSz, "--"); return; }
  if (val < 1000) { snprintf(buf, bufSz, "%d", (int)val); return; }
  if (val < 1000000) {
    int whole = (int)(val / 1000);
    int frac  = (int)((val % 1000) / 100);
    if (whole < 10 && frac > 0) snprintf(buf, bufSz, "%d.%dK", whole, frac);
    else snprintf(buf, bufSz, "%dK", whole);
    return;
  }
  int whole = (int)(val / 1000000);
  int frac  = (int)((val % 1000000) / 100000);
  if (frac > 0 && whole < 100) snprintf(buf, bufSz, "%d.%dM", whole, frac);
  else snprintf(buf, bufSz, "%dM", whole);
}

static String formatDuration(uint32_t sec) {
  if (sec < 60) return String(sec) + "s";
  if (sec < 3600) return String(sec / 60) + "m";
  uint32_t h = sec / 3600;
  uint32_t m = (sec % 3600) / 60;
  if (h < 10 && m > 0) return String(h) + "h" + String(m) + "m";
  return String(h) + "h";
}

// 绘制 7x6 像素小爱心
static void drawPixelHeart(int x, int y, uint16_t col) {
  g_canvas.drawFastHLine(x + 1, y,     2, col);
  g_canvas.drawFastHLine(x + 4, y,     2, col);
  g_canvas.drawFastHLine(x,     y + 1, 7, col);
  g_canvas.drawFastHLine(x,     y + 2, 7, col);
  g_canvas.drawFastHLine(x + 1, y + 3, 5, col);
  g_canvas.drawFastHLine(x + 2, y + 4, 3, col);
  g_canvas.drawPixel(    x + 3, y + 5,    col);
}

// 绘制 5x5 迷你粒子心
static void drawMiniHeart(int x, int y, uint16_t col) {
  g_canvas.drawPixel(x + 1, y, col);
  g_canvas.drawPixel(x + 3, y, col);
  g_canvas.drawFastHLine(x, y + 1, 5, col);
  g_canvas.drawFastHLine(x + 1, y + 2, 3, col);
  g_canvas.drawPixel(x + 2, y + 3, col);
}

// 绘制像素弯月
static void drawPixelMoon(int cx, int cy) {
  uint16_t moonCol = g_canvas.color565(255, 225, 110);
  g_canvas.fillCircle(cx, cy, 6, moonCol);
  g_canvas.fillCircle(cx - 3, cy - 2, 5, TFT_BLACK);
}

// 在 GIF 解码前擦除上一帧粒子与可变寬度气泡区域, 防止残影同时保留 GIF 帧间复用
static void clearDynamicRegionsBeforeGif() {
  if (g_tiltOffsetPx != g_prevTiltPx) {
    g_canvas.fillRect(0, 38, SCR_W, 164, BG_COLOR);
    g_prevTiltPx = g_tiltOffsetPx;
  }
  // 擦除顶部会话卡区域 (y=18..62) 和 气泡+Combo区域 (y=88..133)
  g_canvas.fillRect(0, 18, SCR_W, 44, BG_COLOR);
  g_canvas.fillRect(0, 88, SCR_W, 45, BG_COLOR);

  // 擦除每个粒子的上一帧位置
  for (int i = 0; i < MAX_PARTICLES; i++) {
    if (g_particles[i].life > 0) {
      int px = (int)g_particles[i].x;
      int py = (int)g_particles[i].y;
      g_canvas.fillRect(max(0, px - 4), max(18, py - 5), 9, 10, BG_COLOR);
    }
  }
}

// ---------------------------------------------------------------------------
// 舞台背景氛围 (昼夜星空) + 动态粒子更新与绘制
// ---------------------------------------------------------------------------
static void drawStageAtmosphereAndParticles() {
  uint32_t now = millis();

  // 1. 昼夜星空背景点缀 (y: 44..185)
  static const uint8_t kStarPos[10][2] = {
    {12, 50}, {32, 68}, {68, 48}, {102, 56}, {122, 82},
    {18, 92}, {92, 95}, {14, 132}, {118, 128}, {52, 76}
  };
  bool night = isNightTime();
  for (int i = 0; i < 10; i++) {
    int sx = constrain((int)kStarPos[i][0] + g_tiltOffsetPx / 3, 4, SCR_W - 5);
    int sy = kStarPos[i][1];
    uint32_t phase = (now / 350 + i * 3) % 6;
    if (night) {
      uint16_t sCol = (phase == 0) ? colAmber()
                    : (phase < 3)  ? g_canvas.color565(140, 165, 220)
                    : g_canvas.color565(45, 60, 90);
      g_canvas.drawPixel(sx, sy, sCol);
      if (phase == 0 && (i % 3 == 0)) {
        g_canvas.drawPixel(sx - 1, sy, sCol);
        g_canvas.drawPixel(sx + 1, sy, sCol);
        g_canvas.drawPixel(sx, sy - 1, sCol);
        g_canvas.drawPixel(sx, sy + 1, sCol);
      }
    } else if (i % 2 == 0 && phase < 3) {
      // 白天微弱赛博微尘
      g_canvas.drawPixel(sx, sy, g_canvas.color565(30, 50, 75));
    }
  }
  if (night) {
    drawPixelMoon(constrain(117 + g_tiltOffsetPx / 3, 95, 126), 62);
  }

  // 2. 按当前状态自动生成粒子
  uint32_t spawnInterval = (g_state == PetState::WORKING) ? 280 : 650;
  if (now - g_lastPtSpawnMs >= spawnInterval) {
    g_lastPtSpawnMs = now;
    float spawnX = (SCR_W / 2) + g_tiltOffsetPx + ((int)(esp_random() % 64) - 32);
    spawnX = constrain(spawnX, 10.0f, (float)(SCR_W - 12));

    if (g_feedActive || g_pokeActive) {
      spawnParticle(PT_HEART, spawnX, 160.0f, 0.0f, -0.6f, 26, colCoral());
    } else if (g_state == PetState::WORKING) {
      // 检查当前工具类型
      bool isEdit = false;
      for (int i = 0; i < g_sessionCount; i++) {
        if (g_sessions[i].state == 'R' &&
            (g_sessions[i].tool.indexOf("edit") >= 0 || g_sessions[i].tool.indexOf("write") >= 0)) {
          isEdit = true;
          break;
        }
      }
      if (isEdit) {
        spawnParticle(PT_SPARK, spawnX, 168.0f, ((int)(esp_random() % 11) - 5) * 0.07f, -0.65f, 24, colAmber());
      } else {
        uint8_t bType = (esp_random() % 2 == 0) ? PT_BIT0 : PT_BIT1;
        uint16_t bCol = (g_combo >= 5) ? colPi() : colEmerald();
        spawnParticle(bType, spawnX, 175.0f, 0.0f, -0.55f, 28, bCol);
      }
    } else if (g_state == PetState::DONE || g_state == PetState::ALL_DONE) {
      spawnParticle(PT_STAR, spawnX, 165.0f, ((int)(esp_random() % 11) - 5) * 0.06f, -0.6f, 26, colAmber());
    } else if (g_state == PetState::IDLE && (night || g_mood < 40)) {
      spawnParticle(PT_ZZZ, (SCR_W / 2) + g_tiltOffsetPx + 14, 142.0f, 0.15f, -0.35f, 32, colCodex());
    }
  }

  // 3. 更新并绘制粒子
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  float gravityDrift = g_tiltSmooth * 0.35f;

  for (int i = 0; i < MAX_PARTICLES; i++) {
    Particle& pt = g_particles[i];
    if (pt.life == 0) continue;
    pt.x += pt.vx + gravityDrift;
    pt.y += pt.vy;
    pt.life--;
    int px = (int)pt.x;
    int py = (int)pt.y;
    if (px < 3 || px >= SCR_W - 6 || py < 38 || py >= 198) {
      pt.life = 0;
      continue;
    }
    switch (pt.type) {
      case PT_BIT0:
      case PT_BIT1: {
        g_canvas.setTextColor(pt.col);
        g_canvas.setTextDatum(middle_center);
        g_canvas.drawString(pt.type == PT_BIT0 ? "0" : "1", px, py);
        break;
      }
      case PT_SPARK:
      case PT_STAR: {
        g_canvas.drawPixel(px, py, pt.col);
        g_canvas.drawPixel(px - 1, py, pt.col);
        g_canvas.drawPixel(px + 1, py, pt.col);
        g_canvas.drawPixel(px, py - 1, pt.col);
        g_canvas.drawPixel(px, py + 1, pt.col);
        break;
      }
      case PT_HEART: {
        drawMiniHeart(px - 2, py - 2, pt.col);
        break;
      }
      case PT_ZZZ: {
        g_canvas.setTextColor(pt.col);
        g_canvas.setTextDatum(middle_center);
        g_canvas.drawString((pt.life > 16) ? "z" : "Z", px, py);
        break;
      }
    }
  }
}

// 绘制底部三页圆点指示器 (用于 Page 2 / Page 3)
static void drawPageDots(int activePage) {
  int y = 236;
  int cx = SCR_W / 2;
  for (int i = 0; i < 3; i++) {
    int x = cx + (i - 1) * 10;
    if (i == activePage) {
      g_canvas.fillRoundRect(x - 4, y - 1, 8, 3, 1, colCodex());
    } else {
      g_canvas.fillCircle(x, y, 1, colBorder());
    }
  }
}

// ---------------------------------------------------------------------------
// 通用顶栏 (y: 0..17): 时间/番茄钟 | Lv.x + 爱心心情 | 电池 + 底部 EXP 细条
// ---------------------------------------------------------------------------
static void drawStatusBarOverlay() {
  g_canvas.fillRect(0, 0, SCR_W, 18, colSurfaceAlt());
  g_canvas.drawFastHLine(0, 17, SCR_W, colBorder());
  // 顶栏底边 1px RPG 经验进度条 (EXP Bar)
  int expW = (SCR_W * g_exp) / 100;
  if (expW > 0) {
    g_canvas.drawFastHLine(0, 17, expW, colPi());
  }

  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);

  // 左: 永远固定显示 24h 时间 (不再被番茄钟覆盖)
  char ts[6];
  int m = currentMinOfDay();
  if (m < 0) strcpy(ts, "--:--");
  else       snprintf(ts, sizeof(ts), "%02d:%02d", m / 60, m % 60);
  g_canvas.setTextColor(TFT_WHITE, colSurfaceAlt());
  g_canvas.setTextDatum(top_left);
  g_canvas.drawString(ts, 4, 5);

  // 若开启专注番茄钟, 在后台检查完成并在舞台左上角显示胶囊
  if (g_pomoActive) {
    uint32_t elapsed = millis() - g_pomoStartMs;
    if (elapsed >= POMO_TOTAL_MS) {
      g_pomoActive = false;
      g_mood = min(100, g_mood + 15);
      addExp(50);
      snprintf(g_customBubbleBuf, sizeof(g_customBubbleBuf), "POMO DONE! +50XP");
      g_pokeActive  = true;
      g_pokeStartMs = millis();
      g_pokePhrase  = g_customBubbleBuf;
    }
  }

  // 中: Lv.n + 像素小爱心 + 心情
  char lvStr[8];
  snprintf(lvStr, sizeof(lvStr), "L%d", g_level);
  g_canvas.setTextColor(colAmber(), colSurfaceAlt());
  g_canvas.setTextDatum(top_left);
  g_canvas.drawString(lvStr, 40, 5);

  uint16_t moodCol = (g_mood >= 70) ? colEmerald()
                   : (g_mood >= 35) ? colAmber()
                   : colCoral();
  int hx = (g_level >= 10) ? 60 : 54;
  drawPixelHeart(hx, 6, moodCol);
  char ms[6];
  snprintf(ms, sizeof(ms), "%d", g_mood);
  g_canvas.setTextColor(moodCol, colSurfaceAlt());
  g_canvas.setTextDatum(top_left);
  g_canvas.drawString(ms, hx + 9, 5);

  // 右: 电池百分比 + 电池图标
  int pct = g_battLevel < 0 ? 0 : g_battLevel;
  uint16_t bCol = TFT_WHITE;
  if (g_charging)     bCol = colEmerald();
  else if (pct <= 20) bCol = colCoral();

  char ps[6];
  snprintf(ps, sizeof(ps), "%d%%", pct);
  int bx = SCR_W - 4 - 16;
  int by = 4;
  g_canvas.setTextColor(bCol, colSurfaceAlt());
  g_canvas.setTextDatum(top_right);
  g_canvas.drawString(ps, bx - 2, 5);

  g_canvas.drawRect(bx, by, 16, 9, bCol);
  g_canvas.fillRect(bx + 16, by + 2, 2, 5, bCol);
  int fillW = (14 * pct) / 100;
  if (fillW > 0) g_canvas.fillRect(bx + 1, by + 1, fillW, 7, bCol);
}

// ---------------------------------------------------------------------------
// 统计各端当前活跃数 (Claude / Codex / Pi)
// ---------------------------------------------------------------------------
static void getAgentActiveCounts(int* outC, int* outX, int* outP) {
  if (g_agentWorkC >= 0 && g_agentWorkX >= 0 && g_agentWorkP >= 0) {
    *outC = g_agentWorkC;
    *outX = g_agentWorkX;
    *outP = g_agentWorkP;
    return;
  }
  int c = 0, x = 0, p = 0;
  for (int i = 0; i < g_sessionCount; i++) {
    if (g_sessions[i].state == 'R' || g_sessions[i].state == 'W') {
      if (g_sessions[i].agent == 'X') x++;
      else if (g_sessions[i].agent == 'P') p++;
      else c++;
    }
  }
  *outC = c; *outX = x; *outP = p;
}

// 绘制单个三端迷你胶囊 (C:n / X:n / P:n)
static void drawAgentPill(int x, int y, int w, int h, char label, int count, uint16_t brandCol, uint16_t dimCol) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%c:%d", label, count);
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  if (count > 0) {
    g_canvas.fillRoundRect(x, y, w, h, 3, brandCol);
    g_canvas.setTextColor(TFT_BLACK, brandCol);
  } else {
    g_canvas.fillRoundRect(x, y, w, h, 3, colSurfaceAlt());
    g_canvas.drawRoundRect(x, y, w, h, 3, dimCol);
    g_canvas.setTextColor(brandCol, colSurfaceAlt());
  }
  g_canvas.setTextDatum(middle_center);
  g_canvas.drawString(buf, x + w / 2, y + h / 2 + 1);
}

// ---------------------------------------------------------------------------
// 底部双行 HUD 底座 (y=202..239):
//   第一行: [C:n] [X:n] [P:n] 三端实时胶囊
//   第二行: OK:n | 实时算力脉冲心电图 (ECG Sparkline) | 页码三点
// ---------------------------------------------------------------------------
static void drawBottomHudDock() {
  int dockY = 202, dockH = 37;
  g_canvas.fillRoundRect(2, dockY, SCR_W - 4, dockH, 5, colSurface());
  g_canvas.drawRoundRect(2, dockY, SCR_W - 4, dockH, 5, colBorder());
  // 顶部亮边作为宠物站立的舞台发光地板线
  uint16_t stageLineCol = (g_combo >= 5) ? colPi()
                        : (g_state == PetState::WORKING) ? colAmber()
                        : g_canvas.color565(65, 85, 120);
  g_canvas.drawFastHLine(6, dockY, SCR_W - 12, stageLineCol);

  int cAct = 0, xAct = 0, pAct = 0;
  getAgentActiveCounts(&cAct, &xAct, &pAct);

  // 第一行 (y=205..218): 三端状态胶囊居中均分
  int pillW = 38, pillH = 13, gap = 4;
  int startX = (SCR_W - (pillW * 3 + gap * 2)) / 2;
  int row1Y = dockY + 4;
  drawAgentPill(startX,                   row1Y, pillW, pillH, 'C', cAct, colClaude(), agentDimColor('C'));
  drawAgentPill(startX + pillW + gap,     row1Y, pillW, pillH, 'X', xAct, colCodex(),  agentDimColor('X'));
  drawAgentPill(startX + (pillW + gap)*2, row1Y, pillW, pillH, 'P', pAct, colPi(),     agentDimColor('P'));

  // 第二行 (y=221..235): 左=OK:n  中=实时算力脉冲心电图  右=页码三点
  int row2Y = dockY + 26;
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);

  char doneBuf[10];
  snprintf(doneBuf, sizeof(doneBuf), "OK:%d", g_todayDone);
  g_canvas.setTextColor(colEmerald(), colSurface());
  g_canvas.setTextDatum(middle_left);
  g_canvas.drawString(doneBuf, 7, row2Y);

  // 中间 (x=42..106): 实时算力脉冲波形 (ECG Sparkline)
  int spX = 43, spW = 64, spBaseY = dockY + 32;
  g_canvas.drawFastHLine(spX, spBaseY, spW, colSurfaceAlt());
  uint16_t waveCol = (g_combo >= 5) ? colPi()
                   : (g_state == PetState::WORKING) ? colAmber()
                   : colCodex();
  for (int i = 0; i < SPARK_LEN - 1; i++) {
    int x0 = spX + (i * spW) / (SPARK_LEN - 1);
    int x1 = spX + ((i + 1) * spW) / (SPARK_LEN - 1);
    int y0 = spBaseY - g_spark[i];
    int y1 = spBaseY - g_spark[i + 1];
    g_canvas.drawLine(x0, y0, x1, y1, waveCol);
  }

  // 右下角: 紧凑页码三点
  int dotBaseX = SCR_W - 19;
  int dotY = row2Y;
  g_canvas.fillRoundRect(dotBaseX - 3, dotY - 1, 6, 3, 1, colCodex());
  g_canvas.fillCircle(dotBaseX + 6, dotY, 1, colBorder());
  g_canvas.fillCircle(dotBaseX + 11, dotY, 1, colBorder());
}

// 收集当前活跃 (W/R) 会话下标, W 优先于 R
static int collectActiveSessionIndices(int* outIdx, int maxCount) {
  int n = 0;
  for (int i = 0; i < g_sessionCount && n < maxCount; i++) {
    if (g_sessions[i].state == 'W') outIdx[n++] = i;
  }
  for (int i = 0; i < g_sessionCount && n < maxCount; i++) {
    if (g_sessions[i].state == 'R') outIdx[n++] = i;
  }
  return n;
}

static void drawSessionCardsOverlay() {
  int activeIdx[MAX_SESSIONS];
  int activeCount = collectActiveSessionIndices(activeIdx, MAX_SESSIONS);

  int baseY = 20;

  if (activeCount > 0) {
    int maxShow = 2;
    int pages = (activeCount + maxShow - 1) / maxShow;
    int startIdx = (pages > 1) ? ((g_infoCycle % pages) * maxShow) : 0;
    int endIdx = min(activeCount, startIdx + maxShow);
    bool blink = ((millis() / 450) % 2) == 0;

    for (int row = 0; row < (endIdx - startIdx); row++) {
      const SessionInfo& s = g_sessions[activeIdx[startIdx + row]];
      int cy = baseY + row * 20;
      int ch = 18;
      bool isWait = (s.state == 'W');

      uint16_t agCol = agentColor(s.agent);
      uint16_t stripeCol = s.hasColor ? s.color : agCol;
      uint16_t borderCol = isWait ? (blink ? colCodex() : colBorder()) : colBorder();

      g_canvas.fillRoundRect(3, cy, SCR_W - 6, ch, 4, colSurface());
      g_canvas.drawRoundRect(3, cy, SCR_W - 6, ch, 4, borderCol);
      g_canvas.fillRoundRect(4, cy + 2, 3, ch - 4, 1, stripeCol);

      // 平台微徽章 [C]/[X]/[P]
      g_canvas.fillRoundRect(9, cy + 3, 11, 11, 2, agentDimColor(s.agent));
      g_canvas.setFont(nullptr);
      g_canvas.setTextSize(1);
      g_canvas.setTextColor(agCol);
      g_canvas.setTextDatum(middle_center);
      char agStr[2] = { s.agent ? s.agent : 'C', '\0' };
      g_canvas.drawString(agStr, 14, cy + 8);

      // 项目名称
      g_canvas.setFont(&fonts::efontCN_12);
      g_canvas.setTextSize(1);
      g_canvas.setTextColor(TFT_WHITE);
      g_canvas.setTextDatum(middle_left);
      String nm = s.name.substring(0, 9);
      g_canvas.drawString(nm, 23, cy + ch / 2);

      // 右侧工具/状态标签
      g_canvas.setFont(nullptr);
      g_canvas.setTextSize(1);
      g_canvas.setTextDatum(middle_right);
      if (isWait) {
        g_canvas.setTextColor(blink ? colCodex() : TFT_WHITE);
        g_canvas.drawString("WAIT!", SCR_W - 7, cy + ch / 2 + 1);
      } else if (s.tool.length() > 0) {
        String tLabel = ">" + s.tool.substring(0, 5);
        g_canvas.setTextColor(colAmber());
        g_canvas.drawString(tLabel, SCR_W - 7, cy + ch / 2 + 1);
      } else {
        const char* dots = ((millis() / 400) % 2 == 0) ? "RUN.." : "RUN.";
        g_canvas.setTextColor(colAmber());
        g_canvas.drawString(dots, SCR_W - 7, cy + ch / 2 + 1);
      }
    }

    if (activeCount > maxShow) {
      char moreBuf[8];
      snprintf(moreBuf, sizeof(moreBuf), "+%d", activeCount - maxShow);
      g_canvas.fillRoundRect(SCR_W - 22, baseY - 1, 19, 9, 2, colPi());
      g_canvas.setFont(nullptr);
      g_canvas.setTextSize(1);
      g_canvas.setTextColor(TFT_BLACK);
      g_canvas.setTextDatum(middle_center);
      g_canvas.drawString(moreBuf, SCR_W - 12, baseY + 3);
    }
    return;
  }

  // Fallback: 若 #sess 为空但 #work / #wait 有名字
  int legacyTotal = g_workCount + g_waitCount;
  if (legacyTotal > 0) {
    int show = min(2, legacyTotal);
    for (int i = 0; i < show; i++) {
      int cy = baseY + i * 20;
      bool isWait = (i >= g_workCount);
      const String& nm = isWait ? g_waitNames[i - g_workCount] : g_workNames[i];
      g_canvas.fillRoundRect(3, cy, SCR_W - 6, 18, 4, colSurface());
      g_canvas.drawRoundRect(3, cy, SCR_W - 6, 18, 4, isWait ? colCodex() : colBorder());
      g_canvas.setFont(&fonts::efontCN_12);
      g_canvas.setTextColor(isWait ? colCodex() : TFT_WHITE);
      g_canvas.setTextDatum(middle_left);
      g_canvas.drawString(nm.substring(0, 10), 10, cy + 9);
    }
    return;
  }

  // 空闲态: 若有最近完成任务, 顶部仅显示一条轻量胶囊
  if (g_histCount > 0) {
    const HistEntry& h = g_history[0];
    int pillH = 18;
    uint16_t agCol = agentColor(h.agent);
    g_canvas.fillRoundRect(3, baseY, SCR_W - 6, pillH, 4, colSurfaceAlt());
    g_canvas.drawRoundRect(3, baseY, SCR_W - 6, pillH, 4, colBorder());

    g_canvas.fillRoundRect(6, baseY + 3, 11, 11, 2, agentDimColor(h.agent));
    g_canvas.setFont(nullptr);
    g_canvas.setTextSize(1);
    g_canvas.setTextColor(agCol);
    g_canvas.setTextDatum(middle_center);
    char agStr[2] = { h.agent ? h.agent : 'C', '\0' };
    g_canvas.drawString(agStr, 11, baseY + 8);

    g_canvas.setFont(&fonts::efontCN_12);
    g_canvas.setTextColor(colMuted());
    g_canvas.setTextDatum(middle_left);
    g_canvas.drawString(h.name.substring(0, 8), 21, baseY + 9);

    g_canvas.setFont(nullptr);
    g_canvas.setTextColor(colEmerald());
    g_canvas.setTextDatum(middle_right);
    g_canvas.drawString(String("done ") + h.info, SCR_W - 7, baseY + 9);
  }
}

// 根据当前工具/连击/倾斜/时段生成生动的气泡文案
static const char* resolveDynamicPhrase() {
  if (abs(g_tiltOffsetPx) >= 10) {
    return (g_tiltOffsetPx > 0) ? "sliding >>" : "<< sliding";
  }

  if (g_state == PetState::WORKING) {
    for (int i = 0; i < g_sessionCount; i++) {
      if (g_sessions[i].state == 'R' && g_sessions[i].tool.length() > 0) {
        String t = g_sessions[i].tool;
        t.toLowerCase();
        if (t.indexOf("bash") >= 0)     return "running cmd..";
        if (t.indexOf("edit") >= 0 || t.indexOf("write") >= 0) return "writing code..";
        if (t.indexOf("read") >= 0 || t.indexOf("grep") >= 0)  return "reading code..";
        if (t.indexOf("code") >= 0)     return "hacking js..";
        if (t.indexOf("sub") >= 0)      return "teamwork! ~";
        if (t.indexOf("mcp") >= 0)      return "calling mcp..";
        if (t.indexOf("think") >= 0)    return "thinking..";
      }
    }
    static const char* kWorkCycle[] = { "thinking..", "cooking..", "on it! ~" };
    return kWorkCycle[(millis() / 3000) % 3];
  }

  if (g_state == PetState::IDLE) {
    if (g_mood < 30) {
      static const char* kSad[] = { "bored...", "poke me?", "feed me~" };
      return kSad[(millis() / 5000) % 3];
    }
    int m = currentMinOfDay();
    int hour = (m >= 0) ? (m / 60) : 14;
    if (hour >= 23 || hour < 5) {
      static const char* kNight[] = { "sleepy zzz", "late night~", "rest soon?" };
      return kNight[(millis() / 6000) % 3];
    }
    if (hour >= 5 && hour < 10) {
      static const char* kMorn[] = { "morning! ~", "coffee time", "ready!" };
      return kMorn[(millis() / 6000) % 3];
    }
    static const char* kIdle[] = { "ready! ~", "chilling~", "waiting." };
    return kIdle[(millis() / 6000) % 3];
  }

  return kStyles[(int)g_state].phrase;
}

static void drawBubbleShape(int by, int bh, int r, uint16_t borderCol, const char* text, uint16_t textCol) {
  g_canvas.setFont(&fonts::Font2);
  g_canvas.setTextSize(1);
  int tw = g_canvas.textWidth(text);
  int bw = constrain(tw + 20, 62, SCR_W - 14);
  int cx = constrain(SCR_W / 2 + g_tiltOffsetPx / 2, bw / 2 + 4, SCR_W - bw / 2 - 4);
  int bx = cx - bw / 2;

  g_canvas.fillRoundRect(bx, by, bw, bh, r, colSurfaceAlt());
  g_canvas.drawRoundRect(bx, by, bw, bh, r, borderCol);

  int tipX = constrain(SCR_W / 2 + g_tiltOffsetPx, bx + 10, bx + bw - 10);
  int tailTop = by + bh - 1;
  g_canvas.fillTriangle(tipX - 4, tailTop, tipX + 4, tailTop, tipX, tailTop + 5, colSurfaceAlt());
  g_canvas.drawLine(tipX - 4, tailTop, tipX, tailTop + 5, borderCol);
  g_canvas.drawLine(tipX + 4, tailTop, tipX, tailTop + 5, borderCol);

  g_canvas.setTextColor(textCol);
  g_canvas.setTextDatum(middle_center);
  g_canvas.drawString(text, cx, by + bh / 2);
}

// 绘制 Nx COMBO 悬浮连击角标 & 番茄钟角标
static void drawComboBadge() {
  if (g_pomoActive) {
    uint32_t elapsed = millis() - g_pomoStartMs;
    uint32_t remSec = (elapsed < POMO_TOTAL_MS) ? ((POMO_TOTAL_MS - elapsed) / 1000UL) : 0;
    char pBuf[12];
    snprintf(pBuf, sizeof(pBuf), "P %02d:%02d", (int)(remSec / 60), (int)(remSec % 60));
    g_canvas.fillRoundRect(4, 89, 46, 12, 3, colCoral());
    g_canvas.setFont(nullptr);
    g_canvas.setTextSize(1);
    g_canvas.setTextColor(TFT_BLACK, colCoral());
    g_canvas.setTextDatum(middle_center);
    g_canvas.drawString(pBuf, 27, 95);
  }
  if (g_combo < 2 || (millis() - g_lastComboMs) > COMBO_SHOW_MS) return;
  char cBuf[14];
  snprintf(cBuf, sizeof(cBuf), "%dx COMBO!", g_combo);
  uint16_t bgCol = (g_combo >= 8) ? colPi() : (g_combo >= 4) ? colAmber() : colCodex();
  int w = 58, h = 12;
  int x = SCR_W - w - 5;
  int y = 89;
  g_canvas.fillRoundRect(x, y, w, h, 3, bgCol);
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  g_canvas.setTextColor(TFT_BLACK, bgCol);
  g_canvas.setTextDatum(middle_center);
  g_canvas.drawString(cBuf, x + w / 2, y + h / 2 + 1);
}

static void drawBubbleOverlay() {
  const StateStyle& st = kStyles[(int)g_state];

  int by = 106;
  int bh = 21;
  int r  = 8;

  if (g_shakeActive && g_shakePhrase) {
    drawBubbleShape(by, bh, r, colCoral(), g_shakePhrase, colCoral());
    return;
  }

  if (g_feedActive && g_feedPhrase) {
    uint16_t pinkCol = g_canvas.color565(255, 145, 205);
    drawBubbleShape(by, bh, r, pinkCol, g_feedPhrase, pinkCol);
    return;
  }

  if (g_pokeActive && g_pokePhrase) {
    drawBubbleShape(by, bh, r, colAmber(), g_pokePhrase, colAmber());
    return;
  }

  // CHOOSING: 全屏呼吸亮青外框提醒 + 闪烁气泡
  if (g_state == PetState::CHOOSING) {
    bool on = ((millis() / 380) % 2) == 0;
    uint16_t border = on ? colCodex() : colBorder();
    if (on) {
      g_canvas.drawRect(0, 0, SCR_W, SCR_H, colCodex());
      g_canvas.drawRect(1, 1, SCR_W - 2, SCR_H - 2, colCodex());
    }
    const char* txt = st.phrase;
    if (g_waitCount > 0 && ((millis() / 1200) % 2 == 1)) {
      txt = g_waitNames[g_waitCycle % g_waitCount].c_str();
    }
    drawBubbleShape(by, bh, r, border, txt, colCodex());
    return;
  }

  uint16_t txtCol = g_canvas.color565(st.tr, st.tg, st.tb);
  uint16_t borderCol = (g_state == PetState::IDLE) ? colBorder() : txtCol;
  if (g_state == PetState::IDLE && g_mood < 30) {
    txtCol = colMuted();
  }
  const char* phrase = resolveDynamicPhrase();
  drawBubbleShape(by, bh, r, borderCol, phrase, txtCol);
  drawComboBadge();
}

static void drawOverlay() {
  drawStageAtmosphereAndParticles();
  drawStatusBarOverlay();
  drawSessionCardsOverlay();
  drawBubbleOverlay();
  drawBottomHudDock();
}

static void presentCanvas() {
  drawOverlay();
  g_canvas.pushSprite(0, 0);
}

// ---------------------------------------------------------------------------
// Page 2 (DETAIL): 多端会话卡片舱 + 最近完成记录 + RPG 等级与心情面板
// ---------------------------------------------------------------------------
static void drawDetailContent() {
  uint32_t now = millis();
  int y = 20;

  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  g_canvas.setTextColor(colMuted(), TFT_BLACK);
  g_canvas.setTextDatum(top_left);
  char hdr[24];
  snprintf(hdr, sizeof(hdr), "SESSIONS (%d)", g_sessionCount);
  g_canvas.drawString(hdr, 5, y + 2);

  g_canvas.setTextColor(colClaude(), TFT_BLACK);
  g_canvas.drawString("C", SCR_W - 28, y + 2);
  g_canvas.setTextColor(colCodex(), TFT_BLACK);
  g_canvas.drawString("X", SCR_W - 18, y + 2);
  g_canvas.setTextColor(colPi(), TFT_BLACK);
  g_canvas.drawString("P", SCR_W - 8, y + 2);
  y += 14;

  int maxShow = 5;
  if (g_sessionCount > 0) {
    int pages = (g_sessionCount + maxShow - 1) / maxShow;
    if (g_detailScroll >= pages) g_detailScroll = 0;
    int startIdx = g_detailScroll * maxShow;
    int endIdx = min(g_sessionCount, startIdx + maxShow);

    for (int i = startIdx; i < endIdx; i++) {
      SessionInfo& s = g_sessions[i];
      int cardH = 26;
      uint16_t stCol = stateColor(s.state);
      uint16_t agCol = agentColor(s.agent);
      uint16_t barCol = s.hasColor ? s.color : agCol;

      g_canvas.fillRoundRect(3, y, SCR_W - 6, cardH, 4, colSurface());
      g_canvas.drawRoundRect(3, y, SCR_W - 6, cardH, 4, (s.state == 'W') ? colCodex() : colBorder());
      g_canvas.fillRoundRect(4, y + 3, 3, cardH - 6, 1, barCol);

      g_canvas.fillRoundRect(9, y + 4, 12, 11, 2, agentDimColor(s.agent));
      g_canvas.setFont(nullptr);
      g_canvas.setTextSize(1);
      g_canvas.setTextColor(agCol);
      g_canvas.setTextDatum(middle_center);
      char agBuf[2] = { s.agent ? s.agent : 'C', '\0' };
      g_canvas.drawString(agBuf, 15, y + 9);

      g_canvas.setFont(&fonts::efontCN_12);
      g_canvas.setTextSize(1);
      g_canvas.setTextColor(TFT_WHITE);
      g_canvas.setTextDatum(top_left);
      g_canvas.drawString(s.name.substring(0, 9), 24, y + 2);

      g_canvas.setFont(nullptr);
      g_canvas.setTextSize(1);
      g_canvas.setTextDatum(top_right);
      g_canvas.setTextColor(stCol);
      if (s.state == 'R' && s.tool.length() > 0) {
        String tl = ">" + s.tool.substring(0, 5);
        g_canvas.drawString(tl, SCR_W - 7, y + 4);
      } else {
        g_canvas.drawString(stateLabel(s.state), SCR_W - 7, y + 4);
      }

      uint32_t elapsed = s.durSec + (now - s.recvMs) / 1000;
      g_canvas.setTextColor(colMuted());
      g_canvas.setTextDatum(bottom_right);
      g_canvas.drawString(formatDuration(elapsed), SCR_W - 7, y + cardH - 2);

      y += cardH + 3;
    }
  } else {
    g_canvas.fillRoundRect(3, y, SCR_W - 6, 26, 4, colSurfaceAlt());
    g_canvas.drawRoundRect(3, y, SCR_W - 6, 26, 4, colBorder());
    g_canvas.setFont(nullptr);
    g_canvas.setTextColor(colMuted());
    g_canvas.setTextDatum(middle_center);
    g_canvas.drawString("No active sessions", SCR_W / 2, y + 13);
    y += 30;
  }

  if (y <= 135 && g_histCount > 0) {
    y += 2;
    g_canvas.setFont(nullptr);
    g_canvas.setTextSize(1);
    g_canvas.setTextColor(colMuted());
    g_canvas.setTextDatum(top_left);
    g_canvas.drawString("RECENT DONE", 5, y);
    y += 11;

    for (int i = 0; i < g_histCount && y + 16 <= 178; i++) {
      const HistEntry& h = g_history[i];
      uint16_t agCol = agentColor(h.agent);
      g_canvas.fillRoundRect(3, y, SCR_W - 6, 15, 3, colSurfaceAlt());
      g_canvas.setFont(nullptr);
      g_canvas.setTextColor(agCol);
      g_canvas.setTextDatum(middle_left);
      char agTag[4];
      snprintf(agTag, sizeof(agTag), "%c", h.agent ? h.agent : 'C');
      g_canvas.drawString(agTag, 7, y + 8);

      g_canvas.setFont(&fonts::efontCN_12);
      g_canvas.setTextColor(TFT_WHITE);
      g_canvas.drawString(h.name.substring(0, 8), 17, y + 7);

      g_canvas.setFont(nullptr);
      g_canvas.setTextColor(colEmerald());
      g_canvas.setTextDatum(middle_right);
      g_canvas.drawString(h.info, SCR_W - 7, y + 8);
      y += 17;
    }
  }

  // === 底部 RPG 养成 + 今日统计卡片 (y: 182..231) ===
  int boxY = 182, boxH = 49;
  g_canvas.fillRoundRect(3, boxY, SCR_W - 6, boxH, 5, colSurface());
  g_canvas.drawRoundRect(3, boxY, SCR_W - 6, boxH, 5, colBorder());

  // 左半: RPG 等级称号 + 今日完成/连胜
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  char lvTitle[20];
  snprintf(lvTitle, sizeof(lvTitle), "Lv.%d %s", g_level, levelTitle(g_level));
  g_canvas.setTextColor(colAmber());
  g_canvas.setTextDatum(top_left);
  g_canvas.drawString(lvTitle, 8, boxY + 5);

  g_canvas.setFont(&fonts::Font2);
  g_canvas.setTextColor(colEmerald());
  char dStr[12];
  snprintf(dStr, sizeof(dStr), "%d done", g_todayDone);
  g_canvas.drawString(dStr, 8, boxY + 15);

  g_canvas.setFont(nullptr);
  g_canvas.setTextColor(colClaude());
  char sStr[16];
  snprintf(sStr, sizeof(sStr), "Streak %dd", g_streakDays);
  g_canvas.drawString(sStr, 8, boxY + 35);

  // 右半: EXP 进度条 + MOOD 进度条
  int mx = 74, mw = SCR_W - 82;
  g_canvas.setTextColor(colPi());
  g_canvas.setTextDatum(top_left);
  g_canvas.drawString("EXP", mx, boxY + 5);
  char expVal[8];
  snprintf(expVal, sizeof(expVal), "%d%%", g_exp);
  g_canvas.setTextDatum(top_right);
  g_canvas.drawString(expVal, mx + mw, boxY + 5);

  int eBarY = boxY + 15, barH = 6;
  g_canvas.fillRoundRect(mx, eBarY, mw, barH, 2, colSurfaceAlt());
  int eFill = (mw - 2) * g_exp / 100;
  if (eFill > 1) g_canvas.fillRoundRect(mx + 1, eBarY + 1, eFill, barH - 2, 1, colPi());

  uint16_t mCol = (g_mood >= 70) ? colEmerald() : (g_mood >= 35) ? colAmber() : colCoral();
  g_canvas.setTextColor(mCol);
  g_canvas.setTextDatum(top_left);
  g_canvas.drawString("MOOD", mx, boxY + 25);
  char mVal[8];
  snprintf(mVal, sizeof(mVal), "%d%%", g_mood);
  g_canvas.setTextDatum(top_right);
  g_canvas.drawString(mVal, mx + mw, boxY + 25);

  int mBarY = boxY + 35;
  g_canvas.fillRoundRect(mx, mBarY, mw, barH, 2, colSurfaceAlt());
  int mFill = (mw - 2) * g_mood / 100;
  if (mFill > 1) g_canvas.fillRoundRect(mx + 1, mBarY + 1, mFill, barH - 2, 1, mCol);

  drawPageDots(1);
}

// ---------------------------------------------------------------------------
// Page 3 (TOKEN): 算力能量站 (总览大卡 + 三端占比条 + IN/OUT/CACHE 进度条)
// ---------------------------------------------------------------------------
static void drawTokBarRow(int y, const char* label, int64_t val, int64_t maxVal, uint16_t col) {
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  g_canvas.setTextColor(col);
  g_canvas.setTextDatum(top_left);
  g_canvas.drawString(label, 7, y);

  char vBuf[12];
  fmtTok(val, vBuf, sizeof(vBuf));
  g_canvas.setTextColor(TFT_WHITE);
  g_canvas.setTextDatum(top_right);
  g_canvas.drawString(vBuf, SCR_W - 7, y);

  int bx = 7, by = y + 11, bw = SCR_W - 14, bh = 6;
  g_canvas.fillRoundRect(bx, by, bw, bh, 2, colSurfaceAlt());
  if (val > 0 && maxVal > 0) {
    int fw = (int)((int64_t)bw * val / maxVal);
    fw = constrain(fw, 3, bw);
    g_canvas.fillRoundRect(bx, by, fw, bh, 2, col);
  }
}

static void drawTokView() {
  int y = 20;

  int64_t total = (g_tokIn >= 0 && g_tokOut >= 0) ? (g_tokIn + g_tokOut) : -1;

  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  g_canvas.setTextColor(colMuted());
  g_canvas.setTextDatum(top_left);
  g_canvas.drawString("TOKENS TODAY", 5, y + 2);

  const char* rank = "IDLE";
  uint16_t rankCol = colMuted();
  if (total >= 10000000LL)      { rank = "OVERDRIVE"; rankCol = colPi(); }
  else if (total >= 1000000LL)  { rank = "ON FIRE";   rankCol = colAmber(); }
  else if (total >= 100000LL)   { rank = "CRUISING";  rankCol = colEmerald(); }
  else if (total > 0)           { rank = "WARMUP";    rankCol = colCodex(); }

  g_canvas.setTextColor(rankCol);
  g_canvas.setTextDatum(top_right);
  g_canvas.drawString(rank, SCR_W - 5, y + 2);
  y += 14;

  // 1. Hero 总量卡片 (y: 34..82)
  g_canvas.fillRoundRect(3, y, SCR_W - 6, 48, 5, colSurface());
  g_canvas.drawRoundRect(3, y, SCR_W - 6, 48, 5, colBorder());

  char totalBuf[14];
  fmtTok(total, totalBuf, sizeof(totalBuf));
  g_canvas.setFont(&fonts::Font4);
  g_canvas.setTextSize(1);
  g_canvas.setTextColor(colAmber());
  g_canvas.setTextDatum(middle_center);
  g_canvas.drawString(totalBuf, SCR_W / 2, y + 18);

  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  int64_t allIn = (g_tokIn > 0 ? g_tokIn : 0) + (g_tokCache > 0 ? g_tokCache : 0);
  int hitPct = (allIn > 0 && g_tokCache > 0) ? (int)((g_tokCache * 100LL) / allIn) : 0;
  char subBuf[28];
  snprintf(subBuf, sizeof(subBuf), "IN+OUT  |  Cache %d%%", hitPct);
  g_canvas.setTextColor(colMuted());
  g_canvas.setTextDatum(middle_center);
  g_canvas.drawString(subBuf, SCR_W / 2, y + 39);
  y += 52;

  // 2. 三端算力占比卡片 (Claude / Codex / Pi) (y: 86..136)
  g_canvas.fillRoundRect(3, y, SCR_W - 6, 50, 5, colSurface());
  g_canvas.drawRoundRect(3, y, SCR_W - 6, 50, 5, colBorder());

  g_canvas.setFont(nullptr);
  g_canvas.setTextColor(colMuted());
  g_canvas.setTextDatum(top_left);
  g_canvas.drawString("AGENT SHARE", 8, y + 4);

  int barX = 8, barY = y + 15, barW = SCR_W - 16, barH = 8;
  g_canvas.fillRoundRect(barX, barY, barW, barH, 3, colSurfaceAlt());
  int64_t agSum = g_tokClaude + g_tokCodex + g_tokPi;
  if (agSum > 0) {
    int wC = (int)((int64_t)barW * g_tokClaude / agSum);
    int wX = (int)((int64_t)barW * g_tokCodex  / agSum);
    int wP = barW - wC - wX;
    int curX = barX;
    if (wC > 0) { g_canvas.fillRect(curX, barY, wC, barH, colClaude()); curX += wC; }
    if (wX > 0) { g_canvas.fillRect(curX, barY, wX, barH, colCodex());  curX += wX; }
    if (wP > 0 && g_tokPi > 0) { g_canvas.fillRect(curX, barY, wP, barH, colPi()); }
  }

  char cBuf[10], xBuf[10], pBuf[10];
  fmtTok(g_tokClaude, cBuf, sizeof(cBuf));
  fmtTok(g_tokCodex,  xBuf, sizeof(xBuf));
  fmtTok(g_tokPi,     pBuf, sizeof(pBuf));

  int rowY = y + 28;
  g_canvas.setTextDatum(top_left);
  g_canvas.setTextColor(colClaude());
  g_canvas.drawString(String("C:") + cBuf, 8, rowY);

  g_canvas.setTextDatum(top_center);
  g_canvas.setTextColor(colCodex());
  g_canvas.drawString(String("X:") + xBuf, SCR_W / 2, rowY);

  g_canvas.setTextDatum(top_right);
  g_canvas.setTextColor(colPi());
  g_canvas.drawString(String("P:") + pBuf, SCR_W - 8, rowY);
  y += 55;

  // 3. IN / OUT / CACHE 进度条明细 (y: 141..231)
  g_canvas.fillRoundRect(3, y, SCR_W - 6, 90, 5, colSurface());
  g_canvas.drawRoundRect(3, y, SCR_W - 6, 90, 5, colBorder());

  int64_t maxBar = max((int64_t)1, max(g_tokIn, max(g_tokOut, g_tokCache)));
  drawTokBarRow(y + 7,  "INPUT",  g_tokIn,    maxBar, colAmber());
  drawTokBarRow(y + 35, "OUTPUT", g_tokOut,   maxBar, colEmerald());
  drawTokBarRow(y + 63, "CACHE",  g_tokCache, maxBar, colCodex());

  drawPageDots(2);
}

// ---------------------------------------------------------------------------
// GIF 生命周期
// ---------------------------------------------------------------------------
static void closeGif() {
  if (g_gifOpen) {
    g_gif.close();
    g_gifOpen = false;
  }
}

static void openGifForState() {
  closeGif();
  size_t sz = 0;
  const uint8_t* data = gifDataForState(g_state, &sz);
  if (!data || sz == 0) {
    g_canvas.fillScreen(BG_COLOR);
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
    addExp(25);
    recordPulse(10);
    burstParticles(PT_STAR, colAmber(), 5);
  } else if (s == PetState::WORKING) {
    recordPulse(6);
  } else if (s == PetState::CHOOSING) {
    recordPulse(8);
  }
  openGifForState();
}

// ---------------------------------------------------------------------------
// 命令解析
// ---------------------------------------------------------------------------
static void handleStat(const String& cmd) {
  int t = g_total, w = g_work, d = g_done;
  int c = g_agentWorkC, x = g_agentWorkX, p = g_agentWorkP;
  int n = cmd.length();
  for (int i = 0; i < n; ++i) {
    char ch = cmd[i];
    if ((ch == 't' || ch == 'w' || ch == 'd' || ch == 'c' || ch == 'x' || ch == 'p')
        && i + 1 < n && cmd[i + 1] == '=') {
      int v = 0, j = i + 2; bool got = false;
      while (j < n && cmd[j] >= '0' && cmd[j] <= '9') {
        v = v * 10 + (cmd[j] - '0'); j++; got = true;
      }
      if (got) {
        if (ch == 't') t = v;
        else if (ch == 'w') w = v;
        else if (ch == 'd') d = v;
        else if (ch == 'c') c = v;
        else if (ch == 'x') x = v;
        else if (ch == 'p') p = v;
      }
    }
  }
  g_total = t; g_work = w; g_done = d;
  g_agentWorkC = c; g_agentWorkX = x; g_agentWorkP = p;
}

static void handleWork(const String& raw) {
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
  int colon = cmd.indexOf(':');
  if (colon < 0) return;
  int i = colon - 1, h = 0, hd = 1; bool hg = false;
  while (i >= 0 && cmd[i] >= '0' && cmd[i] <= '9') {
    h += (cmd[i] - '0') * hd; hd *= 10; i--; hg = true;
  }
  int j = colon + 1, m = 0; bool mg = false;
  while (j < (int)cmd.length() && cmd[j] >= '0' && cmd[j] >= '9') {
    m = m * 10 + (cmd[j] - '0'); j++; mg = true;
  }
  if (!hg || !mg || h > 23 || m > 59) return;
  g_baseMinOfDay = h * 60 + m;
  g_baseMillis   = millis();
  g_timeValid    = true;
}

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
    int c1 = tok.indexOf(':');
    int c2 = (c1 >= 0) ? tok.indexOf(':', c1 + 1) : -1;
    int c3 = (c2 >= 0) ? tok.indexOf(':', c2 + 1) : -1;
    int c4 = (c3 >= 0) ? tok.indexOf(':', c3 + 1) : -1;
    int c5 = (c4 >= 0) ? tok.indexOf(':', c4 + 1) : -1;
    if (c1 > 0 && c2 > c1) {
      SessionInfo& s = g_sessions[g_sessionCount];
      s.name   = tok.substring(0, c1);
      s.state  = tok.charAt(c1 + 1);
      String durStr = (c3 > c2) ? tok.substring(c2 + 1, c3) : tok.substring(c2 + 1);
      s.durSec = durStr.toInt();
      s.recvMs = now;
      s.hasColor = false;
      s.color    = 0;
      s.agent    = 'C';
      s.tool     = "";
      if (c3 > c2) {
        String colStr = (c4 > c3) ? tok.substring(c3 + 1, c4) : tok.substring(c3 + 1);
        uint16_t col = parseHexColor(colStr);
        if (col) { s.color = col; s.hasColor = true; }
      }
      if (c4 > c3) {
        String agStr = (c5 > c4) ? tok.substring(c4 + 1, c5) : tok.substring(c4 + 1);
        agStr.trim();
        if (agStr.length() > 0) {
          char a = agStr.charAt(0);
          if (a == 'X' || a == 'x') s.agent = 'X';
          else if (a == 'P' || a == 'p') s.agent = 'P';
          else s.agent = 'C';
        }
      }
      if (c5 > c4) {
        s.tool = tok.substring(c5 + 1);
        s.tool.trim();
      }
      g_sessionCount++;
    }
    if (bar < 0) break;
    start = bar + 1;
  }
  g_detailDirty = true;
  markDirty();
}

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
      String left = tok.substring(0, c1);
      char ag = 'C';
      int tilde = left.indexOf('~');
      if (tilde == 1) {
        ag = left.charAt(0);
        left = left.substring(2);
      }
      g_history[g_histCount].agent = ag;
      g_history[g_histCount].name  = left;
      g_history[g_histCount].info  = tok.substring(c1 + 1);
      g_histCount++;
    }
    if (bar < 0) break;
    start = bar + 1;
  }
  g_detailDirty = true;
}

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
  int si = low.indexOf("s=");
  if (si >= 0) {
    int v = 0, j = si + 2;
    while (j < (int)low.length() && low[j] >= '0' && low[j] <= '9') {
      v = v * 10 + (low[j] - '0'); j++;
    }
    if (v > 0) g_streakDays = v;
  }
  g_detailDirty = true;
  markDirty();
}

static void handleCombo(const String& cmd) {
  int sp = cmd.indexOf(' ');
  if (sp < 0) return;
  int v = cmd.substring(sp + 1).toInt();
  g_combo = max(0, v);
  g_lastComboMs = millis();
  if (g_combo > 0) {
    recordPulse(min(10, 4 + g_combo));
    addExp(4);
    if (g_combo >= 5 && (g_combo % 5 == 0)) {
      burstParticles(PT_SPARK, colPi(), 5);
    }
  }
}

static void handleAchv(const String& raw) {
  int sp = raw.indexOf(' ');
  if (sp < 0) return;
  String name = raw.substring(sp + 1);
  name.trim();
  if (name.length() == 0) return;
  snprintf(g_customBubbleBuf, sizeof(g_customBubbleBuf), "* %s *", name.substring(0, 12).c_str());
  g_pokeActive  = true;
  g_pokeStartMs = millis();
  g_pokePhrase  = g_customBubbleBuf;
  burstParticles(PT_STAR, colAmber(), 6);
}

static void handleFestival(const String& raw) {
  int sp = raw.indexOf(' ');
  if (sp < 0) return;
  String name = raw.substring(sp + 1);
  name.trim();
  if (name.length() == 0) return;
  g_festivalActive  = true;
  g_festivalStartMs = millis();
  g_pokeActive  = true;
  g_pokeStartMs = millis();
  if (name == "christmas")       g_pokePhrase = "Merry Xmas!";
  else if (name == "halloween")  g_pokePhrase = "Happy Halloween";
  else if (name == "valentine")  g_pokePhrase = "Happy Valentine";
  else if (name == "new-year")   g_pokePhrase = "Happy New Year!";
  else                           g_pokePhrase = "Celebrate! ~";
}

static int64_t parseTokKey(const String& low, const char* key) {
  int idx = low.indexOf(key);
  if (idx < 0) return -1;
  int j = idx + strlen(key);
  int64_t v = 0;
  bool got = false;
  while (j < (int)low.length() && low[j] >= '0' && low[j] <= '9') {
    v = v * 10 + (low[j] - '0');
    j++;
    got = true;
  }
  return got ? v : -1;
}

static void handleTok(const String& low) {
  int64_t vIn    = parseTokKey(low, "in=");
  int64_t vOut   = parseTokKey(low, "out=");
  int64_t vCache = parseTokKey(low, "cache=");
  int64_t vC     = parseTokKey(low, " c=");
  int64_t vX     = parseTokKey(low, " x=");
  int64_t vP     = parseTokKey(low, " p=");

  if (vIn >= 0)    g_tokIn = vIn;
  if (vOut >= 0)   g_tokOut = vOut;
  if (vCache >= 0) g_tokCache = vCache;
  if (vC >= 0)     g_tokClaude = vC;
  if (vX >= 0)     g_tokCodex = vX;
  if (vP >= 0)     g_tokPi = vP;
  g_detailDirty = true;
}

static void handleCommand(String cmd) {
  cmd.trim();
  if (cmd.isEmpty()) return;
  Serial.printf("[recv] '%s'\n", cmd.c_str());

  String low = cmd; low.toLowerCase();
  if (low.startsWith("#stat"))     { handleStat(low); return; }
  if (low.startsWith("#work"))     { handleWork(cmd); return; }
  if (low.startsWith("#wait"))     { handleWait(cmd); return; }
  if (low.startsWith("#time"))     { handleTime(low); return; }
  if (low.startsWith("#sess"))     { handleSess(cmd); return; }
  if (low.startsWith("#hist"))     { handleHist(cmd); return; }
  if (low.startsWith("#today"))    { handleToday(low); return; }
  if (low.startsWith("#combo"))    { handleCombo(low); return; }
  if (low.startsWith("#achv"))     { handleAchv(cmd); return; }
  if (low.startsWith("#festival")) { handleFestival(cmd); return; }
  if (low.startsWith("#tok"))      { handleTok(low); return; }

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
    } else if (g_lineLen < (int)sizeof(g_lineBuf) - 2) {
      g_lineBuf[g_lineLen++] = c;
    } else {
      g_lineLen = 0;
    }
  }
}

static void pollBattery() {
  uint32_t now = millis();
  if (now - g_lastBattMs < BATT_POLL_MS && g_battLevel >= 0) return;
  g_lastBattMs = now;
  g_battLevel = M5.Power.getBatteryLevel();
  g_charging  = M5.Power.isCharging();
}

void setup() {
  Serial.begin(115200);
  auto cfg = M5.config();
  M5.begin(cfg);

  M5.Display.setRotation(0);          // 竖屏 135x240
  M5.Display.setBrightness(135);
  M5.Display.fillScreen(BG_COLOR);

  g_canvas.setPsram(true);
  g_canvas.setColorDepth(16);
  g_canvas.createSprite(SCR_W, SCR_H);
  g_canvas.fillScreen(BG_COLOR);

  g_gif.begin(GIF_PALETTE_RGB565_BE);

  g_frameBuf = (uint8_t*)ps_malloc(FRAME_BUF_SIZE);
  if (!g_frameBuf) Serial.println("[gif] FATAL: frame buffer alloc failed");

  for (int i = 0; i < SPARK_LEN; i++) g_spark[i] = 1;

  loadState();

  Serial.println();
  Serial.println("[ClawdPet] boot ok (Multi-Agent Cyber HUD v3)");
  Serial.printf("[ClawdPet] display=%dx%d psram=%u imu=%d\n",
                M5.Display.width(), M5.Display.height(),
                (unsigned)ESP.getPsramSize(), M5.Imu.getType());

  pollBattery();
  g_lastMoodDecay = millis();
  applyState(g_state);
}

void loop() {
  M5.update();

  pollSerial();
  pollBattery();
  pollSave();
  stepSparkline();

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
    if (M5.BtnA.wasPressed()) {
      int maxShow = 5;
      int pages = (g_sessionCount + maxShow - 1) / maxShow;
      if (pages > 1) {
        g_detailScroll = (g_detailScroll + 1) % pages;
        g_detailDirty = true;
      }
    }
    break;
  }

  case Page::TOKEN: {
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
    // --- IMU 读取: 重力倾斜物理 + 摇一摇 + 翻转桌面番茄钟 ---
    if (M5.Imu.isEnabled()) {
      M5.Imu.update();
      float ax, ay, az;
      M5.Imu.getAccel(&ax, &ay, &az);

      // 1. 重力倾斜平滑滤波 (左右倾斜机身 -> 宠物与气泡跟着滑动)
      float targetTilt = constrain(-ax, -0.85f, 0.85f);
      g_tiltSmooth = g_tiltSmooth * 0.82f + targetTilt * 0.18f;
      if (fabsf(g_tiltSmooth) < 0.12f) {
        g_tiltOffsetPx = 0;
      } else {
        g_tiltOffsetPx = constrain((int)(g_tiltSmooth * 18.0f), -15, 15);
      }

      // 2. 翻转桌面检测 (屏幕朝下扣放 0.8s 再翻回来 -> 切换 25 分钟专注番茄钟)
      if (az < -0.72f) {
        if (!g_faceDown) {
          g_faceDown = true;
          g_faceDownStart = millis();
        }
      } else if (az > 0.35f && g_faceDown) {
        uint32_t downDur = millis() - g_faceDownStart;
        g_faceDown = false;
        if (downDur >= 700 && downDur <= 15000) {
          g_pomoActive = !g_pomoActive;
          g_pomoStartMs = millis();
          snprintf(g_customBubbleBuf, sizeof(g_customBubbleBuf),
                   g_pomoActive ? "POMO 25m ON!" : "POMO OFF~");
          g_pokeActive  = true;
          g_pokeStartMs = millis();
          g_pokePhrase  = g_customBubbleBuf;
          burstParticles(PT_STAR, colCoral(), 5);
        }
      }

      // 3. 摇一摇检测
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

    // 焦点会话轮播: 超过 2 条活跃会话时每 2.4s 翻页
    uint32_t tnow = millis();
    int activeIdx[MAX_SESSIONS];
    int activeCount = collectActiveSessionIndices(activeIdx, MAX_SESSIONS);
    if (activeCount > 2 && (tnow - g_lastCycleMs) >= INFO_CYCLE_MS) {
      g_lastCycleMs = tnow;
      int pages = (activeCount + 1) / 2;
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
      clearDynamicRegionsBeforeGif();
      int rc = g_gif.playFrame(false, &delayMs);
      presentCanvas();
      g_frameDelayMs = (delayMs > 0) ? delayMs : 50;
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

    // BtnA 长按=喂食 (+20 心情, +15 EXP, 飘红心粒子)
    if (M5.BtnA.wasHold() && !g_shakeActive && !g_feedActive) {
      g_feedActive  = true;
      g_feedStartMs = millis();
      g_feedPhrase  = kFeedPhrases[esp_random() % FEED_PHRASE_COUNT];
      g_mood = min(100, g_mood + 20);
      addExp(15);
      recordPulse(8);
      burstParticles(PT_HEART, colCoral(), 6);
      markDirty();
      closeGif();
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

    // BtnA 短按=戳一戳 (解锁全部 10 种趣味互动动画 + 经验值)
    if (M5.BtnA.wasClicked() && !g_shakeActive && !g_feedActive) {
      g_mood = min(100, g_mood + 5);
      addExp(3);
      recordPulse(5);
      burstParticles(PT_HEART, colAmber(), 4);
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

    if (g_feedActive && (millis() - g_feedStartMs) >= FEED_DURATION_MS) {
      g_feedActive = false;
      g_feedPhrase = nullptr;
      openGifForState();
    }

    uint32_t pokeDur = g_festivalActive ? FESTIVAL_DURATION_MS : POKE_DURATION_MS;
    if (g_pokeActive && (millis() - g_pokeStartMs) >= pokeDur) {
      g_pokeActive = false;
      g_pokePhrase = nullptr;
      g_festivalActive = false;
      openGifForState();
    }
    break;
  }
  }

  delay(2);
}
