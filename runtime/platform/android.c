// Android host layer (runtime/platform.h). arm64.
//
// Android is Linux, so most of this is runtime/platform/linux.c unchanged. It is its own file rather than #ifdefs
// in that one because the differences are not cosmetic: bionic has no <execinfo.h>, an app cannot read
// /proc/self/pagemap, and it cannot start a child process at all. Each of those is noted where it bites.
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <jni.h>
#include <limits.h>
#include <android/choreographer.h>
#include <android/looper.h>
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android/thermal.h>
#include <math.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/ucontext.h>
#include <sys/utsname.h>
#include <time.h>
#include <stdatomic.h>
#include <unistd.h>
#include <unwind.h>

#include "../paths.h"
#include "../platform.h"
#include "../settings.h"
#include "posix_log.h"

void rt_crash_explain(char *out, size_t n, const char *log_path);  // runtime/crash_explain.c

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

// ---- page write tracking: not available to an app
// Linux does this with a userfaultfd in write-protect mode plus PAGEMAP_SCAN over /proc/self/pagemap. On Android
// /proc/<pid>/pagemap has been privileged since Android 10, and userfaultfd is reachable only through a seccomp
// hole most devices do not open. The callers already cope with no tracking (it is also how an older Linux kernel
// behaves), so this reports nothing rather than pretending.
void plat_write_tracking_off(const char *why) { (void)why; }
int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    (void)addr, (void)size, (void)pages, (void)max, (void)page;
    return -1;
}
int plat_written_pages_peek(uint64_t addr, uint64_t size) { (void)addr; (void)size; return -1; }
bool plat_page_state(uint64_t addr, char *out, size_t n) { (void)addr; (void)out; (void)n; return false; }
void plat_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }
bool plat_mem_usage(uint64_t *process, uint64_t *guest) { (void)process; (void)guest; return false; }
void plat_sample_focus(void) {}

// ---- guest address space
// As on Linux: everything readable/writable from the start, MAP_NORESERVE so the untouched part of the window
// costs nothing. The window sits near 0x7000000000 (~481 GB), which fits a 39-bit user address space (512 GB) but
// not by much - MAP_FIXED_NOREPLACE makes a clash a clean failure instead of a silent relocation.
// On failure this says why, to logcat: the caller (reserve_guest_space in runtime/main.c) is a static
// constructor that runs during dlopen, long before any log file exists, and on Android it cannot re-exec to get a
// fresh layout the way the desktop does - so this is the only chance to find out what went wrong.
static void report_reservation_failure(uint64_t lo, uint64_t hi, int err) {
    __android_log_print(ANDROID_LOG_ERROR, "ZARecomp",
                        "guest window %llx-%llx (%llu MB) refused: %s",
                        (unsigned long long)lo, (unsigned long long)hi,
                        (unsigned long long)((hi - lo) >> 20), strerror(err));
    FILE *m = fopen("/proc/self/maps", "r");
    if (!m) return;
    char line[512];
    uint64_t top = 0;
    int overlaps = 0;
    while (fgets(line, sizeof line, m)) {
        uint64_t a, b;
        if (sscanf(line, "%llx-%llx", (unsigned long long *)&a, (unsigned long long *)&b) != 2) continue;
        if (b > top) top = b;
        if (a < hi && b > lo && overlaps < 8) {  // what is sitting inside the window we wanted
            overlaps++;
            for (char *c = line; *c; c++)
                if (*c == '\n') *c = 0;
            __android_log_print(ANDROID_LOG_ERROR, "ZARecomp", "  in the way: %s", line);
        }
    }
    fclose(m);
    __android_log_print(ANDROID_LOG_ERROR, "ZARecomp",
                        "  %d mapping(s) overlap; highest address in this process: %llx",
                        overlaps, (unsigned long long)top);

    // Where could the window go instead? The base is a choice the port makes (rt.h GUEST_LO / MAIN_BASE, and the
    // address passed to tools/gen.py), not something the Switch fixes, so a free gap of the right size is a
    // candidate - as long as it still fits the 39 bits the generated code assumes.
    m = fopen("/proc/self/maps", "r");
    if (!m) return;
    uint64_t need = hi - lo, prev_end = 0x1000000, found = 0;
    while (fgets(line, sizeof line, m) && found < 6) {
        uint64_t a, b;
        if (sscanf(line, "%llx-%llx", (unsigned long long *)&a, (unsigned long long *)&b) != 2) continue;
        if (a > prev_end && a - prev_end >= need) {
            found++;
            __android_log_print(ANDROID_LOG_ERROR, "ZARecomp", "  free gap: %llx-%llx (%llu GB)",
                                (unsigned long long)prev_end, (unsigned long long)a,
                                (unsigned long long)((a - prev_end) >> 30));
        }
        if (b > prev_end) prev_end = b;
    }
    fclose(m);
    if (!found)
        __android_log_print(ANDROID_LOG_ERROR, "ZARecomp", "  no free gap of %llu GB anywhere",
                            (unsigned long long)(need >> 30));
}

bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    void *p = mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) { report_reservation_failure(lo, hi, errno); return false; }
    if (p != (void *)(uintptr_t)lo) {
        munmap(p, hi - lo);
        __android_log_print(ANDROID_LOG_ERROR, "ZARecomp",
                            "guest window %llx: the kernel ignored MAP_FIXED_NOREPLACE and offered %p",
                            (unsigned long long)lo, p);
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, "ZARecomp", "guest window %llx-%llx reserved",
                        (unsigned long long)lo, (unsigned long long)hi);
    return true;
}

// ---- guest memory the GPU shares (platform.h plat_gpu_share)
// Qualcomm's driver has no VK_EXT_external_memory_host, so the guest window can't be imported by pointer and shader
// global memory (every storage buffer on the Switch's GPU) read zero: no 3D, no characters, empty text boxes. What it
// does import is an AHardwareBuffer. So one range of guest memory is an AHardwareBuffer BLOB: its dma-buf (one of the
// file descriptors AHardwareBuffer_sendHandleToUnixSocket hands out - the NDK's only way to a native handle) mapped
// MAP_SHARED at the guest address, and the same buffer imported by the renderer (exec.c gmem_import). Checked on an
// SM-S936B with tools/linux/android-port/test_ahb_gmem.c: CPU writes reach the GPU and GPU writes the CPU at 256 MB.
// Never much bigger: 1 GB is refused (-129), and 2 GB or more crashed the vendor allocator service on its size
// arithmetic, which restarts Android's whole display stack. One range per boot; LARECOMP_GPU_SHARE=0: off.
#define GPU_SHARE_MAX (512ull << 20)
static struct { uint64_t addr, size; AHardwareBuffer *ahb; } g_share;

