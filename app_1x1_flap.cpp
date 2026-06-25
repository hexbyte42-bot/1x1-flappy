/*
 * app_1x1_flap.cpp — "1×1 Flap"
 *
 *   Multiplication-table trainer with a flappy-style reward minigame.
 *   Question (e.g. "7 × 8 = ?") in the top half, 3×4 keypad in the bottom
 *   half. Tap digits, ⌫ to backspace, ✓ to submit. After 10 correct in a
 *   row → flappy reward (PWR or screen tap = flap). 6 pipes = win.
 *
 * Setup keys (/setup/setup.txt):
 *   MATH_TABLES = "{2,3,5,10}"   — restrict the 1×1 to these tables.
 *                                  Default = all of 1..10.
 *
 * Persistence (NVS namespace "1x1flap"):
 *   bestPipes  — high score (most pipes passed in one game)
 *   bestStreak — longest correct-answer streak
 *   total      — all-time total correct answers
 */

#include "app_1x1_flap.h"
#include "app_common.h"
#include <Arduino.h>
#include <Wire.h>
#include <SD_MMC.h>
#include <FS.h>
#include <Preferences.h>
#include <math.h>
#include "canvas/Arduino_Canvas.h"
#include "pin_config.h"
#include "HWCDC.h"
#include "TouchDrvFT6X36.hpp"

extern USBCDC USBSerial;
extern Arduino_Canvas *g_canvas;

#define BOOT_BTN     0
#define PWR_POLL_MS  50

// ── Globals ──────────────────────────────────────────────────────────────────
static Arduino_Canvas *canvas = nullptr;
static TouchDrvFT6X36  s_touch;
static Preferences     s_prefs;

// MATH_TABLES — bool[1..10], true = include this table.
static bool   s_table[11];          // index 1..10 used; 0 unused.
static int    s_tableCount = 10;

// Persistent stats
static uint16_t s_bestPipes  = 0;
static uint16_t s_bestStreak = 0;
static uint32_t s_total      = 0;
static uint16_t s_winsTotal  = 0;   // total wins across the lifetime; level = (winsTotal % 10) + 1

// ── App state machine ────────────────────────────────────────────────────────
enum Mode {
    MODE_QUIZ,
    MODE_FLAP_INTRO,    // "Get ready!" 1.5 s
    MODE_FLAP,
    MODE_FLAP_PAUSE,    // PWR pressed mid-flight — paused overlay
    MODE_FLAP_WIN,      // GOAL banner + confetti, 3 s, then back to quiz
    MODE_FLAP_LOSE,     // "Good try!" screen, 2.5 s, then back to quiz
    MODE_FLAP_FINALE,   // big level-10 celebration, ~5 s
};
static Mode s_mode = MODE_QUIZ;
static uint32_t s_modeStart = 0;

// ── Quiz state ───────────────────────────────────────────────────────────────
static int     s_qa = 0, s_qb = 0;          // current question a × b
static int     s_qExpected = 0;             // a*b
static char    s_answer[4]  = {0,0,0,0};    // up to 3 digits + nul
static int     s_answerLen  = 0;
static int     s_streak     = 0;            // correct-in-a-row this session
static uint8_t s_flash      = 0;            // 0 = none, 1 = green/correct, 2 = red/wrong
static uint32_t s_flashEnd  = 0;
#define STREAK_FOR_GAME 10

// Touch latch — only register the start of a press, not held finger.
static bool    s_touchHeld   = false;

// ── Flappy state ─────────────────────────────────────────────────────────────
struct Pipe {
    float    x;          // left edge
    int16_t  gapY;       // top of gap
    int16_t  gapH;       // gap height (px)
    bool     scored;     // already counted
    bool     isFinish;   // checker-flag finish line — no collision, single trigger
};
static int s_normalSpawned = 0;   // how many normal pipes have been laid down
static bool s_finishSpawned = false;
static const int PIPE_MAX = 4;
static Pipe   s_pipes[PIPE_MAX];

static float  s_birdY = 0, s_birdV = 0;
static int    s_birdAnim = 0;
static int    s_pipesPassed = 0;
static int    s_cloudsX[3] = {0, 0, 0};

// ── Enemies (exactly 4 per run, themed) ──────────────────────────────────────
// Each slot spawns exactly once per level, triggered by a pipe-pass milestone.
// Pipe-based triggers (vs. ms timers) keep the four enemies spread across the
// whole level independent of framerate — you naturally see one between each
// pair of pipes instead of all bunched at the front.
struct Enemy {
    bool     active;
    bool     spawned;       // has this slot fired yet this run?
    float    x;
    int16_t  baseY;
    float    bobPhase;
    uint8_t  spawnAfterPipe;  // spawn when s_pipesPassed reaches this value
};
static const int ENEMY_MAX = 4;
static Enemy s_enemies[ENEMY_MAX];
static const int ENEMY_R = 10;          // hitbox radius — small to keep wins easy
static const float ENEMY_VX     = 2.6f; // px/frame — a hair faster than pipes
static const float ENEMY_BOB_DV = 0.10f;
static const float ENEMY_BOB_AMP = 32.0f;
static const int PIPES_TO_WIN = 6;
static const float GRAVITY     = 0.45f;     // px/frame²
static const float FLAP_V      = -7.0f;     // px/frame (negative = up)
static const float MAX_FALL_V  = 9.0f;
static const float PIPE_SPEED  = 2.4f;      // px/frame  (~50 px/s at 20 fps; closer to 144 px/s at 60 fps)
static const int   PIPE_W      = 56;
static const int   PIPE_GAP_H  = 180;       // 20% more room than before — kid-friendly
static const float PIPE_SPACING= 220.0f;    // px between pipe-spawns

static bool     s_pwrFlap   = false;
static bool     s_bootWas   = false;

// ── Quiz colours ─────────────────────────────────────────────────────────────
#define C_QUIZ_BG     0x0000   // pure black for AMOLED contrast
#define C_QUIZ_QTXT   0xFFFF
#define C_QUIZ_ANSWER 0xFFE0   // yellow
#define C_PROGRESS_ON 0x07E0   // green
#define C_PROGRESS_OFF 0x4208
#define C_FLASH_OK    0x07E0
#define C_FLASH_BAD   0xF800
#define C_KEY_BG      0x39E7
#define C_KEY_BG_ALT  0x4A69
#define C_KEY_TXT     0xFFFF
#define C_KEY_BACK_BG 0xC8E4   // soft red for ⌫
#define C_KEY_OK_BG   0x2DE7   // pleasant green for ✓
#define C_BIRD        0xFFE0   // bright yellow
#define C_BIRD_BEAK   0xFC60   // orange
#define C_BIRD_EYE    0x0000
#define C_TEXT_DARK   0x18C3
#define C_BANNER      0xFB60   // orange banner

