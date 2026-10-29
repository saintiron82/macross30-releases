/*
 * ps3recomp - cellPad HLE implementation
 *
 * Reads real gamepad input from the host and translates to PS3 pad format.
 *
 * Backend selection:
 *   - Windows: SDL2 GameController when the build has SDL2
 *     (PS3RECOMP_PAD_HAVE_SDL2) and SDL2.dll loads -- DualSense, DualShock 4,
 *     Xbox, Switch Pro ... -- otherwise XInput. PAD_NO_SDL=1 forces XInput.
 *   - Everywhere else / if PS3RECOMP_PAD_USE_SDL2 is defined: SDL2 only.
 *   - Windows, either way: the keyboard (keymap.ini) merges into port 0.
 */

#include <time.h>
#include "cellPad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../runtime/platform/win32_compat.h"   /* GetTickCount64 on POSIX */
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_write*: guest EA -> host */
#include "ps3emu/log_first.h"
#include "pad_test.h"
#include "pad_padmap.h"
#include <sys/stat.h>

/* PAD_CLOCK=flips: PAD_SCRIPT and PAD_STICK times count guest flips instead of
 * wall-clock seconds. A loaded machine runs a title at a third of its usual
 * frame rate, and a wall-clock schedule then presses START on the logos; a
 * flip schedule replays the same screens however slow the run is. */
extern u32 cellGcm_flip_request_count(void);
static double pad_clock(unsigned long long t0_ms)
{
    static int flips = -1;
    if (flips < 0) { const char* e = getenv("PAD_CLOCK"); flips = e && !strcmp(e, "flips"); }
    return flips ? (double)cellGcm_flip_request_count()
                 : (double)(GetTickCount64() - t0_ms) / 1000.0;
}

/* ---------------------------------------------------------------------------
 * Backend selection
 * -----------------------------------------------------------------------*/

#if defined(PS3RECOMP_PAD_USE_SDL2)
  #define PAD_BACKEND_SDL2  1
  #define PAD_BACKEND_XINPUT 0
#elif defined(_WIN32)
  /* XInput always; SDL2 too when the build found it (PS3RECOMP_PAD_HAVE_SDL2,
   * see CMakeLists.txt). SDL2.dll is delay-loaded: with it present SDL2 runs
   * every controller, without it XInput takes over. */
  #define PAD_BACKEND_XINPUT 1
  #if defined(PS3RECOMP_PAD_HAVE_SDL2)
    #define PAD_BACKEND_SDL2 1
  #else
    #define PAD_BACKEND_SDL2 0
  #endif
#else
  #define PAD_BACKEND_SDL2  1
  #define PAD_BACKEND_XINPUT 0
#endif

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

#if PAD_BACKEND_XINPUT
  #include <xinput.h>
  #pragma comment(lib, "xinput.lib")
#endif

#if PAD_BACKEND_SDL2
  #if defined(__has_include)
    #if __has_include(<SDL2/SDL.h>)
      #include <SDL2/SDL.h>
    #else
      #include <SDL.h>          /* the Windows VC package: include/SDL.h */
    #endif
  #else
    #include <SDL2/SDL.h>
  #endif
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

#define PAD_MAX_HOST_PORTS  4  /* XInput supports max 4; SDL may support more */

typedef struct {
    int  connected;
    u16  buttons;           /* CELL_PAD_CTRL_* bitmask */
    u8   analog_lx;         /* 0-255, center=128 */
    u8   analog_ly;
    u8   analog_rx;
    u8   analog_ry;
    u8   trigger_l2;        /* 0-255 */
    u8   trigger_r2;        /* 0-255 */
    /* Pressure-sensitive face buttons (0-255) */
    u8   press_right;
    u8   press_left;
    u8   press_up;
    u8   press_down;
    u8   press_triangle;
    u8   press_circle;
    u8   press_cross;
    u8   press_square;
    u8   press_l1;
    u8   press_r1;
    u32  raw;               /* PAD_SRC_* physical buttons held (pad_padmap.h) */
} PadHostState;

static int           s_pad_initialized = 0;
static u32           s_max_connect = 0;
static u32           s_port_setting[CELL_PAD_MAX_PORT_NUM];
static PadHostState  s_host_state[PAD_MAX_HOST_PORTS];
/* Per-port "a report has arrived since your last read" flag; see cellPadGetData. */
static int s_data_fresh[PAD_MAX_HOST_PORTS];

/* SDL2 is running the controllers (else XInput, on Windows). */
static int s_use_sdl = 0;
/* Controller test window (pad_test.h): host state before the keyboard merge
 * (XInput's per-slot view), port state as the title would read it, and
 * whether the title gets neutral input meanwhile. */
static PadHostState s_view_pads[PAD_MAX_HOST_PORTS];
static PadHostState s_view_port[PAD_MAX_HOST_PORTS];
static volatile int s_test_block;

/* ---------------------------------------------------------------------------
 * Rumble (cellPadSetActDirect)
 *
 * A DualShock 3 has two motors: the small one (motor[0]) is on or off, the
 * large one (motor[1]) takes 0-255. A title sets them and they stay that way
 * until it sets them again; some titles only call when the value changes.
 * SDL's rumble runs for a duration instead, so the SDL pad thread re-sends
 * the last request per port while it is non-zero. If the title stops (a hang,
 * a closed window), the motors stop within PAD_RUMBLE_HOLD_MS. Large -> the
 * host's low-frequency motor, small -> the high-frequency one, which is how
 * DualShock 4, DualSense and Xbox pads are laid out too.
 * -------------------------------------------------------------------------*/

#define PAD_RUMBLE_HOLD_MS     1000   /* one SDL request lasts this long ... */
#define PAD_RUMBLE_RENEW_MS     400   /* ... and is renewed this often */

typedef struct {
    u16 lo, hi;                 /* what the title last asked the host motors for */
} PadRumble;
static PadRumble s_rumble[PAD_MAX_HOST_PORTS];

/* The two host motor levels (0-65535) for a PS3 actuator request. */
static void pad_rumble_levels(u8 small_on, u8 large, u16* lo, u16* hi)
{
    *lo = (u16)(large * 257u);
    *hi = small_on ? 0xFFFFu : 0u;
}

/* ---------------------------------------------------------------------------
 * Shared by the backends
 * -----------------------------------------------------------------------*/

/* Deadzone for analog sticks (same as XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) */
#define PAD_STICK_DEADZONE  7849
#define PAD_TRIGGER_THRESHOLD 30

/* -32768..32767 -> 0..255, center exactly 128 (never 127: see the XInput
 * stick comment below). */
