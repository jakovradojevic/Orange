/* Native Win32 reconstruction of the 16-bit Orange screen mate (Orange.exe).
   Same approach as Scmpoo: original sprite sheets and waves are resources,
   and a layered window replaces the 16-bit desktop blit. */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600

#include <windows.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <stdlib.h>
#include <wchar.h>
#include "mate_protocol.h"

#define CELL 40
#define SHEET_W 640
#define SHEET_H 40
#define SHEET_COUNT 11
#define CELLS 16
#define TIMER_MS 108
#define MAX_MATES 8
#define CHROMA_R 151
#define CHROMA_G 87
#define CHROMA_B 255

#define IDC_GRAVITY 1001
#define IDC_SOUND 1002
#define IDC_ACTIVE 1003
#define IDC_EXIT 3
#define IDB_HELP 4
#define IDC_ANIM_LIST 1040
#define IDC_NUDGE_UP 1032
#define IDC_NUDGE_DOWN 1033
#define IDC_NUDGE_LEFT 1034
#define IDC_NUDGE_RIGHT 1035

typedef struct FloorScan {
    HWND self;
    RECT work;
    int left;
    int right;
    int feet;
    int nextFeet;
    int floorY;
} FloorScan;

/* Word table at DS:0x212. rand() % 80 indexes it. 14 is walk, 8 is run.
   22 is standing smoke, 68 is calm fishing. The tug (70) is menu-only. */
static const int kActionTable[80] = {
    14, 14, 8, 8, 14, 14, 8,
    8, 14, 14, 8, 8, 14, 14, 8, 8, 14, 14, 8, 8, 14, 14, 8,
    8, 14, 14, 8, 8, 14, 14, 8, 8, 14, 14, 8, 8, 14, 14, 8,
    8, 14, 8, 24, 29, 14, 8, 24, 29, 14, 8, 24, 29, 14, 8, 24,
    29, 14, 8, 24, 29, 14, 8, 24, 29, 14, 8, 24, 29, 18, 68, 22,
    50, 44, 44, 48, 52, 50, 52, 22, 48
};

static HINSTANCE g_inst;
static wchar_t g_ini[MAX_PATH];
static HWND g_owner;
static HWND g_hwnd;
static DWORD *g_sheets[SHEET_COUNT];
static HBITMAP g_surface;
static DWORD *g_surfaceBits;
static int g_x;
static int g_y;
/* +1 walks left on the raw sheet. -1 walks right on the mirrored sheet. */
static int g_dir = 1;
static int g_turnCool;
static int g_flinch;
static int g_flinchDir;
static int g_state = 1;
static int g_sprite = 148;
static int g_gate;
static int g_left;
static int g_vy;
static int g_dragging;
static int g_grabX;
static int g_grabY;
static int g_soundOn = 1;
static int g_gravityOn = 1;
static int g_noSleep = 1;
static int g_status;
static int g_unbloom;
static HWND g_caught;
static int g_vx;
static int g_toyMoved;
static int g_toyStill;
static int g_chimePause;
static int g_rightExit;
static const short *g_anim;
static int g_animHold = 4;
static int g_animLoops = 1;
static int g_clipMotion;
static int g_clipTick;
static int g_clipLeft;
static int g_clipBaseY;
static int g_clipStep;
static void Present(void);
static void PlayOrangeWave(int soundId);
static void PlayOrangeCry(void);
static void OpenSettings(void);
static void BeginAction(int action);
static int MateInFront(int nextX, int y);

static void BuildIniPath(void)
{
    wchar_t *slash;
    DWORD n = GetModuleFileNameW(NULL, g_ini, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        lstrcpyW(g_ini, L"scmate.ini");
        return;
    }
    slash = wcsrchr(g_ini, L'\\');
    if (slash != NULL) {
        lstrcpyW(slash + 1, L"scmate.ini");
    } else {
        lstrcpyW(g_ini, L"scmate.ini");
    }
}

static void LoadConfig(void)
{
    g_soundOn = GetPrivateProfileIntW(L"Orange", L"Sound", 1, g_ini) ? 1 : 0;
    g_gravityOn = GetPrivateProfileIntW(L"Orange", L"Gravity", 1, g_ini) ? 1 : 0;
    g_noSleep = GetPrivateProfileIntW(L"Orange", L"NoSleep", 1, g_ini) ? 1 : 0;
}

static void SaveConfig(void)
{
    WritePrivateProfileStringW(L"Orange", L"Sound", g_soundOn ? L"1" : L"0", g_ini);
    WritePrivateProfileStringW(L"Orange", L"Gravity", g_gravityOn ? L"1" : L"0", g_ini);
    WritePrivateProfileStringW(L"Orange", L"NoSleep", g_noSleep ? L"1" : L"0", g_ini);
}

static int LoadSheet(int resourceId, int index)
{
    HRSRC found;
    HGLOBAL loaded;
    const BYTE *dib;
    const BITMAPINFOHEADER *info;
    const RGBQUAD *palette;
    const BYTE *bits;
    int width;
    int height;
    int colors;
    int stride;
    int topDown;
    int y;
    int x;
    DWORD *pixels;

    found = FindResourceW(g_inst, MAKEINTRESOURCEW(resourceId), RT_BITMAP);
    if (found == NULL) {
        return 0;
    }
    loaded = LoadResource(g_inst, found);
    dib = (const BYTE *)LockResource(loaded);
    if (dib == NULL) {
        return 0;
    }
    info = (const BITMAPINFOHEADER *)dib;
    if (info->biSize < 40 || info->biBitCount != 8) {
        return 0;
    }
    width = info->biWidth;
    height = info->biHeight < 0 ? -info->biHeight : info->biHeight;
    topDown = info->biHeight < 0;
    colors = (int)info->biClrUsed;
    if (colors == 0) {
        colors = 256;
    }
    palette = (const RGBQUAD *)(dib + info->biSize);
    bits = (const BYTE *)(palette + colors);
    stride = ((width * 8 + 31) / 32) * 4;
    pixels = (DWORD *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)width * (SIZE_T)height * 4);
    if (pixels == NULL) {
        return 0;
    }
    for (y = 0; y < height; y++) {
        int sourceY = topDown ? y : height - 1 - y;
        const BYTE *row = bits + sourceY * stride;
        for (x = 0; x < width; x++) {
            RGBQUAD color = palette[row[x]];
            DWORD pixel;
            if (color.rgbRed == CHROMA_R && color.rgbGreen == CHROMA_G && color.rgbBlue == CHROMA_B) {
                pixel = 0;
            } else {
                pixel = 0xFF000000u | ((DWORD)color.rgbRed << 16) | ((DWORD)color.rgbGreen << 8) | color.rgbBlue;
            }
            pixels[y * width + x] = pixel;
        }
    }
    g_sheets[index] = pixels;
    return width == SHEET_W && height == SHEET_H;
}

