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

// 时间 (由 #time 校准; 启动时先用 NVS lastMin 或编译时间 __TIME__ 兜底, 永不显示 --:--)
static int compileMinOfDay() {
  int h = (__TIME__[0] - '0') * 10 + (__TIME__[1] - '0');
  int m = (__TIME__[3] - '0') * 10 + (__TIME__[4] - '0');
  if (h < 0 || h > 23 || m < 0 || m > 59) return 12 * 60;
  return h * 60 + m;
}
static bool     g_timeValid       = true;
static int      g_baseMinOfDay    = 0;     // 上次校时对应的当日分钟数 0..1439
static uint32_t g_baseMillis      = 0;
static int      g_lastNvsSavedMin = -1;

// 电量 (固件本地轮询)
static int      g_battLevel  = -1;
static bool     g_charging   = false;
static uint32_t g_lastBattMs = 0;
static const uint32_t BATT_POLL_MS = 5000;

// --- 页面切换 (BtnB 四页循环: MAIN -> DETAIL -> BOSS -> TOKEN -> MAIN) ---
enum class Page { MAIN = 0, DETAIL = 1, BOSS = 2, TOKEN = 3 };
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

// --- RPG 养成系统 (Level & EXP) & 赛博 Bug Boss 讨伐战 ---
static int      g_level = 1;
static int      g_exp   = 0;   // 0..99, 满 100 升一级

static const int BOSS_COUNT = 8;
static const char* kBossNames[BOSS_COUNT] = {
  "NullPtr", "RaceCond", "MemLeak", "Heisenbug",
  "Segfault", "MergeDmn", "Deadlock", "LegacyCore"
};
static int      g_bossIdx          = 0;
static int      g_bossMaxHp        = 300;
static int      g_bossHp           = 300;
static int      g_bossKills        = 0;
static uint32_t g_bossHitMs        = 0;
static uint32_t g_feverUntilMs     = 0;   // 双倍算力暴击 BUFF 截止时刻
static char     g_battleLog[26]    = "Tap [BtnA] to Slash!";
static uint8_t  g_lastSkillHighlight = 0; // 1=Slash, 2=Gacha, 3=Heal

// 舞台伤害飘字 (-25 / CRIT -68!)
static bool     g_dmgActive        = false;
static float    g_dmgX             = 67.0f;
static float    g_dmgY             = 96.0f;
static uint8_t  g_dmgLife          = 0;
static bool     g_dmgCrit          = false;
static char     g_dmgText[18]      = {0};

// 舞台夜空流星许愿 (Shooting Star) & 整点健康提醒 & 双击扭蛋
static bool     g_shootActive      = false;
static float    g_shootX           = 0.0f;
static float    g_shootY           = 0.0f;
static uint8_t  g_shootLife        = 0;
static uint32_t g_lastShootMs      = 0;
static int      g_lastChimeHour    = -1;
static uint32_t g_lastBtnAClickMs  = 0;

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
static bool     g_axBaselineInit = false;
static float    g_axBaseline     = 0.0f;   // 自适应桌面静置基线, 防止斜放/侧放误触 sliding
static float    g_tiltSmooth     = 0.0f;
static int      g_tiltOffsetPx   = 0;      // -14 .. +14 像素动态水平偏移
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
  if (millis() < g_feverUntilMs) amount *= 2;
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

static void hitBoss(int baseDmg, bool forceCrit = false) {
  if (baseDmg <= 0) return;
  bool fever = (millis() < g_feverUntilMs);
  bool crit  = forceCrit || fever || (g_combo >= 5) || ((esp_random() % 100) < 20);
  int dmg    = baseDmg + (int)(esp_random() % 9);
  if (crit) dmg = dmg * 2 + 8;
  g_bossHp   -= dmg;
  g_bossHitMs = millis();

  // 生成浮动伤害飘字 (位于气泡上方 y=86..64, 不挡宠物面部)
  g_dmgActive = true;
  g_dmgCrit   = crit;
  g_dmgX      = constrain((SCR_W / 2) + g_tiltOffsetPx + ((int)(esp_random() % 27) - 13), 24, SCR_W - 24);
  g_dmgY      = 84.0f;
  g_dmgLife   = 16;
  if (crit) snprintf(g_dmgText, sizeof(g_dmgText), "CRIT -%d!", dmg);
  else      snprintf(g_dmgText, sizeof(g_dmgText), "-%d", dmg);

  if (crit) snprintf(g_battleLog, sizeof(g_battleLog), "CRIT! -%d HP to %s", dmg, kBossNames[g_bossIdx]);
  else      snprintf(g_battleLog, sizeof(g_battleLog), "HIT! -%d HP to %s", dmg, kBossNames[g_bossIdx]);

  if (g_bossHp <= 0) {
    g_bossKills++;
    g_bossIdx   = (g_bossIdx + 1) % BOSS_COUNT;
    g_bossMaxHp = 300 + g_bossIdx * 120 + min(1200, g_bossKills * 60);
    g_bossHp    = g_bossMaxHp;
    addExp(45);
    burstParticles(PT_STAR, colAmber(), 8);
    snprintf(g_customBubbleBuf, sizeof(g_customBubbleBuf), "BOSS SLAIN! +45XP");
    snprintf(g_battleLog, sizeof(g_battleLog), "SLAIN! +45XP -> %s", kBossNames[g_bossIdx]);
    g_pokeActive  = true;
    g_pokeStartMs = millis();
    g_pokePhrase  = g_customBubbleBuf;
  }
  g_detailDirty = true;
  markDirty();
}

static int currentMinOfDay();