// ── Theme system: 10 looks that cycle by reward-game level (wins) ────────────
// Each win bumps the level; at level 11 we wrap back to level 1.
struct Theme {
    const char *name;
    uint16_t bgTop, bgBot;     // sky gradient
    uint16_t pipe, pipeEdge;   // pipe colours
    uint16_t cloudCol;         // decoration colour
    uint16_t groundCol;        // ground strip
    uint16_t groundLine;       // line under bottom of pipes
    bool     rainbowPipes;     // each pipe a different colour
    char     deco;             // 'C'=clouds 'S'=stars 'R'=rain 'F'=snow 'N'=neon
                               // 'B'=bubbles 'A'=mini rainbow arcs
    char     enemyType;        // 'B'=bee 'V'=bat 'L'=lollipop 'G'=ghost
                               // 'T'=thundercloud 'U'=umbrella 'S'=snowflake
                               // 'P'=peppermint 'F'=fish 'O'=UFO
};

static const Theme THEMES[10] = {
    // 1 — normal day                                                                          deco enemy
    {"Day",       0x65BF, 0xCEFF, 0x2667, 0x1B25, 0xFFFF, 0xCB85, 0xA483, false, 'C', 'B'},
    // 2 — sundown
    {"Sunset",    0xFB80, 0xF853, 0x6BA0, 0x4080, 0xFE19, 0x68A0, 0x4000, false, 'C', 'V'},
    // 3 — rainbow (mini rainbow arcs in the sky)
    {"Rainbow",   0xAEFF, 0xFFFF, 0x07E0, 0x03C0, 0xFFFF, 0xCB85, 0xA483, true,  'A', 'L'},
    // 4 — night
    {"Night",     0x0001, 0x10A0, 0x2126, 0x10A2, 0xFFFF, 0x0841, 0x0820, false, 'S', 'G'},
    // 5 — dark clouds
    {"Storm",     0x6B4D, 0x528A, 0x3186, 0x18C3, 0xC638, 0x3186, 0x18C3, false, 'C', 'T'},
    // 6 — rain
    {"Rain",      0x4A69, 0x528A, 0x2667, 0x1B25, 0x6D7F, 0x4208, 0x18C3, false, 'R', 'U'},
    // 7 — snow
    {"Snow",      0xCEFF, 0xFFFF, 0x528A, 0x3186, 0xFFFF, 0xCEFF, 0xA51F, false, 'F', 'S'},
    // 8 — candy
    {"Candy",     0xFB7F, 0xFCFF, 0xFBBF, 0xC81F, 0xFFFF, 0xFEDD, 0xC81F, false, 'C', 'P'},
    // 9 — ocean (air bubbles)
    {"Ocean",     0x041F, 0x4A1F, 0xFC60, 0xC2A0, 0xFFFF, 0xCB85, 0x6BA0, false, 'B', 'F'},
    // 10 — space
    {"Space",     0x0000, 0x10A0, 0xF81F, 0x801F, 0x07FF, 0x18C3, 0x0000, true,  'N', 'O'},
};

// Current theme — set on entering reward mode.
static int  s_level = 1;          // 1..10, cycles
static const Theme *s_theme = &THEMES[0];

// Rainbow palette for rainbow-pipe themes
static const uint16_t RAINBOW_PIPES[6] = {
    0xF800, 0xFB80, 0xFFE0, 0x07E0, 0x041F, 0x801F
};

// ── Forward decls ────────────────────────────────────────────────────────────
static void enterMode(Mode m);
static void newQuestion();
static void drawQuiz();
static void drawFlap();
static void drawFlapIntro();
static void drawFlapEnd(bool win);
static void drawFlapPause();
static void drawFlapFinale();
static void resetFlap();
static void tickFlap(bool flapNow);
static void seedTablesFromSetupTxt();

// ── Helpers ──────────────────────────────────────────────────────────────────
static inline uint16_t lerp565(uint16_t a, uint16_t b, float t) {
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int r = ar + (int)((br - ar) * t);
    int g = ag + (int)((bg - ag) * t);
    int bl= ab + (int)((bb - ab) * t);
    return ((r & 0x1F) << 11) | ((g & 0x3F) << 5) | (bl & 0x1F);
}

// Vertical pill-shaped indicator anchored to a hardware button: solid colour,
// rounded ends, sits flush to the right edge so it never overlaps the keypad.
// Replaces the earlier text pills — the user just wants a coloured bar.
static void drawButtonBar(int16_t btnY, uint16_t fillCol) {
    const int barW = 10;
    const int barH = 84;
    const int x    = LCD_WIDTH - barW - 4;     // x = 354
    const int y    = btnY - barH / 2;
    canvas->fillRoundRect(x, y, barW, barH, barW / 2, fillCol);
}

// Hero text follows the project pixelated style: setTextSize(sx, sy, margin)
// with a thin gap between glyph cells. Convention defined in feedback memory.
//   size 4 → (4,5,1), char width 25
//   size 5 → (5,6,1), char width 31
//   size 6 → (6,7,2), char width 38
//   size 7 → (7,8,2), char width 44
//   smaller stays plain.
static int charWidthFor(uint8_t sz) {
    switch (sz) {
        case 4: return 25;
        case 5: return 31;
        case 6: return 38;
        case 7: return 44;
        default: return 6 * sz;
    }
}
static int charHeightFor(uint8_t sz) {
    switch (sz) {
        case 4: return 5 * 8;
        case 5: return 6 * 8;
        case 6: return 7 * 8;
        case 7: return 8 * 8;
        default: return 8 * sz;
    }
}
static void setHeroSize(uint8_t sz) {
    switch (sz) {
        case 4: canvas->setTextSize(4, 5, 1); return;
        case 5: canvas->setTextSize(5, 6, 1); return;
        case 6: canvas->setTextSize(6, 7, 2); return;
        case 7: canvas->setTextSize(7, 8, 2); return;
        default: canvas->setTextSize(sz);     return;
    }
}

static void drawTextCentered(int16_t cy, const char *s, uint16_t col, uint8_t sz) {
    int16_t cw = charWidthFor(sz);
    int16_t ch = charHeightFor(sz);
    int16_t w  = (int16_t)strlen(s) * cw;
    int16_t x  = (LCD_WIDTH - w) / 2;
    int16_t y  = cy - ch / 2;
    canvas->setTextColor(col);
    setHeroSize(sz);
    canvas->setCursor(x, y);
    canvas->print(s);
}

static void drawRoundedRect(int16_t x, int16_t y, int16_t w, int16_t h,
                            int16_t r, uint16_t fill) {
    canvas->fillRoundRect(x, y, w, h, r, fill);
}

// ── Keypad geometry ──────────────────────────────────────────────────────────
#define KEY_X0     8
#define KEY_Y0     170
#define KEY_W      104
#define KEY_H      62
#define KEY_GAPX   10
#define KEY_GAPY   8

// Keypad codes — only digit keys remain. BOOT acts as OK (submit), PWR as
// backspace; the touch ⌫/✓ keys were removed at the user's request.
enum KeyCode { K_NONE = -1 };

static KeyCode keyAt(int row, int col) {
    // Layout:
    //   1 2 3
    //   4 5 6
    //   7 8 9
    //     0       (only the centre cell of row 4 is active)
    if (row < 0 || row > 3 || col < 0 || col > 2) return K_NONE;
    if (row == 3) {
        if (col == 1) return (KeyCode)0;
        return K_NONE;
    }
    return (KeyCode)(row * 3 + col + 1);
}