static int CreateSurface(void)
{
    BITMAPINFO info;
    void *bits;

    ZeroMemory(&info, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = CELL;
    info.bmiHeader.biHeight = -CELL;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    g_surface = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    g_surfaceBits = (DWORD *)bits;
    return g_surface != NULL && g_surfaceBits != NULL;
}

/* Bobber on 108.bmp: the red pixel at the end of the line, already mirrored. */
static int FishBobber(int *hookX)
{
    int x = 1;
    int y = 38;
    switch (g_sprite) {
    case 118: x = 1; y = 37; break;
    case 119: x = 1; y = 38; break;
    case 120: x = 2; y = 38; break;
    case 121: x = 2; y = 38; break;
    case 122: x = 3; y = 39; break;
    case 123: x = 3; y = 39; break;
    default: break;
    }
    if (g_dir < 0) {
        x = 39 - x;
    }
    if (hookX != NULL) {
        *hookX = x;
    }
    return y;
}

/* The carried sheep hangs from the bobber. Keep its feet on the desktop floor. */
static void KeepCarryVisible(void)
{
    int hookY;
    int floorTop;
    int lowest;
    if (g_caught == NULL || !IsWindow(g_caught)) {
        return;
    }
    hookY = FishBobber(NULL);
    floorTop = MateDesktopFloor(g_x + CELL / 2, g_y + CELL);
    /* Sheep top is hookY - 6 and the cell is 40, so the feet are hookY + 34 down. */
    lowest = floorTop - hookY - 34;
    if (g_y > lowest) {
        g_y = lowest;
    }
}

static void Present(void)
{
    int frame;
    int sheet;
    int cell;
    int flip;
    int y;
    int x;
    HDC screenDC;
    HDC memoryDC;
    HGDIOBJ previous;
    POINT destination;
    POINT source;
    SIZE size;
    BLENDFUNCTION blend;

    if (g_hwnd == NULL || g_surfaceBits == NULL) {
        return;
    }
    frame = g_sprite;
    if (frame >= 256) {
        frame -= 256;
    }
    sheet = frame / CELLS;
    cell = frame % CELLS;
    if (sheet < 0 || sheet >= SHEET_COUNT || g_sheets[sheet] == NULL) {
        return;
    }
    /* dir > 0 uses the sheet as stored. dir < 0 uses the mirrored cell (index + 0x100). */
    flip = g_dir <= 0;
    for (y = 0; y < CELL; y++) {
        const DWORD *row = g_sheets[sheet] + y * SHEET_W + cell * CELL;
        for (x = 0; x < CELL; x++) {
            int sourceX = flip ? CELL - 1 - x : x;
            g_surfaceBits[y * CELL + x] = row[sourceX];
        }
    }

    screenDC = GetDC(NULL);
    memoryDC = CreateCompatibleDC(screenDC);
    previous = SelectObject(memoryDC, g_surface);
    destination.x = g_x;
    destination.y = g_y;
    source.x = 0;
    source.y = 0;
    size.cx = CELL;
    size.cy = CELL;
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(g_hwnd, screenDC, &destination, &size, memoryDC, &source, 0, &blend, ULW_ALPHA);
    /* Sheep read another mate's position from window words 0 and 2, not from the window rect. */
    SetWindowWord(g_hwnd, 0, (WORD)(short)g_x);
    SetWindowWord(g_hwnd, 2, (WORD)(short)g_y);
    SetWindowWord(g_hwnd, MATE_WORD_STATUS, (WORD)g_status);
    /* 1 faces left, 2 faces right. The carried sheep reads this. */
    SetWindowWord(g_hwnd, 4, (WORD)(g_dir > 0 ? 1 : 2));
    {
        int hookX;
        int hookY = FishBobber(&hookX);
        SetWindowWord(g_hwnd, MATE_WORD_HOOK_X, (WORD)(short)hookX);
        SetWindowWord(g_hwnd, MATE_WORD_HOOK_Y, (WORD)(short)hookY);
    }
    SelectObject(memoryDC, previous);
    DeleteDC(memoryDC);
    ReleaseDC(NULL, screenDC);
}


static BOOL CALLBACK FloorProc(HWND hwnd, LPARAM param)
{
    FloorScan *scan = (FloorScan *)param;
    wchar_t className[64];
    RECT rect;
    int style;
    int overlap;
    DWORD cloaked = 0;

    if (hwnd == scan->self || hwnd == g_owner || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return TRUE;
    }
    style = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (style & WS_EX_TOOLWINDOW) {
        return TRUE;
    }
    className[0] = 0;
    GetClassNameW(hwnd, className, 64);
    if (wcscmp(className, L"Orange") == 0 ||
        wcscmp(className, L"Progman") == 0 ||
        wcscmp(className, L"WorkerW") == 0 ||
        wcscmp(className, L"Shell_TrayWnd") == 0 ||
        wcscmp(className, L"Shell_SecondaryTrayWnd") == 0) {
        return TRUE;
    }
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
        return TRUE;
    }
    if (!GetWindowRect(hwnd, &rect)) {
        return TRUE;
    }
    if (rect.right - rect.left < 48 || rect.bottom - rect.top < 48) {
        return TRUE;
    }
    if (rect.left <= scan->work.left && rect.top <= scan->work.top &&
        rect.right >= scan->work.right && rect.bottom >= scan->work.bottom - 8) {
        return TRUE;
    }
    overlap = min(scan->right, rect.right) - max(scan->left, rect.left);
    if (overlap < 16) {
        return TRUE;
    }
    if (rect.top >= scan->feet - 3 && rect.top <= scan->nextFeet + 2 && rect.top < scan->floorY) {
        scan->floorY = rect.top;
    }
    return TRUE;
}

static int FindFloor(const RECT *work, int feet, int nextFeet)
{
    FloorScan scan;
    scan.self = g_hwnd;
    scan.work = *work;
    scan.left = g_x + 6;
    scan.right = g_x + CELL - 6;
    scan.feet = feet;
    scan.nextFeet = nextFeet < feet ? feet : nextFeet;
    scan.floorY = MateDesktopFloor(g_x + CELL / 2, feet);
    EnumWindows(FloorProc, (LPARAM)&scan);
    return scan.floorY;
}

static RECT MonitorWorkArea(void)
{
    MONITORINFO info;
    POINT center;
    HMONITOR monitor;
    info.cbSize = sizeof(info);
    center.x = g_x + CELL / 2;
    center.y = g_y + CELL / 2;
    monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(monitor, &info)) {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &info.rcWork, 0);
    }
    return info.rcWork;
}

static int OffScreen(const RECT *work)
{
    return g_x > work->right || g_x < work->left - CELL ||
           g_y < work->top - CELL || g_y > work->bottom;
}