static void saveState() {
  g_prefs.putInt("mood", g_mood);
  g_prefs.putInt("todayDone", g_todayDone);
  g_prefs.putInt("streak", g_streakDays);
  g_prefs.putInt("level", g_level);
  g_prefs.putInt("exp", g_exp);
  g_prefs.putInt("bossIdx", g_bossIdx);
  g_prefs.putInt("bossHp", g_bossHp);
  g_prefs.putInt("bossKills", g_bossKills);
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
  g_bossKills  = max(0, g_prefs.getInt("bossKills", 0));
  g_bossIdx    = constrain(g_prefs.getInt("bossIdx", 0), 0, BOSS_COUNT - 1);
  g_bossMaxHp  = 300 + g_bossIdx * 120 + min(1200, g_bossKills * 60);
  g_bossHp     = constrain(g_prefs.getInt("bossHp", g_bossMaxHp), 1, g_bossMaxHp);
  int savedMin = g_prefs.getInt("lastMin", -1);
  if (savedMin >= 0 && savedMin < 1440) {
    g_baseMinOfDay    = savedMin;
    g_lastNvsSavedMin = savedMin;
  } else {
    g_baseMinOfDay    = compileMinOfDay();
  }
  g_baseMillis = millis();
  g_timeValid  = true;
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
  uint32_t elapsedMin = (millis() - g_baseMillis) / 60000UL;
  int base = (g_baseMinOfDay >= 0 && g_baseMinOfDay < 1440) ? g_baseMinOfDay : compileMinOfDay();
  return (base + (int)elapsedMin) % 1440;
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
  // 擦除顶部会话卡区域 (y=18..62) 和 飘字+流星+气泡区域 (y=62..133)
  g_canvas.fillRect(0, 18, SCR_W, 44, BG_COLOR);
  g_canvas.fillRect(0, 88, SCR_W, 45, BG_COLOR);
  if (g_dmgActive) {
    g_canvas.fillRect(0, max(18, (int)g_dmgY - 7), SCR_W, 14, BG_COLOR);
  }
  if (g_shootActive) {
    g_canvas.fillRect(max(0, (int)g_shootX - 18), max(18, (int)g_shootY - 8), 26, 15, BG_COLOR);
  }

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

  // 4. 舞台流星系统 (每 ~26s 划过夜空/舞台上空, 按 BtnA 可捕捉许愿!)
  if (!g_shootActive && (now - g_lastShootMs >= 26000UL)) {
    g_lastShootMs = now;
    g_shootActive = true;
    g_shootX      = 14.0f;
    g_shootY      = 64.0f;
    g_shootLife   = 28;
  }
  if (g_shootActive) {
    g_shootX += 3.6f;
    g_shootY += 0.65f;
    if (g_shootLife > 0) g_shootLife--;
    int sx = (int)g_shootX;
    int sy = (int)g_shootY;
    if (g_shootLife == 0 || sx >= SCR_W - 6 || sy >= 86) {
      g_shootActive = false;
    } else {
      g_canvas.drawLine(sx - 12, sy - 2, sx, sy, colCodex());
      g_canvas.drawLine(sx - 6,  sy - 1, sx, sy, colAmber());
      g_canvas.fillCircle(sx, sy, 1, TFT_WHITE);
    }
  }

  // 5. Boss 战浮动伤害飘字 (-24 / CRIT -58!)
  if (g_dmgActive) {
    g_dmgY -= 1.15f;
    if (g_dmgLife > 0) g_dmgLife--;
    if (g_dmgLife == 0 || g_dmgY < 62.0f) {
      g_dmgActive = false;
    } else {
      g_canvas.setFont(&fonts::Font0);
      g_canvas.setTextSize(1);
      uint16_t dCol = g_dmgCrit ? colAmber() : colCoral();
      g_canvas.setTextColor(dCol);
      g_canvas.setTextDatum(middle_center);
      g_canvas.drawString(g_dmgText, (int)g_dmgX, (int)g_dmgY);
    }
  }
}

// 绘制底部四页圆点指示器 (用于 Page 2 / Page 3 / Page 4)
static void drawPageDots(int activePage) {
  int y = 236;
  int cx = SCR_W / 2;
  for (int i = 0; i < 4; i++) {
    int x = cx - 15 + i * 10;
    if (i == activePage) {
      g_canvas.fillRoundRect(x - 4, y - 1, 8, 3, 1, colCodex());
    } else {
      g_canvas.fillCircle(x, y, 1, colBorder());
    }
  }
}

// 赛博装甲卡片外框: 深色底板 + 细边框 + 四角霓虹高光护角 (⌜ ⌝ ⌞ ⌟)
static void drawCyberCard(int x, int y, int w, int h, uint16_t bgCol, uint16_t borderCol, uint16_t cornerCol) {
  g_canvas.fillRoundRect(x, y, w, h, 3, bgCol);
  g_canvas.drawRoundRect(x, y, w, h, 3, borderCol);
  int cl = 4;
  // 左上角
  g_canvas.drawFastHLine(x, y, cl, cornerCol);
  g_canvas.drawFastVLine(x, y, cl, cornerCol);
  // 右上角
  g_canvas.drawFastHLine(x + w - cl, y, cl, cornerCol);
  g_canvas.drawFastVLine(x + w - 1, y, cl, cornerCol);
  // 左下角
  g_canvas.drawFastHLine(x, y + h - 1, cl, cornerCol);
  g_canvas.drawFastVLine(x, y + h - cl, cl, cornerCol);
  // 右下角
  g_canvas.drawFastHLine(x + w - cl, y + h - 1, cl, cornerCol);
  g_canvas.drawFastVLine(x + w - 1, y + h - cl, cl, cornerCol);
}

