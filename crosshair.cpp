// crosshair.cpp — low-latency crosshair overlay on DirectComposition + Direct2D,
// with a settings window and a tray icon.
//
// Latency: the crosshair lives in a tiny composition swapchain that is redrawn
// ONLY when a setting changes. No per-frame render loop, no injection into the
// game. The overlay window has a fixed size that fits the largest crosshair, so
// changing shape/size/thickness is a single redraw, never a window or swapchain
// rebuild. The settings window is a separate ordinary window; it does nothing
// while hidden and never touches the overlay path.
//
// Hotkeys:
//   Ctrl+Alt+O             open settings
//   Ctrl+Alt+H             show/hide crosshair
//   Ctrl+Alt+S             next shape
//   Ctrl+Alt+Plus / Minus  bigger / smaller (numpad keys work too)
//   Ctrl+Alt+C             next preset color
//   Ctrl+Alt+Q             quit
//   Ctrl+Alt+Shift+arrows  nudge by 1 px; Ctrl+Alt+Shift+0 resets the offset
// All settings are saved to crosshair.ini next to the exe.
//
// Arguments (optional, override crosshair.ini):
//   --shape N (0..5)  --size N (1..20)  --thick N (1..8)  --outline N (0..4)
//   --color RRGGBB|AARRGGBB  --opacity N (10..100)  --dx N  --dy N
//   --minimized   start in the tray without opening the settings window
//   --no-layered  do not make the overlay layered (MPO experiments)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef _WIN32_IE
#define _WIN32_IE 0x0A00
#endif
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dcomp.h>
#include <cwchar>
#include <cstdlib>
#include <cmath>

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dcomp.lib")
#endif

#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }
static int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------- shapes
enum Shape { SH_CROSS, SH_CROSS_DOT, SH_DOT, SH_T, SH_TRIANGLE, SH_RING, SH_COUNT };
static const wchar_t* kShapeNames[SH_COUNT] = {
    L"Крест", L"Крест с точкой", L"Точка", L"T-образный", L"Треугольник", L"Кольцо с точкой"};

const int kMaxScale = 20, kMaxThick = 8, kMaxOutline = 4, kMaxOffset = 300;

// Geometry as a function of the size step s = 1..20 (step 4 ~ the old default).
static int   CrossLen(int s) { return (int)lround(2.0 + 1.25 * s); }  // 3 .. 27
static int   CrossGap(int s) { return (int)lround(0.4 + 0.6 * s); }   // 1 .. 12
static float DotDiam(int s)  { return 1.5f + 0.6f * s; }              // 2.1 .. 13.5
static float TriSide(int s)  { return 5.0f + 1.5f * s; }              // 6.5 .. 35
static float RingRad(int s)  { return 3.0f + 0.9f * s; }              // 3.9 .. 21

// ---------------------------------------------------------------- settings
struct Settings {
    int shape = SH_CROSS, scale = 4, thick = 2, outline = 1, opacity = 100, dx = 0, dy = 0;
    float r = 0.f, g = 1.f, b = 0.f;
    bool layered = true, minimized = false;
} g_cfg;

static const float kPresets[][3] = {
    {0.f, 1.f, 0.f}, {0.f, 1.f, 1.f}, {1.f, 0.f, 1.f},
    {1.f, 1.f, 0.f}, {1.f, 0.2f, 0.2f}, {1.f, 1.f, 1.f},
};
const int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);
static int g_preset = 0;

static D2D1_COLOR_F CurrentColor() { return {g_cfg.r, g_cfg.g, g_cfg.b, g_cfg.opacity / 100.f}; }
static COLORREF CurrentColorRef() {
    return RGB((int)lround(g_cfg.r * 255), (int)lround(g_cfg.g * 255), (int)lround(g_cfg.b * 255));
}
static void SetColorRef(COLORREF c) {
    g_cfg.r = GetRValue(c) / 255.f; g_cfg.g = GetGValue(c) / 255.f; g_cfg.b = GetBValue(c) / 255.f;
}

// "RRGGBB" or "AARRGGBB"; alpha (if present) becomes the opacity.
static void ParseColor(const wchar_t* s) {
    unsigned long v = wcstoul(s, nullptr, 16);
    g_cfg.r = ((v >> 16) & 0xFF) / 255.f;
    g_cfg.g = ((v >> 8) & 0xFF) / 255.f;
    g_cfg.b = (v & 0xFF) / 255.f;
    if (wcslen(s) > 6) g_cfg.opacity = (int)lround(((v >> 24) & 0xFF) * 100.0 / 255.0);
}

static wchar_t g_ini[MAX_PATH];