static u8 pad_stick_to_u8(int raw, int deadzone)
{
    float normalized;
    if (raw > deadzone)
        normalized = (float)(raw - deadzone) / (float)(32767 - deadzone);
    else if (raw < -deadzone)
        normalized = (float)(raw + deadzone) / (float)(32767 - deadzone);
    else
        normalized = 0.0f;

    /* Map -1.0..1.0 to 0..255 with center at 128 */
    int val = (int)(normalized * 127.0f) + 128;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

/* Pressure-sensitive buttons: host pads are digital, so 0 or 255 */
static void pad_fill_pressure(PadHostState* s)
{
    const u16 btns = s->buttons;
    s->press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
    s->press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
    s->press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
    s->press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
    s->press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
    s->press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
    s->press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
    s->press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
    s->press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
    s->press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;
}

static void pad_test_state(PadTestState* t, const PadHostState* s)
{
    t->connected = s->connected;
    t->buttons = s->buttons;
    t->lx = s->analog_lx; t->ly = s->analog_ly;
    t->rx = s->analog_rx; t->ry = s->analog_ry;
    t->l2 = s->trigger_l2; t->r2 = s->trigger_r2;
    t->raw = s->raw;
}

/* Controller button mapping (padmap.ini, pad_padmap.h; edited in the
 * options window). Re-read when the file changes, checked once a second
 * from whichever thread polls the pads. */
static int s_pmap[PAD_SRC_COUNT];
static void pad_pmap_refresh(void)
{
    static int loaded;
    static unsigned long long checked;
    static time_t stamp;
    static long size;
    const unsigned long long now = (unsigned long long)GetTickCount64();
    if (loaded && now - checked < 1000) return;
    checked = now;
    struct stat st;
    const time_t t = stat("padmap.ini", &st) == 0 ? st.st_mtime : 0;
    const long sz = t ? (long)st.st_size : -1;
    if (loaded && t == stamp && sz == size) return;
    int m[PAD_SRC_COUNT];
    pad_pm_load(m);
    memcpy(s_pmap, m, sizeof m);
    if (loaded) { printf("[cellPad] padmap.ini reloaded\n"); fflush(stdout); }
    stamp = t; size = sz; loaded = 1;
}

/* Raw physical state -> PS3 buttons and L2/R2 through the mapping. */
static void pad_apply_pmap(PadHostState* d, u32 raw, u8 lt, u8 rt)
{
    if (lt > PAD_PM_TRIGGER_THRESHOLD) raw |= 1u << PAD_SRC_LT;
    if (rt > PAD_PM_TRIGGER_THRESHOLD) raw |= 1u << PAD_SRC_RT;
    d->raw = raw;
    pad_pm_apply(s_pmap, raw, lt, rt, &d->buttons, &d->trigger_l2, &d->trigger_r2);
}

/* ---------------------------------------------------------------------------
 * XInput backend
 * -----------------------------------------------------------------------*/

#if PAD_BACKEND_XINPUT

static void pad_poll_xinput(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        XINPUT_STATE state;
        memset(&state, 0, sizeof(state));

        DWORD result = XInputGetState((DWORD)i, &state);
        if (result != ERROR_SUCCESS) {
            s_host_state[i].connected = 0;
            continue;
        }

        s_host_state[i].connected = 1;
        XINPUT_GAMEPAD* gp = &state.Gamepad;

        /* XInput buttons -> physical-button bits -> PS3 buttons (padmap.ini) */
        static const struct { WORD x; int src; } map[] = {
            { XINPUT_GAMEPAD_A, PAD_SRC_A }, { XINPUT_GAMEPAD_B, PAD_SRC_B },
            { XINPUT_GAMEPAD_X, PAD_SRC_X }, { XINPUT_GAMEPAD_Y, PAD_SRC_Y },
            { XINPUT_GAMEPAD_BACK, PAD_SRC_BACK }, { XINPUT_GAMEPAD_START, PAD_SRC_START },
            { XINPUT_GAMEPAD_LEFT_THUMB, PAD_SRC_LS }, { XINPUT_GAMEPAD_RIGHT_THUMB, PAD_SRC_RS },
            { XINPUT_GAMEPAD_LEFT_SHOULDER, PAD_SRC_LB }, { XINPUT_GAMEPAD_RIGHT_SHOULDER, PAD_SRC_RB },
            { XINPUT_GAMEPAD_DPAD_UP, PAD_SRC_UP }, { XINPUT_GAMEPAD_DPAD_DOWN, PAD_SRC_DOWN },
            { XINPUT_GAMEPAD_DPAD_LEFT, PAD_SRC_LEFT }, { XINPUT_GAMEPAD_DPAD_RIGHT, PAD_SRC_RIGHT },
        };
        u32 raw = 0;
        for (size_t m = 0; m < sizeof map / sizeof map[0]; m++)
            if (gp->wButtons & map[m].x) raw |= 1u << map[m].src;
        pad_pmap_refresh();
        pad_apply_pmap(&s_host_state[i], raw, gp->bLeftTrigger, gp->bRightTrigger);

        /* Analog sticks. PS3 Y axis is inverted vs XInput (up = 0). Reflect about
         * 128 (256 - x), NOT 255 - x: the latter turns a centered stick into 127,
         * whose bits (0x7F) alias SELECT+START and can make the guest self-exit.
         * (via sagemono, PR #42) */
        s_host_state[i].analog_lx = pad_stick_to_u8(gp->sThumbLX, PAD_STICK_DEADZONE);
        s_host_state[i].analog_ly = (u8)(256 - pad_stick_to_u8(gp->sThumbLY, PAD_STICK_DEADZONE));
        s_host_state[i].analog_rx = pad_stick_to_u8(gp->sThumbRX, PAD_STICK_DEADZONE);
        s_host_state[i].analog_ry = (u8)(256 - pad_stick_to_u8(gp->sThumbRY, PAD_STICK_DEADZONE));

        pad_fill_pressure(&s_host_state[i]);
    }
}

/* XInput keeps the motors at what they were last set to: no renewal. */
static void pad_xinput_rumble(int port)
{
    if (!s_host_state[port].connected) return;
    XINPUT_VIBRATION vib;
    vib.wLeftMotorSpeed  = (WORD)s_rumble[port].lo;
    vib.wRightMotorSpeed = (WORD)s_rumble[port].hi;
    XInputSetState((DWORD)port, &vib);
}

#endif /* PAD_BACKEND_XINPUT */

/* ---------------------------------------------------------------------------
 * SDL2 backend
 *
 * SDL2's GameController API (libsdl-org/SDL, zlib license) gives every pad it
 * knows -- DualSense, DualShock 4, Switch Pro, Xbox, 8BitDo ... over USB or
 * Bluetooth -- one button layout, and handles hotplug. Its positional names
 * match the PS3: A = bottom = cross, B = right = circle, X = left = square,
 * Y = top = triangle; BACK (Create / View) = SELECT, START (Options / Menu)
 * = START, and the DualSense / DS4 touchpad click is SELECT too.
 *
 * All SDL calls are made on one thread of its own. SDL's Windows device
 * discovery (HIDAPI, device notifications, raw input) runs on hidden windows
 * that only see messages on the thread that initialised SDL, while cellPad is
 * called from whichever guest thread reads the pad. The thread polls at
 * PAD_SDL_POLL_MS, pumps its own messages, renews rumble, and publishes a
 * snapshot that pad_poll_sdl2 copies under a lock.
 *
 * Every controller goes to port 0 (buttons OR'ed, each stick from whichever
 * pad is pushed further, triggers the larger), so any pad picked up drives
 * the game whatever else is plugged in. PAD_SEPARATE_PORTS=1 gives each its
 * own port instead (first free, in connection order).
 * -----------------------------------------------------------------------*/

#if PAD_BACKEND_SDL2

#define PAD_SDL_MAX_DEVICES 8
#define PAD_SDL_POLL_MS     4

static SDL_mutex*   s_sdl_lock;
static SDL_Thread*  s_sdl_thread;
static SDL_atomic_t s_sdl_quit;
static int          s_sdl_ok;                          /* SDL init result */
static PadHostState s_sdl_snap[PAD_MAX_HOST_PORTS];    /* under s_sdl_lock */
static PadTestDevice s_sdl_view[PAD_SDL_MAX_DEVICES];   /* under s_sdl_lock; controller test */
static char         s_sdl_ver[16];

/* pad thread only */
static SDL_GameController* s_sdl_dev[PAD_SDL_MAX_DEVICES];
static PadRumble           s_sdl_sent[PAD_SDL_MAX_DEVICES];
static Uint64              s_sdl_sent_ms[PAD_SDL_MAX_DEVICES];
static int                 s_sdl_separate;
static int                 s_sdl_count = -1;
static PadTestDevice       s_sdl_info[PAD_SDL_MAX_DEVICES];  /* name/type per open pad */

static const char* pad_sdl_type_name(SDL_GameController* gc)
{
    switch (SDL_GameControllerGetType(gc)) {
    case SDL_CONTROLLER_TYPE_PS5:          return "DualSense";
    case SDL_CONTROLLER_TYPE_PS4:          return "DualShock 4";
    case SDL_CONTROLLER_TYPE_PS3:          return "DualShock 3";
    case SDL_CONTROLLER_TYPE_XBOXONE:      return "Xbox One/Series";
    case SDL_CONTROLLER_TYPE_XBOX360:      return "Xbox 360";
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: return "Switch Pro";
    default:                               return "controller";
    }
}

/* Close detached pads, open new ones. Cheap enough to skip only when the
 * device count is unchanged and nothing went away. */
static void pad_sdl_scan(void)
{
    int removed = 0;
    for (int k = 0; k < PAD_SDL_MAX_DEVICES; k++) {
        if (s_sdl_dev[k] && !SDL_GameControllerGetAttached(s_sdl_dev[k])) {
            printf("[cellPad] SDL2: controller %d removed\n", k);
            SDL_GameControllerClose(s_sdl_dev[k]);
            s_sdl_dev[k] = NULL;
            memset(&s_sdl_info[k], 0, sizeof s_sdl_info[k]);
            removed = 1;
        }
    }
    const int n = SDL_NumJoysticks();
    if (n == s_sdl_count && !removed) return;
    s_sdl_count = n;

    for (int j = 0; j < n; j++) {
        if (!SDL_IsGameController(j)) continue;
        const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(j);
        int have = 0, free_k = -1;
        for (int k = 0; k < PAD_SDL_MAX_DEVICES; k++) {
            if (!s_sdl_dev[k]) { if (free_k < 0) free_k = k; continue; }
            if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_sdl_dev[k])) == id) have = 1;
        }
        if (have || free_k < 0) continue;
        SDL_GameController* gc = SDL_GameControllerOpen(j);
        if (!gc) continue;
        s_sdl_dev[free_k] = gc;
        memset(&s_sdl_sent[free_k], 0, sizeof s_sdl_sent[free_k]);
        s_sdl_sent_ms[free_k] = 0;
        SDL_GameControllerSetPlayerIndex(gc, s_sdl_separate ? free_k : 0);
        snprintf(s_sdl_info[free_k].name, sizeof s_sdl_info[free_k].name, "%s",
                 SDL_GameControllerName(gc) ? SDL_GameControllerName(gc) : "?");
        snprintf(s_sdl_info[free_k].type, sizeof s_sdl_info[free_k].type, "%s", pad_sdl_type_name(gc));
        printf("[cellPad] SDL2: controller %d = %s (%s)\n", free_k,
               SDL_GameControllerName(gc) ? SDL_GameControllerName(gc) : "?",
               pad_sdl_type_name(gc));
    }
    fflush(stdout);
}