static void PickAction(const RECT *work)
{
    int span;

    if (OffScreen(work)) {
        span = work->bottom - work->top - CELL;
        if (span < 1) {
            span = 1;
        }
        if ((rand() & 1) == 0) {
            g_dir = 1;
            g_x = work->right;
        } else {
            g_dir = -1;
            g_x = work->left - CELL;
        }
        g_y = work->top + (rand() % span);
        g_state = 14;
        return;
    }
    {
        int action = kActionTable[rand() % 80];
        /* Always Active only skips sleep during wandering. The list still plays Rest. */
        if (g_noSleep && action == 29) {
            action = 24;
        }
        BeginAction(action);
    }
}

/* States 8/9 and 14/15 are the run and walk handlers at CS:306E and CS:33E3. */
static void StepLocomotion(const RECT *work, int step, int otherFrame)
{
    int previous;

    previous = g_gate;
    g_gate++;
    if (previous < 1) {
        return;
    }
    g_gate = 0;
    {
        int nextX = g_x - g_dir * step;
        if (MateInFront(nextX, g_y) && g_turnCool == 0) {
            g_turnCool = 24;
            g_state = 33;
            return;
        }
        g_x = nextX;
    }
    g_sprite = (g_sprite == 148) ? otherFrame : 148;
    if (g_dir > 0 && g_x < work->left) {
        g_state = 33;
    } else if (g_dir < 0 && g_x > work->right - CELL) {
        g_state = 33;
    }
    previous = g_left;
    g_left--;
    if (previous <= 0) {
        g_state = 47;
    }
}

/* 107.bmp cells 0-4. Holds the full flower until a sheep eats it or the timer ends. */
static void AdvanceBloom(void)
{
    if (g_unbloom) {
        /* Sheep is chewing: hold the frame the last bite selected. */
        g_sprite = MATE_BLOOM_FIRST + g_gate;
        g_left++;
        if (g_left > 700) {
            g_unbloom = 0;
            g_status = MATE_STATUS_IDLE;
            g_gate = 0;
            g_left = 0;
            g_sprite = 148;
            g_state = 1;
        }
        return;
    }
    g_left++;
    if (g_gate < (MATE_BLOOM_LAST - MATE_BLOOM_FIRST) && (g_left % 4) == 0) {
        g_gate++;
    }
    g_sprite = MATE_BLOOM_FIRST + g_gate;
    if (g_left > 700) {
        g_status = MATE_STATUS_IDLE;
        g_gate = 0;
        g_left = 0;
        g_sprite = 148;
        g_state = 1;
    }
}

static void StepToy(const RECT *work)
{
    int floorY;
    int minX;
    int maxX;

    g_x += g_vx;
    minX = work->left;
    maxX = work->right - CELL;
    if (maxX < minX) {
        maxX = minX;
    }
    if (g_x < minX) {
        g_x = minX;
        g_vx = -(g_vx / 2);
    } else if (g_x > maxX) {
        g_x = maxX;
        g_vx = -(g_vx / 2);
    }
    if (g_vx < -1) {
        g_dir = 1;
    } else if (g_vx > 1) {
        g_dir = -1;
    }
    g_vy += 2;
    floorY = FindFloor(work, g_y + CELL, g_y + CELL + 64);
    g_y += g_vy;
    if (g_y + CELL >= floorY) {
        int landedHard = g_vy > 8;
        g_y = floorY - CELL;
        if (g_vy > 0) {
            g_vy = -(g_vy / 2);
            g_vx = (g_vx * 3) / 4;
            if (g_vy > -3 && g_vy < 3) {
                g_vy = 0;
            }
            if (landedHard) {
                PlayOrangeCry();
            }
        }
    } else if (g_y < work->top - CELL) {
        g_y = work->top - CELL;
        if (g_vy < 0) {
            g_vy = 0;
        }
    }
    if (g_vx < -2 || g_vx > 2 || g_vy < -2 || g_vy > 2) {
        g_toyMoved = 1;
        g_toyStill = 0;
    }
    if (g_y + CELL < floorY || g_vy < 0) {
        static const short cryFly[] = { 52, 53, 54, 55, 154, 155 };
        g_sprite = cryFly[(g_gate++ / 2) % 6];
        return;
    }
    g_sprite = 148;
    if (g_toyMoved && g_vx > -2 && g_vx < 2 && g_vy == 0) {
        g_toyStill++;
        if (g_toyStill > 8) {
            g_status = MATE_STATUS_IDLE;
            g_toyMoved = 0;
            g_toyStill = 0;
            g_vx = 0;
            g_vy = 0;
            g_state = 1;
            SetWindowLongPtrW(g_hwnd, GWLP_USERDATA, 0);
        }
    }
}

/* Original frame lists. 0 ends the list. Numbers are sheet * 16 + cell. */
static const short kJump[] = { 22, 23, 24, 25, 26, 27, 0 };
static const short kSurprise[] = { 46, 47, 48, 49, 52, 53, 0 };
static const short kCrouch[] = { 128, 129, 0 };
static const short kSmoke[] = { 116, 116, 112, 114, 112, 114, 112, 114, 0 };
static const short kGlare[] = { 91, 92, 93, 94, 93, 94, 93, 94, 0 };
static const short kSleep[] = { 105, 106, 0 };
static const short kIdleHold[] = { 148, 0 };
static const short kFlip[] = { 34, 35, 35, 36, 36, 37, 37, 17, 0 };
static const short kHoop[] = { 74, 75, 74, 75, 76, 77, 76, 77, 78, 79, 78, 79, 0 };
/* State 48 indexes only the first four words at DS:0x594. */
static const short kRead[] = { 2, 0, 2, 4, 0 };
static const short kCry[] = { 46, 47, 48, 49, 50, 51, 52, 53, 0 };
static const short kFish[] = { 118, 120, 122, 120, 0 };
static const short kYawn[] = { 66, 67, 68, 69, 70, 69, 70, 69, 70, 0 };
static const short kLaugh[] = { 158, 159, 158, 160, 0 };
static const short kStrut[] = { 161, 162, 0 };
static const short kShuffle[] = { 161, 163, 0 };
/* State 28 indexes DS:0x610 until the 0. Frame 100 holds for a few seconds. */
static const short kFlower[] = {
    102, 91, 92, 93, 93, 94, 94, 93, 93, 94, 94, 93, 93,
    84, 85, 95, 96, 97, 98, 99, 100, 99, 98, 96,
    95, 85, 83, 92, 91, 102, 105, 0
};
static const short kTumble[] = { 54, 55, 56, 57, 58, 59, 60, 61, 0 };
static const short kWink[] = { 71, 73, 0 };
static const short kFaint[] = { 50, 51, 52, 105, 105, 0 };
static const short kPerform[] = { 17, 50, 52, 105, 0 };
static const short kSmokeWalk[] = { 117, 117, 113, 115, 113, 115, 113, 115, 0 };
/* State 56 indexes only the first four words at DS:0x59C. */
static const short kReadHop[] = { 3, 1, 3, 5, 0 };
static const short kSaunter[] = { 38, 39, 40, 44, 45, 0 };
static const short kHop[] = { 38, 39, 40, 41, 42, 43, 44, 45, 0 };
static const short kFishTug[] = { 119, 121, 123, 121, 0 };
static const short kPortal[] = { 150, 151, 152, 153, 154, 155, 156, 164, 0 };
static const short kBerries[] = { 28, 29, 0 };
/* DS:0x362, one body-height bob over 48 ticks. */
static const short kBob[48] = {
    0, 5, 10, 15, 19, 24, 28, 31, 34, 36, 38, 39, 39, 39, 38, 36,
    34, 31, 28, 24, 20, 15, 10, 5, 0, -6, -11, -16, -20, -25, -29, -32,
    -35, -37, -39, -40, -40, -40, -39, -37, -35, -32, -29, -25, -21, -16, -11, -6
};

