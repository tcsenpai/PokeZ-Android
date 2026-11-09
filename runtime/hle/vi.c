// nn::vi (display/layer) and nv:: (graphics driver init), implemented on the host.
#include <stdlib.h>
#include <time.h>

#include "../hle.h"
#include "../kernel.h"

static uint32_t vsync_r, vsync_w;
static KEvent *vsync_event;
uint32_t vi_vsync_handle(void) { return vsync_r; }
volatile uint64_t g_vsync_count;  // vsyncs signalled so far (runtime/hle/framepace.c, runtime/nvn/pace.c)
volatile uint64_t g_vsync_ns;     // plat_now_ns() of the latest (stored after the count: count >= the vsync it's from)
volatile uint64_t g_vsync_period_ns = 16666667;  // 60 Hz
uint64_t g_native_window;
// The app is in the background (Android: another app in front, the home screen; set by the window's event watch,
// runtime/nvn/vulkan/device.c app_events): the game thread waits at its next present (nvn/objects.c) and the audio
// feeders stop (services/audio.c) until it's back - no frames for a surface that's gone, no CPU, no sound.
volatile bool g_app_background;
// LARECOMP_VSYNC_LOCK=<us>: the timer follows the display's own vsync (platform.h plat_display_vsync: Android's
// Choreographer), firing that many microseconds after it, its period an exact multiple of the display's - a free timer
// drifts against the panel by its clock's ppm, and a held 30 fps frame then lands on a different refresh now and then.
// Corrected by at most 0.25 ms a tick (no jumps). Unset: the free timer.
static void *vsync_thread(void *arg) {
    (void)arg;
    uint64_t period = 16666667;  // (re-read each vsync)
    plat_thread_realtime(period);
    plat_thread_perf_core(PLAT_PERF_VSYNC);
    const char *lk = getenv("LARECOMP_VSYNC_LOCK");
    int64_t lock_off = lk && atoi(lk) > 0 ? (int64_t)atoi(lk) * 1000 : -1;
    int locked_said = 0, ticks = 0;
    if (lock_off >= 0) LOG("[vi] vsync lock asked: %.2f ms after the display's vsync\n", lock_off / 1e6);
    uint64_t next = plat_now_ns();
    for (;;) {
        period = g_vsync_period_ns;
        uint64_t at, dp;
        if (lock_off >= 0 && plat_display_vsync(&at, &dp)) {
            uint64_t r = (period + dp / 2) / dp;
            int64_t miss = (int64_t)period - (int64_t)(r * dp);
            if (r >= 1 && (miss < 0 ? -miss : miss) < (int64_t)period / 50) {  // (2%: the Thor's 60 Hz is 59.65)
                period = r * dp;
                int64_t d = (int64_t)(next + period - at), ph = ((d % (int64_t)dp) + (int64_t)dp) % (int64_t)dp - lock_off;
                if (ph > (int64_t)dp / 2) ph -= (int64_t)dp;
                if (ph < -(int64_t)dp / 2) ph += (int64_t)dp;
                next -= ph > 250000 ? 250000 : ph < -250000 ? -250000 : ph;
                if (!locked_said++) LOG("[vi] vsync locked to the display's: %.3f ms (%llu refreshes), %.2f ms after it\n",
                                        period / 1e6, (unsigned long long)r, lock_off / 1e6);
            } else if (locked_said == 0 && ticks == 600) {
                LOG("[vi] vsync lock: the display's %.3f ms doesn't divide the timer's %.3f ms: free timer\n", dp / 1e6, period / 1e6);
            }
        } else if (lock_off >= 0 && ticks == 600) {
            LOG("[vi] vsync lock: no display vsync after 600 ticks: free timer\n");
        }
        ticks++;
        next += period;
        plat_sleep_until_ns(next);
        uint64_t now = plat_now_ns();
        if (now > next + period * 3) {
            // (~100 ms late every frame: macOS coalescing the timers of a thread that lost its real-time policy while
            // the app isn't frontmost - checked and set again here; the window's focus says whether it was frontmost)
            static int warned;
            int rt = plat_thread_realtime_check(period);
            extern bool ui_window_focused(void);
            if (warned++ < 40) LOG("[vi] vsync timer late by %llums (window focused %d, real-time policy %s)\n", (unsigned long long)((now - next) / 1000000),
                                    ui_window_focused(), rt == 0 ? "holds" : rt == 1 ? "was gone: set again" : "unknown");
            next = now;  // don't burst-signal to catch up
        }
        __atomic_add_fetch(&g_vsync_count, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&g_vsync_ns, now, __ATOMIC_RELEASE);
        pthread_mutex_lock(&g_klock);
        kernel_signal(vsync_event);
        pthread_mutex_unlock(&g_klock);
    }
    return NULL;
}

HLE_FUNC("_ZN2nn2vi10InitializeEv", vi_initialize) { (void)c; }
HLE_FUNC("_ZN2nn2vi8FinalizeEv", vi_finalize) { (void)c; }

HLE_FUNC("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_open_default_display) {
    *(uint64_t *)(uintptr_t)c->x[0] = (uint64_t)hle_alloc(0x100);
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", vi_close_display) { (void)c; }

HLE_FUNC("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_create_layer) {
    *(uint64_t *)(uintptr_t)c->x[0] = (uint64_t)hle_alloc(0x100);
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", vi_destroy_layer) { (void)c; }
HLE_FUNC("_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii", vi_set_layer_crop) { c->x[0] = 0; }

HLE_FUNC("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_get_native_window) {
    if (!g_native_window) g_native_window = (uint64_t)hle_alloc(0x100);
    *(uint64_t *)(uintptr_t)c->x[0] = g_native_window;
    c->x[0] = 0;
}

// Reboot: the vsync timer and its event live on (the kernel's handle table doesn't): new handles at the next boot.
RT_ON_RESET(vi_reset) {
    vsync_r = vsync_w = 0;
    g_native_window = 0;  // (HLE heap)
}

HLE_FUNC("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_get_vsync_event) {
    if (!vsync_event) {
        vsync_event = kernel_create_event_pair(&vsync_w, &vsync_r);
        pthread_t t;
        pthread_create(&t, NULL, vsync_thread, NULL);
    } else if (!vsync_r) {
        vsync_w = kernel_new_handle(vsync_event);
        vsync_r = kernel_new_handle(vsync_event);
    }
    // nn::os::AttachReadableHandleToSystemEvent(event, handle, managed=false, clear_mode=AutoClear)
    c->x[1] = vsync_r;
    c->x[2] = 0;
    c->x[3] = 1;
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nv18InitializeGraphicsEPvm", nv_initialize_graphics) { (void)c; }
HLE_FUNC("_ZN2nv16FinalizeGraphicsEv", nv_finalize_graphics) { (void)c; }
HLE_FUNC("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_set_allocator) { (void)c; }
HLE_FUNC("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_set_dt_allocator) { (void)c; }