static KeyCode hitKey(int16_t tx, int16_t ty) {
    if (ty < KEY_Y0) return K_NONE;
    int row = (ty - KEY_Y0) / (KEY_H + KEY_GAPY);
    int col = (tx - KEY_X0) / (KEY_W + KEY_GAPX);
    if (row < 0 || row > 3 || col < 0 || col > 2) return K_NONE;
    int cy = KEY_Y0 + row * (KEY_H + KEY_GAPY);
    int cx = KEY_X0 + col * (KEY_W + KEY_GAPX);
    if (tx < cx || tx > cx + KEY_W) return K_NONE;
    if (ty < cy || ty > cy + KEY_H) return K_NONE;
    return keyAt(row, col);
}

// ── Persistence ──────────────────────────────────────────────────────────────
// NOTE: s_winsTotal is intentionally NOT persisted. The user wants every
// power-on / reset to start back at level 1.
static void prefsLoad() {
    s_prefs.begin("1x1flap", true);
    s_bestPipes  = s_prefs.getUShort("bestPipes",  0);
    s_bestStreak = s_prefs.getUShort("bestStreak", 0);
    s_total      = s_prefs.getULong ("total",      0UL);
    s_prefs.end();
    s_winsTotal = 0;
}
static void prefsSaveStreak() {
    s_prefs.begin("1x1flap", false);
    s_prefs.putULong("total", s_total);
    if (s_streak > s_bestStreak) {
        s_bestStreak = s_streak;
        s_prefs.putUShort("bestStreak", s_bestStreak);
    }
    s_prefs.end();
}
static void prefsSaveBestPipes() {
    if ((uint16_t)s_pipesPassed > s_bestPipes) {
        s_bestPipes = (uint16_t)s_pipesPassed;
        s_prefs.begin("1x1flap", false);
        s_prefs.putUShort("bestPipes", s_bestPipes);
        s_prefs.end();
    }
}
static void prefsSaveWin() {
    // RAM only — wins reset on power-cycle.
    s_winsTotal++;
}

// ── MATH_TABLES parsing ──────────────────────────────────────────────────────
static void seedTablesAllOn() {
    for (int i = 0; i <= 10; i++) s_table[i] = false;
    for (int i = 1; i <= 10; i++) s_table[i] = true;
    s_tableCount = 10;
}

static void parseTablesStr(const char *s) {
    for (int i = 0; i <= 10; i++) s_table[i] = false;
    s_tableCount = 0;
    while (*s) {
        while (*s && (*s < '0' || *s > '9')) s++;
        if (!*s) break;
        int v = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
        if (v >= 1 && v <= 10 && !s_table[v]) {
            s_table[v] = true;
            s_tableCount++;
        }
    }
    if (s_tableCount == 0) seedTablesAllOn();
}

static void seedTablesFromSetupTxt() {
    seedTablesAllOn();
    if (!SD_MMC.begin("/sdcard", true)) return;
    File f = SD_MMC.open("/setup/setup.txt");
    if (f) {
        char line[160];
        while (f.available()) {
            int len = f.readBytesUntil('\n', line, sizeof(line) - 1);
            line[len] = '\0';
            const char *p = strstr(line, "MATH_TABLES");
            if (p) {
                p += strlen("MATH_TABLES");
                while (*p == ' ' || *p == '=') p++;
                parseTablesStr(p);
                break;
            }
        }
        f.close();
    }
    SD_MMC.end();
}

// ── Question generation ──────────────────────────────────────────────────────
static int pickActiveTable() {
    int picks[10], n = 0;
    for (int i = 1; i <= 10; i++) if (s_table[i]) picks[n++] = i;
    if (n == 0) return 1;
    return picks[esp_random() % n];
}

static void newQuestion() {
    s_qa = pickActiveTable();
    s_qb = 1 + (int)(esp_random() % 10);
    s_qExpected = s_qa * s_qb;
    s_answerLen = 0;
    s_answer[0] = 0;
}

// ── Quiz drawing ─────────────────────────────────────────────────────────────
static void drawProgressBar(int16_t y, int correct) {
    int n = STREAK_FOR_GAME;
    int dotR = 8;
    int gap  = 6;
    int totalW = n * (dotR * 2) + (n - 1) * gap;
    int x = (LCD_WIDTH - totalW) / 2 + dotR;
    for (int i = 0; i < n; i++) {
        uint16_t col = (i < correct) ? C_PROGRESS_ON : C_PROGRESS_OFF;
        canvas->fillCircle(x, y + dotR, dotR, col);
        x += dotR * 2 + gap;
    }
}

static void drawKey(int row, int col, const char *label, uint16_t bg, uint16_t fg) {
    int x = KEY_X0 + col * (KEY_W + KEY_GAPX);
    int y = KEY_Y0 + row * (KEY_H + KEY_GAPY);
    drawRoundedRect(x, y, KEY_W, KEY_H, 12, bg);
    int sz = 4;
    int cw = charWidthFor(sz);
    int ch = charHeightFor(sz);
    int w  = (int)strlen(label) * cw;
    canvas->setTextColor(fg);
    setHeroSize(sz);
    canvas->setCursor(x + (KEY_W - w) / 2, y + (KEY_H - ch) / 2);
    canvas->print(label);
}

static void drawQuiz() {
    canvas->fillScreen(C_QUIZ_BG);

    // Flash overlay
    if (s_flash != 0 && millis() < s_flashEnd) {
        uint16_t col = (s_flash == 1) ? C_FLASH_OK : C_FLASH_BAD;
        canvas->fillScreen(col);
    } else {
        s_flash = 0;
    }

    // Top: LEVEL N header (which round will trigger when streak hits 10).
    int curLevel = (s_winsTotal % 10) + 1;
    char lbuf[16];
    snprintf(lbuf, sizeof(lbuf), "LEVEL %d", curLevel);
    canvas->setTextColor(0x9CD3);
    canvas->setTextSize(2, 2, 1);
    int lblW = (int)strlen(lbuf) * (2 * 6 + 1);
    canvas->setCursor((LCD_WIDTH - lblW) / 2, 6);
    canvas->print(lbuf);

    // Progress dots, just below
    drawProgressBar(36, s_streak);

    // Combined equation: "a x b = answer" in a single hero line.
    char qbuf[32];
    const char *ans = (s_answerLen == 0) ? "_" : s_answer;
    snprintf(qbuf, sizeof(qbuf), "%dx%d=%s", s_qa, s_qb, ans);
    drawTextCentered(115, qbuf, C_QUIZ_ANSWER, 6);

    // Keypad — 3×3 of digits + a single 0 in row 4 col 1. Bigger cells now.
    const char *labels[12] = {
        "1","2","3",
        "4","5","6",
        "7","8","9",
        "" ,"0",""
    };
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++) {
            int idx = r * 3 + c;
            if (labels[idx][0] == '\0') continue;
            uint16_t bg = ((r + c) & 1) ? C_KEY_BG_ALT : C_KEY_BG;
            drawKey(r, c, labels[idx], bg, C_KEY_TXT);
        }
    }

    // Hardware-button indicators: green bar at BOOT (= OK / submit),
    // red bar at PWR (= backspace). Vertical capsule shapes flush to the
    // right edge of the screen, never overlapping the keypad cells.
    drawButtonBar(BOOT_BTN_Y_P, C_KEY_OK_BG);
    drawButtonBar(PWR_BTN_Y_P,  C_KEY_BACK_BG);

    canvas->flush();
}