static int ahb_fds(AHardwareBuffer *b, int *fds, int max) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) return -1;
    if (AHardwareBuffer_sendHandleToUnixSocket(b, sv[0])) { close(sv[0]); close(sv[1]); return -1; }
    char data[4096], ctl[CMSG_SPACE(sizeof(int) * 32)];
    struct iovec iov = {data, sizeof data};
    struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = sizeof ctl};
    ssize_t r = recvmsg(sv[1], &m, 0);
    close(sv[0]);
    close(sv[1]);
    if (r < 0) return -1;
    int n = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
            for (int i = 0; i < (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int)) && n < max; i++) fds[n++] = ((int *)CMSG_DATA(c))[i];
    return n;
}

bool plat_gpu_share(uint64_t addr, uint64_t size) {
    const char *e = getenv("LARECOMP_GPU_SHARE");
    if (g_share.ahb || (e && !atoi(e)) || size > GPU_SHARE_MAX || (addr | size) & 0xFFF) return false;
    AHardwareBuffer_Desc d = {.width = (uint32_t)size, .height = 1, .layers = 1, .format = AHARDWAREBUFFER_FORMAT_BLOB,
                              .usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                                       AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER};
    AHardwareBuffer *b = NULL;
    int err = AHardwareBuffer_allocate(&d, &b);
    if (err) { fprintf(stderr, "[share] AHardwareBuffer of %llu MB: %d\n", (unsigned long long)(size >> 20), err); return false; }
    int fds[32], n = ahb_fds(b, fds, 32), fd = -1;
    for (int i = 0; i < n; i++)  // (the dma-buf is the descriptor as long as the buffer; the other is gralloc's metadata)
        if (fd < 0 && lseek(fds[i], 0, SEEK_END) >= (off_t)size) fd = fds[i];
    void *m = fd < 0 ? MAP_FAILED
                     : mmap((void *)(uintptr_t)addr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    for (int i = 0; i < n; i++) close(fds[i]);  // (the mapping keeps the dma-buf)
    if (m != (void *)(uintptr_t)addr) {
        fprintf(stderr, "[share] %llx +%llx: %s\n", (unsigned long long)addr, (unsigned long long)size,
                fd < 0 ? "no dma-buf among the buffer's descriptors" : strerror(errno));
        if (m != MAP_FAILED) munmap(m, size);
        plat_reset_guest(addr, addr + size);  // (MAP_FIXED may have taken the old pages: fresh ones, as before)
        AHardwareBuffer_release(b);
        return false;
    }
    g_share.addr = addr, g_share.size = size, g_share.ahb = b;
    fprintf(stderr, "[share] guest memory %llx +%llx (%llu MB) is an AHardwareBuffer the GPU imports\n",
            (unsigned long long)addr, (unsigned long long)size, (unsigned long long)(size >> 20));
    return true;
}

void *plat_gpu_shared(uint64_t *addr, uint64_t *size) {
    *addr = g_share.addr, *size = g_share.size;
    return g_share.ahb;
}

// Pages back to zero. Inside the shared range they stay that memory (the renderer's import holds the same pages):
// zeroed in place - unless the whole range goes (a reboot), which lets the buffer go too.
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    uint64_t s0 = g_share.addr, s1 = g_share.addr + g_share.size;
    if (g_share.ahb && lo < s1 && s0 < hi) {
        if (lo <= s0 && hi >= s1) {
            AHardwareBuffer_release(g_share.ahb);  // (the renderer's import keeps its own reference until it is freed)
            memset(&g_share, 0, sizeof g_share);
        } else {
            uint64_t a = lo > s0 ? lo : s0, z = hi < s1 ? hi : s1;
            memset((void *)(uintptr_t)a, 0, z - a);
            bool ok = true;
            if (lo < a) ok = plat_reset_guest(lo, a);
            if (z < hi) ok = plat_reset_guest(z, hi) && ok;
            return ok;
        }
    }
    return mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) == (void *)(uintptr_t)lo;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE;
    return mprotect((void *)(uintptr_t)addr, size, p);
}

// ---- performance hints (platform.h plat_perf_hint): Android's ADPF, API 33 - looked up at run time (the app runs from
// API 31, where there's none: then nothing). A session for the presenting thread gets the frame's real budget (33.3 ms
// at 30 fps, 16.7 at 60) and each frame's work: the scheduler keeps the CPU clocks that meet it instead of holding them
// up for a frame rate the game doesn't run at (ACNHRecomp found the same on its Snapdragon port: less heat at the same
// frame rate). Only the thread that first calls in. LARECOMP_PERF_HINT=0: off.
typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;
void plat_perf_hint(uint64_t work_ns, uint64_t target_ns) {
    static int state = -1;  // (-1 not tried, 0 none, 1 on)
    static APerformanceHintSession *s;
    static int64_t cur_target;
    static int (*report)(APerformanceHintSession *, int64_t);
    static int (*update)(APerformanceHintSession *, int64_t);
    if (state < 0) {
        state = 0;
        const char *e = getenv("LARECOMP_PERF_HINT");
        if (e && !atoi(e)) return;
        void *h = dlopen("libandroid.so", RTLD_NOW);
        APerformanceHintManager *(*get)(void) = h ? (APerformanceHintManager * (*)(void)) dlsym(h, "APerformanceHint_getManager") : NULL;
        APerformanceHintSession *(*create)(APerformanceHintManager *, const int32_t *, size_t, int64_t) =
            h ? (APerformanceHintSession * (*)(APerformanceHintManager *, const int32_t *, size_t, int64_t))
                    dlsym(h, "APerformanceHint_createSession") : NULL;
        report = h ? (int (*)(APerformanceHintSession *, int64_t))dlsym(h, "APerformanceHint_reportActualWorkDuration") : NULL;
        update = h ? (int (*)(APerformanceHintSession *, int64_t))dlsym(h, "APerformanceHint_updateTargetWorkDuration") : NULL;
        APerformanceHintManager *m = get ? get() : NULL;
        int32_t tid = (int32_t)gettid();
        s = m && create && report && update ? create(m, &tid, 1, (int64_t)target_ns) : NULL;
        cur_target = (int64_t)target_ns;
        state = s != NULL;
        fprintf(stderr, "[perfhint] %s\n", s ? "performance hint session on (the frame budget goes to the scheduler)"
                                             : "no performance hints here (API 33+)");
    }
    if (state != 1 || !work_ns) return;
    if ((int64_t)target_ns != cur_target) update(s, (int64_t)target_ns), cur_target = (int64_t)target_ns;
    report(s, (int64_t)work_ns);
}