static void InitIniPath() {
    GetModuleFileNameW(nullptr, g_ini, MAX_PATH);
    wchar_t* dot = wcsrchr(g_ini, L'.');
    if (dot) wcscpy(dot, L".ini"); // crosshair.ini next to the exe
}

static void LoadIni() {
    auto get = [](const wchar_t* k, int def) { return (int)GetPrivateProfileIntW(L"crosshair", k, def, g_ini); };
    g_cfg.shape   = get(L"shape", g_cfg.shape);
    g_cfg.scale   = get(L"scale", g_cfg.scale);
    g_cfg.thick   = get(L"thick", g_cfg.thick);
    g_cfg.outline = get(L"outline", g_cfg.outline);
    g_cfg.opacity = get(L"opacity", g_cfg.opacity);
    g_cfg.dx      = get(L"dx", 0);
    g_cfg.dy      = get(L"dy", 0);
    wchar_t buf[32] = {};
    GetPrivateProfileStringW(L"crosshair", L"color", L"", buf, 32, g_ini);
    size_t n = wcslen(buf);
    if (n >= 6) {
        ParseColor(buf);
    } else if (n > 0) { // old format: preset index
        g_preset = Clamp(_wtoi(buf), 0, kPresetCount - 1);
        g_cfg.r = kPresets[g_preset][0]; g_cfg.g = kPresets[g_preset][1]; g_cfg.b = kPresets[g_preset][2];
    }
}

static void SaveIni() {
    wchar_t buf[32];
    auto put = [&](const wchar_t* k, int v) {
        swprintf(buf, 32, L"%d", v);
        WritePrivateProfileStringW(L"crosshair", k, buf, g_ini);
    };
    put(L"shape", g_cfg.shape);
    put(L"scale", g_cfg.scale);
    put(L"thick", g_cfg.thick);
    put(L"outline", g_cfg.outline);
    put(L"opacity", g_cfg.opacity);
    put(L"dx", g_cfg.dx);
    put(L"dy", g_cfg.dy);
    COLORREF c = CurrentColorRef();
    swprintf(buf, 32, L"%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    WritePrivateProfileStringW(L"crosshair", L"color", buf, g_ini);
}

static void ParseArgs() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return;
    for (int i = 1; i < argc; ++i) {
        auto is = [&](const wchar_t* k) { return wcscmp(argv[i], k) == 0; };
        auto num = [&]() { return (i + 1 < argc) ? _wtoi(argv[++i]) : 0; };
        if (is(L"--shape")) g_cfg.shape = num();
        else if (is(L"--size")) g_cfg.scale = num();
        else if (is(L"--thick")) g_cfg.thick = num();
        else if (is(L"--outline")) g_cfg.outline = num();
        else if (is(L"--opacity")) g_cfg.opacity = num();
        else if (is(L"--dx")) g_cfg.dx = num();
        else if (is(L"--dy")) g_cfg.dy = num();
        else if (is(L"--minimized")) g_cfg.minimized = true;
        else if (is(L"--no-layered")) g_cfg.layered = false;
        else if (is(L"--color") && i + 1 < argc) ParseColor(argv[++i]);
    }
    LocalFree(argv);
}

static void Sanitize() {
    g_cfg.shape   = Clamp(g_cfg.shape, 0, SH_COUNT - 1);
    g_cfg.scale   = Clamp(g_cfg.scale, 1, kMaxScale);
    g_cfg.thick   = Clamp(g_cfg.thick, 1, kMaxThick);
    g_cfg.outline = Clamp(g_cfg.outline, 0, kMaxOutline);
    g_cfg.opacity = Clamp(g_cfg.opacity, 10, 100);
    g_cfg.dx      = Clamp(g_cfg.dx, -kMaxOffset, kMaxOffset);
    g_cfg.dy      = Clamp(g_cfg.dy, -kMaxOffset, kMaxOffset);
}

// ---------------------------------------------------------------- overlay graphics
HWND                  g_hwnd = nullptr;   // overlay window
int                   g_size = 0;         // overlay side, px (odd), fixed
ID3D11Device*         g_d3d = nullptr;
IDXGISwapChain1*      g_swap = nullptr;
ID2D1Factory1*        g_d2dFactory = nullptr;
ID2D1Device*          g_d2dDevice = nullptr;
ID2D1DeviceContext*   g_ctx = nullptr;
ID2D1Bitmap1*         g_target = nullptr;
ID2D1SolidColorBrush* g_brush = nullptr;
ID2D1StrokeStyle*     g_roundStroke = nullptr;
IDCompositionDevice*  g_dcomp = nullptr;
IDCompositionTarget*  g_dtarget = nullptr;
IDCompositionVisual*  g_visual = nullptr;
bool                  g_visible = true;