static u8 pad_sdl_trigger_to_u8(int raw)
{
    /* SDL trigger: 0..32767 -> 0..255 */
    int val = (raw * 255) / 32767;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

static void pad_sdl_read(SDL_GameController* gc, PadHostState* d)
{
    /* SDL buttons -> physical-button bits -> PS3 buttons (padmap.ini; the
     * defaults are the positional layout described above). */
    static const struct { SDL_GameControllerButton b; int src; } map[] = {
        { SDL_CONTROLLER_BUTTON_A, PAD_SRC_A }, { SDL_CONTROLLER_BUTTON_B, PAD_SRC_B },
        { SDL_CONTROLLER_BUTTON_X, PAD_SRC_X }, { SDL_CONTROLLER_BUTTON_Y, PAD_SRC_Y },
        { SDL_CONTROLLER_BUTTON_BACK, PAD_SRC_BACK }, { SDL_CONTROLLER_BUTTON_START, PAD_SRC_START },
        { SDL_CONTROLLER_BUTTON_LEFTSTICK, PAD_SRC_LS }, { SDL_CONTROLLER_BUTTON_RIGHTSTICK, PAD_SRC_RS },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, PAD_SRC_LB }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, PAD_SRC_RB },
        { SDL_CONTROLLER_BUTTON_DPAD_UP, PAD_SRC_UP }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, PAD_SRC_DOWN },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT, PAD_SRC_LEFT }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, PAD_SRC_RIGHT },
        { SDL_CONTROLLER_BUTTON_TOUCHPAD, PAD_SRC_TOUCHPAD }, { SDL_CONTROLLER_BUTTON_MISC1, PAD_SRC_MISC1 },
        { SDL_CONTROLLER_BUTTON_PADDLE1, PAD_SRC_PADDLE1 }, { SDL_CONTROLLER_BUTTON_PADDLE2, PAD_SRC_PADDLE2 },
        { SDL_CONTROLLER_BUTTON_PADDLE3, PAD_SRC_PADDLE3 }, { SDL_CONTROLLER_BUTTON_PADDLE4, PAD_SRC_PADDLE4 },
    };
    u32 raw = 0;
    for (size_t m = 0; m < sizeof map / sizeof map[0]; m++)
        if (SDL_GameControllerGetButton(gc, map[m].b)) raw |= 1u << map[m].src;
    pad_apply_pmap(d, raw,
                   pad_sdl_trigger_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT)),
                   pad_sdl_trigger_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT)));

    /* SDL's Y axes already point down like the PS3's (up = 0). */
    d->analog_lx = pad_stick_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX),  PAD_STICK_DEADZONE);
    d->analog_ly = pad_stick_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY),  PAD_STICK_DEADZONE);
    d->analog_rx = pad_stick_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX), PAD_STICK_DEADZONE);
    d->analog_ry = pad_stick_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY), PAD_STICK_DEADZONE);
    d->connected = 1;
}

/* Fold pad d into port state p (merged mode). */
static void pad_merge(PadHostState* p, const PadHostState* d)
{
    if (!p->connected) { *p = *d; return; }
#define FURTHER(a, b) (abs((int)(a) - 128) >= abs((int)(b) - 128) ? (a) : (b))
    p->buttons  |= d->buttons;
    p->raw      |= d->raw;
    p->analog_lx = FURTHER(p->analog_lx, d->analog_lx);
    p->analog_ly = FURTHER(p->analog_ly, d->analog_ly);
    p->analog_rx = FURTHER(p->analog_rx, d->analog_rx);
    p->analog_ry = FURTHER(p->analog_ry, d->analog_ry);
#undef FURTHER
    if (d->trigger_l2 > p->trigger_l2) p->trigger_l2 = d->trigger_l2;
    if (d->trigger_r2 > p->trigger_r2) p->trigger_r2 = d->trigger_r2;
}

/* Send each pad its port's rumble when it changes, and renew it while on. */
static void pad_sdl_rumble_update(void)
{
    const Uint64 now = SDL_GetTicks64();
    for (int k = 0; k < PAD_SDL_MAX_DEVICES; k++) {
        if (!s_sdl_dev[k]) continue;
        const int port = s_sdl_separate ? k : 0;
        if (port >= PAD_MAX_HOST_PORTS) continue;
        const PadRumble want = s_rumble[port];      /* benign race: two u16 */
        const int on = want.lo || want.hi;
        const int changed = want.lo != s_sdl_sent[k].lo || want.hi != s_sdl_sent[k].hi;
        if (!changed && !(on && now - s_sdl_sent_ms[k] >= PAD_RUMBLE_RENEW_MS)) continue;
        SDL_GameControllerRumble(s_sdl_dev[k], want.lo, want.hi, on ? PAD_RUMBLE_HOLD_MS : 0);
        s_sdl_sent[k] = want;
        s_sdl_sent_ms[k] = now;
    }
}

static int SDLCALL pad_sdl_thread(void* arg)
{
    SDL_sem* ready = (SDL_sem*)arg;
    /* No SDL window exists to have focus; read pads like XInput does. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    s_sdl_ok = SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0;
    if (!s_sdl_ok) {
        printf("[cellPad] SDL2 init failed: %s\n", SDL_GetError());
    } else {
        /* State is read directly; nobody drains SDL's event queue. */
        SDL_JoystickEventState(SDL_IGNORE);
        SDL_GameControllerEventState(SDL_IGNORE);
    }
    SDL_SemPost(ready);
    if (!s_sdl_ok) return 0;

    while (!SDL_AtomicGet(&s_sdl_quit)) {
#ifdef _WIN32
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
#endif
        SDL_GameControllerUpdate();
        pad_sdl_scan();
        pad_pmap_refresh();

        PadHostState st[PAD_MAX_HOST_PORTS];
        PadTestDevice view[PAD_SDL_MAX_DEVICES];
        memset(st, 0, sizeof st);
        memset(view, 0, sizeof view);
        for (int k = 0; k < PAD_SDL_MAX_DEVICES; k++) {
            if (!s_sdl_dev[k]) continue;
            PadHostState d;
            memset(&d, 0, sizeof d);
            pad_sdl_read(s_sdl_dev[k], &d);
            view[k] = s_sdl_info[k];
            pad_test_state(&view[k].st, &d);
            const int port = s_sdl_separate ? k : 0;
            if (port >= PAD_MAX_HOST_PORTS) continue;
            pad_merge(&st[port], &d);
        }
        for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) pad_fill_pressure(&st[i]);
        pad_sdl_rumble_update();

        SDL_LockMutex(s_sdl_lock);
        memcpy(s_sdl_snap, st, sizeof st);
        memcpy(s_sdl_view, view, sizeof view);
        SDL_UnlockMutex(s_sdl_lock);
        SDL_Delay(PAD_SDL_POLL_MS);
    }

    for (int k = 0; k < PAD_SDL_MAX_DEVICES; k++) {
        if (!s_sdl_dev[k]) continue;
        SDL_GameControllerRumble(s_sdl_dev[k], 0, 0, 0);
        SDL_GameControllerClose(s_sdl_dev[k]);
        s_sdl_dev[k] = NULL;
    }
    s_sdl_count = -1;
    memset(s_sdl_info, 0, sizeof s_sdl_info);
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    return 0;
}

/* Start the pad thread; 0 if SDL2 cannot run (then XInput, on Windows). */
static int pad_sdl_start(void)
{
#if PAD_BACKEND_XINPUT
    if (getenv("PAD_NO_SDL")) {
        printf("[cellPad] PAD_NO_SDL: XInput only\n");
        return 0;
    }
    /* Delay-loaded: no SDL call may run unless the DLL is there. */
    if (!LoadLibraryW(L"SDL2.dll")) {
        printf("[cellPad] SDL2.dll not found: XInput only (Xbox pads)\n");
        return 0;
    }
#endif
    SDL_version v;
    SDL_GetVersion(&v);
    snprintf(s_sdl_ver, sizeof s_sdl_ver, "%d.%d.%d", v.major, v.minor, v.patch);
    memset(s_sdl_view, 0, sizeof s_sdl_view);
    s_sdl_separate = getenv("PAD_SEPARATE_PORTS") != NULL;
    memset(s_sdl_snap, 0, sizeof s_sdl_snap);
    SDL_AtomicSet(&s_sdl_quit, 0);
    s_sdl_lock = SDL_CreateMutex();
    SDL_sem* ready = SDL_CreateSemaphore(0);
    s_sdl_thread = s_sdl_lock && ready ? SDL_CreateThread(pad_sdl_thread, "cellPad SDL2", ready) : NULL;
    if (s_sdl_thread) SDL_SemWait(ready);
    if (ready) SDL_DestroySemaphore(ready);
    if (!s_sdl_thread || !s_sdl_ok) {
        if (s_sdl_thread) SDL_WaitThread(s_sdl_thread, NULL);
        s_sdl_thread = NULL;
        if (s_sdl_lock) SDL_DestroyMutex(s_sdl_lock);
        s_sdl_lock = NULL;
        return 0;
    }
    printf("[cellPad] SDL2 %d.%d.%d controllers%s\n", v.major, v.minor, v.patch,
           s_sdl_separate ? " (one port each)" : " (all on port 0)");
    fflush(stdout);
    return 1;
}

static void pad_sdl_stop(void)
{
    if (!s_sdl_thread) return;
    SDL_AtomicSet(&s_sdl_quit, 1);
    SDL_WaitThread(s_sdl_thread, NULL);
    s_sdl_thread = NULL;
    SDL_DestroyMutex(s_sdl_lock);
    s_sdl_lock = NULL;
}