// ---- the display's rate (platform.h plat_frame_rate_hint): ANativeWindow_setFrameRate (API 30, looked up at run time)
// on the window SDL made. The phone's adaptive refresh follows what an app presents and took the S25+'s display from
// 120 Hz down to 30 while the game ran at 30 - with frame generation's 60 images a second shown at 30 Hz, half of them
// never reached the screen and it looked like 30 fps (LARECOMP_FG_TIMING: "display refresh 33.33 ms"). 0: no preference.
// Thor (60/120 Hz panel): the game's own rate, 30 included, so the display picks 60 or 120 (a multiple) and the
// compositor latches every image on the same vsync phase. LARECOMP_DISPLAY_RATE=<hz>: always ask that rate instead
// (120: the panel's top mode when the system's peak refresh rate allows it); =0: no preference at 30 (as shipped).
void plat_frame_rate_hint(void *sdl_window, float fps) {
    static int (*set)(void *, float, int8_t);
    static int tried;
    static float cur = -1, forced = -1;
    if (!tried) {
        tried = 1;
        void *h = dlopen("libandroid.so", RTLD_NOW);
        set = h ? (int (*)(void *, float, int8_t))dlsym(h, "ANativeWindow_setFrameRate") : NULL;
        const char *e = getenv("LARECOMP_DISPLAY_RATE");
        if (e && *e) forced = (float)atof(e);
    }
    if (forced > 0) fps = forced;
    else if (forced == 0 && fps <= 30) fps = 0;
    if (fps == cur || !sdl_window) return;
    void *nw = SDL_GetPointerProperty(SDL_GetWindowProperties((SDL_Window *)sdl_window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL);
    if (!set || !nw) return;
    // (compatibility 1, FIXED_SOURCE: content made at that rate - the display goes to it or a multiple of it)
    int r = set(nw, fps, fps > 0 ? 1 : 0);
    fprintf(stderr, "[display] frame rate asked: %.0f%s (%d)\n", fps, fps > 0 ? " fps" : " (no preference)", r);
    cur = fps;
}

// ---- the display's vsync (platform.h plat_display_vsync): AChoreographer frame callbacks on a looper thread of our
// own, re-posted every frame. frameTimeNanos is the vsync's CLOCK_MONOTONIC time (plat_now_ns's clock). The period is
// measured, not the mode's: the Thor's "60.000004 Hz" panel showed the game's frames 33.53 ms apart in SurfaceFlinger's
// own present times (59.65 Hz), so a timer at 16.667 ms gains a refresh every few seconds. Each interval counts as
// the whole number of refreshes nearest the nominal period (the refresh-rate callback's; before it, the shortest
// interval seen); every 120 refreshes the period is their mean.
static _Atomic uint64_t g_dv_at, g_dv_period, g_dv_nominal;
static atomic_bool g_dv_reset;
static void dv_frame(int64_t t, void *data) {
    static uint64_t prev, acc_ns, acc_k, shortest = UINT64_MAX, calls;
    if (!calls++) fprintf(stderr, "[display] vsync tracker: first display vsync\n");
    if (atomic_exchange(&g_dv_reset, false)) acc_ns = acc_k = 0;
    if (prev && (uint64_t)t > prev) {
        uint64_t iv = (uint64_t)t - prev, nom = atomic_load(&g_dv_nominal);
        if (!nom && iv > 4000000 && iv < shortest) shortest = iv;
        if (!nom) nom = shortest != UINT64_MAX ? shortest : 0;
        uint64_t k = nom ? (iv + nom / 2) / nom : 0;
        if (k >= 1 && k <= 4) acc_ns += iv, acc_k += k;
        if (acc_k >= 120) {
            uint64_t p = acc_ns / acc_k, old = atomic_exchange(&g_dv_period, p);
            static int said;
            if (!said++ || (old > p ? old - p : p - old) > 20000)
                fprintf(stderr, "[display] refresh measured: %.3f ms (%.2f Hz)\n", p / 1e6, 1e9 / p);
            acc_ns = acc_k = 0;
        }
    }
    prev = (uint64_t)t;
    atomic_store(&g_dv_at, (uint64_t)t);
    AChoreographer_postFrameCallback64(AChoreographer_getInstance(), dv_frame, data);
}
static void dv_rate(int64_t period, void *data) {  // (the display's mode, when it changes: the period it says)
    (void)data;
    atomic_store(&g_dv_nominal, (uint64_t)period);
    atomic_store(&g_dv_period, (uint64_t)period);  // (until measured again)
    atomic_store(&g_dv_reset, true);
    fprintf(stderr, "[display] refresh period %.3f ms (the mode's)\n", period / 1e6);
}
static void *dv_thread(void *a) {
    (void)a;
    pthread_setname_np(pthread_self(), "za-dispvsync");
    ALooper *lp = ALooper_prepare(0);
    AChoreographer *c = AChoreographer_getInstance();
    fprintf(stderr, "[display] vsync tracker: looper %p, choreographer %p\n", (void *)lp, (void *)c);
    if (!c) return NULL;
    AChoreographer_registerRefreshRateCallback(c, dv_rate, NULL);
    AChoreographer_postFrameCallback64(c, dv_frame, NULL);
    for (;;) {
        int r = ALooper_pollOnce(-1, NULL, NULL, NULL);
        static int said;
        if (r != ALOOPER_POLL_CALLBACK && said++ < 5) fprintf(stderr, "[display] vsync tracker: looper poll %d\n", r);
    }
    return NULL;
}
static void dv_start(void) {
    pthread_t t;
    if (!pthread_create(&t, NULL, dv_thread, NULL)) pthread_detach(t);
}
bool plat_display_vsync(uint64_t *at_ns, uint64_t *period_ns) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, dv_start);
    *at_ns = atomic_load(&g_dv_at), *period_ns = atomic_load(&g_dv_period);
    return *at_ns && *period_ns;
}

// ---- the device's own modes, for the log: Settings.System keys a vendor keeps there (AYN: performance_mode 0-2,
// fan_mode, fan_speed; the user's peak_refresh_rate caps what plat_frame_rate_hint can get). Through JNI on the calling
// thread (SDL attaches it); a key the system won't let an app read logs "?". Returns true when something changed.
static bool device_modes(char *out, size_t n) {
    static const char *const keys[] = {"performance_mode", "fan_mode", "fan_speed", "peak_refresh_rate", "min_refresh_rate"};
    static char last[256];
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = env ? (jobject)SDL_GetAndroidActivity() : NULL;
    if (!act) return false;
    char buf[256] = "";
    size_t o = 0;
    jclass ac = (*env)->GetObjectClass(env, act);
    jmethodID gcr = (*env)->GetMethodID(env, ac, "getContentResolver", "()Landroid/content/ContentResolver;");
    jobject cr = gcr ? (*env)->CallObjectMethod(env, act, gcr) : NULL;
    jclass sys = cr ? (*env)->FindClass(env, "android/provider/Settings$System") : NULL;
    jmethodID gs = sys ? (*env)->GetStaticMethodID(env, sys, "getString",
                                                   "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;") : NULL;
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env), gs = NULL;
    for (size_t i = 0; gs && i < sizeof keys / sizeof *keys && o < sizeof buf; i++) {
        jstring k = (*env)->NewStringUTF(env, keys[i]);
        jstring v = (jstring)(*env)->CallStaticObjectMethod(env, sys, gs, cr, k);
        const char *s = "?";
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env), v = NULL;
        else if (!v) s = "-";
        const char *u = v ? (*env)->GetStringUTFChars(env, v, NULL) : NULL;
        o += (size_t)snprintf(buf + o, sizeof buf - o, "%s%s %s", i ? ", " : "", keys[i], u ? u : s);
        if (u) (*env)->ReleaseStringUTFChars(env, v, u);
        if (v) (*env)->DeleteLocalRef(env, v);
        (*env)->DeleteLocalRef(env, k);
    }
    if (sys) (*env)->DeleteLocalRef(env, sys);
    if (cr) (*env)->DeleteLocalRef(env, cr);
    (*env)->DeleteLocalRef(env, ac);
    (*env)->DeleteLocalRef(env, act);
    snprintf(out, n, "%s", buf);
    if (!strcmp(buf, last)) return false;
    snprintf(last, sizeof last, "%s", buf);
    return true;
}