// Fixed size that fits the largest shape at the largest settings.
static void ComputeSize() {
    const int s = kMaxScale, t = kMaxThick;
    int half = CrossGap(s) + CrossLen(s) + t;
    int tri = (int)(TriSide(s) * 0.8660254f) + 1;
    int ring = (int)RingRad(s) + t;
    if (tri > half) half = tri;
    if (ring > half) half = ring;
    half += kMaxOutline + 2;
    g_size = 2 * half + 1;
}

static void PlaceWindow() {
    // Overlay pixel c = screen pixel width/2, so the boundary at c in overlay
    // coordinates is the exact screen center (960.0 on 1920).
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int c = g_size / 2;
    SetWindowPos(g_hwnd, HWND_TOPMOST, sw / 2 - c + g_cfg.dx, sh / 2 - c + g_cfg.dy,
                 g_size, g_size, SWP_NOACTIVATE);
}

static void FillRect(float l, float t, float r, float b) {
    g_ctx->FillRectangle(D2D1::RectF(l, t, r, b), g_brush);
}

// Cross arms inflated by `o` px (outline pass). Integer coordinates: pixel-sharp.
static void DrawCross(int o, bool top) {
    const float c = (float)(g_size / 2);
    const int t = g_cfg.thick;
    const float a0 = c - t / 2, a1 = a0 + t; // band [a0, a1) through the center
    const float gap = (float)CrossGap(g_cfg.scale), len = (float)CrossLen(g_cfg.scale);
    FillRect(a1 + gap - o, a0 - o, a1 + gap + len + o, a1 + o);   // right
    FillRect(a0 - gap - len - o, a0 - o, a0 - gap + o, a1 + o);   // left
    FillRect(a0 - o, a1 + gap - o, a1 + o, a1 + gap + len + o);   // down
    if (top)
        FillRect(a0 - o, a0 - gap - len - o, a1 + o, a0 - gap + o); // up
}

static void DrawDot(float diam, int o) {
    const float c = (float)(g_size / 2), r = diam / 2 + o;
    g_ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c, c), r, r), g_brush);
}

// Triangle pointing up; its tip is the aim point (exact screen center).
static void DrawTriangle(bool outlinePass) {
    const float c = (float)(g_size / 2);
    const float s = TriSide(g_cfg.scale), h = s * 0.8660254f;
    ID2D1PathGeometry* geo = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(g_d2dFactory->CreatePathGeometry(&geo))) return;
    if (SUCCEEDED(geo->Open(&sink))) {
        sink->BeginFigure(D2D1::Point2F(c, c), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(c + s / 2, c + h));
        sink->AddLine(D2D1::Point2F(c - s / 2, c + h));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        SafeRelease(sink);
        if (outlinePass)
            g_ctx->DrawGeometry(geo, g_brush, 2.f * g_cfg.outline, g_roundStroke);
        else
            g_ctx->FillGeometry(geo, g_brush);
    }
    SafeRelease(geo);
}

static void DrawRing(int o) {
    const float c = (float)(g_size / 2), r = RingRad(g_cfg.scale);
    g_ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(c, c), r, r), g_brush,
                       (float)(g_cfg.thick + 2 * o));
}

static void DrawShape(bool outlinePass) {
    const int o = outlinePass ? g_cfg.outline : 0;
    switch (g_cfg.shape) {
    case SH_CROSS:     DrawCross(o, true); break;
    case SH_CROSS_DOT: DrawCross(o, true); DrawDot((float)g_cfg.thick, o); break;
    case SH_DOT:       DrawDot(DotDiam(g_cfg.scale), o); break;
    case SH_T:         DrawCross(o, false); break;
    case SH_TRIANGLE:  DrawTriangle(outlinePass); break;
    case SH_RING:      DrawRing(o); DrawDot(2.f, o); break;
    }
}

static void Render() {
    if (!g_ctx) return;
    const D2D1_COLOR_F col = CurrentColor();
    g_ctx->BeginDraw();
    g_ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
    if (g_cfg.outline > 0) {
        g_brush->SetColor(D2D1::ColorF(0, 0, 0, col.a));
        DrawShape(true);
    }
    g_brush->SetColor(col);
    DrawShape(false);
    g_ctx->EndDraw();
    g_swap->Present(1, 0); // once per change, not every frame
}