static void pad_poll_sdl2(void)
{
    SDL_LockMutex(s_sdl_lock);
    memcpy(s_host_state, s_sdl_snap, sizeof s_host_state);
    SDL_UnlockMutex(s_sdl_lock);
}

#endif /* PAD_BACKEND_SDL2 */

static void pad_init_backend(void)
{
#if PAD_BACKEND_SDL2
    s_use_sdl = pad_sdl_start();
#endif
}

static void pad_shutdown_backend(void)
{
#if PAD_BACKEND_SDL2
    if (s_use_sdl) pad_sdl_stop();
    s_use_sdl = 0;
#endif
    memset(s_rumble, 0, sizeof s_rumble);
}

/* ---------------------------------------------------------------------------
 * Keyboard (Windows), port 0
 *
 * On a machine with no controller plugged in a title would get a pad that is
 * reported present and never presses anything -- it can be watched but not
 * played. That is what The Simpsons Arcade Game hit: it reached its attract
 * loop and no input existed to start a game.
 *
 * With a controller on port 0 the keyboard is merged into it (buttons OR'ed,
 * a stick direction held on the keyboard wins); without one the keyboard is
 * the whole pad. Keys are read only while a window of THIS process is in the
 * foreground, so typing in another application does not drive the game. Set
 * PAD_NO_KEYBOARD=1 to disable it entirely.
 *
 * Keys come from keymap.ini (pad_keymap.h; edited in the options window,
 * re-read here when it changes). Defaults: arrows = d-pad, Z/X/A/S =
 * cross/circle/square/triangle, Q/W = L1/R1, 1/2 = L2/R2, C/V = L3/R3,
 * Enter or Esc = START, Tab = SELECT, I/J/K/L = right stick. With no key on
 * the left stick it mirrors the d-pad, so a title that reads the stick
 * instead is playable too.
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
#include "pad_keymap.h"
/* Port 0 state came from the keyboard alone (no controller this sweep): the
 * backends reset port 0 each sweep, so this tells a stale keyboard-made
 * "connected" from a real pad. */
static int s_kb_only_port0;
static int s_kb_active;   /* a mapped key was down last sweep (controller test) */
static int pad_host_window_focused(void)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return 0;
    /* Not while the options / key-setting windows have focus: a key pressed
     * there to bind it must not also play the game. */
    wchar_t cls[64];
    if (GetClassNameW(fg, cls, 64) && !wcsncmp(cls, L"ps3recomp_opt", 13)) return 0;
    return 1;
}

static void pad_poll_keyboard(void)
{
    static int off = -1;
    if (off < 0) off = getenv("PAD_NO_KEYBOARD") ? 1 : 0;
    if (off) return;

    PadHostState* hs = &s_host_state[0];
    const int pad = hs->connected && !s_kb_only_port0;
    s_kb_active = 0;
    if (!pad_host_window_focused()) {
        /* Release everything on focus loss, or a key held while alt-tabbing
         * would stay down forever. */
        if (!pad) {
            hs->buttons = 0;
            hs->analog_lx = hs->analog_ly = 128;
            hs->analog_rx = hs->analog_ry = 128;
            hs->connected = 1;
            s_kb_only_port0 = 1;
        }
        return;
    }

    /* keymap.ini, re-read when its timestamp changes (checked once a second). */
    static int vk[PAD_KM_COUNT]; static int loaded; static ULONGLONG chk; static FILETIME stamp;
    { const ULONGLONG now = GetTickCount64();
      if (!loaded || now - chk > 1000) {
          chk = now;
          WIN32_FILE_ATTRIBUTE_DATA fa; FILETIME t = {0, 0};
          if (GetFileAttributesExA("keymap.ini", GetFileExInfoStandard, &fa)) t = fa.ftLastWriteTime;
          if (!loaded || CompareFileTime(&t, &stamp) != 0) {
              pad_km_load(vk); stamp = t;
              if (loaded) { printf("[cellPad] keymap.ini reloaded\n"); fflush(stdout); }
              loaded = 1;
          }
      } }

    u16 btns = 0;
    int ax[5] = {0, 0, 0, 0, 0}, ls_bound = 0;
    for (int i = 0; i < PAD_KM_COUNT; i++) {
        if (k_pad_km[i].axis == 1 || k_pad_km[i].axis == 2) ls_bound |= vk[i] != 0;
        if (!vk[i] || !(GetAsyncKeyState(vk[i]) & 0x8000)) continue;
        if (k_pad_km[i].axis) ax[k_pad_km[i].axis] += k_pad_km[i].dir;
        else btns |= k_pad_km[i].btn;
        s_kb_active = 1;
    }

    if (pad) {
        hs->buttons |= btns;
        u8* dst[5] = { NULL, &hs->analog_lx, &hs->analog_ly, &hs->analog_rx, &hs->analog_ry };
        if (!ls_bound) {
            ax[1] = (btns & CELL_PAD_CTRL_LEFT) ? -1 : (btns & CELL_PAD_CTRL_RIGHT) ? 1 : 0;
            ax[2] = (btns & CELL_PAD_CTRL_UP) ? -1 : (btns & CELL_PAD_CTRL_DOWN) ? 1 : 0;
        }
        for (int a = 1; a <= 4; a++) if (ax[a]) *dst[a] = (u8)(ax[a] < 0 ? 0 : 255);
        if (btns & CELL_PAD_CTRL_L2) hs->trigger_l2 = 255;
        if (btns & CELL_PAD_CTRL_R2) hs->trigger_r2 = 255;
        if (btns) { static int said = 0;
            if (!said) { said = 1; printf("[cellPad] keyboard merged into port 0 (with a controller)\n"); fflush(stdout); } }
        return;
    }
    s_kb_only_port0 = 1;
    hs->buttons   = btns;
    hs->connected = 1;
    if (!ls_bound) {
        ax[1] = (btns & CELL_PAD_CTRL_LEFT) ? -1 : (btns & CELL_PAD_CTRL_RIGHT) ? 1 : 0;
        ax[2] = (btns & CELL_PAD_CTRL_UP) ? -1 : (btns & CELL_PAD_CTRL_DOWN) ? 1 : 0;
    }
#define KM_AXIS(v) (u8)((v) < 0 ? 0 : (v) > 0 ? 255 : 128)
    hs->analog_lx = KM_AXIS(ax[1]);
    hs->analog_ly = KM_AXIS(ax[2]);
    hs->analog_rx = KM_AXIS(ax[3]);
    hs->analog_ry = KM_AXIS(ax[4]);
#undef KM_AXIS
    hs->trigger_l2 = (u8)((btns & CELL_PAD_CTRL_L2) ? 255 : 0);
    hs->trigger_r2 = (u8)((btns & CELL_PAD_CTRL_R2) ? 255 : 0);

    { static int said = 0;
      if (!said && btns) { said = 1;
          printf("[cellPad] keyboard fallback active on port 0 (no controller)\n");
          fflush(stdout); } }
}
#endif

/* ---------------------------------------------------------------------------
 * Poll dispatcher
 * -----------------------------------------------------------------------*/

/* Re-poll the host at most every PAD_POLL_INTERVAL_MS; serve cached state in
 * between.
 *
 * XInputGetState is a driver call, and on a slot with nothing plugged in it is
 * an expensive one: measured at 97.6 us for a sweep of the 7 host ports on a
 * machine with no controller attached. cellPadGetData ran that sweep on EVERY
 * call, and a guest that polls the pad from a spin loop -- The Simpsons Arcade
 * Game does, in its input stage -- therefore spends its whole main thread
 * inside the input driver.
 *
 * 4 ms is 250 Hz, well above the 60 Hz a title can actually observe (and above
 * a real DualShock 3's own report rate), so no game can tell the difference,
 * including one sampling for button-press edges. */
#define PAD_POLL_INTERVAL_MS 4

static unsigned long long pad_now_ms(void)
{
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull +
           (unsigned long long)ts.tv_nsec / 1000000ull;
#endif
}

