/* Shared window protocol between Scmpoo and Orange.
   Include after windows.h. Both processes must run at the same integrity
   level or WM_USER will not cross. */

#ifndef MATE_PROTOCOL_H
#define MATE_PROTOCOL_H

#include <stdlib.h>
#include <time.h>
#include <wchar.h>

#define MATE_DIRECTOR_CLASS L"ScreenMateDirector"
#define MATE_DIRECTOR_MUTEX L"Local\\ScreenMateDirectorMutex"
#define MATE_BALL_TITLE L"Screen Mate Ball"
#define MATE_ORANGE_CLASS L"Orange"
#define MATE_SHEEP_CLASS L"ScreenMatePoo"

#define MATE_WORD_X 0
#define MATE_WORD_Y 2
#define MATE_WORD_STATUS 6
/* End of the fishing line, relative to the orange window, already mirrored. */
#define MATE_WORD_HOOK_X 8
#define MATE_WORD_HOOK_Y 10

#define MATE_STATUS_IDLE 0
#define MATE_STATUS_BLOOM 1
#define MATE_STATUS_TOY 2
#define MATE_STATUS_FISH 3

/* WM_USER wParam. 1-5 already belong to Scmpoo (hello, goodbye, knock, kick, pop). */
#define MATE_MSG_KICK 4
#define MATE_MSG_REGISTER 6
#define MATE_MSG_CHIME 7
#define MATE_MSG_APPROACH_EAT 8
#define MATE_MSG_BECOME_TOY 9
#define MATE_MSG_EATEN 10
#define MATE_MSG_END_ROLE 11
#define MATE_MSG_UNREGISTER 12
#define MATE_MSG_CAUGHT 13
#define MATE_MSG_DROP 14
#define MATE_MSG_UNBLOOM 15

/* 107.bmp cells 0-4: orange splits and a flower grows. Frame = sheet 6 * 16 + cell. */
#define MATE_BLOOM_FIRST 96
#define MATE_BLOOM_LAST 100
#define MATE_HORN_TICKS 1111 /* ~2 minutes at 108ms */
#define MATE_TOY_TICKS 120 /* 120 * 500ms = 1 minute */
#define MATE_DIRECTOR_TIMER_MS 500
#define MATE_MAX_ROSTER 16

#if defined(__GNUC__)
#define MATE_MAYBE_UNUSED __attribute__((unused))
#else
#define MATE_MAYBE_UNUSED
#endif

/* Feet stand on this Y. A bottom taskbar blocks unless it auto-hides or sits on another edge. */
static int MateDesktopFloor(int x, int y)
{
    APPBARDATA pos;
    APPBARDATA state;
    MONITORINFO info;
    POINT pt;
    HMONITOR monitor;
    UINT barState;

    info.cbSize = sizeof(info);
    pt.x = x;
    pt.y = y;
    monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfo(monitor, &info)) {
        return GetSystemMetrics(SM_CYSCREEN);
    }
    pos.cbSize = sizeof(pos);
    state.cbSize = sizeof(state);
    if (SHAppBarMessage(ABM_GETTASKBARPOS, &pos) == 0) {
        return (int)info.rcMonitor.bottom;
    }
    barState = (UINT)SHAppBarMessage(ABM_GETSTATE, &state);
    if (pos.uEdge == ABE_BOTTOM && (barState & ABS_AUTOHIDE) == 0 &&
        pos.rc.top > info.rcMonitor.top && pos.rc.top < info.rcMonitor.bottom &&
        pos.rc.left < info.rcMonitor.right && pos.rc.right > info.rcMonitor.left) {
        return (int)pos.rc.top;
    }
    return (int)info.rcMonitor.bottom;
}

static HWND g_mateDirector;
static int g_mateDirectorOwned;
static HWND g_mateRegisteredWith;
static HWND g_mateRoster[MATE_MAX_ROSTER];
static int g_mateChimeHour = -1;
static HWND g_mateToy;
static int g_mateToyTicks;
static HWND g_mateBloomOrange;
static HWND g_mateBloomSheep;

static LRESULT CALLBACK MateDirectorProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
static void MateDirectorTick(void);