static bool InitGraphics() {
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, &g_d3d, nullptr, nullptr);
    if (FAILED(hr)) return false;

    IDXGIDevice* dxgiDev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    if (FAILED(g_d3d->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) return false;
    dxgiDev->GetAdapter(&adapter);
    adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
    SafeRelease(adapter);
    if (!factory) { SafeRelease(dxgiDev); return false; }

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = g_size;
    sd.Height = g_size;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    sd.Scaling = DXGI_SCALING_STRETCH;
    hr = factory->CreateSwapChainForComposition(g_d3d, &sd, nullptr, &g_swap);
    SafeRelease(factory);
    if (FAILED(hr)) { SafeRelease(dxgiDev); return false; }

    D2D1_FACTORY_OPTIONS fo = {};
    bool ok =
        SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                    &fo, (void**)&g_d2dFactory)) &&
        SUCCEEDED(g_d2dFactory->CreateDevice(dxgiDev, &g_d2dDevice)) &&
        SUCCEEDED(g_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_ctx));
    if (!ok) { SafeRelease(dxgiDev); return false; }
    // Antialiasing on: round shapes look smooth; the cross uses integer
    // coordinates and stays pixel-sharp anyway.
    g_ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    g_d2dFactory->CreateStrokeStyle(
        D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                    D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_ROUND),
        nullptr, 0, &g_roundStroke);

    IDXGISurface* surface = nullptr;
    g_swap->GetBuffer(0, __uuidof(IDXGISurface), (void**)&surface);
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    hr = g_ctx->CreateBitmapFromDxgiSurface(surface, &bp, &g_target);
    SafeRelease(surface);
    if (FAILED(hr)) { SafeRelease(dxgiDev); return false; }
    g_ctx->SetTarget(g_target);
    g_ctx->CreateSolidColorBrush(CurrentColor(), &g_brush);

    hr = DCompositionCreateDevice(dxgiDev, __uuidof(IDCompositionDevice), (void**)&g_dcomp);
    SafeRelease(dxgiDev);
    if (FAILED(hr)) return false;
    if (FAILED(g_dcomp->CreateTargetForHwnd(g_hwnd, TRUE, &g_dtarget))) return false;
    if (FAILED(g_dcomp->CreateVisual(&g_visual))) return false;
    g_visual->SetContent(g_swap);
    g_dtarget->SetRoot(g_visual);

    Render();
    return SUCCEEDED(g_dcomp->Commit());
}

static void ShutdownGraphics() {
    SafeRelease(g_visual); SafeRelease(g_dtarget); SafeRelease(g_dcomp);
    SafeRelease(g_roundStroke); SafeRelease(g_brush); SafeRelease(g_target);
    SafeRelease(g_ctx); SafeRelease(g_d2dDevice); SafeRelease(g_d2dFactory);
    SafeRelease(g_swap); SafeRelease(g_d3d);
}

static void BringToTop() {
    if (g_hwnd && g_visible)
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void SetVisible(bool v) {
    g_visible = v;
    ShowWindow(g_hwnd, v ? SW_SHOWNOACTIVATE : SW_HIDE);
    BringToTop();
}

// ---------------------------------------------------------------- settings window
const wchar_t* kSettingsClass = L"LowLatencyCrosshairSettings";
const UINT WM_APP_TRAY = WM_APP + 1;

enum {
    IDC_SHAPE = 101, IDC_SCALE, IDC_THICK, IDC_OUTLINE, IDC_OPACITY,
    IDC_SCALE_VAL, IDC_THICK_VAL, IDC_OUTLINE_VAL, IDC_OPACITY_VAL,
    IDC_SWATCH, IDC_COLORBTN, IDC_DX, IDC_DX_UD, IDC_DY, IDC_DY_UD, IDC_RESET,
    IDC_VISIBLE, IDC_HIDE, IDC_EXIT,
    IDM_SETTINGS = 201, IDM_TOGGLE, IDM_EXIT,
};

HINSTANCE g_inst = nullptr;
HWND      g_settings = nullptr;
HFONT     g_uiFont = nullptr;
HBRUSH    g_swatchBrush = nullptr;
HICON     g_iconBig = nullptr, g_iconSmall = nullptr;
UINT      g_dpi = 96;
UINT      g_msgTaskbarCreated = 0;
bool      g_syncing = false;   // ignore control notifications while filling them
bool      g_quitting = false;
static COLORREF g_custColors[16];

static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }

static HWND Ctl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h,
                int id, DWORD exStyle = 0) {
    HWND c = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                             g_settings, (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
    return c;
}

static HWND Item(int id) { return GetDlgItem(g_settings, id); }

static void SetValueText(int id, int v, const wchar_t* suffix) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%d%ls", v, suffix);
    SetWindowTextW(Item(id), buf);
}