// ---- thermal: how much of the render scale the temperature allows (platform.h plat_thermal_scale)
// AThermal_getThermalHeadroom (API 31) is the system's forecast of how close the device will be, on average over the
// next forecast seconds, to its severe throttling point - 1.0 is that point. The thermal profile selects the render
// scale curve: the cooler profile uses the tested 100/85/70/60% steps; the no-cooler profile reacts earlier and adds
// 50%. The API wants a call a second at most (more often it answers NaN). A "[thermal]" line every 30 s and on each step.
// LARECOMP_THERMAL=0: off; =hal: Android's headroom only; =sysfs: the zones below only; unset: the headroom, and the
// zones when it has answered nothing for 5 s.
//
// The zones: on the AYN Thor the thermal HAL never becomes ready ("HAL Ready: false"), so the headroom is NaN for the
// whole session and nothing ever stepped (a 4 min run sat at 93-95 C CPU, GPU 62->80, xo-therm 53->64). The kernel's
// thermal zones are readable by an app, trip points included, so the same headroom is computed from them the way
// Android's thermal service does it for its skin sensor: 0 at 30 C below the severe point, 1.0 at it. Per group the
// hottest zone; the severe point is the group's lowest passive trip under 125 C (Thor: CPU 108, GPU 95, xo-therm 78);
// the device's headroom is the highest group's. The skin zone is slow (a board thermistor), so as Android does it is
// forecast 10 s ahead from its trend over the last 10 s; the CPU and GPU zones follow the load within a second (the
// first try forecast those too: a 88 -> 95 C CPU jump at a scene load read as 0.84 and stepped down for nothing), so
// they count as their 5 s mean.
// LARECOMP_THERMAL_CPU / _GPU / _SKIN=<C>: another severe point for that group.
static const double thermal_steps_cooler[] = {1.0, 0.85, 0.7, 0.6};
static const double thermal_steps_no_cooler[] = {1.0, 0.85, 0.7, 0.6, 0.5};

static double thermal_scale_cache = 1.0;  // (the scale above, for plat_thermal_throttled)

int plat_thermal_throttled(void) { return thermal_scale_cache < 1.0; }

enum { TZ_CPU, TZ_GPU, TZ_SKIN, TZ_N };
static const char *const tz_names[TZ_N] = {"cpu", "gpu", "skin"};
static struct {
    int fd[96], group[96], n;
    double severe[TZ_N], temp[TZ_N];
    char skin[32];
    double hist[TZ_N][10];  // (each group's temperature a second, newest last)
    int nhist;
} g_tz;

static int tz_group(const char *type) {
    if (!strncmp(type, "cpu-", 4) || !strncmp(type, "cpuss-", 6) || !strncmp(type, "cpu_", 4)) return TZ_CPU;
    if (!strncmp(type, "gpuss-", 6) || !strncmp(type, "gpu", 3)) return TZ_GPU;
    if (!strcmp(type, "xo-therm") || strstr(type, "skin") || !strcmp(type, "quiet-therm") || !strcmp(type, "sys-therm-0"))
        return TZ_SKIN;
    return -1;
}

static long sysfs_long(const char *path) {
    char b[32];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t k = fd < 0 ? -1 : read(fd, b, sizeof b - 1);
    if (fd >= 0) close(fd);
    if (k <= 0) return LONG_MIN;
    b[k] = 0;
    return strtol(b, NULL, 10);
}

static bool tz_scan(void) {
    static const double fallback[TZ_N] = {105, 95, 50};
    static const char *const env[TZ_N] = {"LARECOMP_THERMAL_CPU", "LARECOMP_THERMAL_GPU", "LARECOMP_THERMAL_SKIN"};
    double lowest[TZ_N] = {1e9, 1e9, 1e9};
    int count[TZ_N] = {0};
    for (int i = 0; i < 256 && g_tz.n < (int)(sizeof g_tz.fd / sizeof *g_tz.fd); i++) {
        char p[96], type[48];
        snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/type", i);
        FILE *f = fopen(p, "r");
        if (!f) { if (i > 128) break; continue; }
        bool ok = fgets(type, sizeof type, f) != NULL;
        fclose(f);
        if (!ok) continue;
        type[strcspn(type, "\r\n")] = 0;
        int g = tz_group(type);
        if (g < 0) continue;
        snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/temp", i);
        long t = sysfs_long(p);
        if (t == LONG_MIN || t < -30000 || t > 200000) continue;  // (a disabled sensor answers EINVAL)
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        g_tz.fd[g_tz.n] = fd, g_tz.group[g_tz.n++] = g, count[g]++;
        if (g == TZ_SKIN && !g_tz.skin[0]) snprintf(g_tz.skin, sizeof g_tz.skin, "%s", type);
        for (int k = 0; k < 16; k++) {
            char tp[112], tt[24] = "";
            snprintf(tp, sizeof tp, "/sys/class/thermal/thermal_zone%d/trip_point_%d_type", i, k);
            FILE *tf = fopen(tp, "r");
            if (!tf) break;
            ok = fgets(tt, sizeof tt, tf) != NULL;
            fclose(tf);
            snprintf(tp, sizeof tp, "/sys/class/thermal/thermal_zone%d/trip_point_%d_temp", i, k);
            long v = sysfs_long(tp);
            if (ok && !strncmp(tt, "passive", 7) && v > 40000 && v < 125000 && v / 1000.0 < lowest[g]) lowest[g] = v / 1000.0;
        }
    }
    for (int g = 0; g < TZ_N; g++) {
        const char *e = getenv(env[g]);
        g_tz.severe[g] = e && atof(e) > 0 ? atof(e) : lowest[g] < 1e9 ? lowest[g] : fallback[g];
    }
    fprintf(stderr, "[thermal] sysfs zones: %d cpu, %d gpu, %d skin (%s); severe at cpu %.0f, gpu %.0f, skin %.0f C\n",
            count[TZ_CPU], count[TZ_GPU], count[TZ_SKIN], g_tz.skin[0] ? g_tz.skin : "none", g_tz.severe[TZ_CPU],
            g_tz.severe[TZ_GPU], g_tz.severe[TZ_SKIN]);
    return g_tz.n > 0;
}

