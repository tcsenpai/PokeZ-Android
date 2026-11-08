// Frame pacing: the game's speed doesn't depend on the frame rate.
// The game advances its world each frame by its frame period (16.67 ms) times the swap interval its frame timer
// (f_700000c910) reads from the system object - normally 1. When a frame takes longer than a vsync (GPU-bound at
// 32:9, a hitch), the next frame starts a vsync later but still advanced only 16.67 ms: slow motion (30 fps = half
// speed). tools/patches.py FRAME_STEPS_SITE routes that read here: we return how many vsyncs really passed since
// the last frame, so the world advances by the time that passed. The game's own waiting is unaffected (it reads
// the interval nowhere else). Shared by the macOS and Windows builds.
// FINDING (2026-10-09): forcing 2 steps did NOT change Link's walking speed - gameplay is a true fixed step per
// frame; this timer feeds something else. Kept for research, off by default. (2026-10-18: the read is the game's
// dynamic resolution's frame budget - tools/patches.py GAME_DYNRES_SITE, now off, so it no longer runs at all.)
//   LARECOMP_FRAMEPACE=1     hand the timer the vsyncs elapsed (default: the game's own value)
//   LARECOMP_FRAME_STEPS_MAX maximum steps per frame (default 4: after a long stall the game doesn't jump ahead)
//   LARECOMP_FORCE_STEPS=<n> (test) always n
#include <stdlib.h>

#include "../hle.h"

extern volatile uint64_t g_vsync_count;  // runtime/hle/vi.c
uint64_t g_frame_steps_late;             // (stats) frames that advanced more than one step

uint32_t rt_frame_steps(uint32_t interval) {
    static int on = -1, max_steps, forced;
    if (on < 0) {
        const char *e = ENV("LARECOMP_FRAMEPACE");
        on = e ? atoi(e) != 0 : 0;  // off by default: the world (Link's movement) doesn't use this step (tested)
        e = ENV("LARECOMP_FRAME_STEPS_MAX");
        max_steps = e ? atoi(e) : 4;
        if (max_steps < 1) max_steps = 1;
        e = ENV("LARECOMP_FORCE_STEPS");
        forced = e ? atoi(e) : 0;
    }
    static int ncall;
    if (ENV("LARECOMP_LOG_FRAMEPACE") && ncall++ % 120 == 0) LOG("[framepace] call %d interval %u vsync %llu\n", ncall, interval, (unsigned long long)g_vsync_count);
    if (forced > 0) return (uint32_t)forced;
    static uint64_t last;
    uint64_t now = __atomic_load_n(&g_vsync_count, __ATOMIC_RELAXED);
    uint64_t steps = last ? now - last : 1;
    last = now;
    if (!on) return interval;
    if (steps < interval) steps = interval;  // (the game's own 30 fps mode: it waits that many vsyncs anyway)
    if (steps < 1) steps = 1;
    if (steps > (uint64_t)max_steps) steps = (uint64_t)max_steps;
    if (steps > 1) g_frame_steps_late++;
    return (uint32_t)steps;
}