static void PlayOrangeWave(int soundId)
{
    if (!g_soundOn || soundId == 0) {
        return;
    }
    PlaySoundW(MAKEINTRESOURCEW(soundId), g_inst, SND_ASYNC | SND_RESOURCE | SND_NODEFAULT);
}

static void StartAnim(const short *frames, int hold, int loops, int soundId)
{
    g_anim = frames;
    g_animHold = hold < 1 ? 1 : hold;
    g_animLoops = loops < 1 ? 1 : loops;
    g_clipMotion = 0;
    g_gate = 0;
    g_left = 0;
    g_sprite = frames[0];
    g_state = 90;
    PlayOrangeWave(soundId);
}

static void StartDrift(const short *frames, int hold, int loops, int step)
{
    StartAnim(frames, hold, loops, 0);
    g_clipMotion = 2;
    g_clipStep = step < 1 ? 1 : step;
}

static void StartBob(const short *frames, int ticks, int step)
{
    g_anim = frames;
    g_clipMotion = 1;
    g_clipTick = 0;
    g_clipLeft = ticks < 1 ? 1 : ticks;
    g_clipBaseY = g_y;
    g_clipStep = step;
    g_sprite = frames[0];
    g_state = 91;
}

static void StepAnim(void)
{
    int count;

    if (g_anim == NULL || g_anim[0] == 0) {
        g_anim = NULL;
        g_sprite = 148;
        g_state = 1;
        return;
    }
    count = 0;
    while (g_anim[count] != 0) {
        count++;
    }
    g_left++;
    if (g_left < g_animHold) {
        g_sprite = g_anim[g_gate];
        return;
    }
    g_left = 0;
    g_gate++;
    if (g_gate >= count) {
        g_animLoops--;
        if (g_animLoops > 0) {
            g_gate = 0;
            g_sprite = g_anim[0];
            return;
        }
        g_anim = NULL;
        g_clipMotion = 0;
        g_sprite = 148;
        g_state = 1;
        return;
    }
    g_sprite = g_anim[g_gate];
}

/* Menu order from Orange.exe CS:63FF, indices 1-29. */
static const struct DebugAnim {
    const wchar_t *name;
    int action;
} kDebugAnims[] = {
    { L"Run", 8 },
    { L"Walk", 14 },
    { L"Push-ups", 18 },
    { L"Smoke", 22 },
    { L"Doze", 24 },
    { L"Rest", 29 },
    { L"About face", 33 },
    { L"Flip", 39 },
    { L"Hoop", 44 },
    { L"Read", 48 },
    { L"Babble", 50 },
    { L"Fish", 68 },
    { L"Cry", 52 },
    { L"Sit-ups", 20 },
    { L"Peeled", 16 },
    { L"Peeled slow", 10 },
    { L"Flower", 27 },
    { L"Spin", 72 },
    { L"Drip", 80 },
    { L"Calm", 76 },
    { L"Skit", 101 },
    { L"Smoke walk", 54 },
    { L"Read hop", 56 },
    { L"Saunter", 58 },
    { L"Hop", 60 },
    { L"Hoop walk", 62 },
    { L"Fish tug", 70 },
    { L"Hatch", 123 },
    { L"Berries", 129 }
};

static void BeginAction(int action)
{
    if (g_clipMotion == 1) {
        g_y = g_clipBaseY;
    }
    g_clipMotion = 0;
    switch (action) {
    case 8:
    case 14:
        g_anim = NULL;
        g_state = action;
        return;
    case 33:
        g_dir = -g_dir;
        StartAnim(kIdleHold, 14, 1, 0);
        return;
    case 18:
        StartAnim(kCrouch, 4, 8, 0);
        return;
    case 22:
        StartAnim(kSmoke, 2, 3, 0);
        return;
    case 24:
        StartAnim(kGlare, 8, 1, 0);
        return;
    case 29:
        StartAnim(kSleep, 8, 5, 0);
        return;
    case 39:
        StartAnim(kFlip, 4, 2, 0);
        return;
    case 44:
        StartAnim(kHoop, 4, 2, 117);
        return;
    case 48:
        StartAnim(kRead, 2, 6, 0);
        return;
    case 50:
        StartAnim(kCry, 3, 2, 118);
        return;
    case 68:
        StartAnim(kFish, 4, 4, 0);
        return;
    case 52:
        StartAnim(kYawn, 5, 1, 114);
        return;
    case 20:
        StartAnim(kLaugh, 5, 3, 0);
        return;
    case 16:
        StartDrift(kStrut, 4, 8, 6);
        return;
    case 10:
        StartDrift(kShuffle, 4, 8, 2);
        return;
    case 27:
        g_gate = 0;
        g_left = 0;
        g_sprite = kFlower[0];
        g_state = 28;
        return;
    case 72:
        StartAnim(kTumble, 3, 2, 116);
        return;
    case 80:
        StartAnim(kWink, 6, 4, 0);
        return;
    case 76:
        StartAnim(kFaint, 6, 1, 0);
        return;
    case 101:
        StartAnim(kPerform, 6, 2, 0);
        return;
    case 54:
        StartBob(kSmokeWalk, 96, 4);
        return;
    case 56:
        StartBob(kReadHop, (rand() % 3 + 1) * 48, 4);
        return;
    case 58:
        StartDrift(kSaunter, 4, 6, 4);
        return;
    case 60:
        StartBob(kHop, 96, 4);
        return;
    case 62:
        StartDrift(kHoop, 4, 2, 4);
        return;
    case 70:
        StartBob(kFishTug, 96, 4);
        return;
    case 123:
        StartAnim(kPortal, 6, 1, 0);
        return;
    case 129:
        StartDrift(kBerries, 4, 8, 6);
        return;
    default:
        g_anim = NULL;
        g_sprite = 148;
        g_state = 1;
        return;
    }
}