static void pad_poll_backend(void)
{
    /* Benign race: two threads may poll in the same tick. That costs one extra
     * sweep, never a missed or stale-forever read. */
    static unsigned long long s_last_ms = 0;
    unsigned long long now = pad_now_ms();
    if (s_last_ms && now - s_last_ms < PAD_POLL_INTERVAL_MS) return;
    s_last_ms = now;
    /* A real pad streams reports at roughly this rate whether or not anything
     * changed, and libpad buffers them; a fresh sweep is a fresh report for
     * every port. cellPadGetData hands each one out exactly once. */
    for (int _i = 0; _i < PAD_MAX_HOST_PORTS; _i++) s_data_fresh[_i] = 1;

#if PAD_BACKEND_SDL2
    if (s_use_sdl) pad_poll_sdl2();
#endif
#if PAD_BACKEND_XINPUT
    if (!s_use_sdl) pad_poll_xinput();
#endif
    memcpy(s_view_pads, s_host_state, sizeof s_view_pads);   /* controller test, XInput */
#ifdef _WIN32
    if (s_host_state[0].connected) s_kb_only_port0 = 0;   /* a real pad this sweep */
    pad_poll_keyboard();   /* merged into port 0 with or without a pad */
#endif
    memcpy(s_view_port, s_host_state, sizeof s_view_port);
    if (s_test_block) {
        /* The controller test window is open: the title sees released
         * buttons and centred sticks (still connected). */
        for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
            PadHostState* h = &s_host_state[i];
            const int c = h->connected;
            memset(h, 0, sizeof *h);
            h->connected = c;
            h->analog_lx = h->analog_ly = h->analog_rx = h->analog_ry = 128;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Controller test (pad_test.h)
 * -----------------------------------------------------------------------*/

void cellPad_test_info(PadTestInfo* out)
{
    memset(out, 0, sizeof *out);
    if (!s_pad_initialized) return;
    pad_poll_backend();
    out->blocked = s_test_block;
#ifdef _WIN32
    out->keyboard = s_kb_active;
#endif
    pad_test_state(&out->port0, &s_view_port[0]);
#if PAD_BACKEND_SDL2
    if (s_use_sdl) {
        out->backend = 2;
        snprintf(out->sdl_version, sizeof out->sdl_version, "%s", s_sdl_ver);
        PadTestDevice view[PAD_SDL_MAX_DEVICES];
        SDL_LockMutex(s_sdl_lock);
        memcpy(view, s_sdl_view, sizeof view);
        SDL_UnlockMutex(s_sdl_lock);
        for (int k = 0; k < PAD_SDL_MAX_DEVICES && out->ndev < PAD_TEST_MAX_DEVICES; k++)
            if (view[k].st.connected) out->dev[out->ndev++] = view[k];
        return;
    }
#endif
#if PAD_BACKEND_XINPUT
    out->backend = 1;
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        if (!s_view_pads[i].connected) continue;
        PadTestDevice* d = &out->dev[out->ndev++];
        snprintf(d->name, sizeof d->name, "XInput controller %d", i + 1);
        snprintf(d->type, sizeof d->type, "Xbox (XInput)");
        pad_test_state(&d->st, &s_view_pads[i]);
    }
#endif
}

void cellPad_test_block(int on)
{
    /* counted: the test and pad-setting windows can both be open */
    s_test_block += on ? 1 : -1;
    if (s_test_block < 0) s_test_block = 0;
}

void cellPad_test_rumble(int on)
{
    static PadRumble saved;
    static int active;
    if (!s_pad_initialized || !on == !active) return;
    if (on) {
        saved = s_rumble[0];
        s_rumble[0].lo = 0xC000;
        s_rumble[0].hi = 0xFFFF;
    } else {
        s_rumble[0] = saved;
    }
    active = on;
#if PAD_BACKEND_XINPUT
    if (!s_use_sdl) {
        /* every XInput pad, not just port 0 */
        for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
            if (!s_view_pads[i].connected) continue;
            XINPUT_VIBRATION vib;
            vib.wLeftMotorSpeed  = on ? 0xC000 : (WORD)s_rumble[i].lo;
            vib.wRightMotorSpeed = on ? 0xFFFF : (WORD)s_rumble[i].hi;
            XInputSetState((DWORD)i, &vib);
        }
    }
#endif
    /* SDL2: the pad thread sends port 0's rumble to every merged pad. */
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellPadInit(u32 max_connect)
{
    printf("[cellPad] Init(max_connect=%u)\n", max_connect);

    if (s_pad_initialized)
        return CELL_PAD_ERROR_ALREADY_OPENED;

    /* The bound is the 127-slot pad table, not the 7 ports: larger requests
     * are accepted and clamped. Resistance: Fall of Man asks for 8; rejected,
     * the library stayed closed, every GetInfo failed and the title never
     * polled the pad -- stuck on "Press the START button". */
    if (max_connect == 0 || max_connect > 127)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_pad_initialized = 1;
    s_max_connect = max_connect > CELL_PAD_MAX_PORT_NUM ? CELL_PAD_MAX_PORT_NUM : max_connect;
    memset(s_port_setting, 0, sizeof(s_port_setting));
    memset(s_host_state, 0, sizeof(s_host_state));

    pad_init_backend();

    /* Do an initial poll to detect connected controllers */
    pad_poll_backend();

    return CELL_OK;
}

s32 cellPadEnd(void)
{
    printf("[cellPad] End()\n");

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    pad_shutdown_backend();

    s_pad_initialized = 0;
    s_max_connect = 0;
    return CELL_OK;
}

void cellPad_poll(void)
{
    if (s_pad_initialized) {
        pad_poll_backend();
    }
}

/* HLE args are GUEST effective addresses; guest structs are BIG-ENDIAN. The
 * vm_write helpers from ppu_memory.h translate + byte-swap. (cellPad was
 * written for host pointers and was never actually invoked until its NIDs
 * were fixed.) These used to be redeclared here with a 64-bit address
 * parameter, which conflicts with the header now that it is included. */

typedef struct {
    unsigned mask;
    int polls, have_axes;
    u8 axes[4]; /* lx,ly,rx,ry */
} PadFileCommand;

/* Accept only legacy mask/[polls], or all six fields. A partial analog
 * request must never degrade to an unintended digital press. */
static int pad_file_parse(const char* text, PadFileCommand* result)
{
    long long values[6]; unsigned count = 0;
    const char* p = text;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        if (count == 6) return 0;
        char* end;
        values[count] = strtoll(p, &end, 0);
        if (end == p || (*end && *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n')) return 0;
        count++; p = end;
    }
    if (count != 1 && count != 2 && count != 6) return 0;
    if (values[0] < 0 || (unsigned long long)values[0] > 0xFFFFFFFFull) return 0;
    PadFileCommand command; memset(&command, 0, sizeof command);
    command.mask = (unsigned)values[0];
    command.polls = count > 1 && values[1] > 0 && values[1] <= 0x7FFFFFFFll ? (int)values[1] : 40;
    if (count == 6) {
        if (values[1] < 1 || values[1] > 120) return 0;
        for (unsigned i = 0; i < 4; i++) {
            if (values[i + 2] < 0 || values[i + 2] > 255) return 0;
            command.axes[i] = (u8)values[i + 2];
        }
        command.have_axes = 1;
    } else if (!command.mask) return 0;
    *result = command;
    return 1;
}
static void pad_file_serialize_axes(u16* button, const u8 axes[4])
{
    button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X] = axes[0];
    button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y] = axes[1];
    button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] = axes[2];
    button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] = axes[3];
}