// ── Quiz logic ───────────────────────────────────────────────────────────────
static void onDigit(int d) {
    if (s_answerLen >= 3) return;
    s_answer[s_answerLen++] = '0' + d;
    s_answer[s_answerLen]   = 0;
    drawQuiz();
}
static void onBackspace() {
    if (s_answerLen == 0) return;
    s_answer[--s_answerLen] = 0;
    drawQuiz();
}
static void onSubmit() {
    if (s_answerLen == 0) return;
    int v = atoi(s_answer);
    if (v == s_qExpected) {
        s_streak++;
        s_total++;
        s_flash    = 1;
        s_flashEnd = millis() + 320;
        prefsSaveStreak();
        if (s_streak >= STREAK_FOR_GAME) {
            // start the reward game
            enterMode(MODE_FLAP_INTRO);
            return;
        }
        newQuestion();
        drawQuiz();
    } else {
        // Soft penalty: subtract one from the streak (floor at 0), keep the
        // same question so the kid can try again without restarting.
        if (s_streak > 0) s_streak--;
        s_flash    = 2;
        s_flashEnd = millis() + 480;
        s_answerLen = 0; s_answer[0] = 0;
        drawQuiz();
    }
}

// ── Flappy-mode logic ────────────────────────────────────────────────────────
static void resetFlap() {
    s_birdY  = LCD_HEIGHT * 0.4f;
    s_birdV  = 0;
    s_birdAnim = 0;
    s_pipesPassed = 0;
    s_normalSpawned = 0;
    s_finishSpawned = false;

    // Constrained gap-Y so the bigger 180-px gap still fits between the
    // top corner zone (~50) and the ground (LCD_HEIGHT - 28).
    auto pickGapY = []() -> int16_t {
        int range = LCD_HEIGHT - 28 - PIPE_GAP_H - 50;
        if (range < 1) range = 1;
        return (int16_t)(50 + (esp_random() % range));
    };
    for (int i = 0; i < PIPE_MAX; i++) {
        s_pipes[i].x        = LCD_WIDTH + i * PIPE_SPACING + 80;
        s_pipes[i].gapY     = pickGapY();
        s_pipes[i].gapH     = PIPE_GAP_H;
        s_pipes[i].scored   = false;
        s_pipes[i].isFinish = false;
        s_normalSpawned++;
    }
    s_cloudsX[0] = 60;  s_cloudsX[1] = 200; s_cloudsX[2] = 300;

    // Spawn 4 enemies, each triggered when the bird passes a specific pipe.
    // 6 pipes per level → one enemy in the gaps after pipe 1, 2, 3, 4. Each gets
    // its own random y and bob phase so they don't bob in lockstep.
    static const uint8_t spawnPipe[ENEMY_MAX] = {1, 2, 3, 4};
    int yMin = 100, yMax = LCD_HEIGHT - 140;
    for (int i = 0; i < ENEMY_MAX; i++) {
        s_enemies[i].active         = false;
        s_enemies[i].spawned        = false;
        s_enemies[i].x              = 0;
        s_enemies[i].baseY          = yMin + (int)(esp_random() % (uint32_t)(yMax - yMin));
        s_enemies[i].bobPhase       = (float)(esp_random() % 628) / 100.0f;
        s_enemies[i].spawnAfterPipe = spawnPipe[i];
    }

    // Pick the theme for this run.
    s_level = (s_winsTotal % 10) + 1;
    s_theme = &THEMES[s_level - 1];

    // Defensive: clear any latched input state so a stuck BOOT or pending
    // PWR-IRQ from the quiz can't fire phantom flaps in the first frame.
    s_pwrFlap   = false;
    s_bootWas   = false;
    s_touchHeld = false;
    common_drain_pwr();
}

static void tickFlap(bool flapNow) {
    if (flapNow) s_birdV = FLAP_V;
    s_birdV += GRAVITY;
    if (s_birdV > MAX_FALL_V) s_birdV = MAX_FALL_V;
    s_birdY += s_birdV;
    s_birdAnim++;

    // Move pipes
    for (int i = 0; i < PIPE_MAX; i++) {
        s_pipes[i].x -= PIPE_SPEED;
        if (!s_pipes[i].scored && s_pipes[i].x + PIPE_W < 80) {
            s_pipes[i].scored = true;
            s_pipesPassed++;
            if (s_pipesPassed >= PIPES_TO_WIN) {
                prefsSaveBestPipes();
                prefsSaveWin();
                enterMode(MODE_FLAP_WIN);
                return;
            }
        }
        if (s_pipes[i].x + PIPE_W < -8) {
            // Recycle. Normal pipes until we've placed 5; then one finish
            // line; then nothing — the slot stays parked far off-screen so it
            // doesn't keep cycling fake obstacles after the level ends.
            float maxX = 0;
            for (int j = 0; j < PIPE_MAX; j++) if (s_pipes[j].x > maxX) maxX = s_pipes[j].x;
            if (s_normalSpawned < 5) {
                int range = LCD_HEIGHT - 28 - PIPE_GAP_H - 50;
                if (range < 1) range = 1;
                s_pipes[i].x        = maxX + PIPE_SPACING;
                s_pipes[i].gapY     = (int16_t)(50 + (esp_random() % range));
                s_pipes[i].gapH     = PIPE_GAP_H;
                s_pipes[i].scored   = false;
                s_pipes[i].isFinish = false;
                s_normalSpawned++;
            } else if (!s_finishSpawned) {
                s_pipes[i].x        = maxX + PIPE_SPACING;
                s_pipes[i].gapY     = 0;
                s_pipes[i].gapH     = LCD_HEIGHT - 28;   // full clear gap
                s_pipes[i].scored   = false;
                s_pipes[i].isFinish = true;
                s_finishSpawned     = true;
            } else {
                // Park well off-screen — bird already passed the finish line
                // (or will any frame now). No more obstacles.
                s_pipes[i].x = LCD_WIDTH * 4;
            }
        }
    }

    // Clouds
    for (int i = 0; i < 3; i++) {
        s_cloudsX[i] -= 1;
        if (s_cloudsX[i] < -60) s_cloudsX[i] = LCD_WIDTH + 30;
    }

    // Enemies — staggered spawns. Each slot wakes at its spawnAt offset and
    // scrolls left, bobbing on a sine wave. Speed is slightly above the pipe
    // speed so the leftward motion reads clearly even with all the bobbing.
    for (int i = 0; i < ENEMY_MAX; i++) {
        Enemy &e = s_enemies[i];
        // One-shot spawn: each slot fires exactly once per level, never re-spawns.
        // Trigger is bird-passes-pipe so distribution doesn't depend on framerate.
        if (!e.spawned && s_pipesPassed >= e.spawnAfterPipe) {
            e.spawned = true;
            e.active  = true;
            e.x       = LCD_WIDTH + 30;
        }
        if (!e.active) continue;
        e.x        -= ENEMY_VX;
        e.bobPhase += ENEMY_BOB_DV;
        if (e.x < -40) e.active = false;
    }

    // Clamp at the ceiling — too-high should not lose, just stop rising.
    if (s_birdY < 16) { s_birdY = 16; if (s_birdV < 0) s_birdV = 0; }

    // Collision: bird at x≈80, radius 16
    int bx = 80;
    int by = (int)s_birdY;
    int br = 16;
    // Floor only — game over if bird drops to the ground.
    // Note: prefsSaveWin() advances winsTotal even on a fail so the user
    // progresses through the levels by playing, win or lose — EXCEPT on
    // level 10, where they have to actually beat it to trigger the finale.
    // Failing level 10 keeps them replaying level 10.
    if (by + br > LCD_HEIGHT - 28) {
        prefsSaveBestPipes();
        if (s_level < 10) prefsSaveWin();
        enterMode(MODE_FLAP_LOSE);
        return;
    }
    // Pipes — finish line has no collision (bird flies straight through)
    for (int i = 0; i < PIPE_MAX; i++) {
        Pipe &p = s_pipes[i];
        if (p.isFinish) continue;
        if (bx + br < p.x) continue;
        if (bx - br > p.x + PIPE_W) continue;
        // in horizontal range — must be in gap
        if (by - br < p.gapY)             { prefsSaveBestPipes(); if (s_level < 10) prefsSaveWin(); enterMode(MODE_FLAP_LOSE); return; }
        if (by + br > p.gapY + p.gapH)    { prefsSaveBestPipes(); if (s_level < 10) prefsSaveWin(); enterMode(MODE_FLAP_LOSE); return; }
    }
    // Enemies — small hitboxes, forgiving.
    for (int i = 0; i < ENEMY_MAX; i++) {
        Enemy &e = s_enemies[i];
        if (!e.active) continue;
        int ex = (int)e.x;
        int ey = e.baseY + (int)(sinf(e.bobPhase) * ENEMY_BOB_AMP);
        int dx = ex - bx;
        int dy = ey - by;
        int rsum = ENEMY_R + br;
        if (dx * dx + dy * dy < rsum * rsum) {
            prefsSaveBestPipes();
            if (s_level < 10) prefsSaveWin();
            enterMode(MODE_FLAP_LOSE);
            return;
        }
    }
}