static HWND FindRealBall(void)
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
        scanned++;
        if ((GetWindowLongW(wnd, GWL_STYLE) & WS_VISIBLE) == 0) {
            continue;
        }
        title[0] = 0;
        GetWindowTextW(wnd, title, 32);
        if (wcscmp(title, L"Screen Mate Ball") == 0) {
            return wnd;
        }
    }
    return NULL;
}

/* Chase only when the ball is on this floor. A high ball is not a hit. */
static int TryStartBallChase(void)
{
    HWND ball;
    int dx;
    int dy;

    if (g_status != MATE_STATUS_IDLE) {
        return 0;
    }
    ball = FindRealBall();
    if (ball == NULL) {
        return 0;
    }
    dx = ((short)GetWindowWord(ball, MATE_WORD_X) + 20) - (g_x + 20);
    dy = (short)GetWindowWord(ball, MATE_WORD_Y) - g_y;
    if (dx < 0) {
        dx = -dx;
    }
    if (dy < 0) {
        dy = -dy;
    }
    if (dx > 240 || dy > 36) {
        return 0;
    }
    g_left = 48;
    g_gate = 0;
    g_state = 230;
    return 1;
}

static void ReleaseCatch(void)
{
    if (g_caught != NULL && IsWindow(g_caught)) {
        SendMessageW(g_caught, WM_USER, MATE_MSG_DROP, 0);
    }
    g_caught = NULL;
    if (g_status == MATE_STATUS_FISH) {
        g_status = MATE_STATUS_IDLE;
    }
}

/* Nearest visible sheep, or NULL. Used to start a flying cast beside one. */
static HWND FindNearestSheep(void)
{
    HWND wnd;
    HWND best;
    UINT cmd;
    int scanned;
    int bestDist;

    wnd = GetDesktopWindow();
    cmd = GW_CHILD;
    scanned = 0;
    best = NULL;
    bestDist = 1 << 30;
    while ((wnd = GetWindow(wnd, cmd)) != NULL && scanned < 128) {
        RECT rc;
        int dx;
        int dy;
        int dist;

        cmd = GW_HWNDNEXT;
        scanned++;
        if ((GetWindowLongW(wnd, GWL_STYLE) & WS_VISIBLE) == 0) {
            continue;
        }
        if (!MateClassIs(wnd, MATE_SHEEP_CLASS)) {
            continue;
        }
        if (!GetWindowRect(wnd, &rc)) {
            continue;
        }
        dx = rc.left - g_x;
        dy = rc.top - g_y;
        dist = dx * dx + dy * dy;
        if (dist < bestDist) {
            bestDist = dist;
            best = wnd;
        }
    }
    return best;
}

/* Flying cast only. The sheep hangs on the line and follows the bob. */
static void TryHookSheep(void)
{
    HWND wnd;
    UINT cmd;
    int scanned;

    if (g_caught != NULL) {
        return;
    }
    wnd = GetDesktopWindow();
    cmd = GW_CHILD;
    scanned = 0;
    while ((wnd = GetWindow(wnd, cmd)) != NULL && scanned < 128) {
        RECT rc;
        int dx;
        int dy;

        cmd = GW_HWNDNEXT;
        scanned++;
        if ((GetWindowLongW(wnd, GWL_STYLE) & WS_VISIBLE) == 0) {
            continue;
        }
        if (!MateClassIs(wnd, MATE_SHEEP_CLASS)) {
            continue;
        }
        if (!GetWindowRect(wnd, &rc)) {
            continue;
        }
        dx = rc.left - g_x;
        dy = rc.top - g_y;
        if (dx < 0) {
            dx = -dx;
        }
        if (dy < 0) {
            dy = -dy;
        }
        if (dx > 48 || dy > 64) {
            continue;
        }
        if (SendMessageW(wnd, WM_USER, MATE_MSG_CAUGHT, (LPARAM)g_hwnd) != 0) {
            g_caught = wnd;
            g_status = MATE_STATUS_FISH;
            return;
        }
    }
}

static void TickState(const RECT *work)
{
    int previous;

    switch (g_state) {
    case 0:
    case 1:
        if ((rand() % 3) == 0 && TryStartBallChase()) {
            break;
        }
        PickAction(work);
        break;
    case 8:
        g_gate = 0;
        g_sprite = 148;
        g_left = (rand() % 10) + 10;
        g_state = 9;
        break;
    case 9:
        StepLocomotion(work, 16, 90);
        break;
    case 14:
        g_gate = 0;
        g_sprite = 148;
        g_left = (rand() % 10) + 10;
        g_state = 15;
        break;
    case 15:
        StepLocomotion(work, 6, 149);
        break;
    case 33:
        g_dir = -g_dir;
        g_sprite = 148;
        g_gate = 0;
        g_left = (rand() % 12) + 18;
        g_state = 15;
        break;
    case 47:
        previous = g_gate;
        g_gate++;
        if (previous < 1) {
            break;
        }
        g_gate = 0;
        g_sprite = 105;
        g_left = 0;
        g_state = 1;
        break;
    case 19:
        g_sprite = 148;
        g_vy = 0;
        g_state = 1;
        break;
    case 230: {
        HWND ball;
        HWND owner;
        int ballX;
        int ballY;
        int dx;
        int dy;

        ball = FindRealBall();
        if (ball == NULL || g_status != MATE_STATUS_IDLE) {
            g_state = 1;
            break;
        }
        ballX = (short)GetWindowWord(ball, MATE_WORD_X);
        ballY = (short)GetWindowWord(ball, MATE_WORD_Y);
        dx = (ballX + 20) - (g_x + 20);
        dy = ballY - g_y;
        if (dy < 0) {
            dy = -dy;
        }
        if (dy > 48) {
            g_state = 1;
            break;
        }
        if (dx > 0) {
            g_dir = -1;
        } else if (dx < 0) {
            g_dir = 1;
        }
        if (dx < 0) {
            dx = -dx;
        }
        if (dx <= 28 && dy <= 28) {
            owner = (HWND)GetWindowLongPtrW(ball, GWLP_USERDATA);
            if (owner != NULL && IsWindow(owner)) {
                SendMessageW(owner, WM_USER, MATE_MSG_KICK, (LPARAM)(short)g_dir);
            }
            PlayOrangeWave(114);
            g_state = 33;
            break;
        }
        g_x -= g_dir * 10;
        g_sprite = (g_gate++ & 1) ? 90 : 148;
        if (--g_left <= 0) {
            g_state = 1;
        }
        break;
    }
    case 90:
        StepAnim();
        if (g_state == 90 && g_clipMotion == 2) {
            g_x -= g_dir * g_clipStep;
            if (g_x < work->left) {
                g_x = work->left;
                g_dir = -1;
            }
            if (g_x > work->right - CELL) {
                g_x = work->right - CELL;
                g_dir = 1;
            }
        }
        break;
    case 91: {
        int count = 0;
        int index;
        while (g_anim != NULL && g_anim[count] != 0) {
            count++;
        }
        if (count < 1) {
            g_clipMotion = 0;
            g_state = 1;
            break;
        }
        g_clipTick++;
        index = (g_clipTick / 2) % count;
        g_sprite = g_anim[index];
        g_y = g_clipBaseY + kBob[g_clipTick % 48];
        g_x -= g_dir * g_clipStep;
        if (g_x < work->left) {
            g_x = work->left;
            g_dir = -1;
        }
        if (g_x > work->right - CELL) {
            g_x = work->right - CELL;
            g_dir = 1;
        }
        if (g_anim == kFishTug) {
            TryHookSheep();
        }
        KeepCarryVisible();
        if (g_clipTick >= g_clipLeft) {
            if (g_status == MATE_STATUS_FISH) {
                ReleaseCatch();
            }
            g_y = g_clipBaseY;
            g_anim = NULL;
            g_clipMotion = 0;
            g_sprite = 148;
            g_state = 1;
        }
        break;
    }
    case 28: {
        int waited = g_left;
        g_left++;
        if (waited < 2) {
            break;
        }
        g_left = 0;
        g_gate++;
        if (kFlower[g_gate] == 0) {
            g_sprite = 148;
            g_state = 1;
            break;
        }
        g_sprite = kFlower[g_gate];
        if (g_sprite == 84) {
            g_x += g_dir * 3;
        } else if (g_sprite == 83) {
            g_sprite = 84;
            g_x -= g_dir * 3;
        }
        if (g_x < work->left) {
            g_x = work->left;
        }
        if (g_x > work->right - CELL) {
            g_x = work->right - CELL;
        }
        if (kFlower[g_gate] == 100) {
            g_left = -20 - (rand() % 20);
        }
        break;
    }
    case 69:
        AdvanceBloom();
        break;
    default:
        /* Action ids from the table whose handlers are not transcribed yet. */
        g_state = 1;
        break;
    }
}