// The device's headroom from the zones (Android's scale: 1.0 = severe; the skin zone's forecast 10 s ahead); NaN if none
static float tz_headroom(void) {
    double hot[TZ_N] = {-1e9, -1e9, -1e9};
    for (int i = 0; i < g_tz.n; i++) {
        char b[24];
        ssize_t k = pread(g_tz.fd[i], b, sizeof b - 1, 0);
        if (k <= 0) continue;
        b[k] = 0;
        double t = strtol(b, NULL, 10) / 1000.0;
        if (t > hot[g_tz.group[i]]) hot[g_tz.group[i]] = t;
    }
    if (g_tz.nhist == 10) {
        for (int g = 0; g < TZ_N; g++) memmove(g_tz.hist[g], g_tz.hist[g] + 1, 9 * sizeof **g_tz.hist);
        g_tz.nhist--;
    }
    int n = ++g_tz.nhist;
    double h = -1e9;
    for (int g = 0; g < TZ_N; g++) {
        g_tz.temp[g] = g_tz.hist[g][n - 1] = hot[g];
        if (hot[g] <= -1e9) continue;
        double t = 0;
        if (g != TZ_SKIN) {  // (the mean of the last 5 s)
            int k = n < 5 ? n : 5;
            for (int i = n - k; i < n; i++) t += g_tz.hist[g][i] > -1e9 ? g_tz.hist[g][i] : hot[g];
            t /= k;
        } else if (n < 5) t = hot[g];
        else {  // (least squares over the samples, a second apart, 10 s past the newest)
            double mx = (n - 1) / 2.0, my = 0, sxy = 0, sxx = 0;
            for (int i = 0; i < n; i++) my += g_tz.hist[g][i] > -1e9 ? g_tz.hist[g][i] : hot[g];
            my /= n;
            for (int i = 0; i < n; i++)
                sxy += (i - mx) * ((g_tz.hist[g][i] > -1e9 ? g_tz.hist[g][i] : hot[g]) - my), sxx += (i - mx) * (i - mx);
            t = my + sxy / sxx * (n - 1 - mx + 10);
        }
        h = fmax(h, (t - (g_tz.severe[g] - 30)) / 30);
    }
    return h == -1e9 ? NAN : (float)fmax(h, 0);
}

double plat_thermal_scale(double now) {
    static AThermalManager *mgr;
    static int off = -1, step, last_profile = -1, src, nan_s;  // (src: 0 Android's headroom, 1 the sysfs zones)
    static double next_read, last_change = -1e9, last_line = -1e9, scale = 1.0;
    int profile = g_settings.thermal_profile == THERMAL_PROFILE_NO_COOLER ? THERMAL_PROFILE_NO_COOLER : THERMAL_PROFILE_COOLER;
    const double *steps = profile == THERMAL_PROFILE_NO_COOLER ? thermal_steps_no_cooler : thermal_steps_cooler;
    int nsteps = profile == THERMAL_PROFILE_NO_COOLER ?
                 (int)(sizeof thermal_steps_no_cooler / sizeof *thermal_steps_no_cooler) :
                 (int)(sizeof thermal_steps_cooler / sizeof *thermal_steps_cooler);
    if (last_profile != profile) {
        int nearest = 0;
        for (int i = 1; i < nsteps; i++) if (fabs(steps[i] - scale) < fabs(steps[nearest] - scale)) nearest = i;
        step = nearest;
        scale = steps[step];
        last_profile = profile;
        last_change = -1e9;
        fprintf(stderr, "[thermal] profile: %s\n", profile == THERMAL_PROFILE_NO_COOLER ? "without cooler" : "with cooler");
    }
    if (off < 0) {
        const char *e = getenv("LARECOMP_THERMAL");
        off = e && !strcmp(e, "0");
        bool sysfs = e && !strcmp(e, "sysfs"), hal = e && !strcmp(e, "hal");
        if (!off && !sysfs) mgr = AThermal_acquireManager();
        if (!off && (sysfs || !mgr)) {
            if (!hal && tz_scan()) src = 1;
            else off = 1;
        }
        if (hal) nan_s = -1;  // (never falls back)
        fprintf(stderr, "[thermal] render scale by thermal headroom: %s\n", off ? "off" : src ? "on (sysfs zones)" : "on");
    }
    if (off || now < next_read) return thermal_scale_cache = scale;
    next_read = now + 1.0;
    float h = src ? tz_headroom() : AThermal_getThermalHeadroom(mgr, 10);
    if (isnan(h)) {
        if (!src && nan_s >= 0 && ++nan_s == 5) {
            fprintf(stderr, "[thermal] no headroom from the system in 5 s (thermal HAL not ready?): the sysfs zones instead\n");
            if (tz_scan()) src = 1;
            else off = 1;
        }
        return thermal_scale_cache = scale;
    }
    if (!src) nan_s = -1;  // (it answered: Android's headroom stays the source)
    int last = nsteps - 1, was = step;
    float down_at = profile == THERMAL_PROFILE_NO_COOLER ? 0.70f : 0.80f;
    float up_below = profile == THERMAL_PROFILE_NO_COOLER ? 0.45f : 0.60f;
    double down_wait = profile == THERMAL_PROFILE_NO_COOLER ? 8.0 : 10.0;
    double up_wait = profile == THERMAL_PROFILE_NO_COOLER ? 45.0 : 30.0;
    if (h >= down_at && step < last && now - last_change >= down_wait) step++;
    else if (h < up_below && step > 0 && now - last_change >= up_wait) step--;
    if (step != was) last_change = now;
    scale = steps[step];
    thermal_scale_cache = scale;
    if (step != was || now - last_line >= 30) {
        last_line = now;
        char temps[96] = "", modes[256];
        if (src) snprintf(temps, sizeof temps, " (cpu %.0f, gpu %.0f, %s %.0f C)", g_tz.temp[TZ_CPU], g_tz.temp[TZ_GPU],
                          g_tz.skin[0] ? g_tz.skin : tz_names[TZ_SKIN], g_tz.temp[TZ_SKIN]);
        fprintf(stderr, "[thermal] headroom %.2f (10 s forecast)%s: render scale %.0f%%%s\n", h, temps, scale * 100,
                step > was ? " - stepped down" : step < was ? " - stepped up" : "");
        if (device_modes(modes, sizeof modes)) fprintf(stderr, "[device] %s\n", modes);
    }
    return scale;
}

// ---- time (identical to Linux: bionic has all of these)
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