// ── Flappy drawing ───────────────────────────────────────────────────────────
static void drawSky() {
    // Vertical gradient
    const int strips = 20;
    int sh = LCD_HEIGHT / strips;
    for (int i = 0; i < strips; i++) {
        float t = (float)i / (strips - 1);
        uint16_t c = lerp565(s_theme->bgTop, s_theme->bgBot, t);
        canvas->fillRect(0, i * sh, LCD_WIDTH, sh + 1, c);
    }
    // Ground strip
    canvas->fillRect(0, LCD_HEIGHT - 28, LCD_WIDTH, 28, s_theme->groundCol);
    canvas->fillRect(0, LCD_HEIGHT - 30, LCD_WIDTH, 2, s_theme->groundLine);
}

static void drawDeco() {
    char d = s_theme->deco;
    uint16_t col = s_theme->cloudCol;
    for (int i = 0; i < 3; i++) {
        int cx = s_cloudsX[i];
        int cy = 36 + i * 50;
        if (d == 'C') {
            canvas->fillCircle(cx,      cy,     16, col);
            canvas->fillCircle(cx + 18, cy - 6, 14, col);
            canvas->fillCircle(cx + 30, cy + 4, 16, col);
            canvas->fillCircle(cx + 12, cy + 8, 14, col);
        } else if (d == 'S') {
            // Stars — 5 dots scattered
            canvas->fillCircle(cx,      cy,      2, col);
            canvas->fillCircle(cx + 14, cy - 8,  2, col);
            canvas->fillCircle(cx + 28, cy + 4,  2, col);
            canvas->fillCircle(cx + 8,  cy + 12, 2, col);
            canvas->fillCircle(cx + 24, cy - 4,  3, col);
        } else if (d == 'R') {
            // Rain streaks
            for (int j = 0; j < 5; j++) {
                int rx = cx + j * 8;
                canvas->drawLine(rx, cy - 6, rx + 2, cy + 8, col);
            }
        } else if (d == 'F') {
            // Snowflakes — small plus-shapes
            for (int j = 0; j < 4; j++) {
                int sx = cx + j * 12;
                int sy = cy + ((j & 1) ? 6 : -4);
                canvas->fillCircle(sx, sy, 2, col);
                canvas->drawFastHLine(sx - 4, sy, 9, col);
                canvas->drawFastVLine(sx, sy - 4, 9, col);
            }
        } else if (d == 'N') {
            // Neon dots — varied colors
            uint16_t neons[4] = {0xF81F, 0x07FF, 0xFFE0, 0x07E0};
            for (int j = 0; j < 4; j++) {
                canvas->fillCircle(cx + j * 10, cy + ((j & 1) ? -4 : 4), 3, neons[j]);
            }
        } else if (d == 'B') {
            // Air bubbles — hollow circles, with a tiny highlight on each.
            int rs[5] = {10, 6, 4, 7, 5};
            int dx[5] = { 0, 18,  6, 22, -4};
            int dy[5] = { 0,-12, 16,  4, 10};
            for (int j = 0; j < 5; j++) {
                int bx = cx + dx[j], by = cy + dy[j];
                canvas->drawCircle(bx, by, rs[j],     col);
                canvas->drawCircle(bx, by, rs[j] - 1, col);
                canvas->drawPixel (bx - rs[j]/3, by - rs[j]/3, 0xFFFF);
            }
        } else if (d == 'A') {
            // Mini rainbow arc — 7 thin bands forming an arc above the slot.
            static const uint16_t rcols[7] = {
                0xF800, 0xFB80, 0xFFE0, 0x07E0, 0x041F, 0x801F, 0xF81F,
            };
            int acx = cx + 14, acy = cy + 30;     // center below the cloud slot
            for (int j = 0; j < 7; j++) {
                int r = 28 - j * 3;
                if (r < 6) continue;
                canvas->drawCircle(acx, acy, r,     rcols[j]);
                canvas->drawCircle(acx, acy, r - 1, rcols[j]);
            }
        }
    }
}

static void drawFinishLine(int16_t x) {
    // Two vertical posts (left + right edges) with checker flag on top, and
    // a thin checker stripe across the full play-field height in between.
    const int CHECK = 14;
    const int W     = 36;
    int xL = x;
    int xR = x + W - 4;
    // Posts
    canvas->fillRect(xL, 0, 4, LCD_HEIGHT - 28, 0xFFFF);
    canvas->fillRect(xR, 0, 4, LCD_HEIGHT - 28, 0xFFFF);
    // Faint translucent strip — alternating black/white squares that only
    // colour every other row, so the bird is still clearly visible behind.
    int playH = LCD_HEIGHT - 28;
    int rows  = playH / CHECK;
    for (int r = 0; r < rows; r++) {
        int y = r * CHECK;
        int cols = W / CHECK;
        for (int c = 0; c < cols; c++) {
            int cx = x + c * CHECK + 4;
            uint16_t col = ((r + c) & 1) ? 0xFFFF : 0x0000;
            canvas->fillRect(cx, y, CHECK, CHECK, col);
        }
    }
    // Top "flag" — 6 rows of checker, bold
    for (int r = 0; r < 6; r++) {
        int y = r * CHECK;
        int cols = W / CHECK;
        for (int c = 0; c < cols; c++) {
            int cx = x + c * CHECK + 4;
            uint16_t col = ((r + c) & 1) ? 0xFFFF : 0x0000;
            canvas->fillRect(cx, y, CHECK, CHECK, col);
        }
    }
    // Label
    canvas->setTextColor(0xFFE0);
    canvas->setTextSize(2, 2, 1);
    canvas->setCursor(x - 6, playH / 2 - 8);
    canvas->print("GOAL");
}