static void OnTick(void)
{
    RECT work;
    int floorY;

    MateEnsureDirector(g_hwnd);
    if (g_turnCool > 0) {
        g_turnCool--;
    }
    if (g_dragging || g_hwnd == NULL) {
        return;
    }
    work = MonitorWorkArea();
    if (g_flinch > 0) {
        g_x -= g_flinchDir * 6;
        if (g_x < work.left) {
            g_x = work.left;
        }
        if (g_x > work.right - CELL) {
            g_x = work.right - CELL;
        }
        g_flinch--;
    }
    if (g_status == MATE_STATUS_TOY) {
        g_chimePause = 0;
        StepToy(&work);
        Present();
        return;
    }
    if (g_chimePause > 0) {
        g_chimePause--;
        g_sprite = 148;
        Present();
        return;
    }
    floorY = FindFloor(&work, g_y + CELL, g_y + CELL + 64);
    if (g_gravityOn && g_y + CELL < floorY && !(g_state == 91 && g_clipMotion == 1)) {
        /* State 13: sprite 90, vertical speed starts negative and grows by 2. */
        g_vy += 2;
        g_y += g_vy;
        if (g_y + CELL >= floorY) {
            g_y = floorY - CELL;
            g_vy = 0;
            if (g_state == 19) {
                g_sprite = 148;
                g_state = 1;
            }
        } else if (g_state == 19) {
            int falling = g_vy >= 0;
            int step = g_gate > 2 ? 2 : g_gate;
            if ((g_left++ & 1) == 0 && g_gate < 2) {
                g_gate++;
            }
            g_sprite = kJump[(falling ? 3 : 0) + step];
        } else if (g_flinch > 0) {
            g_sprite = kSurprise[g_flinch % 6];
        } else if (g_status == MATE_STATUS_BLOOM) {
            AdvanceBloom();
        } else if (g_status != MATE_STATUS_FISH) {
            g_sprite = 90;
        }
        Present();
        return;
    }
    TickState(&work);
    if (g_gravityOn && g_status != MATE_STATUS_BLOOM && g_state != 19 &&
        !(g_state == 91 && g_clipMotion == 1)) {
        floorY = FindFloor(&work, g_y + CELL, g_y + CELL);
        g_y = floorY - CELL;
    }
    if (g_flinch > 0 && g_status != MATE_STATUS_IDLE) {
        g_sprite = kSurprise[g_flinch % 6];
    }
    Present();
}

static INT_PTR CALLBACK SettingsProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    switch (message) {
    case WM_INITDIALOG:
        CheckDlgButton(dialog, IDC_GRAVITY, g_gravityOn ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_SOUND, g_soundOn ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_ACTIVE, g_noSleep ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK:
            g_gravityOn = IsDlgButtonChecked(dialog, IDC_GRAVITY) == BST_CHECKED;
            g_soundOn = IsDlgButtonChecked(dialog, IDC_SOUND) == BST_CHECKED;
            g_noSleep = IsDlgButtonChecked(dialog, IDC_ACTIVE) == BST_CHECKED;
            SaveConfig();
            if (!g_soundOn) {
                PlaySoundW(NULL, NULL, 0);
            }
            EndDialog(dialog, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        case IDC_EXIT:
            EndDialog(dialog, IDC_EXIT);
            return TRUE;
        case IDB_HELP:
            MessageBoxW(dialog,
                L"Drag the orange to move it.\n"
                L"Double-click for these settings.\n"
                L"Ctrl+Shift+double-click opens Animations.\n"
                L"Right-click for Settings, Animations or Exit.\n"
                L"Right double-click exits.\n"
                L"Drop a .wav file on it to play that sound.\n\n"
                L"Gravity drops it onto the taskbar and window tops.\n"
                L"Sound On plays the original cries.\n"
                L"Always Active keeps it from falling asleep.",
                L"Orange", MB_OK | MB_ICONINFORMATION);
            return TRUE;
        default:
            break;
        }
        break;
    default:
        break;
    }
    return FALSE;
}

static void OpenSettings(void)
{
    INT_PTR result;
    KillTimer(g_hwnd, 1);
    result = DialogBoxW(g_inst, MAKEINTRESOURCEW(107), g_hwnd, SettingsProc);
    if (result == IDC_EXIT) {
        DestroyWindow(g_hwnd);
        return;
    }
    if (IsWindow(g_hwnd)) {
        SetTimer(g_hwnd, 1, TIMER_MS, NULL);
        Present();
    }
}

static void PlayChosenAction(int action)
{
    if (g_status == MATE_STATUS_FISH) {
        ReleaseCatch();
    }
    if (g_status == MATE_STATUS_BLOOM || g_status == MATE_STATUS_TOY) {
        g_unbloom = 0;
        g_status = MATE_STATUS_IDLE;
        g_vx = 0;
        SetWindowLongPtrW(g_hwnd, GWLP_USERDATA, 0);
    }
    g_chimePause = 0;
    g_anim = NULL;
    g_clipMotion = 0;
    g_gate = 0;
    g_left = 0;
    BeginAction(action);
    Present();
}