uint64_t plat_thread_cpu_ns(void) {
    struct timespec ts;
    return clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0 ? (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec : 0;
}

void plat_sleep_until_ns(uint64_t t) {
    struct timespec ts = {.tv_sec = (time_t)(t / 1000000000u), .tv_nsec = (long)(t % 1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

int plat_cpu_count(void) {
    cpu_set_t set;
    return sched_getaffinity(0, sizeof set, &set) == 0 ? CPU_COUNT(&set) : 0;
}

// ---- core placement (LARECOMP_AFFINITY). Cores by top frequency: "fast" = the single fastest core (8 Gen 2: cpu7,
// X3), "big" = the next tier (cpu3-6). =render (the default): the render thread on the fastest core, the game's main
// thread on the big ones (EoWRecomp's choice) - on the Thor at 30 fps the render thread's CPU per frame went from 8.1
// to 5.3 ms in the first street (it had been on the A715s; the X3 is ~1.45x one of those), at the same app CPU;
// =x3: the game's main thread alone on the fastest core, the render thread on the big ones, every other guest thread
// off the fastest (no gain measured); =big: game and render threads on fast + big; =0: Android decides.
static cpu_set_t g_set_fast, g_set_big, g_set_notfast;
static int g_aff_mode;  // 0 off, 1 x3, 2 render, 3 big
static void aff_scan(void) {
    const char *e = getenv("LARECOMP_AFFINITY");
    g_aff_mode = !e || !*e || !strcmp(e, "render") ? 2 : !strcmp(e, "0") ? 0 : !strcmp(e, "big") ? 3 : 1;
    if (!g_aff_mode) return;
    long f[CPU_SETSIZE] = {0}, best = 0, second = 0;
    int n = 0;
    for (; n < CPU_SETSIZE; n++) {
        char p[96];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", n);
        FILE *fp = fopen(p, "r");
        if (!fp) break;
        if (fscanf(fp, "%ld", &f[n]) != 1) f[n] = 0;
        fclose(fp);
        if (f[n] > best) best = f[n];
    }
    for (int i = 0; i < n; i++) if (f[i] < best && f[i] > second) second = f[i];
    CPU_ZERO(&g_set_fast), CPU_ZERO(&g_set_big), CPU_ZERO(&g_set_notfast);
    for (int i = 0; i < n; i++) {
        if (f[i] == best) CPU_SET(i, &g_set_fast); else CPU_SET(i, &g_set_notfast);
        if (f[i] == second) CPU_SET(i, &g_set_big);
    }
    if (!CPU_COUNT(&g_set_fast) || !CPU_COUNT(&g_set_big)) g_aff_mode = 0;
    fprintf(stderr, "[cpu] affinity mode %d: %d fastest core(s) %ld MHz, %d big %ld MHz\n", g_aff_mode,
            CPU_COUNT(&g_set_fast), best / 1000, CPU_COUNT(&g_set_big), second / 1000);
}
static void aff_apply(const cpu_set_t *s, const char *who) {
    if (sched_setaffinity(0, sizeof *s, s) != 0) fprintf(stderr, "[cpu] affinity for %s: %s\n", who, strerror(errno));
}
// (the threads get names too - all of them were "SDLThread" in top/systrace: za-game, za-render / za-render2 (the
// render queue's and the recorder's), za-vsync, za-guest for the game's other threads)
void plat_thread_guest(int prio) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    (void)prio;
    pthread_once(&once, aff_scan);
    pthread_setname_np(pthread_self(), "za-guest");
    if (g_aff_mode == 1 || g_aff_mode == 3) aff_apply(&g_set_notfast, "guest");
}
// The recorder thread (record.c, PLAT_PERF_RECORDER; PzRender): never on the fastest core, which is the render
// thread's - LARECOMP_AFFINITY_RECORDER=fast puts it back there (as before), =big on the big ones, =0 Android decides.
// With the render thread on the X3, each packet batch the render thread published woke the recorder on the same core,
// which preempted it (Lumiose street, user save: the render thread runnable but not running 29 % of the time, ~21k
// switches/s). On the big cores: 25.5 -> 28.3 fps, the game thread's wait for the translation 29.5 -> 13.1 ms a frame.
void plat_thread_perf_core(int role) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, aff_scan);
    pthread_setname_np(pthread_self(), role == PLAT_PERF_GAME ? "za-game" : role == PLAT_PERF_VSYNC ? "za-vsync" :
                                       role == PLAT_PERF_RECORDER ? "za-recorder" : "za-render");
    if (!g_aff_mode || role == PLAT_PERF_VSYNC) return;
    cpu_set_t both = g_set_fast;
    CPU_OR(&both, &both, &g_set_big);
    bool game = role == PLAT_PERF_GAME;
    const cpu_set_t *s = g_aff_mode == 3 ? &both : (g_aff_mode == 1) == game ? &g_set_fast : &g_set_big;
    if (role == PLAT_PERF_RECORDER) {
        const char *e = getenv("LARECOMP_AFFINITY_RECORDER");
        if (e && !strcmp(e, "0")) return;
        if (e && !strcmp(e, "fast")) s = &g_set_fast;
        else if ((e && !strcmp(e, "big")) || s == &g_set_fast) s = &g_set_big;
        fprintf(stderr, "[cpu] recorder thread on the %s core(s)\n", s == &g_set_fast ? "fastest" : s == &g_set_big ? "big" : "fast + big");
    }
    if (sched_setaffinity(0, sizeof *s, s) != 0) aff_apply(&both, game ? "game (fastest core parked: fast + big)" : "render");
}
// (PzRender) the render thread's helpers (shader translation, pipeline builds, texture unpacking) on every core but the
// fastest: they inherited the render thread's affinity, so on the Thor all of them ran on the X3 with it.
// LARECOMP_AFFINITY_WORKERS=0: left as inherited (as before)
void plat_thread_worker(const char *name) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, aff_scan);
    pthread_setname_np(pthread_self(), name);
    const char *e = getenv("LARECOMP_AFFINITY_WORKERS");
    if (g_aff_mode && (!e || atoi(e))) aff_apply(&g_set_notfast, name);
}

// SCHED_FIFO needs a privilege an app never has, so in practice this is the setpriority path. Worth keeping: on a
// phone the vsync thread competing with the guest threads is the difference between steady and stuttering frames.
void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
    struct sched_param sp = {.sched_priority = 1};
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) return;
    setpriority(PRIO_PROCESS, (id_t)gettid(), -10);
}
int plat_thread_realtime_check(uint64_t period_ns) { (void)period_ns; return -1; }

// ---- entry point
// SDLActivity does not call main(): it dlsym's the name getMainFunction() returns, which is "SDL_main", out of
// libmain.so. On the desktop SDL3/SDL_main.h arranges that with a #define over main, but runtime/main.c includes
// only SDL.h, and changing it would reach every platform. This bridge is Android-only and costs one call.
int main(int argc, char **argv);
int SDL_main(int argc, char **argv);
int SDL_main(int argc, char **argv) { return main(argc, argv); }