// Pokémon Z-A's frame mode (tools/patches.py ZA_BUILDS): its configured vsyncs per frame - 1 (1: 30 fps). With
// the fps60 setting (or LARECOMP_FPS=60) 0: it presents every vsync and steps its world 1/60 s a frame. A frame
// that takes longer still steps 1/60 s: below 60 fps the whole game moves in slow motion (a player's log: 35 fps
// = 58% speed; the CPU-side game code couldn't keep up, an RTX 4060 waiting half the time). So the setting backs off
// by itself: presented below FPS60_MIN for FPS60_SLOW_S seconds in a row (after FPS60_WARM_S to settle - shaders
// building right after the switch), the game goes back to 30 fps for the rest of the session and the overlay says so
// (menu_notice). Choosing 60 again in F1 tries again. LARECOMP_FPS=30: as is; LARECOMP_FPS=60 or F1's "60 fps (always)"
// (fps60_lock): 60, no backing off - for players who'd rather have some slow motion than 30 fps.
// Seconds spent building shaders count neither way (#22: an RX 9070 XT with a cold pipeline cache had three 1-2 s
// shader stalls in a battle and went back to 30 for the session, holding 60 otherwise).
// Back up again (dynamic 30 / 60): held at 30, 60 is tried again - with a shorter test (FPS60_RETRY_WARM_S +
// FPS60_RETRY_SLOW_S: a failed try is about that much slight slow motion) - when the CPU has room (nvn/pace.c
// pace_frame_costs: the game's or the render thread's CPU time per frame under FPS60_BACK_MS) and the load may have
// changed: a scene change or any other hitch settled (pace_hitch: a load, the menu, a resolution step), or
// FPS60_RETRY_S seconds held. Not by the GPU's time at 30: a phone's GPU clocks down to fill the longer frame (a Galaxy
// S25+ read 24-27 ms a frame at 30 at 85% and at 70% resolution alike), so it says nothing of 60. A try that fails
// within FPS60_FLICKER_S doubles the wait, up to 8x (a phone right at the edge, cooling at 30 and warming at 60: no
// 60 / 30 flicker); a minute at 60 resets it. The overlay says each change. LARECOMP_FPS60_DYNAMIC=0: back to 30 for
// the rest of the session, as before.
#include "../settings.h"
#include "../platform.h"
#include "../nvn/pace.h"
#define FPS60_MIN 50
#define FPS60_SLOW_S 5
#define FPS60_WARM_S 5
#define FPS60_BACK_MS 14.0
#define FPS60_RETRY_S 20
#define FPS60_RETRY_WARM_S 1
#define FPS60_RETRY_SLOW_S 2
#define FPS60_FLICKER_S 20
bool g_za_frame_hook;    // (the game is Pokémon Z-A: exec.c leaves its GPU timing counters alone)
bool g_fps60_fell_back;  // (60 fps chosen, but the game is at 30 now: runtime/ui/menu.mm says so)
static bool fps60_holds(void) {
    extern uint64_t renderer_present_count(void);
    extern void menu_notice(const char *text, int seconds);
    static int last_setting = -1, warm, slow, held_s, backoff = 1, dyn = -1;
    static bool retry;  // (60 again after a fall back: the shorter test)
    static uint64_t t0, p0, back_at, seen_hitch;
    if (dyn < 0) dyn = !ENV("LARECOMP_FPS60_DYNAMIC") || atoi(ENV("LARECOMP_FPS60_DYNAMIC")) != 0;
    int setting = g_settings.fps60 | g_settings.fps60_lock << 1 | g_settings.fps45 << 2;  // (the 45 mode: 60's state fresh)
    if (last_setting != setting) {  // (chosen again in F1: a new try)
        last_setting = setting;
        g_fps60_fell_back = retry = false, warm = slow = held_s = 0, backoff = 1, t0 = back_at = 0;
    }
    if (!g_settings.fps60) return false;
    if (g_settings.fps60_lock) return true;
    uint64_t now = plat_now_ns(), pc = renderer_present_count();
    if (!t0) { t0 = now, p0 = pc; return !g_fps60_fell_back; }
    if (now - t0 < 1000000000ull) return !g_fps60_fell_back;
    double fps = (double)(pc - p0) * 1e9 / (double)(now - t0);
    t0 = now, p0 = pc;
    if (g_fps60_fell_back) {  // (held at 30, once a second: is there room for 60 again?)
        double cpu, gpu;
        pace_frame_costs(&cpu, &gpu);
        uint64_t h = pace_last_hitch_ns();
        bool scene = h && h != seen_hitch && now - h > 2000000000ull;  // (a load, the menu, a resolution step: settled)
        if (scene) seen_hitch = h;
        held_s++;
        if (held_s % 10 == 0) LOG("[pace] at 30: %.1f ms CPU, GPU %.1f ms a frame\n", cpu, gpu);
        static int said_hot;
        if (plat_thermal_throttled()) {  // (while the thermal governor holds the render scale down, a try only heats
            if (!said_hot) {             // the phone more and keeps it there: retries wait for the scale to recover;
                LOG("[pace] holding 30: the phone is throttling (render scale down) - no 60 fps tries until it recovers\n");
                said_hot = 1;
            }
            return false;
        }
        said_hot = 0;
        bool timer = held_s >= FPS60_RETRY_S * backoff;
        if (!dyn || cpu < 0 || cpu >= FPS60_BACK_MS || !(scene || timer)) return false;
        LOG("[pace] trying 60 fps again (%s; %.1f ms CPU a frame)%s\n", scene ? "the scene changed" : "a while at 30", cpu,
            backoff > 1 ? " (backing off)" : "");
        menu_notice(UI_TR("Trying 60 fps again.", "Tentando 60 fps de novo."), 3);
        g_fps60_fell_back = false, retry = true, warm = slow = held_s = 0, back_at = now;
        return true;
    }
    if (back_at && now - back_at > 60000000000ull) back_at = 0, backoff = 1, retry = false;  // (a minute at 60: as new)
    if (warm < (retry ? FPS60_RETRY_WARM_S : FPS60_WARM_S)) { warm++; return true; }
    static double min = -1;  // LARECOMP_FPS60_MIN: another floor (tests: 59 makes a busy scene back off)
    if (min < 0) min = ENV("LARECOMP_FPS60_MIN") ? atof(ENV("LARECOMP_FPS60_MIN")) : FPS60_MIN;
    extern uint64_t renderer_shader_builds(void);  // (the renderer: builds done, top bit = one being built now)
    static uint64_t b0;
    uint64_t b = renderer_shader_builds();
    bool building = (b >> 63) || (b & ~(1ull << 63)) - (b0 & ~(1ull << 63)) >= 2;  // (one now, or a few this second)
    b0 = b;
    if (!building) slow = fps < min ? slow + 1 : 0;
    if (slow < (retry ? FPS60_RETRY_SLOW_S : FPS60_SLOW_S)) return true;
    g_fps60_fell_back = true, held_s = 0, seen_hitch = pace_last_hitch_ns();
    if (back_at && now - back_at < FPS60_FLICKER_S * 1000000000ull && backoff < 8) backoff *= 2;
    LOG("[pace] 60 fps not held (%.0f fps for %d s): back to 30 fps - below 60 the game moves in slow motion%s\n", fps,
        slow, dyn ? " (60 again when there is room)" : "");
    if (dyn) menu_notice(UI_TR("30 fps for now: 60 wasn't held, and below that the game moves in slow motion. Back to 60 "
                               "by itself when there is room.",
                               "30 fps por enquanto: não segurou 60, e abaixo disso o jogo fica em câmera lenta. Volta para "
                               "60 sozinho quando houver folga."), 5);
    else menu_notice(UI_TR("60 fps turned off: this PC didn't keep 60 fps, and below that the game moves in slow motion. "
                           "Back to 30 fps (choose 60 again in F1 to retry).",
                           "60 fps desligado: não segurou 60, e abaixo disso o jogo fica em câmera lenta. De volta a 30 "
                           "fps (escolha 60 de novo no menu para tentar outra vez)."), 8);
    return false;
}
int32_t rt_za_frame_cfg(int32_t cfg) {
    g_za_frame_hook = true;
    const char *e = ENV("LARECOMP_FPS");
    // The 45 fps mode (the user's fps45): the game's own 30 fps machinery - two world steps and a present interval of
    // 2, forced by pace.c's hold - over a 90 Hz vsync: each frame is 2/90 s of world and shown 22.2 ms apart, the same
    // speed and smoothness as a real 45 fps. A frame the GPU misses (over 22.2 ms) still steps 2: the world slows a
    // little instead of the 60 fps mode's cliff - and nothing retries, so it never oscillates. LARECOMP_FPS wins (tests).
    // LARECOMP_FPS45_HZ=<hz>: that vsync instead of 90 - 80 makes it a 40 fps mode, each frame 25 ms = exactly three
    // refreshes of a 120 Hz panel (the AYN Thor's top mode; 45 on it would alternate 2 and 3 refreshes).
    extern volatile uint64_t g_vsync_period_ns;  // runtime/hle/vi.c
    bool f45 = !e && g_settings.fps45;    static bool was45;
    if (f45 != was45) {
        was45 = f45;
        const char *hz = ENV("LARECOMP_FPS45_HZ");
        double v = hz && atof(hz) >= 40 && atof(hz) <= 240 ? atof(hz) : 90;
        g_vsync_period_ns = f45 ? (uint64_t)(1e9 / v) : 16666667;
        LOG("[pace] 45 fps mode %s: a %.0f Hz vsync, two world steps and a present every two\n", f45 ? "on" : "off", v);
    }
    extern bool g_fps45_hold;  // runtime/nvn/pace.c hold_30: forced on while in the mode
    g_fps45_hold = f45;
    bool on = e ? atoi(e) >= 60 : f45 ? false : fps60_holds();
    static int logged = -1;
    if (logged != (int)on) { logged = on; LOG("[pace] game frame mode %d -> %d (%s)\n", cfg, on ? 0 : cfg, on ? "60 fps" : "as is"); }
    {   // the game's own time, in vsyncs: each frame it steps max(mode + 1, the vsyncs that really passed) - a replay
        // keyed by this lands its inputs on the same moment of the game on a faster or slower PC (runtime/hle/input.c)
        extern uint64_t g_game_vsyncs;
        static uint64_t last;
        uint64_t now = __atomic_load_n(&g_vsync_count, __ATOMIC_RELAXED), passed = last ? now - last : 0;
        uint64_t step = (uint64_t)(on ? 0 : cfg) + 1;
        if (passed > step) step = passed > 8 ? 8 : passed;  // (a long stall: the game caps its step too)
        last = now;
        g_game_vsyncs += step;
    }
    return on ? 0 : cfg;
}
uint64_t g_game_vsyncs;  // (see rt_za_frame_cfg)
bool g_fps45_hold;  // (see rt_za_frame_cfg: the 45 fps mode forces pace.c's hold - only once the game's frame hook has
                    // run: pace.c reaches here on frames before the game manager exists, and its pointer isn't mapped yet)