static int MateRosterAdd(HWND hwnd)
{
    int i;
    if (hwnd == NULL || !IsWindow(hwnd)) {
        return 0;
    }
    for (i = 0; i < MATE_MAX_ROSTER; i++) {
        if (g_mateRoster[i] == hwnd) {
            return 1;
        }
    }
    for (i = 0; i < MATE_MAX_ROSTER; i++) {
        if (g_mateRoster[i] == NULL) {
            g_mateRoster[i] = hwnd;
            return 1;
        }
    }
    return 0;
}

static void MateRosterRemove(HWND hwnd)
{
    int i;
    for (i = 0; i < MATE_MAX_ROSTER; i++) {
        if (g_mateRoster[i] == hwnd) {
            g_mateRoster[i] = NULL;
        }
    }
}

static void MateRosterPrune(void)
{
    int i;
    for (i = 0; i < MATE_MAX_ROSTER; i++) {
        if (g_mateRoster[i] != NULL && !IsWindow(g_mateRoster[i])) {
            g_mateRoster[i] = NULL;
        }
    }
}

static int MateClassIs(HWND hwnd, const wchar_t *name)
{
    wchar_t buf[64];
    buf[0] = 0;
    if (GetClassNameW(hwnd, buf, 64) == 0) {
        return 0;
    }
    return wcscmp(buf, name) == 0;
}

static int MateRealBallExists(void)
{
    HWND wnd;
    UINT cmd;
    int scanned;
    wchar_t title[32];

    wnd = GetDesktopWindow();
    cmd = GW_CHILD;
    scanned = 0;
    while ((wnd = GetWindow(wnd, cmd)) != NULL && scanned < 128) {
        cmd = GW_HWNDNEXT;
        scanned += 1;
        if ((GetWindowLongW(wnd, GWL_STYLE) & WS_VISIBLE) == 0) {
            continue;
        }
        title[0] = 0;
        GetWindowTextW(wnd, title, 32);
        if (wcscmp(title, MATE_BALL_TITLE) == 0) {
            return 1;
        }
    }
    return 0;
}

static HWND MateFindOrange(int status) MATE_MAYBE_UNUSED;

static HWND MateFindOrange(int status)
{
    HWND wnd;
    UINT cmd;
    int scanned;

    wnd = GetDesktopWindow();
    cmd = GW_CHILD;
    scanned = 0;
    while ((wnd = GetWindow(wnd, cmd)) != NULL && scanned < 128) {
        cmd = GW_HWNDNEXT;
        scanned += 1;
        if ((GetWindowLongW(wnd, GWL_STYLE) & WS_VISIBLE) == 0) {
            continue;
        }
        if (!MateClassIs(wnd, MATE_ORANGE_CLASS)) {
            continue;
        }
        if (GetWindowWord(wnd, MATE_WORD_STATUS) != (WORD)status) {
            continue;
        }
        return wnd;
    }
    return NULL;
}

static HWND MateFindToyOrange(void)
{
    return MateFindOrange(MATE_STATUS_TOY);
}

static int MateAssignEater(HWND orange)
{
    int order[MATE_MAX_ROSTER];
    int n;
    int i;
    int j;
    int ox;
    int oy;

    ox = (short)GetWindowWord(orange, MATE_WORD_X);
    oy = (short)GetWindowWord(orange, MATE_WORD_Y);
    n = 0;
    for (i = 0; i < MATE_MAX_ROSTER; i++) {
        if (g_mateRoster[i] != NULL && MateClassIs(g_mateRoster[i], MATE_SHEEP_CLASS)) {
            order[n] = i;
            n += 1;
        }
    }
    for (i = 1; i < n; i++) {
        int key = order[i];
        int dx = (short)GetWindowWord(g_mateRoster[key], MATE_WORD_X) - ox;
        int dy = (short)GetWindowWord(g_mateRoster[key], MATE_WORD_Y) - oy;
        int dist = dx * dx + dy * dy;
        j = i - 1;
        while (j >= 0) {
            int prev = order[j];
            int pdx = (short)GetWindowWord(g_mateRoster[prev], MATE_WORD_X) - ox;
            int pdy = (short)GetWindowWord(g_mateRoster[prev], MATE_WORD_Y) - oy;
            if (pdx * pdx + pdy * pdy <= dist) {
                break;
            }
            order[j + 1] = order[j];
            j -= 1;
        }
        order[j + 1] = key;
    }
    for (i = 0; i < n; i++) {
        HWND sheep = g_mateRoster[order[i]];
        if (sheep != NULL && IsWindow(sheep) &&
            SendMessageW(sheep, WM_USER, MATE_MSG_APPROACH_EAT, (LPARAM)orange) != 0) {
            g_mateBloomSheep = sheep;
            return 1;
        }
    }
    return 0;
}