// ---- process
static char g_log_path[1024];

// An app has no terminal and its stderr goes nowhere useful by default, so the log always goes to a file (and a
// copy of the fatal lines to logcat, which is what `adb logcat` shows while debugging on a cable).
static void log_to_file(void) {
    char dir[900];
    if (!rt_data_root_early(dir, sizeof dir)) return;
    for (char *c = dir + 1; *c; c++)
        if (*c == '/') { *c = 0; mkdir(dir, 0755); *c = '/'; }
    mkdir(dir, 0755);
    session_log_open(dir, g_log_path, sizeof g_log_path);
}

// runtime/paths.c works out the data folder from $HOME, which on Android points nowhere useful. The app's own
// directories are what it should use, and paths.c already honours LARECOMP_DATA_ROOT, so setting that is the whole
// fix. External storage first on purpose: /sdcard/Android/data/<package>/files is reachable from a file manager
// and over USB, and the player has to copy several GB of game files into it by hand. Internal storage would be
// private to the app and need root to write to.
static void pick_data_root(void) {
    if (getenv("LARECOMP_DATA_ROOT")) return;  // an explicit choice wins (adb shell setprop / a dev run)
    const char *p = SDL_GetAndroidExternalStoragePath();
    if (!p || !*p) p = SDL_GetAndroidInternalStoragePath();
    if (p && *p) setenv("LARECOMP_DATA_ROOT", p, 1);
}

// The game code library ships in the APK next to libmain.so, which is where gamelib.c looks anyway - but it also
// wants lasgame.so.id beside it, and an APK's lib directory only ever holds .so files. Naming the library through
// LARECOMP_GAME_LIB makes gamelib.c take it as given and let the id embedded in it decide instead.
static void point_at_game_lib(void) {
    if (getenv("LARECOMP_GAME_LIB")) return;
    char p[1024];
    if (!plat_exe_path(p, sizeof p)) return;
    char *slash = strrchr(p, '/');
    if (!slash) return;
    snprintf(slash + 1, sizeof p - (size_t)(slash + 1 - p), "lasgame.so");
    if (access(p, R_OK) == 0) setenv("LARECOMP_GAME_LIB", p, 1);
}

// The free fonts the desktop builds ship beside the executable (third_party/fonts: BIZ UDPGothic under the SIL OFL,
// DejaVu Sans under its own free licence) travel as APK assets here, because the APK's lib directory only holds .so
// files. runtime/hle/pl.c needs one of them on a phone with no font dump from the player's console: Android's own
// Roboto lacks the "□" (U+25A1) the game looks up unchecked at startup (issue #45), so pl.c rightly passes it over.
// Copied out to internal storage once (again only if the size changed); rt_resource finds them through
// LARECOMP_RESOURCE_DIR. SDL_IOFromFile reads a relative path from the APK's assets on Android.
static void unpack_bundled_resources(void) {
    if (getenv("LARECOMP_RESOURCE_DIR")) return;
    const char *base = SDL_GetAndroidInternalStoragePath();
    if (!base || !*base) return;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/res", base);
    mkdir(dir, 0755);
    static const char *const files[] = {"BIZUDPGothic-Regular.ttf", "DejaVuSans.ttf"};
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        char asset[128], out[1100], tmp[1110];
        snprintf(asset, sizeof asset, "fonts/%s", files[i]);
        snprintf(out, sizeof out, "%s/%s", dir, files[i]);
        SDL_IOStream *in = SDL_IOFromFile(asset, "rb");
        if (!in) {
            __android_log_print(ANDROID_LOG_WARN, "ZARecomp", "asset %s: %s", asset, SDL_GetError());
            continue;
        }
        Sint64 size = SDL_GetIOSize(in);
        struct stat st;
        if (size > 0 && stat(out, &st) == 0 && st.st_size == size) { SDL_CloseIO(in); continue; }  // (already there)
        snprintf(tmp, sizeof tmp, "%s.tmp", out);
        FILE *f = fopen(tmp, "wb");
        bool ok = f != NULL;
        char buf[65536];
        size_t k;
        while (ok && (k = SDL_ReadIO(in, buf, sizeof buf)) > 0) ok = fwrite(buf, 1, k, f) == k;
        SDL_CloseIO(in);
        if (f) ok = fclose(f) == 0 && ok;
        if (ok && rename(tmp, out) == 0)
            __android_log_print(ANDROID_LOG_INFO, "ZARecomp", "unpacked %s (%lld bytes)", asset, (long long)size);
        else
            remove(tmp);
    }
    setenv("LARECOMP_RESOURCE_DIR", dir, 1);
}

// An app has no environment of its own to set, and the runtime's switches (LARECOMP_*) are environment variables:
// <data root>/env.txt holds them, one NAME=value a line (# comments), pushed with adb for a test run. Read after
// pick_data_root, so LARECOMP_DATA_ROOT itself can't come from here; a variable already set wins.
static void load_env_file(void) {
    const char *root = getenv("LARECOMP_DATA_ROOT");
    if (!root) return;
    char p[1024], line[1024];
    snprintf(p, sizeof p, "%s/env.txt", root);
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq || eq == line) continue;
        *eq = 0;
        setenv(line, eq + 1, 0);
        __android_log_print(ANDROID_LOG_INFO, "ZARecomp", "env.txt: %s=%s", line, eq + 1);
    }
    fclose(f);
}

void plat_init(void) {
    unpack_bundled_resources();
    // The UI is laid out for landscape. The manifest asks for sensorLandscape, but SDLActivity overrides it with
    // its own setRequestedOrientation from this hint, and without one it followed the phone into portrait.
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    // the Back gesture opens / closes the menu (menu.mm, as F1) instead of finishing the activity
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    // Android's own keyboard never comes up (it covered half the screen and its text often didn't reach the field):
    // text input stays on for a hardware keyboard, and the name entry and our text fields use the app's on-screen
    // keyboard instead, by touch too (runtime/ui/osk.mm, menu.mm keyboard_draw)
    SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0");
    point_at_game_lib();
    pick_data_root();  // before log_to_file: that is the first thing to read the data folder
    load_env_file();
    log_to_file();
    raise_open_file_limit();
    char modes[256];  // (AYN: performance / fan mode, refresh-rate caps - the conditions of every measurement)
    if (device_modes(modes, sizeof modes)) fprintf(stderr, "[device] %s\n", modes);
}

// /proc/self/exe is app_process64 (every app is forked from the zygote), which tells the caller nothing. The
// equivalent here is our own shared library, which is what the paths code actually wants to sit next to.
bool plat_exe_path(char *buf, size_t size) {
    Dl_info di = {0};
    if (!dladdr((void *)plat_exe_path, &di) || !di.dli_fname) return false;
    snprintf(buf, size, "%s", di.dli_fname);
    return true;
}