static void UpdateSwatch() {
    if (g_swatchBrush) DeleteObject(g_swatchBrush);
    g_swatchBrush = CreateSolidBrush(CurrentColorRef());
    InvalidateRect(Item(IDC_SWATCH), nullptr, TRUE);
}

// Fill all controls from g_cfg (after hotkeys or on startup).
static void SyncUI() {
    if (!g_settings) return;
    g_syncing = true;
    SendMessageW(Item(IDC_SHAPE), CB_SETCURSEL, g_cfg.shape, 0);
    SendMessageW(Item(IDC_SCALE), TBM_SETPOS, TRUE, g_cfg.scale);
    SendMessageW(Item(IDC_THICK), TBM_SETPOS, TRUE, g_cfg.thick);
    SendMessageW(Item(IDC_OUTLINE), TBM_SETPOS, TRUE, g_cfg.outline);
    SendMessageW(Item(IDC_OPACITY), TBM_SETPOS, TRUE, g_cfg.opacity);
    SetValueText(IDC_SCALE_VAL, g_cfg.scale, L"");
    SetValueText(IDC_THICK_VAL, g_cfg.thick, L" px");
    SetValueText(IDC_OUTLINE_VAL, g_cfg.outline, L" px");
    SetValueText(IDC_OPACITY_VAL, g_cfg.opacity, L"%");
    SendMessageW(Item(IDC_DX_UD), UDM_SETPOS32, 0, g_cfg.dx);
    SendMessageW(Item(IDC_DY_UD), UDM_SETPOS32, 0, g_cfg.dy);
    CheckDlgButton(g_settings, IDC_VISIBLE, g_visible ? BST_CHECKED : BST_UNCHECKED);
    UpdateSwatch();
    g_syncing = false;
}

static void Changed() {       // appearance changed: one redraw + save
    Sanitize();
    Render();
    SaveIni();
}

static void Moved() {         // offset changed
    Sanitize();
    PlaceWindow();
    SaveIni();
}

static void ShowSettings() {
    ShowWindow(g_settings, SW_SHOWNORMAL);
    SetForegroundWindow(g_settings);
}