s32 cellPadGetData(u32 port_no, CellPadData* data_guest)
{
    { static int _once = 0;
      if (!_once++) printf("[cellPad] GetData polling begins (port %u)\n", port_no); }
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= s_max_connect || !data_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Build in a local host struct, then copy to guest memory big-endian. */
    CellPadData _d; CellPadData* data = &_d;
    int file_release_mask = -1;
    memset(data, 0, sizeof(CellPadData));

    /* Poll fresh state */
    pad_poll_backend();

    /* YDKJ_INJECT_PAD: the game's wait loop polls the pad (cellPadSetActDirect /
     * GetData) -- if it's parked on a "press button" prompt after loading, no
     * host input means it waits forever. Inject a real button PULSE (CROSS+START+
     * CIRCLE) so it advances. Legit input simulation, not forged pixels. */
    /* NOTE: cache getenv() -- this runs on every poll from multiple threads, and the
     * uncached version crashed deterministically (varying fault address = race). Every
     * other probe in this tree uses the cached `static int x=-1` pattern; match it. */
    static int  s_inj   = -1;
    static u16  s_btn   = 0x0008u;   /* default START */
    static long s_delay = 3000;
    if (s_inj < 0) {
        s_inj = getenv("YDKJ_INJECT_PAD") ? 1 : 0;
        const char* be = getenv("YDKJ_PAD_BTN");   if (be) s_btn   = (u16)strtoul(be,0,0);
        const char* de = getenv("YDKJ_PAD_DELAY"); if (de) s_delay = strtol(de,0,0);
    }
    /* Gate on WALL-CLOCK, not poll count: the game polls the pad hundreds of thousands
     * of times inside an early wait loop, so any count-based delay fires during init
     * (crash: guest ctr=0x0005F8A0 r3=0 -- button handler on an unconstructed object).
     * YDKJ_PAD_DELAY is now SECONDS to wait before the first injected press. */
    if (s_inj && port_no < PAD_MAX_HOST_PORTS) {
        static long _pc = 0; _pc++;
        /* Cheap counter gate FIRST: clock() is a slow call and this poll loop is hammered
         * by the guest from multiple threads; sampling it every poll perturbs timing. Only
         * consult the clock once every 256 polls, and never after arming. */
        static int     s_armed = 0;
        static clock_t s_t0    = 0;
        if (!s_armed) {
            if ((_pc & 0xFF) == 0) {
                if (!s_t0) s_t0 = clock();
                else if ((double)(clock() - s_t0) / (double)CLOCKS_PER_SEC >= (double)s_delay) s_armed = 1;
            }
            goto skip_inject;
        }
        u16 btn = s_btn; long delay = 0;
        /* Delay past init, then a single clean pulse every ~180 calls (press 15, release 165). */
        if (_pc > delay) {
            long ph = (_pc - delay) % 180;
            if (ph < 15) s_host_state[port_no].buttons |= btn;
            else         s_host_state[port_no].buttons &= ~btn;
            s_host_state[port_no].connected = 1;
        }
    }
skip_inject: ;

    if (port_no >= PAD_MAX_HOST_PORTS || (!s_host_state[port_no].connected && port_no != 0)) {
        goto emit;   /* data stays zeroed -> len=0 */
    }
    /* port 0 is always a valid (virtual) pad; s_host_state[0] is neutral if no
     * physical device, which yields no-buttons-pressed data below. */

    {
    PadHostState* hs = &s_host_state[port_no];
    u32 setting = s_port_setting[port_no];

    /* Determine data length based on port settings */
    s32 len = CELL_PAD_LEN_CHANGE_DEFAULT;
    if (setting & CELL_PAD_SETTING_SENSOR_ON)
        len = CELL_PAD_LEN_CHANGE_SENSOR_ON;
    else if (setting & CELL_PAD_SETTING_PRESS_ON)
        len = CELL_PAD_LEN_CHANGE_PRESS_ON;

    data->len = len;
    /* button[0] is reserved and 0 on hardware; button[1] carries the pad mode
     * in its high nibble (0x7) and len/2 in its low one. GH3 drops any packet
     * whose button[0] is non-zero -- with len (8) there, every press was
     * received and thrown away, and "Press any button" never advanced. */
    data->button[0] = 0;
    data->button[1] = (u16)(0x70 | ((len / 2) & 0xF));

    /* Digital buttons. hs->buttons packs both halves into one 16-bit mask
     * (SELECT=bit0..LEFT=bit7 = DIGITAL1; L2=bit8..SQUARE=bit15 = DIGITAL2).
     * Split correctly — writing the whole value into DIGITAL1 with DIGITAL2=0
     * left every face button (cross/circle/triangle/square, L1/L2/R1/R2) dead.
     * (via sagemono, PR #42) */
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] = (u16)(hs->buttons & 0xFF);
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] = (u16)((hs->buttons >> 8) & 0xFF);

    /* PAD_AUTOPRESS: headless bring-up input -- pulse CROSS then START every few
     * seconds of polling so boot screens that wait for input advance without a
     * human at the pad. Env-gated test scaffolding, off by default. */
    { static int s_ap = -1; static unsigned s_apn = 0;
      if (s_ap < 0) s_ap = getenv("PAD_AUTOPRESS") ? 1 : 0;
      if (s_ap && port_no == 0) {
          unsigned ph = s_apn++ % 240;
          if (ph < 12)
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= 0x40;   /* CROSS */
          else if (ph >= 120 && ph < 132)
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= 0x08;   /* START */
      } }

    /* PAD_SCRIPT="<sec>:<mask>,<sec>:<mask>,..." -- press a NAMED button at a
     * given wall-clock second (or flip, under PAD_CLOCK=flips), each held ~250 ms. PAD_AUTOPRESS only pulses
     * CROSS and START, which is enough to clear a "press start" screen but not
     * to navigate a menu: You Don't Know Jack needs DOWN to move off its name
     * field before CROSS means anything. Masks are CELL_PAD_CTRL_* packed as
     * (DIGITAL2 << 8) | DIGITAL1: START 0x0008, UP 0x0010, DOWN 0x0040,
     * LEFT 0x0080, RIGHT 0x0020, CROSS 0x4000, CIRCLE 0x2000, TRIANGLE 0x1000.
     * Legit input simulation on the same footing as PAD_AUTOPRESS/PAD_STICK. */
    { static int s_sc = -1;
      static struct { double t; unsigned mask; } ev[256]; static int n_ev = 0;
      static unsigned long long t0 = 0;   /* not ULONGLONG: this builds on POSIX too */
      if (s_sc < 0) {
          s_sc = 0;
          const char* e = getenv("PAD_SCRIPT");
          if (e && *e) {
              s_sc = 1; t0 = GetTickCount64();
              const char* p = e;
              while (*p && n_ev < 256) {
                  double t = strtod(p, (char**)&p);
                  if (*p == ':') p++;
                  unsigned m = (unsigned)strtoul(p, (char**)&p, 0);
                  ev[n_ev].t = t; ev[n_ev].mask = m; n_ev++;
                  while (*p == ',' || *p == ' ') p++;
              }
              printf("[cellPad] PAD_SCRIPT: %d event(s)%s\n", n_ev,
                     *p ? " -- TRUNCATED at 256, the rest are ignored" : "");
          }
      }
      if (s_sc && port_no == 0) {
          double now = pad_clock(t0);
          /* Latch, do not window. A fixed time window is missed whenever the
           * title stops polling the pad for longer than the window (it does
           * exactly that on a loading screen), so the press silently never
           * happens. Fire on the first poll at or after the event time and
           * hold it for a fixed number of polls, which is what a real press
           * looks like regardless of poll rate. */
          static int fired[256], held[256];
          for (int i = 0; i < n_ev; i++) {
              if (!fired[i] && now >= ev[i].t) { fired[i] = 1; held[i] = 40; }
              if (held[i] > 0) {
                  held[i]--;
                  data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(ev[i].mask & 0xFF);
                  data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((ev[i].mask >> 8) & 0xFF);
                  static int said[256];
                  if (!said[i]) { said[i] = 1;
                      printf("[cellPad] PAD_SCRIPT t=%.1fs press 0x%04X\n", ev[i].t, ev[i].mask); }
              }
          }
      } }

    /* PAD_FILE=<path> -- closed-loop input. Each poll, if the file holds a
     * button mask (same encoding as PAD_SCRIPT), press it for a fixed number
     * of polls and empty the file. A driver outside the process can then look
     * at the log or a frame dump, decide which screen the title is on, and
     * write the next button -- which a fixed time schedule cannot do, because
     * a menu system with several screens wanders when driven blind.
     *
     *   echo 0x0008 > pad.txt    # START
     *   echo 0x4000 > pad.txt    # CROSS
     *   echo 0x0040 4 > pad.txt  # DOWN for 4 polls (menus auto-repeat a
     *                            # long hold; the default is 40)
     *   echo 0 3 128 255 128 128 > pad.txt # analog-only left-stick down
     * Optional format: mask polls lx ly rx ry, axes0..255, polls1..120.
     * All four axes are required; malformed/partial requests inject nothing.
     * File axes override PAD_STICK/PAD_SWEEP during their hold, without
     * modifying host state. Afterwards fresh host/scheduled axes resume.
     * PAD_FILE_DIAG=1 appends monotonic host milliseconds, guest flip-request
     * count (not presented frames), and polls to acceptance; logs the final
     * held packet after guest serialization. This is not proof of guest use.
     */
    int file_axes_active = 0; u8 file_axes[4];
    int file_diag = 0;
    { static int s_pf = -1; static char pf_path[512];
      static int s_pf_diag = -1;
      static PadFileCommand held; static int held_n = 0;
      if (s_pf_diag < 0) { const char* e = getenv("PAD_FILE_DIAG"); s_pf_diag = e && atoi(e) != 0; }
      file_diag = s_pf_diag;
      if (s_pf < 0) { const char* e = getenv("PAD_FILE");
                      s_pf = (e && *e) ? 1 : 0;
                      if (s_pf) { strncpy(pf_path, e, sizeof pf_path - 1); pf_path[sizeof pf_path - 1] = 0; } }
      if (s_pf && port_no == 0) {
          if (held_n <= 0) {
              FILE* f = fopen(pf_path, "rb");
              if (f) {
                  char buf[128]; const size_t bytes = fread(buf, 1, sizeof buf - 1, f);
                  const int complete = fgetc(f) == EOF; buf[bytes] = 0;
                  if (complete && bytes && pad_file_parse(buf, &held)) {
                      held_n = held.polls;
                      if (file_diag)
                          printf("[cellPad] PAD_FILE press 0x%04X monotonic_ms=%llu flip_request_count=%u polls=%d\n",
                              held.mask, (unsigned long long)GetTickCount64(), cellGcm_flip_request_count(), held.polls);
                      else printf("[cellPad] PAD_FILE press 0x%04X\n", held.mask);
                      if (held.have_axes)
                          printf("[cellPad] PAD_FILE analog lx=%u ly=%u rx=%u ry=%u polls=%d priority=file\n",
                              held.axes[0], held.axes[1], held.axes[2], held.axes[3], held.polls);
                      fflush(stdout);
                  }
                  fclose(f);
                  if (held_n > 0) { FILE* w = fopen(pf_path, "wb"); if (w) fclose(w); }
              }
          }
          if (held_n > 0) { held_n--;
              if (!held_n && file_diag) file_release_mask = (int)held.mask;
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(held.mask & 0xFF);
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((held.mask >> 8) & 0xFF);
              if (held.have_axes) { file_axes_active = 1; memcpy(file_axes, held.axes, sizeof file_axes); }
          }
      } }

    /* Analog sticks */
    /* PAD_STICK="lx,ly,rx,ry" (0-255, 128 = centred): hold the analog sticks at
     * fixed positions. Rubber Ducky drives its camera from the sticks and never
     * moves on its own, so with no host pad the view is frozen wherever it
     * started -- which can leave the subject of the demo off-screen or too far
     * away to make out. Legit input simulation, same footing as YDKJ_INJECT_PAD. */
    { static int s_st = -1;
      static unsigned char s_v[4] = {128,128,128,128};
      /* Optional 5th/6th fields: the seconds the hold starts and ends, so the menus
       * before gameplay are not driven by it (PAD_STICK="128,0,128,128,300,320"). */
      static double s_from = 0, s_until = 1e30; static unsigned long long s_t0 = 0;
      if (!s_t0) s_t0 = GetTickCount64();
      if (s_st < 0) { const char* e = getenv("PAD_STICK");
        s_st = e ? 1 : 0;
        if (e) { int a=128,b=128,c=128,d=128;
                 sscanf(e, "%d,%d,%d,%d,%lf,%lf", &a,&b,&c,&d,&s_from,&s_until);
                 s_v[0]=(unsigned char)a; s_v[1]=(unsigned char)b;
                 s_v[2]=(unsigned char)c; s_v[3]=(unsigned char)d; } }
      if (s_st && pad_clock(s_t0) >= s_from && pad_clock(s_t0) < s_until) { hs->analog_lx = s_v[0]; hs->analog_ly = s_v[1];
                  hs->analog_rx = s_v[2]; hs->analog_ry = s_v[3];
                  hs->connected = 1; }
      /* PAD_SWEEP=<seconds per step>: walk the sticks through a fixed set of
       * deflections instead of holding one. Guessing a single stick position and
       * re-running costs ~5 minutes a try; a sweep answers "can input move the
       * camera anywhere useful" in one run, and the frame dumps say which step
       * paid off. Steps: centre, each left-stick direction, then each right. */
      { static int sw = -1; static clock_t t0 = 0;
        if (sw < 0) { const char* e = getenv("PAD_SWEEP");
                      sw = e ? atoi(e) : 0; if (sw < 0) sw = 0; }
        if (sw) {
            static const unsigned char steps[9][4] = {
                {128,128,128,128},
                {128,  0,128,128}, {128,255,128,128},
                {  0,128,128,128}, {255,128,128,128},
                {128,128,128,  0}, {128,128,128,255},
                {128,128,  0,128}, {128,128,255,128},
            };
            if (!t0) t0 = clock();
            long el = (long)((double)(clock() - t0) / (double)CLOCKS_PER_SEC);
            int k = (int)((el / (sw > 0 ? sw : 1)) % 9);
            hs->analog_lx = steps[k][0]; hs->analog_ly = steps[k][1];
            hs->analog_rx = steps[k][2]; hs->analog_ry = steps[k][3];
            hs->connected = 1;
            { static int lastk = -1;
              if (k != lastk) { lastk = k;
                fprintf(stderr, "[PADSWEEP] step %d: l=(%u,%u) r=(%u,%u)%c", k,
                        steps[k][0], steps[k][1], steps[k][2], steps[k][3], 10); } }
        } } }
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] = hs->analog_rx;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] = hs->analog_ry;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X]  = hs->analog_lx;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y]  = hs->analog_ly;
    /* File axes override PAD_STICK/PAD_SWEEP only in this emitted packet.
     * Never mutate host axes: expiry restores fresh host/scheduled input. */
    if (file_axes_active) pad_file_serialize_axes(data->button, file_axes);

    /* Pressure-sensitive buttons (only meaningful if PRESS_ON) */
    if (setting & CELL_PAD_SETTING_PRESS_ON) {
        data->button[CELL_PAD_BTN_OFFSET_PRESS_RIGHT]    = hs->press_right;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_LEFT]     = hs->press_left;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_UP]       = hs->press_up;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_DOWN]     = hs->press_down;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE] = hs->press_triangle;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_CIRCLE]   = hs->press_circle;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_CROSS]    = hs->press_cross;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_SQUARE]   = hs->press_square;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_L1]       = hs->press_l1;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_R1]       = hs->press_r1;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_L2]       = hs->trigger_l2;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_R2]       = hs->trigger_r2;
        /* A button whose digital bit is set reads full pressure. Injected
         * input (PAD_FILE/PAD_SCRIPT/PAD_AUTOPRESS, keyboard) only sets the
         * digital bits, and the host values above zeroed any pressure it had
         * written -- so a title keying its menus off the pressure words (GH3,
         * Tornado Outbreak's title screen) never saw the press. */
        static const struct { u8 word; u8 bit; u8 off; } pm[] = {
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x20, CELL_PAD_BTN_OFFSET_PRESS_RIGHT },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x80, CELL_PAD_BTN_OFFSET_PRESS_LEFT },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x10, CELL_PAD_BTN_OFFSET_PRESS_UP },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x40, CELL_PAD_BTN_OFFSET_PRESS_DOWN },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x10, CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x20, CELL_PAD_BTN_OFFSET_PRESS_CIRCLE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x40, CELL_PAD_BTN_OFFSET_PRESS_CROSS },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x80, CELL_PAD_BTN_OFFSET_PRESS_SQUARE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x04, CELL_PAD_BTN_OFFSET_PRESS_L1 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x08, CELL_PAD_BTN_OFFSET_PRESS_R1 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x01, CELL_PAD_BTN_OFFSET_PRESS_L2 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x02, CELL_PAD_BTN_OFFSET_PRESS_R2 } };
        for (unsigned k = 0; k < sizeof pm / sizeof pm[0]; k++)
            if ((data->button[pm[k].word] & pm[k].bit) && data->button[pm[k].off] < 255)
                data->button[pm[k].off] = 255;
    }

    /* Sensor data (only meaningful if SENSOR_ON) */
    if (setting & CELL_PAD_SETTING_SENSOR_ON) {
        /* Default sensor values: accelerometer at rest (512 = 1g center) */
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_X] = 512;
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_Y] = 399; /* gravity */
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_Z] = 512;
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_G] = 512;
    }
    }  /* end connected block */