static void MateDirectorTick(void)
{
    time_t now;
    struct tm *local;
    HWND copy[MATE_MAX_ROSTER];
    int n;
    int i;

    MateRosterPrune();
    n = 0;
    for (i = 0; i < MATE_MAX_ROSTER; i++) {
        if (g_mateRoster[i] != NULL) {
            copy[n] = g_mateRoster[i];
            n += 1;
        }
    }

    time(&now);
    local = localtime(&now);
    if (local != NULL && local->tm_min == 0) {
        int hour = local->tm_hour % 12;
        if (hour == 0) {
            hour = 12;
        }
        if (hour != g_mateChimeHour) {
            g_mateChimeHour = hour;
            for (i = 0; i < n; i++) {
                if (IsWindow(copy[i])) {
                    SendMessageW(copy[i], WM_USER, MATE_MSG_CHIME, (LPARAM)hour);
                }
            }
        }
    }

    if (g_mateBloomOrange != NULL && (!IsWindow(g_mateBloomOrange) ||
        GetWindowWord(g_mateBloomOrange, MATE_WORD_STATUS) != MATE_STATUS_BLOOM)) {
        g_mateBloomOrange = NULL;
        g_mateBloomSheep = NULL;
    }
    if (g_mateBloomSheep != NULL && !IsWindow(g_mateBloomSheep)) {
        g_mateBloomSheep = NULL;
    }
    if (g_mateBloomOrange == NULL) {
        g_mateBloomOrange = MateFindOrange(MATE_STATUS_BLOOM);
    }
    if (g_mateBloomOrange != NULL && g_mateBloomSheep == NULL) {
        MateAssignEater(g_mateBloomOrange);
    }

    if (g_mateToy != NULL && (!IsWindow(g_mateToy) ||
        GetWindowWord(g_mateToy, MATE_WORD_STATUS) != MATE_STATUS_TOY ||
        MateRealBallExists())) {
        if (IsWindow(g_mateToy) && GetWindowWord(g_mateToy, MATE_WORD_STATUS) == MATE_STATUS_TOY) {
            SendMessageW(g_mateToy, WM_USER, MATE_MSG_END_ROLE, 0);
        }
        g_mateToy = NULL;
        g_mateToyTicks = 0;
    }
    if (g_mateToy != NULL) {
        g_mateToyTicks -= 1;
        if (g_mateToyTicks <= 0) {
            SendMessageW(g_mateToy, WM_USER, MATE_MSG_END_ROLE, 0);
            g_mateToy = NULL;
            g_mateToyTicks = 0;
        }
    }
    if (g_mateToy == NULL) {
        HWND existing = MateFindToyOrange();
        if (existing != NULL) {
            g_mateToy = existing;
            g_mateToyTicks = MATE_TOY_TICKS;
        } else if (g_mateBloomOrange == NULL && !MateRealBallExists() && (rand() % 180) == 0) {
            HWND candidate = MateFindOrange(MATE_STATUS_IDLE);
            if (candidate != NULL &&
                SendMessageW(candidate, WM_USER, MATE_MSG_BECOME_TOY, 0) != 0 &&
                GetWindowWord(candidate, MATE_WORD_STATUS) == MATE_STATUS_TOY) {
                g_mateToy = candidate;
                g_mateToyTicks = MATE_TOY_TICKS;
            }
        }
    }
}