static void drawPipes() {
    for (int i = 0; i < PIPE_MAX; i++) {
        Pipe &p = s_pipes[i];
        if (p.x > LCD_WIDTH || p.x + PIPE_W < 0) continue;
        int x = (int)p.x;
        if (p.isFinish) {
            drawFinishLine(x);
            continue;
        }
        uint16_t pipeCol  = s_theme->pipe;
        uint16_t pipeEdge = s_theme->pipeEdge;
        if (s_theme->rainbowPipes) {
            pipeCol = RAINBOW_PIPES[i % 6];
            pipeEdge = lerp565(pipeCol, 0x0000, 0.45f);
        }
        // Top pipe
        canvas->fillRect(x, 0, PIPE_W, p.gapY, pipeCol);
        canvas->fillRect(x - 4, p.gapY - 16, PIPE_W + 8, 16, pipeCol);
        canvas->drawRect(x, 0, PIPE_W, p.gapY, pipeEdge);
        canvas->drawRect(x - 4, p.gapY - 16, PIPE_W + 8, 16, pipeEdge);
        // Bottom pipe
        int by = p.gapY + p.gapH;
        canvas->fillRect(x, by, PIPE_W, LCD_HEIGHT - 28 - by, pipeCol);
        canvas->fillRect(x - 4, by, PIPE_W + 8, 16, pipeCol);
        canvas->drawRect(x, by, PIPE_W, LCD_HEIGHT - 28 - by, pipeEdge);
        canvas->drawRect(x - 4, by, PIPE_W + 8, 16, pipeEdge);
    }
}

static void drawBird() {
    int bx = 80;
    int by = (int)s_birdY;
    // Wing flap based on velocity
    int wingDy = ((s_birdAnim / 4) & 1) ? -3 : 3;
    if (s_birdV < -2) wingDy = -5;
    canvas->fillCircle(bx, by, 16, C_BIRD);
    canvas->drawCircle(bx, by, 16, 0xA460);
    // Wing
    canvas->fillCircle(bx - 6, by + wingDy, 8, 0xFC60);
    // Eye
    canvas->fillCircle(bx + 6, by - 5, 4, 0xFFFF);  // eye highlight stays white
    canvas->fillCircle(bx + 7, by - 5, 2, C_BIRD_EYE);
    // Beak
    canvas->fillTriangle(bx + 14, by - 2, bx + 14, by + 6, bx + 24, by + 2, C_BIRD_BEAK);
}

static void drawOneEnemy(int ex, int ey, int wf) {
    char t = s_theme->enemyType;
    if (t == 'B') {                       // bee
        canvas->fillCircle(ex, ey, 10, 0xFFE0);
        canvas->fillRect(ex - 9, ey - 3, 18, 2, 0x0000);
        canvas->fillRect(ex - 9, ey + 1, 18, 2, 0x0000);
        canvas->fillCircle(ex - 4, wf ? ey - 7 : ey - 5, 4, 0xCEFF);
        canvas->fillCircle(ex + 4, wf ? ey - 7 : ey - 5, 4, 0xCEFF);
    } else if (t == 'V') {                // bat
        canvas->fillTriangle(ex - 16, ey - 4, ex - 2, ey - 2, ex - 2, ey + 4, 0x18C3);
        canvas->fillTriangle(ex + 16, ey - 4, ex + 2, ey - 2, ex + 2, ey + 4, 0x18C3);
        canvas->fillCircle(ex, ey, 7, 0x18C3);
        canvas->drawPixel(ex - 2, ey - 2, 0xF800);
        canvas->drawPixel(ex + 2, ey - 2, 0xF800);
    } else if (t == 'L') {                // lollipop
        canvas->fillCircle(ex, ey, 11, 0xF81F);
        canvas->fillCircle(ex, ey,  8, 0xFFFF);
        canvas->fillCircle(ex, ey,  5, 0xF81F);
        canvas->drawCircle(ex, ey, 11, 0xC81F);
        canvas->fillRect(ex - 1, ey + 10, 2, 12, 0xFFFF);
    } else if (t == 'G') {                // ghost
        canvas->fillCircle(ex, ey - 2, 10, 0xFFFF);
        canvas->fillRect(ex - 10, ey - 2, 21, 11, 0xFFFF);
        for (int j = 0; j < 4; j++) {
            int gx = ex - 8 + j * 5;
            canvas->fillCircle(gx, ey + 9, 3, 0xFFFF);
        }
        canvas->fillCircle(ex - 4, ey - 2, 2, 0x0000);
        canvas->fillCircle(ex + 4, ey - 2, 2, 0x0000);
    } else if (t == 'T') {                // thundercloud
        canvas->fillCircle(ex - 8, ey - 2, 7, 0x4208);
        canvas->fillCircle(ex + 8, ey - 2, 7, 0x4208);
        canvas->fillCircle(ex,     ey - 6, 8, 0x4208);
        canvas->fillTriangle(ex - 2, ey + 4, ex + 5, ey + 4, ex, ey + 14, 0xFFE0);
    } else if (t == 'U') {                // umbrella
        canvas->fillCircle(ex, ey + 2, 12, 0xF800);
        canvas->fillRect(ex - 13, ey + 3, 27, 12, s_theme->bgBot);
        canvas->drawFastVLine(ex, ey + 2, 16, 0x4208);
        canvas->drawPixel(ex - 1, ey + 17, 0x4208);
        canvas->drawPixel(ex - 2, ey + 17, 0x4208);
    } else if (t == 'S') {                // snowflake
        canvas->drawFastHLine(ex - 11, ey, 23, 0xFFFF);
        canvas->drawFastVLine(ex, ey - 11, 23, 0xFFFF);
        canvas->drawLine(ex - 8, ey - 8, ex + 8, ey + 8, 0xFFFF);
        canvas->drawLine(ex - 8, ey + 8, ex + 8, ey - 8, 0xFFFF);
        canvas->fillCircle(ex, ey, 3, 0xFFFF);
    } else if (t == 'P') {                // peppermint
        canvas->fillCircle(ex, ey, 11, 0xFFFF);
        canvas->fillRect(ex - 11, ey - 2, 23, 4, 0xF800);
        canvas->drawCircle(ex, ey, 11, 0xF800);
    } else if (t == 'F') {                // fish
        canvas->fillCircle(ex, ey, 10, 0xFC60);
        canvas->fillTriangle(ex + 8, ey, ex + 18, ey - 7, ex + 18, ey + 7, 0xFC60);
        canvas->fillCircle(ex - 4, ey - 2, 2, 0xFFFF);
        canvas->fillCircle(ex - 4, ey - 2, 1, 0x0000);
    } else if (t == 'O') {                // UFO
        canvas->fillRoundRect(ex - 14, ey, 28, 7, 3, 0x7BEF);
        canvas->fillCircle(ex, ey - 2, 8, 0xAEFF);
        canvas->drawCircle(ex, ey - 2, 8, 0x7BEF);
        uint16_t lights[4] = {0xF800, 0xFFE0, 0x07E0, 0x07FF};
        for (int j = 0; j < 4; j++) {
            canvas->fillCircle(ex - 9 + j * 6, ey + 4, 1 + ((s_birdAnim/4 + j) & 1), lights[j]);
        }
    }
}