static void NudgeOrange(int dx, int dy)
{
    g_x += dx;
    g_y += dy;
    Present();
}

static INT_PTR CALLBACK AnimationsProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    int i;
    int selected;
    int count = (int)(sizeof(kDebugAnims) / sizeof(kDebugAnims[0]));
    (void)lParam;

    switch (message) {
    case WM_INITDIALOG:
        for (i = 0; i < count; i++) {
            SendDlgItemMessageW(dialog, IDC_ANIM_LIST, LB_ADDSTRING, 0, (LPARAM)kDebugAnims[i].name);
        }
        SendDlgItemMessageW(dialog, IDC_ANIM_LIST, LB_SETCURSEL, 0, 0);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(dialog, (int)LOWORD(wParam));
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_ANIM_LIST && HIWORD(wParam) == LBN_SELCHANGE) {
            selected = (int)SendDlgItemMessageW(dialog, IDC_ANIM_LIST, LB_GETCURSEL, 0, 0);
            if (selected >= 0 && selected < count) {
                PlayChosenAction(kDebugAnims[selected].action);
            }
            return TRUE;
        }
        switch (LOWORD(wParam)) {
        case IDC_NUDGE_UP:
            NudgeOrange(0, -20);
            return TRUE;
        case IDC_NUDGE_DOWN:
            NudgeOrange(0, 20);
            return TRUE;
        case IDC_NUDGE_LEFT:
            NudgeOrange(-20, 0);
            return TRUE;
        case IDC_NUDGE_RIGHT:
            NudgeOrange(20, 0);
            return TRUE;
        default:
            break;
        }
        break;
    default:
        break;
    }
    return FALSE;
}

static void OpenAnimations(void)
{
    DialogBoxW(g_inst, MAKEINTRESOURCEW(108), g_hwnd, AnimationsProc);
    if (IsWindow(g_hwnd)) {
        Present();
    }
}

static void PlayOrangeCry(void)
{
    if (!g_soundOn) {
        return;
    }
    PlaySoundW(MAKEINTRESOURCEW(113), g_inst, SND_ASYNC | SND_RESOURCE | SND_NODEFAULT);
}

static LRESULT CALLBACK MateProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    POINT cursor;
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(NULL, IDC_HAND));
        return TRUE;
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        GetCursorPos(&cursor);
        g_grabX = cursor.x - g_x;
        g_grabY = cursor.y - g_y;
        g_dragging = 1;
        return 0;
    case WM_MOUSEMOVE:
        if (g_dragging) {
            GetCursorPos(&cursor);
            g_x = cursor.x - g_grabX;
            g_y = cursor.y - g_grabY;
            Present();
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_dragging) {
            g_dragging = 0;
            ReleaseCapture();
            g_state = 1;
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        if (g_dragging) {
            g_dragging = 0;
            ReleaseCapture();
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000)) {
            OpenAnimations();
        } else {
            OpenSettings();
        }
        return 0;
    case WM_RBUTTONDBLCLK:
        g_rightExit = 1;
        KillTimer(hwnd, 2);
        DestroyWindow(hwnd);
        return 0;
    case WM_RBUTTONUP: {
        if (g_rightExit) {
            return 0;
        }
        /* Wait out the double-click window so Exit is not eaten by the menu. */
        SetTimer(hwnd, 2, GetDoubleClickTime(), NULL);
        return 0;
    }
    case WM_CONTEXTMENU:
        return 0;
    case WM_TIMER:
        if (wParam == 2) {
            HMENU menu;
            int command;
            POINT cursor;
            KillTimer(hwnd, 2);
            if (g_rightExit) {
                return 0;
            }
            menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, L"Settings...");
        AppendMenuW(menu, MF_STRING, 2, L"Animations...");
        AppendMenuW(menu, MF_STRING, 3, L"Exit");
        GetCursorPos(&cursor);
        command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, hwnd, NULL);
        DestroyMenu(menu);
        if (command == 1) {
            OpenSettings();
        } else if (command == 2) {
            OpenAnimations();
        } else if (command == 3) {
            DestroyWindow(hwnd);
        }
        return 0;
        }
        OnTick();
        return 0;
    case WM_DROPFILES: {
        wchar_t path[MAX_PATH];
        HDROP drop = (HDROP)wParam;
        if (DragQueryFileW(drop, 0, path, MAX_PATH) > 0) {
            int n = lstrlenW(path);
            if (n >= 4 && (lstrcmpiW(path + n - 4, L".wav") == 0)) {
                PlaySoundW(path, NULL, SND_ASYNC | SND_FILENAME | SND_NODEFAULT);
                PlayChosenAction(50);
            }
        }
        DragFinish(drop);
        return 0;
    }
    case WM_USER:
        if (wParam == 3) {
            int dir = (int)(short)lParam;
            if (dir == 0) {
                dir = 1;
            }
            if (g_status == MATE_STATUS_TOY) {
                g_vx = -dir * (16 + (rand() & 7));
                g_vy = -(14 + (rand() & 7));
                g_toyMoved = 1;
                PlayOrangeCry();
                return 0;
            }
            if (!g_dragging) {
                g_dir = dir;
                g_flinch = 10;
                g_flinchDir = dir;
                PlayOrangeCry();
                if (g_status == MATE_STATUS_IDLE) {
                    StartAnim(kSurprise, 3, 1, 0);
                } else {
                    g_sprite = kSurprise[0];
                    Present();
                }
            }
            return 0;
        }
        if (wParam == MATE_MSG_KICK && g_status == MATE_STATUS_TOY) {
            int dir = (int)(short)lParam;
            if (dir == 0) {
                dir = 1;
            }
            g_vx = -dir * (16 + (rand() & 7));
            g_vy = -(14 + (rand() & 7));
            PlayOrangeCry();
            return 0;
        }
        if (wParam == MATE_MSG_CHIME) {
            if (g_status == MATE_STATUS_IDLE && !g_dragging) {
                g_chimePause = 18;
                g_sprite = 148;
                PlayOrangeCry();
                Present();
            }
            return 0;
        }
        if (wParam == MATE_MSG_BECOME_TOY) {
            if (g_status != MATE_STATUS_IDLE) {
                return 0;
            }
            g_status = MATE_STATUS_TOY;
            g_toyMoved = 0;
            g_toyStill = 0;
            g_vx = 0;
            g_vy = 0;
            g_sprite = 148;
            g_state = 1;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)hwnd);
            Present();
            return 1;
        }
        if (wParam == MATE_MSG_UNBLOOM) {
            if (g_status != MATE_STATUS_BLOOM) {
                return 0;
            }
            if (g_unbloom == 0) {
                g_unbloom = 1;
                g_left = 0;
            }
            if (g_gate > 0) {
                g_gate--;
            }
            g_sprite = MATE_BLOOM_FIRST + g_gate;
            Present();
            return 0;
        }
        if (wParam == MATE_MSG_EATEN) {
            g_unbloom = 0;
            g_status = MATE_STATUS_IDLE;
            g_gate = 0;
            g_left = 0;
            g_vx = 0;
            g_vy = 0;
            g_sprite = 148;
            g_state = 1;
            Present();
            return 0;
        }
        if (wParam == MATE_MSG_END_ROLE) {
            if (g_status == MATE_STATUS_TOY) {
                g_vx = 0;
                g_vy = 0;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                g_unbloom = 0;
                g_gate = 0;
                g_left = 0;
                g_sprite = MATE_BLOOM_FIRST;
                g_status = MATE_STATUS_BLOOM;
                g_state = 69;
                Present();
            }
            return 0;
        }
        return 0;
    case WM_DESTROY:
        ReleaseCatch();
        MateDirectorDetach(hwnd);
        PlaySoundW(NULL, NULL, 0);
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