emit:
    /* A non-zero len means "here is a CHANGE packet" -- the constant is literally
     * named CELL_PAD_LEN_CHANGE_DEFAULT. Hardware reports len = 0 once the port
     * has nothing new, and titles DRAIN on that: The Simpsons Arcade Game's
     * input stage is
     *
     *     while (cellPadGetData(port, &d) == CELL_OK && d.len > 0) { ... }
     *
     * (func_0013CFA8, loop 0x13D648 -> 0x13D040). Reporting a change on every
     * call made that loop infinite: the main thread pegged one core inside
     * libpad and the title never got past its first rendered screen.
     *
     * So hand out one packet per host report and report len = 0 for any read
     * after that until the next one arrives (pad_poll_backend sets the flag when
     * it actually re-polls, every PAD_POLL_INTERVAL_MS). That is what the
     * hardware does: a pad streams reports at a fixed rate whether or not
     * anything changed, and libpad buffers them.
     *
     * Reporting only on CHANGE instead is wrong and was tried: a HELD button
     * produces exactly one packet, so a title that reads button[] only when
     * len > 0 sees the press for a single frame and then sees nothing. In this
     * game that came out as jump being the only control that seemed to work and
     * attack firing once.
     *
     * PAD_ALWAYS_CHANGE=1 restores the old always-a-packet behaviour. */
    {
        static int always = -1;
        if (always < 0) always = getenv("PAD_ALWAYS_CHANGE") ? 1 : 0;
        if (!always && port_no < PAD_MAX_HOST_PORTS && data->len) {
            if (!s_data_fresh[port_no]) data->len = 0;   /* nothing new: end the drain */
            else                        s_data_fresh[port_no] = 0;
        }
    }
    {
        unsigned int ea = (unsigned int)(uintptr_t)data_guest;
        vm_write32((unsigned long long)ea + 0, (unsigned int)data->len);   /* len (s32) */
        for (int _i = 0; _i < CELL_PAD_MAX_CODES; _i++)                    /* button[] u16 */
            vm_write16((unsigned long long)ea + 4 + (unsigned int)_i * 2, data->button[_i]);
    }
    if (file_release_mask >= 0) {
        printf("[cellPad] PAD_FILE release 0x%04X monotonic_ms=%llu flip_request_count=%u packet_len=%d stage=last_held_packet_written\n",
            (unsigned)file_release_mask, (unsigned long long)GetTickCount64(),
            cellGcm_flip_request_count(), data->len);
        fflush(stdout);
    }
    return CELL_OK;
}

/* cellPadGetInfo (v1 API, NID 0x3AAAD464) — used by PSL1GHT's ioPadGetInfo.
 * Layout (RPCS3 cellPad.h CellPadInfo, all big-endian):
 *   +0x00 u32 max_connect        +0x04 u32 now_connect   +0x08 u32 system_info
 *   +0x0C u16 vendor_id[127]     +0x10A u16 product_id[127] +0x208 u8 status[127]
 *   (CELL_MAX_PADS / PSL1GHT MAX_PADS = 127 -- NOT 7; see below)
 * Leaving this unimplemented let the guest read stack garbage as pad state and
 * run off into the weeds before ever reaching gcm init. */