static void AddTrayIcon() {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = g_settings;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_iconSmall;
    wcscpy(nid.szTip, L"Прицел — двойной щелчок для настроек");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void RemoveTrayIcon() {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = g_settings;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void QuitApp() {
    if (g_quitting) return;
    g_quitting = true;
    if (g_settings) DestroyWindow(g_settings);
    if (g_hwnd) DestroyWindow(g_hwnd); // posts WM_QUIT
}

static void ShowTrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"Настройки…");
    AppendMenuW(m, MF_STRING | (g_visible ? MF_CHECKED : 0), IDM_TOGGLE, L"Показывать прицел");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Выход");
    SetMenuDefaultItem(m, IDM_SETTINGS, FALSE);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_settings); // required for the menu to close properly
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_settings, nullptr);
    PostMessageW(g_settings, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void PickColor() {
    CHOOSECOLORW cc = {sizeof(cc)};
    cc.hwndOwner = g_settings;
    cc.rgbResult = CurrentColorRef();
    cc.lpCustColors = g_custColors;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
    if (ChooseColorW(&cc)) {
        SetColorRef(cc.rgbResult);
        UpdateSwatch();
        Changed();
    }
}

static void CreateTrackbarRow(const wchar_t* label, int y, int id, int valId, int lo, int hi) {
    Ctl(L"STATIC", label, SS_LEFT, 16, y + 4, 110, 20, -1);
    HWND tb = Ctl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 124, y, 190, 28, id);
    SendMessageW(tb, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
    SendMessageW(tb, TBM_SETPAGESIZE, 0, (hi - lo) > 20 ? 10 : 1);
    Ctl(L"STATIC", L"", SS_LEFT, 318, y + 4, 50, 20, valId);
}

static void CreateOffsetBox(int x, int y, int editId, int udId) {
    HWND ed = Ctl(L"EDIT", L"0", ES_AUTOHSCROLL | WS_TABSTOP, x, y, 64, 24, editId, WS_EX_CLIENTEDGE);
    HWND ud = CreateWindowExW(0, UPDOWN_CLASSW, nullptr,
                              WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT |
                              UDS_ARROWKEYS | UDS_NOTHOUSANDS,
                              0, 0, 0, 0, g_settings, (HMENU)(INT_PTR)udId, g_inst, nullptr);
    SendMessageW(ud, UDM_SETBUDDY, (WPARAM)ed, 0);
    SendMessageW(ud, UDM_SETRANGE32, -kMaxOffset, kMaxOffset);
}

static void BuildSettingsUI() {
    g_syncing = true;
    Ctl(L"STATIC", L"Форма:", SS_LEFT, 16, 18, 100, 20, -1);
    HWND cb = Ctl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 124, 14, 240, 200, IDC_SHAPE);
    for (int i = 0; i < SH_COUNT; ++i) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)kShapeNames[i]);

    CreateTrackbarRow(L"Размер:", 50, IDC_SCALE, IDC_SCALE_VAL, 1, kMaxScale);
    CreateTrackbarRow(L"Толщина линий:", 84, IDC_THICK, IDC_THICK_VAL, 1, kMaxThick);
    CreateTrackbarRow(L"Обводка:", 118, IDC_OUTLINE, IDC_OUTLINE_VAL, 0, kMaxOutline);
    CreateTrackbarRow(L"Непрозрачность:", 152, IDC_OPACITY, IDC_OPACITY_VAL, 10, 100);

    Ctl(L"STATIC", L"Цвет:", SS_LEFT, 16, 194, 100, 20, -1);
    Ctl(L"STATIC", L"", SS_LEFT | WS_BORDER, 124, 190, 40, 26, IDC_SWATCH);
    Ctl(L"BUTTON", L"Выбрать цвет…", BS_PUSHBUTTON | WS_TABSTOP, 172, 189, 140, 28, IDC_COLORBTN);

    Ctl(L"STATIC", L"Смещение X:", SS_LEFT, 16, 234, 100, 20, -1);
    CreateOffsetBox(124, 230, IDC_DX, IDC_DX_UD);
    Ctl(L"STATIC", L"Y:", SS_LEFT, 200, 234, 20, 20, -1);
    CreateOffsetBox(220, 230, IDC_DY, IDC_DY_UD);
    Ctl(L"BUTTON", L"Сбросить", BS_PUSHBUTTON | WS_TABSTOP, 294, 228, 70, 28, IDC_RESET);

    Ctl(L"BUTTON", L"Показывать прицел", BS_AUTOCHECKBOX | WS_TABSTOP, 16, 270, 250, 22, IDC_VISIBLE);

    Ctl(L"STATIC",
        L"Горячие клавиши:\n"
        L"Ctrl+Alt+O — настройки,  Ctrl+Alt+H — показать/скрыть\n"
        L"Ctrl+Alt+S — форма,  Ctrl+Alt+Плюс/Минус — размер\n"
        L"Ctrl+Alt+C — цвет,  Ctrl+Alt+Q — выход\n"
        L"Ctrl+Alt+Shift+стрелки — сдвиг на 1 px,  +0 — сброс\n"
        L"Совет: чётная толщина линий центрируется точнее всего.",
        SS_LEFT, 16, 302, 356, 104, -1);

    Ctl(L"BUTTON", L"Свернуть в трей", BS_PUSHBUTTON | WS_TABSTOP, 16, 414, 170, 32, IDC_HIDE);
    Ctl(L"BUTTON", L"Выход", BS_PUSHBUTTON | WS_TABSTOP, 196, 414, 168, 32, IDC_EXIT);
    g_syncing = false;
    SyncUI();
}