static void drawEnemy() {
    int wf = (s_birdAnim / 5) & 1;
    for (int i = 0; i < ENEMY_MAX; i++) {
        Enemy &e = s_enemies[i];
        if (!e.active) continue;
        int ex = (int)e.x;
        int ey = e.baseY + (int)(sinf(e.bobPhase) * ENEMY_BOB_AMP);
        drawOneEnemy(ex, ey, wf);
    }
}

static void drawHud() {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d / %d", s_pipesPassed, PIPES_TO_WIN);
    int sz = 3;
    int w = (int)strlen(buf) * 6 * sz;
    drawRoundedRect(LCD_WIDTH - w - 20, 8, w + 12, 8 * sz + 8, 6, 0x18C3);
    canvas->setTextColor(C_QUIZ_QTXT);
    canvas->setTextSize(sz);
    canvas->setCursor(LCD_WIDTH - w - 14, 12);
    canvas->print(buf);
}

static void drawFlap() {
    drawSky();
    drawDeco();
    drawPipes();
    drawEnemy();
    drawBird();
    drawHud();
    canvas->flush();
}

static void drawFlapIntro() {
    drawSky();
    drawDeco();
    drawBird();
    // Banner
    int by = LCD_HEIGHT / 2 - 38;
    drawRoundedRect(LCD_WIDTH/2 - 140, by, 280, 110, 14, C_BANNER);
    drawTextCentered(by + 24, "Level", C_TEXT_DARK, 4);
    char lbuf[16];
    snprintf(lbuf, sizeof(lbuf), "%d  %s", s_level, s_theme->name);
    drawTextCentered(by + 60, lbuf, C_TEXT_DARK, 3);
    drawTextCentered(by + 92, "tap = flap", C_TEXT_DARK, 2);
    canvas->flush();
}

// ── Confetti ─────────────────────────────────────────────────────────────────
struct Confetti { int16_t x, y; uint16_t col; };
static const int CONFETTI_N = 36;
static Confetti s_confetti[CONFETTI_N];
static const uint16_t CONFETTI_COLS[6] = {
    0xF800, 0xFFE0, 0x07E0, 0x07FF, 0x801F, 0xF81F,
};

static void confetti_init() {
    for (int i = 0; i < CONFETTI_N; i++) {
        s_confetti[i].x   = esp_random() % LCD_WIDTH;
        s_confetti[i].y   = -(int16_t)(esp_random() % 240);
        s_confetti[i].col = CONFETTI_COLS[i % 6];
    }
}
static void confetti_tick() {
    for (int i = 0; i < CONFETTI_N; i++) {
        s_confetti[i].y += 5;
        if (s_confetti[i].y > LCD_HEIGHT + 6) {
            s_confetti[i].x = esp_random() % LCD_WIDTH;
            s_confetti[i].y = -8;
        }
    }
}
static void confetti_draw() {
    for (int i = 0; i < CONFETTI_N; i++) {
        canvas->fillCircle(s_confetti[i].x, s_confetti[i].y, 3, s_confetti[i].col);
    }
}

static void drawFlapEnd(bool win) {
    drawSky();
    drawDeco();
    drawBird();
    drawPipes();
    if (win) {
        confetti_draw();   // sprinkle on the existing celebratory frame
    }
    int by = LCD_HEIGHT / 2 - 60;
    drawRoundedRect(LCD_WIDTH/2 - 150, by, 300, 130, 14,
                    win ? C_PROGRESS_ON : C_BANNER);
    drawTextCentered(by + 28, win ? "GOAL!" : "Good try!", C_TEXT_DARK, 5);
    char buf[32];
    snprintf(buf, sizeof(buf), "level %d done", s_level);
    if (!win) snprintf(buf, sizeof(buf), "%d pipes", s_pipesPassed);
    drawTextCentered(by + 72, buf, C_TEXT_DARK, 3);
    snprintf(buf, sizeof(buf), "best: %d", (int)s_bestPipes);
    drawTextCentered(by + 104, buf, C_TEXT_DARK, 2);
    canvas->flush();
}

// ── Pause overlay ─────────────────────────────────────────────────────────────
static void drawFlapPause() {
    // Re-render the live game scene as a backdrop, then drop a card on top.
    drawSky();
    drawDeco();
    drawPipes();
    drawBird();
    int by = LCD_HEIGHT / 2 - 50;
    drawRoundedRect(LCD_WIDTH/2 - 130, by, 260, 100, 14, 0xFFE0);
    drawTextCentered(by + 26, "PAUSED",       C_TEXT_DARK, 5);
    drawTextCentered(by + 70, "press to play", C_TEXT_DARK, 2);
    canvas->flush();
}

// ── Fireworks ────────────────────────────────────────────────────────────────
// 6 anchored explosion sites, each offset in phase so something is always
// blooming somewhere. Each cycle goes from a bright dense burst out to wide
// fading dots over ~900 ms, then the site goes quiet until its turn comes
// around in the 1500 ms global cycle.
static void drawFireworks(uint32_t t) {
    static const struct {
        int16_t cx, cy;
        uint16_t col;
        uint16_t phase;     // ms offset within the global period
    } FW[6] = {
        { 70,  110, 0xF800,    0},
        {210,   80, 0xFFE0,  240},
        {300,  170, 0x07FF,  480},
        {150,  230, 0xF81F,  720},
        {275,  300, 0x07E0,  960},
        { 80,  340, 0xFFE0, 1200},
    };
    const uint32_t period = 1500;
    for (int i = 0; i < 6; i++) {
        uint32_t local = (t + FW[i].phase) % period;
        if (local > 900) continue;
        int radius = (int)(local * 56 / 900);              // 0 → 56 px outward
        int dotR   = 5 - (int)(local * 5 / 900);            // 5 → 0 px size
        if (dotR < 1) continue;
        // 14 sparks in a circle
        for (int j = 0; j < 14; j++) {
            float a  = j * 6.2831853f / 14.0f;
            int   dx = (int)(radius * cosf(a));
            int   dy = (int)(radius * sinf(a));
            canvas->fillCircle(FW[i].cx + dx, FW[i].cy + dy, dotR, FW[i].col);
        }
        // White centre flash for the first 200 ms of the burst
        if (local < 200) {
            canvas->fillCircle(FW[i].cx, FW[i].cy, 6 - (int)(local * 6 / 200), 0xFFFF);
        }
    }
}