s32 cellPadGetInfo(CellPadInfo2* info)
{
    uint32_t gaddr = (uint32_t)(uintptr_t)info;
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (!gaddr)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    pad_poll_backend();

    /* Present a virtual pad on port 0 when nothing physical is attached, the
     * same as cellPadGetInfo2 does.
     *
     * The two entry points disagreed: GetInfo2 carried the fallback and GetInfo
     * did not, so which one a title imports decided whether it believed a
     * controller existed. The Orange Box's chooser imports GetInfo (v1) and
     * never GetInfo2, so it saw zero pads, opened "SIXAXIS wireless controller
     * not detected", took the auto-OK, and opened it again -- forever, before
     * it ever finished booting. Both entry points describe the same hardware
     * and have to answer the same way. */
    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < PAD_MAX_HOST_PORTS; i++)
        if (s_host_state[i].connected || i == 0) connected++;

    vm_write32(gaddr + 0x00, s_max_connect);
    vm_write32(gaddr + 0x04, connected);
    vm_write32(gaddr + 0x08, 0);                       /* system_info */
    /* CellPadInfo (v1) sizes its arrays by CELL_MAX_PADS = 127, not by the 7
     * ports: vendor_id[127] @0x0C, product_id[127] @0x10A, status[127] @0x208.
     * Packed for 7 ports, status landed at 0x28 and GH3 -- which reads
     * info+0x208+port -- saw port 0 disconnected and never polled the pad. */
    for (u32 i = 0; i < CELL_PAD_MAX_PORT_NUM; i++) {
        int on = (i < s_max_connect && i < PAD_MAX_HOST_PORTS &&
                  (s_host_state[i].connected || i == 0));
        vm_write16(gaddr + 0x00C + i * 2, on ? 0x054C : 0);   /* vendor: Sony  */
        vm_write16(gaddr + 0x10A + i * 2, on ? 0x0268 : 0);   /* product: DS3  */
        vm_write8(gaddr + 0x208 + i, on ? CELL_PAD_STATUS_CONNECTED : 0);
    }
    return CELL_OK;
}

s32 cellPadGetInfo2(CellPadInfo2* info_guest)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (!info_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Poll to get latest connection state */
    pad_poll_backend();

    /* Build in a local host struct, then copy to guest memory big-endian. */
    CellPadInfo2 _in; CellPadInfo2* info = &_in;
    memset(info, 0, sizeof(CellPadInfo2));
    info->max_connect = s_max_connect;

    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < PAD_MAX_HOST_PORTS; i++) {
        /* Always present a virtual pad on port 0 (standard emulator behavior) so
         * boot flows that block until a controller is connected can proceed even
         * with no physical device attached. */
        if (s_host_state[i].connected || i == 0) {
            info->port_status[i]       = CELL_PAD_STATUS_CONNECTED;
            info->port_setting[i]      = s_port_setting[i];
            info->device_capability[i] = CELL_PAD_CAPABILITY_PS3_CONFORMITY
                                       | CELL_PAD_CAPABILITY_PRESS_MODE
                                       | CELL_PAD_CAPABILITY_SENSOR_MODE
                                       | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                                       | CELL_PAD_CAPABILITY_ACTUATOR;
            info->device_type[i]       = CELL_PAD_DEV_TYPE_STANDARD;
            connected++;
        } else {
            info->port_status[i] = CELL_PAD_STATUS_DISCONNECTED;
        }
    }
    info->now_connect = connected;

    /* copy to guest big-endian (CellPadInfo2 is all u32 fields) */
    {
        unsigned int ea = (unsigned int)(uintptr_t)info_guest;
        for (unsigned int _o = 0; _o < sizeof(CellPadInfo2); _o += 4)
            vm_write32((unsigned long long)ea + _o, *(u32*)((char*)info + _o));
    }
    return CELL_OK;
}

/* cellPadPeriphGetInfo -- the peripheral-class view of the pad ports (NID
 * 0x4CC9B68D). LBP polls this during boot; leaving it unresolved returned
 * CELL_OK with the caller's CellPadPeriphInfo left as uninitialised stack
 * garbage, so the game read junk port_status/pclass_type for all 7 ports.
 *
 * Layout (SDK cell/pad/pad_codes.h) -- all u32, so the whole struct marshals
 * big-endian in one sweep like cellPadGetInfo2:
 *   max_connect, now_connect, system_info,
 *   port_status[7], port_setting[7], device_capability[7],
 *   device_type[7], pclass_type[7], pclass_profile[7]
 * We report a standard DUALSHOCK-class pad (pclass_type STANDARD = 0, no
 * profile bits) mirroring the ports cellPadGetInfo2 already reports. */
s32 cellPadPeriphGetInfo(CellPadPeriphInfo* info_guest)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (!info_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    pad_poll_backend();

    CellPadPeriphInfo _in; CellPadPeriphInfo* info = &_in;
    memset(info, 0, sizeof(CellPadPeriphInfo));
    info->max_connect = s_max_connect;
    info->system_info = 0;

    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < CELL_PAD_MAX_PORT_NUM; i++) {
        /* Same virtual-pad-on-port-0 rule as cellPadGetInfo2, so both views of
         * the ports agree (a game that cross-checks them must see one truth). */
        if ((i < PAD_MAX_HOST_PORTS && s_host_state[i].connected) || i == 0) {
            info->port_status[i]       = CELL_PAD_STATUS_CONNECTED;
            info->port_setting[i]      = s_port_setting[i];
            info->device_capability[i] = CELL_PAD_CAPABILITY_PS3_CONFORMITY
                                       | CELL_PAD_CAPABILITY_PRESS_MODE
                                       | CELL_PAD_CAPABILITY_SENSOR_MODE
                                       | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                                       | CELL_PAD_CAPABILITY_ACTUATOR;
            info->device_type[i]       = CELL_PAD_DEV_TYPE_STANDARD;
            info->pclass_type[i]       = CELL_PAD_PCLASS_TYPE_STANDARD;
            info->pclass_profile[i]    = 0;
            connected++;
        } else {
            info->port_status[i] = CELL_PAD_STATUS_DISCONNECTED;
        }
    }
    info->now_connect = connected;

    { unsigned int ea = (unsigned int)(uintptr_t)info_guest;
      for (unsigned int _o = 0; _o < sizeof(CellPadPeriphInfo); _o += 4)
          vm_write32((unsigned long long)ea + _o, *(u32*)((char*)info + _o)); }

    { static int _n = 0;
      if (PS3_LOG_FIRST(_n, 2))
          printf("[cellPad] PeriphGetInfo(max=%u now=%u) -> STANDARD class\n",
                 info->max_connect, connected); }
    return CELL_OK;
}

s32 cellPadSetPortSetting(u32 port_no, u32 port_setting)
{
    printf("[cellPad] SetPortSetting(port=%u, setting=0x%X)\n",
           port_no, port_setting);

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_port_setting[port_no] = port_setting;
    return CELL_OK;
}

/* cellPadInfoPressMode (NID 0x0E2DFAAD) / cellPadSetPressMode (NID 0xF83F8182)
 *
 * Press mode makes the digital buttons report an analog 0..255 pressure in the
 * extended part of CellPadData. A DS3 supports it, and the virtual pad reported
 * by cellPadGetInfo/GetInfo2 already advertises CELL_PAD_CAPABILITY_PRESS_MODE,
 * so these have to agree with that claim rather than fall through to the
 * unresolved-NID path.
 *
 * Info returns 1 when the port supports press mode; Set toggles it, recorded in
 * the same s_port_setting word cellPadSetPortSetting uses so a title that reads
 * the setting back sees what it asked for. */
s32 cellPadInfoPressMode(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    return 1;                                  /* supported */
}

s32 cellPadSetPressMode(u32 port_no, u32 mode)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    if (mode)
        s_port_setting[port_no] |= CELL_PAD_SETTING_PRESS_ON;
    else
        s_port_setting[port_no] &= ~(u32)CELL_PAD_SETTING_PRESS_ON;
    return CELL_OK;
}

s32 cellPadGetCapabilityInfo(u32 port_no, CellPadCapabilityInfo* info)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM || !info)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    u32 info_ea = (u32)(uintptr_t)info;
    for (u32 i = 0; i < CELL_PAD_MAX_CODES; i++)
        vm_write32(info_ea + i * 4, 0);

    /* Report standard DualShock 3 capabilities */
    vm_write32(info_ea, CELL_PAD_CAPABILITY_PS3_CONFORMITY
                        | CELL_PAD_CAPABILITY_PRESS_MODE
                        | CELL_PAD_CAPABILITY_SENSOR_MODE
                        | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                        | CELL_PAD_CAPABILITY_ACTUATOR);

    return CELL_OK;
}

s32 cellPadSetActDirect(u32 port_no, CellPadActParam* param)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM || !param)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    param = GUEST_PTR(param, CellPadActParam*);

    if (port_no >= PAD_MAX_HOST_PORTS)
        return CELL_OK;

    u16 lo, hi;
    pad_rumble_levels(param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL],
                      param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE], &lo, &hi);
    s_rumble[port_no].lo = lo;
    s_rumble[port_no].hi = hi;
    {   /* does the title use the motors? first calls, every 256th, and the
         * first nonzero requests (the game sends zeros every frame) */
        static unsigned calls, nonzero;
        const int on = param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] || param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE];
        if (++calls <= 8 || (calls & 255u) == 0 || (on && ++nonzero <= 32)) {
            printf("[cellPad] SetActDirect #%u port=%u small=%u large=%u\n", calls, port_no,
                   param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL], param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE]);
            fflush(stdout);
        }
    }

    /* The SDL2 pad thread picks the new levels up on its next sweep. */
#if PAD_BACKEND_XINPUT
    if (!s_use_sdl) pad_xinput_rumble((int)port_no);
#endif

    return CELL_OK;
}

s32 cellPadClearBuf(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Nothing to clear in our implementation -- state is polled fresh */
    return CELL_OK;
}