// 严格按像素宽度裁切字符串 (兼容 UTF-8 中文字符边界, 绝不越界重叠)
static void drawClippedString(const String& raw, int x, int y, int maxPx, textdatum_t datum = top_left) {
  g_canvas.setTextDatum(datum);
  if (raw.length() == 0 || maxPx <= 0) return;
  if (g_canvas.textWidth(raw) <= maxPx) {
    g_canvas.drawString(raw, x, y);
    return;
  }
  int len = raw.length();
  while (len > 0) {
    len--;
    // 跳过 UTF-8 续字节 (10xxxxxx), 保证不在多字节字符中间截断
    while (len > 0 && ((uint8_t)raw[len] & 0xC0) == 0x80) {
      len--;
    }
    String sub = raw.substring(0, len);
    if (g_canvas.textWidth(sub) <= maxPx) {
      g_canvas.drawString(sub, x, y);
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// 通用顶栏 (y: 0..17): 时间 | [Lv.x] 胶囊 + 跳动像素爱心 | 电池 + 2px EXP 光条
// ---------------------------------------------------------------------------
static void drawStatusBarOverlay() {
  g_canvas.fillRect(0, 0, SCR_W, 18, colSurfaceAlt());
  g_canvas.drawFastHLine(0, 16, SCR_W, g_canvas.color565(22, 28, 42));
  g_canvas.drawFastHLine(0, 17, SCR_W, colBorder());
  // 顶栏底边 2px 霓虹 RPG 经验条 + 亮白光标头
  int expW = (SCR_W * constrain(g_exp, 0, 100)) / 100;
  if (expW > 0) {
    g_canvas.drawFastHLine(0, 16, expW, colPi());
    g_canvas.drawFastHLine(0, 17, expW, colCodex());
    g_canvas.fillRect(max(0, expW - 2), 16, 2, 2, TFT_WHITE);
  }

  // 左: 永远固定显示 24h 时间
  char ts[12];
  int m = currentMinOfDay();
  snprintf(ts, sizeof(ts), "%02d:%02d", m / 60, m % 60);
  g_canvas.setFont(&fonts::Font0);
  g_canvas.setTextSize(1);
  g_canvas.setTextColor(TFT_WHITE, colSurfaceAlt());
  g_canvas.setTextDatum(top_left);
  g_canvas.drawString(ts, 3, 4);

  // 若开启专注番茄钟, 在后台检查完成
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

  // 中左: 金色装甲胶囊 [L22] (x=36..60)
  char lvStr[8];
  snprintf(lvStr, sizeof(lvStr), "L%d", g_level);
  int lvW = (g_level >= 10) ? 23 : 17;
  g_canvas.fillRoundRect(36, 2, lvW, 12, 2, g_canvas.color565(55, 42, 14));
  g_canvas.drawRoundRect(36, 2, lvW, 12, 2, colAmber());
  g_canvas.setTextColor(colAmber());
  g_canvas.setTextDatum(middle_center);
  g_canvas.drawString(lvStr, 36 + lvW / 2, 8);

  // 中右: 呼吸跳动像素爱心 + 心情值
  uint16_t moodCol = (g_mood >= 70) ? colEmerald()
                   : (g_mood >= 35) ? colAmber()
                   : colCoral();
  int hx = 36 + lvW + 4;
  int hy = 5 + (((millis() / 600) % 2 == 0) ? 0 : 1);
  drawPixelHeart(hx, hy, moodCol);
  char ms[6];
  snprintf(ms, sizeof(ms), "%d", g_mood);
  g_canvas.setTextColor(moodCol, colSurfaceAlt());
  g_canvas.setTextDatum(top_left);
  g_canvas.drawString(ms, hx + 9, 4);

  // 右: 电池百分比 + 电池图标
  int pct = g_battLevel < 0 ? 0 : g_battLevel;
  uint16_t bCol = TFT_WHITE;
  if (g_charging)     bCol = colEmerald();
  else if (pct <= 20) bCol = colCoral();

  char ps[6];
  snprintf(ps, sizeof(ps), "%d%%", pct);
  int bx = SCR_W - 4 - 15;
  int by = 3;
  g_canvas.setTextColor(bCol, colSurfaceAlt());
  g_canvas.setTextDatum(top_right);
  g_canvas.drawString(ps, bx - 2, 4);

  g_canvas.drawRect(bx, by, 15, 9, bCol);
  g_canvas.fillRect(bx + 15, by + 2, 2, 5, bCol);
  int fillW = (13 * pct) / 100;
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

// 绘制单个三端迷你立体胶囊 (C:n / X:n / P:n)
static void drawAgentPill(int x, int y, int w, int h, char label, int count, uint16_t brandCol, uint16_t dimCol) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%c:%d", label, count);
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  if (count > 0) {
    g_canvas.fillRoundRect(x, y, w, h, 3, brandCol);
    g_canvas.drawFastHLine(x + 2, y + 1, w - 4, TFT_WHITE);
    g_canvas.setTextColor(TFT_BLACK, brandCol);
  } else {
    g_canvas.fillRoundRect(x, y, w, h, 3, colSurfaceAlt());
    g_canvas.drawRoundRect(x, y, w, h, 3, dimCol);
    g_canvas.drawFastHLine(x + 3, y, w - 6, brandCol);
    g_canvas.setTextColor(brandCol, colSurfaceAlt());
  }
  g_canvas.setTextDatum(middle_center);
  g_canvas.drawString(buf, x + w / 2, y + h / 2 + 1);
}

// ---------------------------------------------------------------------------
// 底部双行赛博 HUD 底座 (y=202..239):
//   顶边 2px: 实时 Bug Boss 血条 + 受击闪白
//   第一行: [C:n] [X:n] [P:n] 三端立体胶囊
//   第二行: OK:n | 示波器网格脉冲心电图 (带亮白扫描头) | 四页指示点
// ---------------------------------------------------------------------------
static void drawBottomHudDock() {
  int dockY = 202, dockH = 37;
  drawCyberCard(2, dockY, SCR_W - 4, dockH, colSurface(), colBorder(), colCodex());

  // 顶部亮边: 兼具舞台发光地板线 + 实时 Bug Boss 血条 (2px)
  int barTrackW = SCR_W - 14;
  g_canvas.drawFastHLine(7, dockY,     barTrackW, colSurfaceAlt());
  g_canvas.drawFastHLine(7, dockY + 1, barTrackW, colSurfaceAlt());
  int bossFillW = (barTrackW * constrain(g_bossHp, 0, g_bossMaxHp)) / max(1, g_bossMaxHp);
  bool bossHitFlash = (millis() - g_bossHitMs) < 350;
  bool feverActive  = (millis() < g_feverUntilMs);
  uint16_t stageLineCol = bossHitFlash ? TFT_WHITE
                        : feverActive  ? colPi()
                        : (g_state == PetState::WORKING) ? colCoral()
                        : colAmber();
  if (bossFillW > 0) {
    g_canvas.drawFastHLine(7, dockY,     bossFillW, stageLineCol);
    g_canvas.drawFastHLine(7, dockY + 1, bossFillW, stageLineCol);
  }

  int cAct = 0, xAct = 0, pAct = 0;
  getAgentActiveCounts(&cAct, &xAct, &pAct);

  // 第一行 (y=206..218): 三端状态胶囊居中均分
  int pillW = 38, pillH = 13, gap = 4;
  int startX = (SCR_W - (pillW * 3 + gap * 2)) / 2;
  int row1Y = dockY + 4;
  drawAgentPill(startX,                   row1Y, pillW, pillH, 'C', cAct, colClaude(), agentDimColor('C'));
  drawAgentPill(startX + pillW + gap,     row1Y, pillW, pillH, 'X', xAct, colCodex(),  agentDimColor('X'));
  drawAgentPill(startX + (pillW + gap)*2, row1Y, pillW, pillH, 'P', pAct, colPi(),     agentDimColor('P'));

  // 第二行 (y=221..235): 左=OK:n  中=示波器网格心电图  右=四页指示点
  int row2Y = dockY + 27;
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);

  char doneBuf[10];
  snprintf(doneBuf, sizeof(doneBuf), "OK:%d", g_todayDone);
  g_canvas.setTextColor(colEmerald(), colSurface());
  g_canvas.setTextDatum(middle_left);
  g_canvas.drawString(doneBuf, 6, row2Y);

  // 中间 (x=37..97): 带暗网格与亮白扫描头的迷你示波器 (Mini Oscilloscope)
  int spX = 37, spY = dockY + 20, spW = 60, spH = 13;
  g_canvas.fillRoundRect(spX, spY, spW, spH, 2, colSurfaceAlt());
  g_canvas.drawRoundRect(spX, spY, spW, spH, 2, g_canvas.color565(30, 42, 62));
  for (int gx = spX + 10; gx < spX + spW - 4; gx += 12) {
    g_canvas.drawPixel(gx, spY + 4, g_canvas.color565(28, 38, 56));
    g_canvas.drawPixel(gx, spY + 9, g_canvas.color565(28, 38, 56));
  }
  int spBaseY = spY + spH - 2;
  uint16_t waveCol = (g_combo >= 5) ? colPi()
                   : (g_state == PetState::WORKING) ? colAmber()
                   : colCodex();
  int lastX = spX + 2, lastY = spBaseY - 1;
  for (int i = 0; i < SPARK_LEN - 1; i++) {
    int x0 = spX + 2 + (i * (spW - 5)) / (SPARK_LEN - 1);
    int x1 = spX + 2 + ((i + 1) * (spW - 5)) / (SPARK_LEN - 1);
    int y0 = constrain(spBaseY - g_spark[i],     spY + 2, spBaseY);
    int y1 = constrain(spBaseY - g_spark[i + 1], spY + 2, spBaseY);
    g_canvas.drawLine(x0, y0, x1, y1, waveCol);
    lastX = x1;
    lastY = y1;
  }
  // 扫描头高亮脉冲点
  g_canvas.fillRect(lastX - 1, lastY - 1, 2, 2, TFT_WHITE);

  // 右下角: 四页指示点 (━ ○ ○ ○)
  int dotBaseX = 104;
  int dotY = row2Y;
  g_canvas.fillRoundRect(dotBaseX - 2, dotY - 1, 6, 3, 1, colCodex());
  g_canvas.fillCircle(dotBaseX + 7,  dotY, 1, colBorder());
  g_canvas.fillCircle(dotBaseX + 13, dotY, 1, colBorder());
  g_canvas.fillCircle(dotBaseX + 19, dotY, 1, colBorder());
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

      drawCyberCard(3, cy, SCR_W - 6, ch, colSurface(), borderCol, agCol);
      g_canvas.fillRoundRect(5, cy + 3, 2, ch - 6, 1, stripeCol);

      // 平台微徽章 [C]/[X]/[P]
      g_canvas.fillRoundRect(9, cy + 3, 11, 11, 2, agentDimColor(s.agent));
      g_canvas.setFont(nullptr);
      g_canvas.setTextSize(1);
      g_canvas.setTextColor(agCol);
      g_canvas.setTextDatum(middle_center);
      char agStr[2] = { s.agent ? s.agent : 'C', '\0' };
      g_canvas.drawString(agStr, 14, cy + 8);

      // 项目名称: 严格限制最大像素宽度 58px (x=23..81), 绝不与右侧工具胶囊重叠!
      g_canvas.setFont(&fonts::efontCN_12);
      g_canvas.setTextSize(1);
      g_canvas.setTextColor(TFT_WHITE);
      drawClippedString(s.name, 23, cy + ch / 2, 58, middle_left);

      // 右侧工具/状态胶囊 (x=84..128)
      int pillX = 84, pillW = 44, pillY = cy + 3, pillH = 12;
      g_canvas.fillRoundRect(pillX, pillY, pillW, pillH, 2, colSurfaceAlt());
      g_canvas.setFont(nullptr);
      g_canvas.setTextSize(1);
      g_canvas.setTextDatum(middle_center);
      if (isWait) {
        g_canvas.drawRoundRect(pillX, pillY, pillW, pillH, 2, blink ? colCodex() : colBorder());
        g_canvas.setTextColor(blink ? colCodex() : TFT_WHITE);
        g_canvas.drawString("WAIT!", pillX + pillW / 2, pillY + pillH / 2 + 1);
      } else if (s.tool.length() > 0) {
        String tLabel = ">" + s.tool.substring(0, 5);
        g_canvas.setTextColor(colAmber());
        g_canvas.drawString(tLabel, pillX + pillW / 2, pillY + pillH / 2 + 1);
      } else {
        const char* dots = ((millis() / 400) % 2 == 0) ? "RUN.." : "RUN.";
        g_canvas.setTextColor(colAmber());
        g_canvas.drawString(dots, pillX + pillW / 2, pillY + pillH / 2 + 1);
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
      drawCyberCard(3, cy, SCR_W - 6, 18, colSurface(), isWait ? colCodex() : colBorder(), colAmber());
      g_canvas.setFont(&fonts::efontCN_12);
      g_canvas.setTextColor(isWait ? colCodex() : TFT_WHITE);
      drawClippedString(nm, 10, cy + 9, 110, middle_left);
    }
    return;
  }

  // 空闲态: 若有最近完成任务, 顶部仅显示一条轻量赛博胶囊
  if (g_histCount > 0) {
    const HistEntry& h = g_history[0];
    int pillH = 18;
    uint16_t agCol = agentColor(h.agent);
    drawCyberCard(3, baseY, SCR_W - 6, pillH, colSurfaceAlt(), colBorder(), agCol);

    g_canvas.fillRoundRect(6, baseY + 3, 11, 11, 2, agentDimColor(h.agent));
    g_canvas.setFont(nullptr);
    g_canvas.setTextSize(1);
    g_canvas.setTextColor(agCol);
    g_canvas.setTextDatum(middle_center);
    char agStr[2] = { h.agent ? h.agent : 'C', '\0' };
    g_canvas.drawString(agStr, 11, baseY + 8);

    g_canvas.setFont(&fonts::efontCN_12);
    g_canvas.setTextColor(colMuted());
    drawClippedString(h.name, 21, baseY + 9, 58, middle_left);

    g_canvas.setFont(nullptr);
    g_canvas.setTextColor(colEmerald());
    g_canvas.setTextDatum(middle_right);
    g_canvas.drawString(String("OK ") + h.info.substring(0, 4), SCR_W - 7, baseY + 9);
  }
}

// 根据当前工具/连击/倾斜/时段生成生动的气泡文案
static const char* resolveDynamicPhrase() {
  if (g_state == PetState::IDLE && abs(g_tiltOffsetPx) >= 11) {
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
  } else if (millis() < g_feverUntilMs) {
    uint32_t remSec = (g_feverUntilMs - millis()) / 1000UL;
    char fBuf[14];
    snprintf(fBuf, sizeof(fBuf), "2xATK %ds", (int)remSec);
    g_canvas.fillRoundRect(4, 89, 54, 12, 3, colPi());
    g_canvas.setFont(nullptr);
    g_canvas.setTextSize(1);
    g_canvas.setTextColor(TFT_BLACK, colPi());
    g_canvas.setTextDatum(middle_center);
    g_canvas.drawString(fBuf, 31, 95);
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
// Page 2 (DETAIL): 多端会话雷达舱 + 最近完成时间线 (彻底消除双列文字重叠!)
// ---------------------------------------------------------------------------
static void drawDetailContent() {
  uint32_t now = millis();
  int y = 20;

  int cAct = 0, xAct = 0, pAct = 0;
  getAgentActiveCounts(&cAct, &xAct, &pAct);

  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  g_canvas.setTextColor(colMuted());
  g_canvas.setTextDatum(top_left);
  char hdr[24];
  snprintf(hdr, sizeof(hdr), "SESSIONS (%d)", g_sessionCount);
  g_canvas.drawString(hdr, 5, y + 2);

  // 右上角显示三端活跃数摘要
  char agSum[20];
  snprintf(agSum, sizeof(agSum), "C:%d X:%d P:%d", cAct, xAct, pAct);
  g_canvas.setTextColor(colCodex());
  g_canvas.setTextDatum(top_right);
  g_canvas.drawString(agSum, SCR_W - 5, y + 2);
  y += 14;

  int maxShow = 4;
  if (g_sessionCount > 0) {
    int pages = (g_sessionCount + maxShow - 1) / maxShow;
    if (g_detailScroll >= pages) g_detailScroll = 0;
    int startIdx = g_detailScroll * maxShow;
    int endIdx = min(g_sessionCount, startIdx + maxShow);

    for (int i = startIdx; i < endIdx; i++) {
      SessionInfo& s = g_sessions[i];
      int cardH = 28;
      uint16_t stCol = stateColor(s.state);
      uint16_t agCol = agentColor(s.agent);
      uint16_t barCol = s.hasColor ? s.color : agCol;

      drawCyberCard(3, y, SCR_W - 6, cardH, colSurface(),
                    (s.state == 'W') ? colCodex() : colBorder(), agCol);
      g_canvas.fillRoundRect(5, y + 3, 2, cardH - 6, 1, barCol);

      // 平台微徽章 [C]/[X]/[P]
      g_canvas.fillRoundRect(9, y + 4, 12, 11, 2, agentDimColor(s.agent));
      g_canvas.setFont(nullptr);
      g_canvas.setTextSize(1);
      g_canvas.setTextColor(agCol);
      g_canvas.setTextDatum(middle_center);
      char agBuf[2] = { s.agent ? s.agent : 'C', '\0' };
      g_canvas.drawString(agBuf, 15, y + 9);

      // 第一行左: 项目名称 (严格限制 56px 宽度 x=24..80, 绝不碰撞右侧胶囊!)
      g_canvas.setFont(&fonts::efontCN_12);
      g_canvas.setTextSize(1);
      g_canvas.setTextColor(TFT_WHITE);
      drawClippedString(s.name, 24, y + 2, 56, top_left);

      // 第一行右: 工具/状态胶囊 (x=83..128)
      int pillX = 83, pillW = 45, pillY = y + 3, pillH = 11;
      g_canvas.fillRoundRect(pillX, pillY, pillW, pillH, 2, colSurfaceAlt());
      g_canvas.setFont(nullptr);
      g_canvas.setTextSize(1);
      g_canvas.setTextDatum(middle_center);
      g_canvas.setTextColor(stCol);
      if (s.state == 'R' && s.tool.length() > 0) {
        String tl = ">" + s.tool.substring(0, 5);
        g_canvas.drawString(tl, pillX + pillW / 2, pillY + pillH / 2 + 1);
      } else {
        g_canvas.drawString(stateLabel(s.state), pillX + pillW / 2, pillY + pillH / 2 + 1);
      }

      // 第二行左 (x=24..84): 动态扫描进度条
      int barX = 24, barY = y + 19, barW = 58, barH = 4;
      g_canvas.fillRoundRect(barX, barY, barW, barH, 1, colSurfaceAlt());
      if (s.state == 'R') {
        int scanPos = ((now / 80) + i * 11) % (barW - 14);
        g_canvas.fillRoundRect(barX + scanPos, barY, 14, barH, 1, agCol);
      } else if (s.state == 'W') {
        g_canvas.fillRoundRect(barX, barY, barW, barH, 1, colCodex());
      } else if (s.state == 'D') {
        g_canvas.fillRoundRect(barX, barY, barW, barH, 1, colEmerald());
      }

      // 第二行右 (x=86..127): 耗时
      uint32_t elapsed = s.durSec + (now - s.recvMs) / 1000;
      g_canvas.setTextColor(colMuted());
      g_canvas.setTextDatum(bottom_right);
      g_canvas.drawString(formatDuration(elapsed), SCR_W - 7, y + cardH - 2);

      y += cardH + 3;
    }
  } else {
    drawCyberCard(3, y, SCR_W - 6, 32, colSurfaceAlt(), colBorder(), colCodex());
    g_canvas.setFont(nullptr);
    g_canvas.setTextColor(colMuted());
    g_canvas.setTextDatum(middle_center);
    g_canvas.drawString("No active sessions", SCR_W / 2, y + 16);
    y += 36;
  }

  // === 底部: 最近完成时间线卡片 (y: max(156, y+2) .. 229) ===
  int histBoxY = max(y + 2, 156);
  int histBoxH = 229 - histBoxY;
  if (histBoxH >= 28) {
    drawCyberCard(3, histBoxY, SCR_W - 6, histBoxH, colSurfaceAlt(), colBorder(), colEmerald());
    g_canvas.setFont(nullptr);
    g_canvas.setTextSize(1);
    g_canvas.setTextColor(colMuted());
    g_canvas.setTextDatum(top_left);
    g_canvas.drawString("RECENT DONE", 8, histBoxY + 4);

    char tdStr[14];
    snprintf(tdStr, sizeof(tdStr), "TODAY:%d", g_todayDone);
    g_canvas.setTextColor(colEmerald());
    g_canvas.setTextDatum(top_right);
    g_canvas.drawString(tdStr, SCR_W - 8, histBoxY + 4);

    int hy = histBoxY + 15;
    if (g_histCount == 0) {
      g_canvas.setTextColor(colMuted());
      g_canvas.setTextDatum(middle_center);
      g_canvas.drawString("Waiting for tasks...", SCR_W / 2, histBoxY + histBoxH / 2 + 4);
    } else {
      for (int i = 0; i < g_histCount && (hy + 14) <= (histBoxY + histBoxH - 2); i++) {
        const HistEntry& h = g_history[i];
        uint16_t agCol = agentColor(h.agent);
        g_canvas.fillRoundRect(7, hy + 1, 10, 10, 2, agentDimColor(h.agent));
        g_canvas.setFont(nullptr);
        g_canvas.setTextColor(agCol);
        g_canvas.setTextDatum(middle_center);
        char agTag[2] = { h.agent ? h.agent : 'C', '\0' };
        g_canvas.drawString(agTag, 12, hy + 6);

        g_canvas.setFont(&fonts::efontCN_12);
        g_canvas.setTextColor(TFT_WHITE);
        drawClippedString(h.name, 20, hy + 6, 66, middle_left);

        g_canvas.setFont(nullptr);
        g_canvas.setTextColor(colEmerald());
        g_canvas.setTextDatum(middle_right);
        g_canvas.drawString(h.info.substring(0, 6), SCR_W - 8, hy + 6);
        hy += 15;
      }
    }
  }

  drawPageDots(1);
}

// ---------------------------------------------------------------------------
// Page 3 (BOSS RAID & RPG): 像素深渊打怪竞技场 + 全宽 RPG 养成面板 + 技能键
// ---------------------------------------------------------------------------
// 8 大程序员 Bug Boss 的 12x10 像素精灵图 (1=身体主色, 每行低 12 位有效)
static const uint16_t kBossSprites[BOSS_COUNT][10] = {
  // 0: NullPtr (幽灵故障体)
  { 0x03C0, 0x07E0, 0x0FF0, 0x0DB0, 0x0FF0, 0x0FF0, 0x0FF0, 0x0A50, 0x0990, 0x0420 },
  // 1: RaceCond (双核狂暴眼)
  { 0x0C30, 0x0660, 0x0FF0, 0x0990, 0x0FF0, 0x0BD0, 0x0FF0, 0x0660, 0x0C30, 0x0810 },
  // 2: MemLeak (膨胀史莱姆)
  { 0x0180, 0x03C0, 0x07E0, 0x0DB0, 0x0FF0, 0x0FF0, 0x0E70, 0x0FF0, 0x0FF0, 0x07E0 },
  // 3: Heisenbug (量子闪烁魔方)
  { 0x0FF0, 0x0810, 0x0BD0, 0x0A50, 0x0FF0, 0x0A50, 0x0BD0, 0x0810, 0x0FF0, 0x0660 },
  // 4: Segfault (深渊骷髅王)
  { 0x07E0, 0x0FF0, 0x0DB0, 0x0FF0, 0x07E0, 0x03C0, 0x05A0, 0x03C0, 0x0660, 0x0C30 },
  // 5: MergeDmn (牛角冲突恶魔)
  { 0x0810, 0x0C30, 0x0FF0, 0x0DB0, 0x0FF0, 0x07E0, 0x0FF0, 0x0A50, 0x0C30, 0x0420 },
  // 6: Deadlock (死锁重装机甲)
  { 0x03C0, 0x0FF0, 0x0DB0, 0x0FF0, 0x0FF0, 0x0C30, 0x0FF0, 0x0990, 0x0990, 0x0F0F },
  // 7: LegacyCore (远古巨石核心)
  { 0x03C0, 0x07E0, 0x0FF0, 0x0E70, 0x0DB0, 0x0E70, 0x0FF0, 0x07E0, 0x05A0, 0x0A50 }
};

static void drawPixelBoss(int cx, int cy, int bossIdx, bool hitFlash) {
  uint32_t now = millis();
  int bobY = ((now / 320) % 2 == 0) ? -1 : 1;
  int shakeX = hitFlash ? (((now / 35) % 2 == 0) ? -3 : 3) : 0;

  uint16_t kCols[BOSS_COUNT] = {
    colCodex(), colAmber(), colEmerald(), colPi(),
    colCoral(), colClaude(), colCodex(), colPi()
  };
  uint16_t bodyCol = hitFlash ? TFT_WHITE : kCols[bossIdx % BOSS_COUNT];
  uint16_t eyeCol  = hitFlash ? colCoral() : TFT_WHITE;

  int scale = 3;
  int x0 = cx - (12 * scale) / 2 + shakeX;
  int y0 = cy - (10 * scale) / 2 + bobY;

  for (int r = 0; r < 10; r++) {
    uint16_t mask = kBossSprites[bossIdx % BOSS_COUNT][r];
    for (int c = 0; c < 12; c++) {
      if (mask & (1 << (11 - c))) {
        g_canvas.fillRect(x0 + c * scale, y0 + r * scale, scale, scale, bodyCol);
      } else if (r == 3 && (c == 4 || c == 7)) {
        // 发光像素双眼
        g_canvas.fillRect(x0 + c * scale, y0 + r * scale, scale, scale, eyeCol);
      }
    }
  }

  // 受击时叠加炫酷交叉激光斩击特效 (X-Slash Beam)
  if (hitFlash) {
    g_canvas.drawLine(cx - 18, cy - 14, cx + 18, cy + 14, colAmber());
    g_canvas.drawLine(cx - 17, cy - 14, cx + 19, cy + 14, TFT_WHITE);
    g_canvas.drawLine(cx + 18, cy - 14, cx - 18, cy + 14, colCodex());
    g_canvas.drawLine(cx + 17, cy - 14, cx - 19, cy + 14, TFT_WHITE);
  }
}

static void drawSegmentedBar(int x, int y, int w, int h, int pct, uint16_t fillCol) {
  g_canvas.fillRoundRect(x, y, w, h, 2, colSurfaceAlt());
  g_canvas.drawRoundRect(x, y, w, h, 2, g_canvas.color565(32, 42, 62));
  int fw = ((w - 2) * constrain(pct, 0, 100)) / 100;
  if (fw > 1) {
    g_canvas.fillRoundRect(x + 1, y + 1, fw, h - 2, 1, fillCol);
    // 亮白高光顶线
    g_canvas.drawFastHLine(x + 1, y + 1, fw, TFT_WHITE);
  }
  // 分段刻度线
  for (int sx = x + w / 4; sx < x + w - 4; sx += w / 4) {
    g_canvas.drawFastVLine(sx, y + 1, h - 2, colSurfaceAlt());
  }
}

static void drawBossRaidView() {
  uint32_t now = millis();
  bool hitFlash = (now - g_bossHitMs) < 260;
  bool fever    = (now < g_feverUntilMs);

  // === 1. 顶部 Boss 竞技场大卡 (y: 20..112, h=92) ===
  drawCyberCard(3, 20, SCR_W - 6, 92, colSurface(),
                hitFlash ? TFT_WHITE : colBorder(), colCoral());

  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  char stageBuf[16];
  snprintf(stageBuf, sizeof(stageBuf), "RAID #%d", g_bossKills + 1);
  g_canvas.setTextColor(colAmber());
  g_canvas.setTextDatum(top_left);
  g_canvas.drawString(stageBuf, 8, 24);

  char bossTitle[20];
  snprintf(bossTitle, sizeof(bossTitle), "vs %s", kBossNames[g_bossIdx]);
  g_canvas.setTextColor(colCoral());
  g_canvas.setTextDatum(top_right);
  g_canvas.drawString(bossTitle, SCR_W - 8, 24);

  // 左侧 Boss 准星舱 (x=8..54, y=35..79)
  int arenaX = 8, arenaY = 35, arenaW = 46, arenaH = 44;
  g_canvas.fillRoundRect(arenaX, arenaY, arenaW, arenaH, 3, colSurfaceAlt());
  g_canvas.drawRoundRect(arenaX, arenaY, arenaW, arenaH, 3, hitFlash ? colAmber() : g_canvas.color565(32, 42, 62));
  drawPixelBoss(arenaX + arenaW / 2, arenaY + arenaH / 2, g_bossIdx, hitFlash);

  // 右侧 Boss 血量与战况面板 (x=58..126)
  int rx = 58, rw = SCR_W - 8 - rx;
  g_canvas.setTextColor(colCoral());
  g_canvas.setTextDatum(top_left);
  g_canvas.drawString("HP", rx, 36);

  char hpNums[16];
  snprintf(hpNums, sizeof(hpNums), "%d/%d", g_bossHp, g_bossMaxHp);
  g_canvas.setTextColor(TFT_WHITE);
  g_canvas.setTextDatum(top_right);
  g_canvas.drawString(hpNums, rx + rw, 36);

  int bossPct = (g_bossHp * 100) / max(1, g_bossMaxHp);
  drawSegmentedBar(rx, 46, rw, 8, bossPct, hitFlash ? colAmber() : colCoral());

  char killStr[18];
  snprintf(killStr, sizeof(killStr), "Slain: %d", g_bossKills);
  g_canvas.setTextColor(colEmerald());
  g_canvas.setTextDatum(top_left);
  g_canvas.drawString(killStr, rx, 58);

  int atkVal = 18 + g_level * 2;
  if (fever) atkVal *= 2;
  char atkStr[18];
  snprintf(atkStr, sizeof(atkStr), fever ? "ATK:%d(2x)" : "ATK: %d", atkVal, atkVal);
  g_canvas.setTextColor(fever ? colPi() : colCodex());
  g_canvas.drawString(atkStr, rx, 69);

  // 竞技场底部实时战报横幅 (y=83..107)
  g_canvas.fillRoundRect(8, 83, SCR_W - 16, 23, 3, colSurfaceAlt());
  g_canvas.drawRoundRect(8, 83, SCR_W - 16, 23, 3, hitFlash ? colAmber() : colBorder());
  g_canvas.setFont(nullptr);
  g_canvas.setTextColor(hitFlash ? colAmber() : colCodex());
  g_canvas.setTextDatum(middle_center);
  g_canvas.drawString(g_battleLog, SCR_W / 2, 95);

  // === 2. 中部玩家全宽 RPG 属性卡 (y: 115..183, h=68, 绝无分列重叠!) ===
  drawCyberCard(3, 115, SCR_W - 6, 68, colSurface(), colBorder(), colPi());

  // 第一行: 等级称号 (左) + 连胜天数 (右)
  char lvFull[22];
  snprintf(lvFull, sizeof(lvFull), "Lv.%d %s", g_level, levelTitle(g_level));
  g_canvas.setTextColor(colAmber());
  g_canvas.setTextDatum(top_left);
  g_canvas.drawString(lvFull, 8, 120);

  char strkBuf[14];
  snprintf(strkBuf, sizeof(strkBuf), "%dd streak", g_streakDays);
  g_canvas.setTextColor(colClaude());
  g_canvas.setTextDatum(top_right);
  g_canvas.drawString(strkBuf, SCR_W - 8, 120);

  // 第二行: 全宽 EXP 进度条
  g_canvas.setTextColor(colPi());
  g_canvas.setTextDatum(top_left);
  g_canvas.drawString("EXP", 8, 133);
  char expTxt[12];
  snprintf(expTxt, sizeof(expTxt), "%d/100", g_exp);
  g_canvas.setTextDatum(top_right);
  g_canvas.drawString(expTxt, SCR_W - 8, 133);
  drawSegmentedBar(8, 143, SCR_W - 16, 7, g_exp, colPi());

  // 第三行: 全宽 MOOD 进度条
  uint16_t mCol = (g_mood >= 70) ? colEmerald() : (g_mood >= 35) ? colAmber() : colCoral();
  g_canvas.setTextColor(mCol);
  g_canvas.setTextDatum(top_left);
  g_canvas.drawString("MOOD", 8, 155);
  char moodTxt[12];
  snprintf(moodTxt, sizeof(moodTxt), "%d%%", g_mood);
  g_canvas.setTextDatum(top_right);
  g_canvas.drawString(moodTxt, SCR_W - 8, 155);
  drawSegmentedBar(8, 165, SCR_W - 16, 7, g_mood, mCol);

  // === 3. 底部三键交互技能槽 (y: 186..228, h=42) ===
  const char* kBtnTop[3] = { "[A]",   "[2xA]", "[HOLD]" };
  const char* kBtnBot[3] = { "SLASH", "GACHA", "HEAL"   };
  uint16_t    kBtnCol[3] = { colCoral(), colPi(), colEmerald() };
  for (int i = 0; i < 3; i++) {
    int bx = 3 + i * 44;
    int bw = 41, by = 187, bh = 40;
    bool hi = (hitFlash && g_lastSkillHighlight == (i + 1));
    drawCyberCard(bx, by, bw, bh, hi ? kBtnCol[i] : colSurface(),
                  hi ? TFT_WHITE : colBorder(), kBtnCol[i]);
    g_canvas.setFont(nullptr);
    g_canvas.setTextSize(1);
    g_canvas.setTextColor(hi ? TFT_BLACK : kBtnCol[i]);
    g_canvas.setTextDatum(middle_center);
    g_canvas.drawString(kBtnTop[i], bx + bw / 2, by + 13);
    g_canvas.setTextColor(hi ? TFT_BLACK : TFT_WHITE);
    g_canvas.drawString(kBtnBot[i], bx + bw / 2, by + 27);
  }

  drawPageDots(2);
}

// ---------------------------------------------------------------------------
// Page 4 (TOKEN): 算力能量站 (总览大卡 + 三端占比条 + IN/OUT/CACHE 进度条)
// ---------------------------------------------------------------------------
static void drawTokBarRow(int y, const char* label, int64_t val, int64_t maxVal, uint16_t col) {
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  g_canvas.setTextColor(col);
  g_canvas.setTextDatum(top_left);
  g_canvas.drawString(label, 8, y);

  char vBuf[12];
  fmtTok(val, vBuf, sizeof(vBuf));
  g_canvas.setTextColor(TFT_WHITE);
  g_canvas.setTextDatum(top_right);
  g_canvas.drawString(vBuf, SCR_W - 8, y);

  int pct = (val > 0 && maxVal > 0) ? (int)((val * 100LL) / maxVal) : 0;
  if (val > 0 && pct < 4) pct = 4;
  drawSegmentedBar(8, y + 11, SCR_W - 16, 7, pct, col);
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
  drawCyberCard(3, y, SCR_W - 6, 48, colSurface(), colBorder(), colAmber());

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
  drawCyberCard(3, y, SCR_W - 6, 50, colSurface(), colBorder(), colCodex());

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
  drawCyberCard(3, y, SCR_W - 6, 90, colSurface(), colBorder(), colPi());

  int64_t maxBar = max((int64_t)1, max(g_tokIn, max(g_tokOut, g_tokCache)));
  drawTokBarRow(y + 7,  "INPUT",  g_tokIn,    maxBar, colAmber());
  drawTokBarRow(y + 35, "OUTPUT", g_tokOut,   maxBar, colEmerald());
  drawTokBarRow(y + 63, "CACHE",  g_tokCache, maxBar, colCodex());

  drawPageDots(3);
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
    hitBoss(45, true);
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
  while (j < (int)cmd.length() && cmd[j] >= '0' && cmd[j] <= '9') {
    m = m * 10 + (cmd[j] - '0'); j++; mg = true;
  }
  if (!hg || !mg || h > 23 || m > 59) return;
  int newMin = h * 60 + m;
  g_baseMinOfDay = newMin;
  g_baseMillis   = millis();
  g_timeValid    = true;
  g_detailDirty  = true;
  if (newMin != g_lastNvsSavedMin) {
    g_lastNvsSavedMin = newMin;
    g_prefs.putInt("lastMin", newMin);
    Serial.printf("[time-ok] %02d:%02d min=%d (saved to NVS)\n", h, m, newMin);
  }
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
    hitBoss(14 + min(30, g_combo * 4), g_combo >= 6);
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

  // BtnB: 四页循环切换 MAIN -> DETAIL -> BOSS -> TOKEN -> MAIN
  if (M5.BtnB.wasPressed()) {
    Page prev = g_page;
    g_page = (Page)(((int)g_page + 1) % 4);
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
    if (tnow - lastDetailRefresh >= 250) {
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
      int maxShow = 4;
      int pages = (g_sessionCount + maxShow - 1) / maxShow;
      if (pages > 1) {
        g_detailScroll = (g_detailScroll + 1) % pages;
        g_detailDirty = true;
      }
    }
    break;
  }

  case Page::BOSS: {
    // 像素打怪竞技场: ~16 FPS 实时动画 + BtnA 斩击/扭蛋/充能
    static uint32_t lastBossFrame = 0;
    uint32_t tnow = millis();
    if (M5.BtnA.wasHold()) {
      g_lastSkillHighlight = 3;
      g_mood = min(100, g_mood + 25);
      addExp(20);
      g_bossHitMs = tnow;
      snprintf(g_battleLog, sizeof(g_battleLog), "HEAL! +25 MOOD +20XP");
      g_detailDirty = true;
    } else if (M5.BtnA.wasClicked()) {
      bool isDouble = (tnow - g_lastBtnAClickMs) <= 450;
      g_lastBtnAClickMs = tnow;
      if (isDouble) {
        g_lastSkillHighlight = 2;
        g_feverUntilMs = tnow + 180000UL;
        addExp(30);
        hitBoss(75, true);
        snprintf(g_battleLog, sizeof(g_battleLog), "ULTIMATE! 2xATK + CRIT!");
      } else {
        g_lastSkillHighlight = 1;
        addExp(5);
        hitBoss(18 + g_level * 2, false);
      }
      g_detailDirty = true;
    }
    if (g_detailDirty || (tnow - lastBossFrame >= 80)) {
      lastBossFrame = tnow;
      g_canvas.fillScreen(TFT_BLACK);
      drawStatusBarOverlay();
      drawBossRaidView();
      g_canvas.pushSprite(0, 0);
      g_detailDirty = false;
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

      // 1. 自适应基线动态倾斜滤波:
      //    无论设备在桌面斜靠、平放还是受线材拉扯倾斜, g_axBaseline 会在 ~1s 内自动适应当前静置姿态;
      //    只有主动拿起左右晃动/倾斜机身产生相对变化量 (ax - g_axBaseline) 时才会触发滑动!
      if (!g_axBaselineInit) {
        g_axBaseline     = ax;
        g_axBaselineInit = true;
      }
      g_axBaseline = g_axBaseline * 0.94f + ax * 0.06f;
      float deltaAx = constrain(-(ax - g_axBaseline), -0.85f, 0.85f);
      g_tiltSmooth = g_tiltSmooth * 0.78f + deltaAx * 0.22f;
      if (fabsf(g_tiltSmooth) < 0.18f) {
        g_tiltOffsetPx = 0;
      } else {
        g_tiltOffsetPx = constrain((int)(g_tiltSmooth * 22.0f), -14, 14);
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

      // 3. 摇一摇 -> D20 极客决策骰子 (Shake-to-Roll D20 Oracle)
      if (!isInteracting()) {
        float mag = sqrtf(ax * ax + ay * ay + az * az);
        if (mag > SHAKE_THRESHOLD) {
          g_shakeCount++;
          if (g_shakeCount >= SHAKE_FRAMES) {
            g_shakeCount = 0;
            g_shakeActive  = true;
            g_shakeStartMs = millis();
            int d20 = 1 + (esp_random() % 20);
            if (d20 == 20) {
              snprintf(g_customBubbleBuf, sizeof(g_customBubbleBuf), "D20=20 NAT CRIT!!");
              addExp(30);
              hitBoss(80, true);
              burstParticles(PT_STAR, colAmber(), 8);
            } else if (d20 >= 16) {
              snprintf(g_customBubbleBuf, sizeof(g_customBubbleBuf), "D20=%d SHIP IT!", d20);
              hitBoss(25, false);
            } else if (d20 >= 11) {
              snprintf(g_customBubbleBuf, sizeof(g_customBubbleBuf), "D20=%d LGTM ~", d20);
              hitBoss(15, false);
            } else if (d20 >= 6) {
              snprintf(g_customBubbleBuf, sizeof(g_customBubbleBuf), "D20=%d ADD TESTS", d20);
            } else if (d20 >= 2) {
              snprintf(g_customBubbleBuf, sizeof(g_customBubbleBuf), "D20=%d REFACTOR..", d20);
            } else {
              snprintf(g_customBubbleBuf, sizeof(g_customBubbleBuf), "D20=1 DONT PUSH!");
            }
            g_shakePhrase = g_customBubbleBuf;
            g_mood = min(100, g_mood + 5);
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

    // 整点静音健康提醒 (每逢整点 XX:00 触发饮水/伸展动画提醒)
    int curMin = currentMinOfDay();
    if (millis() > 15000 && (curMin % 60 == 0) && (curMin / 60 != g_lastChimeHour)) {
      g_lastChimeHour = curMin / 60;
      snprintf(g_customBubbleBuf, sizeof(g_customBubbleBuf), "%02d:00 HYDRATE! ~", g_lastChimeHour);
      g_pokeActive  = true;
      g_pokeStartMs = millis();
      g_pokePhrase  = g_customBubbleBuf;
      burstParticles(PT_STAR, colCodex(), 6);
      closeGif();
      size_t sz = watering_gif_end - watering_gif_start;
      if (g_frameBuf && g_gif.open((uint8_t*)watering_gif_start, (int)sz, gifDraw)) {
        g_gif.setDrawType(GIF_DRAW_COOKED);
        g_gif.setFrameBuf(g_frameBuf);
        g_gifOpen = true;
        g_frameDelayMs = 50;
        g_lastFrameMs  = 0;
        g_canvas.fillScreen(BG_COLOR);
      }
    }

    // BtnA 短按: 捕捉流星 / 双击赛博扭蛋机 (Cyber Gacha) / 单击戳一戳攻击 Boss
    if (M5.BtnA.wasClicked() && !g_shakeActive && !g_feedActive) {
      uint32_t clickNow = millis();
      bool isDouble = (clickNow - g_lastBtnAClickMs) <= 450;
      g_lastBtnAClickMs = clickNow;

      const uint8_t* chosenStart = nullptr;
      const uint8_t* chosenEnd   = nullptr;

      if (g_shootActive) {
        // 1. 成功捕捉夜空流星!
        g_shootActive = false;
        snprintf(g_customBubbleBuf, sizeof(g_customBubbleBuf), "STAR WISH! +40XP");
        g_pokePhrase = g_customBubbleBuf;
        addExp(40);
        hitBoss(65, true);
        burstParticles(PT_STAR, colAmber(), 8);
        chosenStart = cheer_gif_start;
        chosenEnd   = cheer_gif_end;
      } else if (isDouble) {
        // 2. 双击 BtnA -> 赛博幸运扭蛋机 (Cyber Gacha)
        int roll = esp_random() % 6;
        if (roll == 0) {
          snprintf(g_customBubbleBuf, sizeof(g_customBubbleBuf), "SSR! 0-BUG +40XP");
          g_feverUntilMs = clickNow + 180000UL;
          addExp(40);
          hitBoss(75, true);
          burstParticles(PT_STAR, colAmber(), 8);
          chosenStart = cheer_gif_start; chosenEnd = cheer_gif_end;
        } else if (roll == 1) {
          snprintf(g_customBubbleBuf, sizeof(g_customBubbleBuf), "777 JACKPOT! +50");
          addExp(50);
          hitBoss(110, true);
          burstParticles(PT_HEART, colCoral(), 8);
          chosenStart = dragon_boat_gif_start; chosenEnd = dragon_boat_gif_end;
        } else if (roll == 2) {
          snprintf(g_customBubbleBuf, sizeof(g_customBubbleBuf), "BUFF: 2xATK 3min");
          g_feverUntilMs = clickNow + 180000UL;
          hitBoss(45, true);
          burstParticles(PT_SPARK, colPi(), 7);
          chosenStart = singing_gif_start; chosenEnd = singing_gif_end;
        } else if (roll == 3) {
          snprintf(g_customBubbleBuf, sizeof(g_customBubbleBuf), "SR! 1-SHOT PASS~");
          addExp(25);
          hitBoss(40, false);
          chosenStart = gaming_gif_start; chosenEnd = gaming_gif_end;
        } else if (roll == 4) {
          snprintf(g_customBubbleBuf, sizeof(g_customBubbleBuf), "Gacha: Coffee ~");
          addExp(15);
          chosenStart = coffee_gif_start; chosenEnd = coffee_gif_end;
        } else {
          snprintf(g_customBubbleBuf, sizeof(g_customBubbleBuf), "Gacha: Game Time");
          addExp(15);
          chosenStart = gaming_gif_start; chosenEnd = gaming_gif_end;
        }
        g_pokePhrase = g_customBubbleBuf;
      } else {
        // 3. 普通单击戳一戳 + 攻击当前 Bug Boss
        addExp(3);
        hitBoss(18, false);
        burstParticles(PT_HEART, colAmber(), 4);
        g_pokePhrase = kPokePhrases[esp_random() % POKE_PHRASE_COUNT];
        const GifEntry& pe = kPokeGifs[esp_random() % POKE_GIF_COUNT];
        chosenStart = pe.start;
        chosenEnd   = pe.end;
      }

      g_mood = min(100, g_mood + 6);
      recordPulse(7);
      markDirty();
      g_pokeActive  = true;
      g_pokeStartMs = clickNow;
      closeGif();
      size_t sz = chosenEnd - chosenStart;
      if (g_frameBuf && g_gif.open((uint8_t*)chosenStart, (int)sz, gifDraw)) {
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