// ── Level-10 finale (~15 s party) ─────────────────────────────────────────────
static void drawFlapFinale() {
    confetti_tick();
    canvas->fillScreen(0x0000);
    drawFireworks(millis() - s_modeStart);
    confetti_draw();
    // Rainbow stripes behind the banner
    static const uint16_t stripeCol[7] = {
        0xF800, 0xFB80, 0xFFE0, 0x07E0, 0x041F, 0x801F, 0xF81F,
    };
    int sH = 18;
    int sy0 = LCD_HEIGHT / 2 - 70;
    for (int i = 0; i < 7; i++) {
        canvas->fillRect(0, sy0 + i * sH, LCD_WIDTH, sH, stripeCol[i]);
    }
    // Big banner — text rotates every 3 s through 5 phrases.
    int by = LCD_HEIGHT / 2 - 60;
    drawRoundedRect(LCD_WIDTH/2 - 165, by, 330, 140, 18, 0xFFFF);
    static const char *titles[5] = {
        "AMAZING!", "WOOHOO!", "MEGA!", "TOP STAR", "ALL DONE!",
    };
    static const char *subs[5] = {
        "All 10 done!", "10 levels!", "you finished", "every level", "what a player",
    };
    int phase = (int)((millis() - s_modeStart) / 3000) % 5;
    drawTextCentered(by + 28,  titles[phase], C_TEXT_DARK, 5);
    drawTextCentered(by + 76,  subs[phase],   C_TEXT_DARK, 3);
    drawTextCentered(by + 110, "you rock",    C_TEXT_DARK, 2);
    canvas->flush();
}

// ── Mode transitions ─────────────────────────────────────────────────────────
static void enterMode(Mode m) {
    s_mode = m;
    s_modeStart = millis();
    if (m == MODE_QUIZ) {
        s_streak = 0;
        s_flash = 0;
        newQuestion();
        drawQuiz();
    } else if (m == MODE_FLAP_INTRO) {
        resetFlap();
        drawFlapIntro();
    } else if (m == MODE_FLAP) {
        drawFlap();
    } else if (m == MODE_FLAP_WIN) {
        confetti_init();
        drawFlapEnd(true);
    } else if (m == MODE_FLAP_LOSE) {
        drawFlapEnd(false);
    } else if (m == MODE_FLAP_PAUSE) {
        drawFlapPause();
    } else if (m == MODE_FLAP_FINALE) {
        confetti_init();
        drawFlapFinale();
    }
}

// ── Touch helpers ────────────────────────────────────────────────────────────
// The FT3168 / FT6X36 touch IC has an interrupt line on GPIO TP_INT that goes
// LOW only when something is being touched. Polling I²C every frame even
// when nothing is happening can stall the loop (the touch IC seems to drowse
// and answers I²C reads slowly), causing the BOOT-only freeze the user
// observed. Gate the I²C call on TP_INT — INT high = no touch, skip I²C.
static bool readTap(int16_t &tx, int16_t &ty) {
    if (digitalRead(TP_INT) == HIGH) {
        // No touch event — release any held latch and bail without I²C.
        s_touchHeld = false;
        return false;
    }
    bool present = s_touch.getPoint(&tx, &ty, 1);
    if (present && !s_touchHeld) {
        s_touchHeld = true;
        return true;
    }
    if (!present) s_touchHeld = false;
    return false;
}

// ── Public API ───────────────────────────────────────────────────────────────
void app_1x1_flap_setup(Arduino_OLED *gfx) {
    (void)gfx;
    canvas = g_canvas;
    pinMode(BOOT_BTN, INPUT_PULLUP);
    pinMode(TP_INT,   INPUT_PULLUP);
    s_touch.begin(Wire, 0x38, IIC_SDA, IIC_SCL);
    seedTablesFromSetupTxt();
    prefsLoad();
    newQuestion();
    enterMode(MODE_QUIZ);
}

void app_1x1_flap_loop() {
    common_tick();

    // Drain a stale flash
    if (s_flash != 0 && millis() >= s_flashEnd) { s_flash = 0; if (s_mode == MODE_QUIZ) drawQuiz(); }

    // PWR press: in QUIZ does nothing, in FLAP triggers flap
    bool pwrShort = common_consume_pwr_short();
    if (pwrShort) s_pwrFlap = true;

    // BOOT press (rising edge): cancel/skip end screens, flap
    bool bootDown = (digitalRead(BOOT_BTN) == LOW);
    bool bootEdge = (bootDown && !s_bootWas);
    s_bootWas = bootDown;

    int16_t tx, ty;
    bool tap = readTap(tx, ty);

    switch (s_mode) {
    case MODE_QUIZ: {
        // Touch on a digit cell → input that digit.
        if (tap) {
            KeyCode k = hitKey(tx, ty);
            if (k >= (KeyCode)0 && k <= (KeyCode)9) onDigit((int)k);
        }
        // BOOT = OK (submit). PWR = backspace.
        if (bootEdge) onSubmit();
        if (pwrShort) onBackspace();
        s_pwrFlap = false;   // consumed; nothing to do in quiz with the latch
        break;
    }
    case MODE_FLAP_INTRO: {
        if (millis() - s_modeStart > 1500) {
            enterMode(MODE_FLAP);
        }
        break;
    }
    case MODE_FLAP: {
        // PWR = pause. BOOT/touch = flap.
        if (pwrShort) {
            s_pwrFlap = false;
            enterMode(MODE_FLAP_PAUSE);
            break;
        }
        bool flapNow = bootEdge || tap;
        s_pwrFlap = false;
        tickFlap(flapNow);
        if (s_mode == MODE_FLAP) drawFlap();   // tickFlap may have transitioned
        break;
    }
    case MODE_FLAP_PAUSE: {
        // Any input resumes.
        if (pwrShort || bootEdge || tap) {
            enterMode(MODE_FLAP);
        }
        break;
    }
    case MODE_FLAP_WIN: {
        // Cap at 3 s; touch / BOOT advances. PWR (if it slips through) too.
        bool done = (millis() - s_modeStart > 3000) || tap || bootEdge || pwrShort;
        if (done) {
            // After level 10 win, jump to the FINALE before returning to quiz.
            if (s_level >= 10) enterMode(MODE_FLAP_FINALE);
            else               enterMode(MODE_QUIZ);
        }
        break;
    }
    case MODE_FLAP_LOSE: {
        bool done = (millis() - s_modeStart > 2500) || tap || bootEdge || pwrShort;
        if (done) enterMode(MODE_QUIZ);
        break;
    }
    case MODE_FLAP_FINALE: {
        // 15 s party. Any input still skips it for impatient kids.
        bool done = (millis() - s_modeStart > 15000) || tap || bootEdge || pwrShort;
        if (done) {
            // Wrap back to level 1 — winsTotal already advanced past 10.
            enterMode(MODE_QUIZ);
        } else {
            // Re-render every loop so confetti animates.
            drawFlapFinale();
        }
        break;
    }
    }

    delay(16);   // ~60 fps cap; matches the physics tuning
}