// There is no /etc/os-release: the Android release and the device come from system properties.
void plat_os_name(char *buf, size_t size) {
    char rel[PROP_VALUE_MAX] = "", sdk[PROP_VALUE_MAX] = "", model[PROP_VALUE_MAX] = "";
    __system_property_get("ro.build.version.release", rel);
    __system_property_get("ro.build.version.sdk", sdk);
    __system_property_get("ro.product.model", model);
    struct utsname u;
    const char *kern = uname(&u) == 0 ? u.release : "?";
    snprintf(buf, size, "Android %s (API %s)%s%s (%s)", rel[0] ? rel : "?", sdk[0] ? sdk : "?",
             model[0] ? ", " : "", model, kern);
}

void plat_system_info(PlatSysInfo *si) {
    memset(si, 0, sizeof *si);
    plat_os_name(si->os, sizeof si->os);
    // /proc/cpuinfo on arm64 has no "model name": the SoC is a property, and the core count is what we can count.
    char soc[PROP_VALUE_MAX] = "", manuf[PROP_VALUE_MAX] = "";
    __system_property_get("ro.soc.model", soc);
    __system_property_get("ro.soc.manufacturer", manuf);
    if (soc[0]) snprintf(si->cpu, sizeof si->cpu, "%s%s%s", manuf, manuf[0] ? " " : "", soc);
    else snprintf(si->cpu, sizeof si->cpu, "arm64");
    // arm64 has no SMT, so every hardware thread is a core: the two counts are the same here.
    si->cores = si->threads = (int)sysconf(_SC_NPROCESSORS_CONF);
    long pages = sysconf(_SC_PHYS_PAGES), psz = sysconf(_SC_PAGESIZE);
    if (pages > 0 && psz > 0) si->ram = (uint64_t)pages * (uint64_t)psz;
    si->package = "apk";
}

// Re-exec means restarting the process, which an app does not get to do: the Activity is the one that can.
void plat_exec(char *const argv[]) { (void)argv; }

// ---- diagnostics
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    Dl_info di = {0};
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_sname) return NULL;
    if (offset) *offset = pc - (uint64_t)(uintptr_t)di.dli_saddr;
    return di.dli_sname;
}

void plat_fatal_notice(void) {
    char text[1600];
    rt_crash_explain(text, sizeof text, g_log_path);
    __android_log_write(ANDROID_LOG_FATAL, "ZARecomp", text);
    if (!g_log_path[0]) return;
    fflush(stderr);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "ZA Recomp", text, NULL);
}

static void *notice_thread(void *text) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "ZA Recomp", text, NULL);
    free(text);
    return NULL;
}
void plat_notice(const char *text) {
    __android_log_write(ANDROID_LOG_WARN, "ZARecomp", text);
    if (!g_log_path[0]) return;
    pthread_t t;
    char *copy = strdup(text);
    if (copy && !pthread_create(&t, NULL, notice_thread, copy)) pthread_detach(t);
    else free(copy);
}

bool plat_module_name(uint64_t pc, char *out, size_t n) {
    Dl_info di = {0};
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_fname) return false;
    snprintf(out, n, "%s", di.dli_fname);
    return true;
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

// bionic has no backtrace(): the unwinder is reached through <unwind.h>, which is what libc++ and the platform's
// own crash reporter use. Same output as the Linux version, so the log lines stay comparable between the two.
typedef struct { void **pcs; int n, max; } Unwind;

static _Unwind_Reason_Code unwind_frame(struct _Unwind_Context *ctx, void *arg) {
    Unwind *u = arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (!pc || u->n >= u->max) return _URC_END_OF_STACK;
    u->pcs[u->n++] = (void *)pc;
    return _URC_NO_REASON;
}

void plat_print_backtrace(void) {
    void *bt[64];
    Unwind u = {.pcs = bt, .n = 0, .max = 64};
    _Unwind_Backtrace(unwind_frame, &u);
    Dl_info self = {0};
    dladdr((void *)plat_print_backtrace, &self);
    for (int i = 0; i < u.n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        Dl_info di = {0};
        if (!dladdr(bt[i], &di)) di.dli_fname = NULL;
        bool ours = di.dli_fbase && di.dli_fbase == self.dli_fbase;
        const char *s = plat_symbol(pc, &off);
        if (!s && ours) s = rt_host_fn_name(pc, &off);
        const char *mod = di.dli_fname ? (strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname) : "?";
        uint64_t moff = pc - (uint64_t)(uintptr_t)di.dli_fbase;
        if (s && ours) fprintf(stderr, "  #%-2d %016llx %s+%llx (lasrecomp+%llx)\n", i, (unsigned long long)pc, s,
                               (unsigned long long)off, (unsigned long long)moff);
        else if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx (%s)\n", i, (unsigned long long)pc, s, (unsigned long long)off, mod);
        else fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, mod, (unsigned long long)moff);
    }
}

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    static volatile int depth;
    if (sig == SIGABRT && depth++ > 0) { signal(SIGABRT, SIG_DFL); abort(); }
    uint64_t fa = (uint64_t)(uintptr_t)si->si_addr;
    uint64_t pc = uc->uc_mcontext.pc, lr = uc->uc_mcontext.regs[30];
    if (rt_fault_watch(fa, pc)) return;
    char what[32];
    snprintf(what, sizeof what, "signal %d", sig);
    rt_fault_report(what, fa, pc, lr);
}

// Our handler runs first and debuggerd stays chained behind it, so a fault we do not recognise still produces the
// system tombstone. rt_fault_watch returning true (a guest page fault we handle) never reaches debuggerd at all.
void plat_crash_handler_install(void) {
    stack_t ss = {.ss_sp = malloc(1 << 20), .ss_size = 1 << 20};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

// ---- child processes: there are none
// An app cannot exec a binary of its own (W^X on the data directory), which is exactly what the start screen does
// on the desktop when it compiles the game code. On Android that library is built ahead of time and shipped, so
// nothing here should ever be called; failing cleanly is what lets the caller say so instead of crashing.
#include <ftw.h>
#include <sys/wait.h>

PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    (void)argv, (void)log_path, (void)append_log;
    errno = ENOSYS;
    return 0;
}
void plat_proc_kill(PlatProc p) { (void)p; }
int plat_proc_wait(PlatProc p, bool block) { (void)p, (void)block; return 255; }

static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb, (void)flag, (void)ftw;
    return remove(path) == 0 || errno == ENOENT ? 0 : -1;
}
bool plat_remove_tree(const char *dir) { return nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS) == 0; }
bool plat_onedrive_synced(const char *dir) { (void)dir; return false; }

void plat_terminate(void) { _exit(0); }

int plat_open_file(const char *path, int flags) { return open(path, flags, 0644); }
int plat_rename(const char *from, const char *to) {
    if (!access(to, F_OK)) { errno = EEXIST; return -1; }
    return rename(from, to);
}

bool plat_window_screen_hash(void *hwnd, uint32_t *out) { (void)hwnd, (void)out; return false; }
