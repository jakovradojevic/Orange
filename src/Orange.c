/* Native Win32 reconstruction of the 16-bit Orange screen mate (Orange.exe).
   Same approach as Scmpoo: original sprite sheets and waves are resources,
   and a layered window replaces the 16-bit desktop blit. */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600

#include <windows.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <dwmapi.h>
#include <stdlib.h>
#include <wchar.h>

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

typedef struct FloorScan {
    HWND self;
    RECT work;
    int left;
    int right;
    int feet;
    int nextFeet;
    int floorY;
} FloorScan;

/* Word table at DS:0x212. rand() % 80 indexes it. 14 is walk, 8 is run. */
static const int kActionTable[80] = {
    14, 14, 8, 8, 14, 14, 8, 8, 14, 14, 8, 8, 14, 14, 8, 8,
    14, 14, 8, 8, 14, 14, 8, 8, 14, 14, 8, 8, 14, 14, 8, 8,
    14, 14, 8, 8, 14, 14, 8, 8, 14, 8, 24, 29, 14, 8, 24, 29,
    14, 8, 24, 29, 14, 8, 24, 29, 14, 8, 24, 29, 14, 8, 24, 29,
    14, 8, 24, 29, 18, 68, 22, 50, 44, 44, 48, 52, 50, 52, 22, 48
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
static void Present(void);
static void OpenSettings(void);
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
    scan.floorY = work->bottom;
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
    g_state = kActionTable[rand() % 80];
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
        if (MateInFront(nextX, g_y)) {
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

static void TickState(const RECT *work)
{
    int previous;

    switch (g_state) {
    case 0:
    case 1:
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
        g_state = 1;
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

    if (g_dragging || g_hwnd == NULL) {
        return;
    }
    work = MonitorWorkArea();
    floorY = FindFloor(&work, g_y + CELL, g_y + CELL + 64);
    if (g_gravityOn && g_y + CELL < floorY) {
        /* State 13: sprite 90, vertical speed starts negative and grows by 2. */
        g_vy += 2;
        g_y += g_vy;
        if (g_y + CELL >= floorY) {
            g_y = floorY - CELL;
            g_vy = 0;
        } else {
            g_sprite = 90;
            Present();
            return;
        }
    }
    TickState(&work);
    if (g_gravityOn) {
        floorY = FindFloor(&work, g_y + CELL, g_y + CELL);
        g_y = floorY - CELL;
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
                L"Right-click for Settings or Exit.\n\n"
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
        OpenSettings();
        return 0;
    case WM_RBUTTONUP: {
        HMENU menu = CreatePopupMenu();
        int command;
        AppendMenuW(menu, MF_STRING, 1, L"Settings...");
        AppendMenuW(menu, MF_STRING, 2, L"Exit");
        GetCursorPos(&cursor);
        command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, hwnd, NULL);
        DestroyMenu(menu);
        if (command == 1) {
            OpenSettings();
        } else if (command == 2) {
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_TIMER:
        OnTick();
        return 0;
    case WM_DESTROY:
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
    if (rect.right > probe->x && rect.left < probe->x + CELL &&
        rect.bottom > probe->y && rect.top < probe->y + CELL) {
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
    mateClass.cbWndExtra = 8;
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
    Present();
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    SetTimer(g_hwnd, 1, TIMER_MS, NULL);

    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return (int)message.wParam;
}