static LRESULT CALLBACK MateDirectorProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_USER:
        if (wParam == MATE_MSG_REGISTER) {
            MateRosterAdd((HWND)lParam);
        } else if (wParam == MATE_MSG_UNREGISTER) {
            MateRosterRemove((HWND)lParam);
            if ((HWND)lParam == g_mateBloomSheep) {
                g_mateBloomSheep = NULL;
            }
        } else if (wParam == MATE_MSG_END_ROLE) {
            if ((HWND)lParam == g_mateBloomSheep) {
                g_mateBloomSheep = NULL;
            }
        }
        return 0;
    case WM_TIMER:
        MateDirectorTick();
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

static int MateDirectorPresent(void) MATE_MAYBE_UNUSED;

static int MateDirectorPresent(void)
{
    if (g_mateDirector != NULL && IsWindow(g_mateDirector)) {
        return 1;
    }
    return FindWindowW(MATE_DIRECTOR_CLASS, NULL) != NULL;
}

static void MateEnsureDirector(HWND self)
{
    HANDLE mutex;
    int i;

    if (self == NULL) {
        return;
    }
    if (g_mateDirector != NULL && IsWindow(g_mateDirector)) {
        if (g_mateRegisteredWith != g_mateDirector) {
            SendMessageW(g_mateDirector, WM_USER, MATE_MSG_REGISTER, (LPARAM)self);
            g_mateRegisteredWith = g_mateDirector;
        }
        return;
    }

    g_mateDirectorOwned = 0;
    g_mateRegisteredWith = NULL;
    g_mateDirector = FindWindowW(MATE_DIRECTOR_CLASS, NULL);
    if (g_mateDirector == NULL) {
        mutex = CreateMutexW(NULL, FALSE, MATE_DIRECTOR_MUTEX);
        if (mutex != NULL) {
            WaitForSingleObject(mutex, 1000);
        }
        g_mateDirector = FindWindowW(MATE_DIRECTOR_CLASS, NULL);
        if (g_mateDirector == NULL) {
            WNDCLASSW windowClass;
            HINSTANCE instance = GetModuleHandleW(NULL);
            ZeroMemory(&windowClass, sizeof(windowClass));
            windowClass.lpfnWndProc = MateDirectorProc;
            windowClass.hInstance = instance;
            windowClass.lpszClassName = MATE_DIRECTOR_CLASS;
            RegisterClassW(&windowClass);
            g_mateDirector = CreateWindowExW(
                WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                MATE_DIRECTOR_CLASS,
                L"",
                WS_POPUP,
                0, 0, 0, 0,
                NULL, NULL, instance, NULL);
            if (g_mateDirector != NULL) {
                g_mateDirectorOwned = 1;
                g_mateChimeHour = -1;
                g_mateToy = NULL;
                g_mateToyTicks = 0;
                g_mateBloomOrange = NULL;
                g_mateBloomSheep = NULL;
                for (i = 0; i < MATE_MAX_ROSTER; i++) {
                    g_mateRoster[i] = NULL;
                }
                SetTimer(g_mateDirector, 1, MATE_DIRECTOR_TIMER_MS, NULL);
            }
        }
        if (mutex != NULL) {
            ReleaseMutex(mutex);
            CloseHandle(mutex);
        }
    }
    if (g_mateDirector != NULL) {
        SendMessageW(g_mateDirector, WM_USER, MATE_MSG_REGISTER, (LPARAM)self);
        g_mateRegisteredWith = g_mateDirector;
    }
}

static void MateDirectorDetach(HWND self)
{
    if (g_mateDirector != NULL && IsWindow(g_mateDirector)) {
        SendMessageW(g_mateDirector, WM_USER, MATE_MSG_UNREGISTER, (LPARAM)self);
    }
    if (g_mateDirectorOwned && g_mateDirector != NULL && IsWindow(g_mateDirector)) {
        DestroyWindow(g_mateDirector);
    }
    g_mateDirector = NULL;
    g_mateDirectorOwned = 0;
    g_mateRegisteredWith = NULL;
}

static void MateDirectorDropJob(HWND self) MATE_MAYBE_UNUSED;

static void MateDirectorDropJob(HWND self)
{
    if (g_mateDirector != NULL && IsWindow(g_mateDirector)) {
        SendMessageW(g_mateDirector, WM_USER, MATE_MSG_END_ROLE, (LPARAM)self);
    }
}

#endif