static LRESULT CALLBACK SettingsProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == g_msgTaskbarCreated && g_msgTaskbarCreated) { AddTrayIcon(); return 0; }
    switch (m) {
    case WM_CREATE: {
        g_settings = h;
        LOGFONTW lf = {};
        lf.lfHeight = -MulDiv(9, (int)g_dpi, 72);
        lf.lfWeight = FW_NORMAL;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy(lf.lfFaceName, L"Segoe UI");
        g_uiFont = CreateFontIndirectW(&lf);
        BuildSettingsUI();
        return 0;
    }
    case WM_HSCROLL: {
        if (g_syncing || !l) break;
        int id = GetDlgCtrlID((HWND)l);
        int pos = (int)SendMessageW((HWND)l, TBM_GETPOS, 0, 0);
        switch (id) {
        case IDC_SCALE:   g_cfg.scale = pos;   SetValueText(IDC_SCALE_VAL, pos, L""); break;
        case IDC_THICK:   g_cfg.thick = pos;   SetValueText(IDC_THICK_VAL, pos, L" px"); break;
        case IDC_OUTLINE: g_cfg.outline = pos; SetValueText(IDC_OUTLINE_VAL, pos, L" px"); break;
        case IDC_OPACITY: g_cfg.opacity = pos; SetValueText(IDC_OPACITY_VAL, pos, L"%"); break;
        default: return 0;
        }
        Changed();
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(w), code = HIWORD(w);
        if (g_syncing) return 0;
        switch (id) {
        case IDC_SHAPE:
            if (code == CBN_SELCHANGE) {
                g_cfg.shape = (int)SendMessageW(Item(IDC_SHAPE), CB_GETCURSEL, 0, 0);
                Changed();
            }
            break;
        case IDC_COLORBTN: PickColor(); break;
        case IDC_DX: case IDC_DY:
            if (code == EN_CHANGE) {
                BOOL err = FALSE;
                int v = (int)SendMessageW(Item(id == IDC_DX ? IDC_DX_UD : IDC_DY_UD), UDM_GETPOS32, 0, (LPARAM)&err);
                if (!err) { (id == IDC_DX ? g_cfg.dx : g_cfg.dy) = v; Moved(); }
            }
            break;
        case IDC_RESET:
            g_cfg.dx = g_cfg.dy = 0;
            Moved();
            SyncUI();
            break;
        case IDC_VISIBLE:
            SetVisible(IsDlgButtonChecked(h, IDC_VISIBLE) == BST_CHECKED);
            break;
        case IDC_HIDE:     ShowWindow(h, SW_HIDE); break;
        case IDC_EXIT:     QuitApp(); break;
        case IDM_SETTINGS: ShowSettings(); break;
        case IDM_TOGGLE:   SetVisible(!g_visible); SyncUI(); break;
        case IDM_EXIT:     QuitApp(); break;
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        if ((HWND)l == Item(IDC_SWATCH)) return (LRESULT)g_swatchBrush;
        SetBkMode((HDC)w, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_APP_TRAY:
        if (l == WM_LBUTTONDBLCLK) ShowSettings();
        else if (l == WM_RBUTTONUP || l == WM_CONTEXTMENU) ShowTrayMenu();
        return 0;
    case WM_CLOSE:          // the X button hides to the tray
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_DESTROY:
        RemoveTrayIcon();
        g_settings = nullptr;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static bool CreateSettingsWindow() {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = SettingsProc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = g_iconBig;
    wc.hIconSm = g_iconSmall;
    wc.lpszClassName = kSettingsClass;
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const DWORD ex = WS_EX_CONTROLPARENT;
    RECT rc = {0, 0, S(380), S(462)};
    AdjustWindowRectExForDpi(&rc, style, FALSE, ex, g_dpi);
    int w = rc.right - rc.left, hgt = rc.bottom - rc.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - hgt) / 2;
    HWND h = CreateWindowExW(ex, kSettingsClass, L"Прицел — настройки", style,
                             x, y, w, hgt, nullptr, nullptr, g_inst, nullptr);
    if (!h) return false;
    AddTrayIcon();
    if (!g_cfg.minimized) ShowSettings();
    return true;
}

// ---------------------------------------------------------------- overlay window
enum { HK_TOGGLE = 1, HK_COLOR, HK_QUIT, HK_SETTINGS,
       HK_LEFT, HK_RIGHT, HK_UP, HK_DOWN, HK_RESET,
       HK_SHAPE, HK_BIGGER, HK_BIGGER2, HK_SMALLER, HK_SMALLER2 };
enum { TIMER_TOPMOST = 1 };

// When the game becomes foreground or switches to fullscreen, it may cover
// the overlay. Reassert topmost right away instead of waiting for the timer.
static void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    BringToTop();
}

static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_HOTKEY:
        switch (w) {
        case HK_TOGGLE:   SetVisible(!g_visible); break;
        case HK_SETTINGS: ShowSettings(); break;
        case HK_COLOR:
            g_preset = (g_preset + 1) % kPresetCount;
            g_cfg.r = kPresets[g_preset][0]; g_cfg.g = kPresets[g_preset][1]; g_cfg.b = kPresets[g_preset][2];
            Changed();
            break;
        case HK_SHAPE:    g_cfg.shape = (g_cfg.shape + 1) % SH_COUNT; Changed(); break;
        case HK_BIGGER: case HK_BIGGER2:
            if (g_cfg.scale < kMaxScale) { g_cfg.scale++; Changed(); }
            break;
        case HK_SMALLER: case HK_SMALLER2:
            if (g_cfg.scale > 1) { g_cfg.scale--; Changed(); }
            break;
        case HK_LEFT:  g_cfg.dx--; Moved(); break;
        case HK_RIGHT: g_cfg.dx++; Moved(); break;
        case HK_UP:    g_cfg.dy--; Moved(); break;
        case HK_DOWN:  g_cfg.dy++; Moved(); break;
        case HK_RESET: g_cfg.dx = g_cfg.dy = 0; Moved(); break;
        case HK_QUIT:  QuitApp(); return 0;
        }
        SyncUI();
        return 0;
    case WM_TIMER:
        if (w == TIMER_TOPMOST) BringToTop();
        return 0;
    case WM_DISPLAYCHANGE:
        PlaceWindow();   // a fullscreen game may change the resolution: recenter
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void RegisterHotkeys() {
    const UINT ca = MOD_CONTROL | MOD_ALT;
    RegisterHotKey(g_hwnd, HK_TOGGLE,   ca | MOD_NOREPEAT, 'H');
    RegisterHotKey(g_hwnd, HK_SETTINGS, ca | MOD_NOREPEAT, 'O');
    RegisterHotKey(g_hwnd, HK_COLOR,    ca | MOD_NOREPEAT, 'C');
    RegisterHotKey(g_hwnd, HK_QUIT,     ca | MOD_NOREPEAT, 'Q');
    RegisterHotKey(g_hwnd, HK_SHAPE,    ca | MOD_NOREPEAT, 'S');
    RegisterHotKey(g_hwnd, HK_BIGGER,   ca, VK_OEM_PLUS);  // '=' / '+' key, repeats when held
    RegisterHotKey(g_hwnd, HK_BIGGER2,  ca, VK_ADD);       // numpad +
    RegisterHotKey(g_hwnd, HK_SMALLER,  ca, VK_OEM_MINUS); // '-' key
    RegisterHotKey(g_hwnd, HK_SMALLER2, ca, VK_SUBTRACT);  // numpad -
    // Ctrl+Alt+Shift+arrows: plain Ctrl+Alt+arrows rotate the screen on some Intel drivers.
    const UINT nudge = ca | MOD_SHIFT;
    RegisterHotKey(g_hwnd, HK_LEFT,  nudge, VK_LEFT);
    RegisterHotKey(g_hwnd, HK_RIGHT, nudge, VK_RIGHT);
    RegisterHotKey(g_hwnd, HK_UP,    nudge, VK_UP);
    RegisterHotKey(g_hwnd, HK_DOWN,  nudge, VK_DOWN);
    RegisterHotKey(g_hwnd, HK_RESET, nudge | MOD_NOREPEAT, '0');
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    g_inst = inst;

    // Single instance: a second launch just opens the settings of the first one.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"LowLatencyCrosshair.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(kSettingsClass, nullptr);
        if (other) {
            ShowWindow(other, SW_SHOWNORMAL);
            SetForegroundWindow(other);
        }
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // real pixels
    g_dpi = GetDpiForSystem();
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_BAR_CLASSES | ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    g_iconBig = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    g_iconSmall = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!g_iconBig) g_iconBig = LoadIcon(nullptr, IDI_APPLICATION);
    if (!g_iconSmall) g_iconSmall = g_iconBig;
    for (int i = 0; i < 16; ++i) {
        const float* p = kPresets[i % kPresetCount];
        g_custColors[i] = (i < kPresetCount)
            ? RGB((int)(p[0] * 255), (int)(p[1] * 255), (int)(p[2] * 255)) : RGB(255, 255, 255);
    }

    InitIniPath();
    LoadIni();      // saved settings; command-line arguments override them
    ParseArgs();
    Sanitize();
    ComputeSize();

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"LowLatencyCrosshair";
    RegisterClassExW(&wc);

    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP;
    if (g_cfg.layered) ex |= WS_EX_LAYERED | WS_EX_TRANSPARENT; // click-through
    g_hwnd = CreateWindowExW(ex, wc.lpszClassName, L"Crosshair", WS_POPUP,
                             0, 0, g_size, g_size, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;
    if (g_cfg.layered) SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);
    PlaceWindow();

    if (!InitGraphics()) {
        MessageBoxW(nullptr, L"Не удалось инициализировать D3D11 / DirectComposition.",
                    L"Прицел", MB_ICONERROR);
        ShutdownGraphics();
        return 1;
    }

    RegisterHotkeys();
    SetTimer(g_hwnd, TIMER_TOPMOST, 1000, nullptr);
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);

    const DWORD hookFlags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    HWINEVENTHOOK hkFg = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                         nullptr, OnWinEvent, 0, 0, hookFlags);
    HWINEVENTHOOK hkMin = SetWinEventHook(EVENT_SYSTEM_MINIMIZEEND, EVENT_SYSTEM_MINIMIZEEND,
                                          nullptr, OnWinEvent, 0, 0, hookFlags);

    CreateSettingsWindow();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_settings && IsDialogMessageW(g_settings, &msg)) continue; // Tab navigation
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (hkFg) UnhookWinEvent(hkFg);
    if (hkMin) UnhookWinEvent(hkMin);
    if (g_settings) DestroyWindow(g_settings);
    ShutdownGraphics();
    if (g_uiFont) DeleteObject(g_uiFont);
    if (g_swatchBrush) DeleteObject(g_swatchBrush);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