static LRESULT CALLBACK OwnerProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

static BOOL CALLBACK CountProc(HWND hwnd, LPARAM param)
{
    wchar_t title[32];
    title[0] = 0;
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }
    GetWindowTextW(hwnd, title, 32);
    if (wcscmp(title, L"Screen Mate") == 0) {
        (*(int *)param)++;
    }
    return TRUE;
}

typedef struct MateProbe {
    HWND self;
    int x;
    int y;
    int hit;
} MateProbe;

static BOOL CALLBACK MateProbeProc(HWND hwnd, LPARAM param)
{
    MateProbe *probe = (MateProbe *)param;
    wchar_t title[32];
    RECT rect;
    int ox;
    int oy;

    if (hwnd == probe->self || !IsWindowVisible(hwnd)) {
        return TRUE;
    }
    title[0] = 0;
    GetWindowTextW(hwnd, title, 32);
    if (wcscmp(title, L"Screen Mate") != 0) {
        return TRUE;
    }
    ox = (short)GetWindowWord(hwnd, 0);
    oy = (short)GetWindowWord(hwnd, 2);
    if (!GetWindowRect(hwnd, &rect)) {
        return TRUE;
    }
    if (rect.right - rect.left < 20 || rect.bottom - rect.top < 20 ||
        rect.right - rect.left > 120 || rect.bottom - rect.top > 120) {
        return TRUE;
    }
    /* A mate built like Scmpoo publishes sprite x,y in window words.
       The on-screen window rect is the fallback when those words were never set. */
    if (ox != 0 || oy != 0) {
        rect.left = ox;
        rect.top = oy;
        rect.right = ox + CELL;
        rect.bottom = oy + CELL;
    }
    if (rect.right > probe->x + 14 && rect.left < probe->x + CELL - 14 &&
        rect.bottom > probe->y + 10 && rect.top < probe->y + CELL - 10) {
        probe->hit = 1;
        return FALSE;
    }
    return TRUE;
}

static int MateInFront(int nextX, int y)
{
    MateProbe probe;
    probe.self = g_hwnd;
    probe.x = nextX;
    probe.y = y;
    probe.hit = 0;
    EnumWindows(MateProbeProc, (LPARAM)&probe);
    return probe.hit;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show)
{
    WNDCLASSW mateClass;
    WNDCLASSW ownerClass;
    INITCOMMONCONTROLSEX controls;
    RECT work;
    MSG message;
    int mates = 0;
    int i;
    (void)previous;
    (void)command;
    (void)show;

    g_inst = instance;
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);
    srand((unsigned)GetTickCount());
    BuildIniPath();
    LoadConfig();

    EnumWindows(CountProc, (LPARAM)&mates);
    if (mates >= MAX_MATES) {
        MessageBoxW(NULL,
            L"Screen Mate: maximum 8 instances",
            L"Screen Mate", MB_OK | MB_ICONEXCLAMATION);
        return 0;
    }

    for (i = 0; i < SHEET_COUNT; i++) {
        if (!LoadSheet(101 + i, i)) {
            MessageBoxW(NULL, L"Could not load the orange sprites.", L"Orange", MB_OK | MB_ICONERROR);
            return 0;
        }
    }
    if (!CreateSurface()) {
        return 0;
    }

    ZeroMemory(&ownerClass, sizeof(ownerClass));
    ownerClass.lpfnWndProc = OwnerProc;
    ownerClass.hInstance = instance;
    ownerClass.lpszClassName = L"OrangeOwner";
    RegisterClassW(&ownerClass);

    ZeroMemory(&mateClass, sizeof(mateClass));
    mateClass.lpfnWndProc = MateProc;
    mateClass.cbWndExtra = 12;
    mateClass.hInstance = instance;
    mateClass.hCursor = LoadCursorW(NULL, IDC_HAND);
    mateClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(100));
    mateClass.lpszClassName = L"Orange";
    RegisterClassW(&mateClass);

    g_owner = CreateWindowExW(0, L"OrangeOwner", L"", 0, 0, 0, 0, 0, NULL, NULL, instance, NULL);
    g_x = 0;
    g_y = 0;
    work = MonitorWorkArea();
    if (work.right - work.left > CELL) {
        g_x = work.left + (rand() % (work.right - work.left - CELL));
    } else {
        g_x = work.left;
    }
    g_y = work.top;
    g_vy = 0;
    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"Orange", L"Screen Mate", WS_POPUP,
        g_x, g_y, CELL, CELL, g_owner, NULL, instance, NULL);
    if (g_hwnd == NULL) {
        return 0;
    }
    g_state = 1;
    g_sprite = 148;
    if (wcsstr(GetCommandLineW(), L"--bloom") != NULL) {
        g_status = MATE_STATUS_BLOOM;
        g_gate = 0;
        g_left = 0;
        g_sprite = MATE_BLOOM_FIRST;
        g_state = 69;
    }
    if (wcsstr(GetCommandLineW(), L"--fish") != NULL) {
        HWND sheep;
        Sleep(700);
        sheep = FindNearestSheep();
        if (sheep != NULL) {
            RECT rc;
            if (GetWindowRect(sheep, &rc)) {
                /* Face left and start just to the sheep's right, so the cast flies over it. */
                g_dir = 1;
                g_x = rc.left + 72;
                g_y = rc.top;
                if (g_x > work.right - CELL) {
                    g_x = work.right - CELL;
                }
                if (g_x < work.left) {
                    g_x = work.left;
                }
            }
        }
        BeginAction(70);
    }
    Present();
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    DragAcceptFiles(g_hwnd, TRUE);
    SetTimer(g_hwnd, 1, TIMER_MS, NULL);

    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return (int)message.wParam;
}
