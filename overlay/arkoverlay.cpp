#include "arkoverlay.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <algorithm>

using namespace Gdiplus;

#ifdef _MSC_VER
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdiplus.lib")
#endif

namespace ao = arkoverlay;

namespace {

struct OverlayState
{
    HWND hwnd = nullptr;

    HDC memDC = nullptr;
    HBITMAP hbm = nullptr;
    HGDIOBJ oldBmp = nullptr;
    BYTE* bits = nullptr;

    int w = 0;
    int h = 0;

    Bitmap* surfaceBitmap = nullptr;
    Graphics* surfaceGraphics = nullptr;

    Font* font = nullptr;
    StringFormat* sfCenter = nullptr;
    StringFormat* sfLeft = nullptr;
    float fontKey = -1.0f;

    ULONG_PTR gdiToken = 0;
    bool gdiStarted = false;

    float dpi = 96.0f;
};

OverlayState S;

constexpr float PI = 3.14159265358979323846f;

float Dot(const ao::Vec3& a, const ao::Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

float Distance(const ao::Vec3& a, const ao::Vec3& b)
{
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

template <typename T>
T Clamp(T v, T lo, T hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

bool IsFinite(float v)
{
    return std::isfinite(v);
}

void EnablePerMonitorDpiV2()
{
    typedef BOOL(WINAPI* SetProcessDpiAwarenessContextFn)(HANDLE);

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32)
    {
        auto fn = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));

        if (fn)
        {
            fn(reinterpret_cast<HANDLE>(-4));
            return;
        }
    }

    SetProcessDPIAware();
}

float GetWindowDpi(HWND hwnd)
{
    typedef UINT(WINAPI* GetDpiForWindowFn)(HWND);

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32)
    {
        auto fn = reinterpret_cast<GetDpiForWindowFn>(
            GetProcAddress(user32, "GetDpiForWindow"));

        if (fn)
        {
            UINT d = fn(hwnd);
            if (d > 0)
                return static_cast<float>(d);
        }
    }

    HDC dc = GetDC(nullptr);
    if (dc)
    {
        int d = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
        if (d > 0)
            return static_cast<float>(d);
    }

    return 96.0f;
}

float FocalLength(const ao::Camera& cam)
{
    if (cam.screenH <= 0)
        return 1.0f;

    float fov = cam.fovDegrees * PI / 180.0f;

    if (fov < 1.0f * PI / 180.0f)
        fov = 1.0f * PI / 180.0f;

    float t = tanf(fov * 0.5f);
    if (!(t > 1e-6f))
        return 1.0f;

    return (cam.screenH * 0.5f) / t;
}

bool WorldToScreen(
    const ao::Camera& cam,
    float focal,
    const ao::Vec3& world,
    float& sx,
    float& sy,
    float& depth)
{
    if (cam.screenW <= 0 || cam.screenH <= 0)
        return false;

    ao::Vec3 d;
    d.x = world.x - cam.pos.x;
    d.y = world.y - cam.pos.y;
    d.z = world.z - cam.pos.z;

    depth = Dot(d, cam.forward);

    if (!(depth > 0.001f))
        return false;

    float x = Dot(d, cam.right);
    float y = Dot(d, cam.up);

    sx = cam.screenW * 0.5f + (x / depth) * focal;
    sy = cam.screenH * 0.5f - (y / depth) * focal;

    if (!IsFinite(sx) || !IsFinite(sy) || !IsFinite(depth))
        return false;

    return true;
}

void EnsureStringFormats()
{
    if (!S.gdiStarted)
        return;

    if (!S.sfCenter)
    {
        S.sfCenter = new StringFormat();
        if (S.sfCenter->GetLastStatus() != Ok)
        {
            delete S.sfCenter;
            S.sfCenter = nullptr;
            return;
        }

        S.sfCenter->SetAlignment(StringAlignmentCenter);
        S.sfCenter->SetLineAlignment(StringAlignmentCenter);
        S.sfCenter->SetTrimming(StringTrimmingEllipsisCharacter);
    }

    if (!S.sfLeft)
    {
        S.sfLeft = new StringFormat();
        if (S.sfLeft->GetLastStatus() != Ok)
        {
            delete S.sfLeft;
            S.sfLeft = nullptr;
            return;
        }

        S.sfLeft->SetAlignment(StringAlignmentNear);
        S.sfLeft->SetLineAlignment(StringAlignmentNear);
    }
}

bool EnsureGdiplus()
{
    if (S.gdiStarted)
        return true;

    GdiplusStartupInput input;
    Status st = GdiplusStartup(&S.gdiToken, &input, nullptr);

    if (st != Ok)
        return false;

    S.gdiStarted = true;
    EnsureStringFormats();

    return true;
}

void EnsureFont(float fontScale)
{
    if (!EnsureGdiplus())
        return;

    float scale = fontScale;
    if (!IsFinite(scale) || scale < 0.1f)
        scale = 0.1f;
    if (scale > 10.0f)
        scale = 10.0f;

    float dpi = (S.dpi > 1.0f) ? S.dpi : 96.0f;
    float key = (scale * 10000.0f) + dpi;

    if (S.font && fabsf(key - S.fontKey) < 0.001f)
        return;

    if (S.font)
    {
        delete S.font;
        S.font = nullptr;
    }

    const FontFamily* family = FontFamily::GenericSansSerif();
    if (!family)
        return;

    float size = 11.0f * scale * dpi / 96.0f;
    if (!(size > 1.0f))
        size = 1.0f;
    if (size > 200.0f)
        size = 200.0f;

    S.font = new Font(family, size, FontStyleRegular, UnitPixel);

    if (S.font->GetLastStatus() != Ok)
    {
        delete S.font;
        S.font = nullptr;
        return;
    }

    S.fontKey = key;
}

void DestroySurface()
{
    if (S.surfaceGraphics)
    {
        delete S.surfaceGraphics;
        S.surfaceGraphics = nullptr;
    }

    if (S.surfaceBitmap)
    {
        delete S.surfaceBitmap;
        S.surfaceBitmap = nullptr;
    }

    if (S.memDC)
    {
        if (S.oldBmp)
        {
            SelectObject(S.memDC, S.oldBmp);
            S.oldBmp = nullptr;
        }
    }

    if (S.hbm)
    {
        DeleteObject(S.hbm);
        S.hbm = nullptr;
    }

    S.bits = nullptr;
    S.w = 0;
    S.h = 0;
}

bool CreateSurface(int w, int h)
{
    DestroySurface();

    if (w <= 0 || h <= 0)
        return false;

    if (!S.memDC)
    {
        S.memDC = CreateCompatibleDC(nullptr);
        if (!S.memDC)
            return false;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;

    S.hbm = CreateDIBSection(
        nullptr,
        &bmi,
        DIB_RGB_COLORS,
        &bits,
        nullptr,
        0
    );

    if (!S.hbm || !bits)
    {
        if (S.hbm)
        {
            DeleteObject(S.hbm);
            S.hbm = nullptr;
        }
        return false;
    }

    S.bits = static_cast<BYTE*>(bits);
    S.oldBmp = SelectObject(S.memDC, S.hbm);

    S.surfaceBitmap = new Bitmap(
        w,
        h,
        w * 4,
        PixelFormat32bppARGB,
        S.bits
    );

    if (!S.surfaceBitmap || S.surfaceBitmap->GetLastStatus() != Ok)
    {
        if (S.surfaceBitmap)
        {
            delete S.surfaceBitmap;
            S.surfaceBitmap = nullptr;
        }

        DestroySurface();
        return false;
    }

    S.surfaceGraphics = new Graphics(S.surfaceBitmap);

    if (!S.surfaceGraphics || S.surfaceGraphics->GetLastStatus() != Ok)
    {
        if (S.surfaceGraphics)
        {
            delete S.surfaceGraphics;
            S.surfaceGraphics = nullptr;
        }

        DestroySurface();
        return false;
    }

    S.w = w;
    S.h = h;

    S.surfaceGraphics->SetSmoothingMode(SmoothingModeAntiAlias);
    S.surfaceGraphics->SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    S.surfaceGraphics->SetPixelOffsetMode(PixelOffsetModeHalf);

    return true;
}

void PremultiplyAlpha()
{
    if (!S.bits || S.w <= 0 || S.h <= 0)
        return;

    BYTE* p = S.bits;
    const size_t count = static_cast<size_t>(S.w) * static_cast<size_t>(S.h);

    for (size_t i = 0; i < count; ++i, p += 4)
    {
        const BYTE a = p[3];

        if (a == 0)
        {
            p[0] = 0;
            p[1] = 0;
            p[2] = 0;
        }
        else if (a < 255)
        {
            p[0] = static_cast<BYTE>((p[0] * a) / 255);
            p[1] = static_cast<BYTE>((p[1] * a) / 255);
            p[2] = static_cast<BYTE>((p[2] * a) / 255);
        }
    }
}

void Present()
{
    if (!S.hwnd || !S.memDC || !S.hbm)
        return;

    if (S.w <= 0 || S.h <= 0)
        return;

    if (!IsWindowVisible(S.hwnd))
        return;

    PremultiplyAlpha();

    GdiFlush();

    RECT rc = {};
    GetWindowRect(S.hwnd, &rc);

    POINT dstPos{ rc.left, rc.top };
    SIZE size{ S.w, S.h };
    POINT srcPos{ 0, 0 };

    BLENDFUNCTION bf = {};
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(
        S.hwnd,
        nullptr,
        &dstPos,
        &size,
        S.memDC,
        &srcPos,
        0,
        &bf,
        ULW_ALPHA
    );
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(wParam);
    UNREFERENCED_PARAMETER(lParam);

    switch (msg)
    {
        case WM_NCHITTEST:
            return HTTRANSPARENT;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            return 0;

        case WM_DPICHANGED:
            S.dpi = static_cast<float>(HIWORD(wParam));
            if (S.dpi <= 1.0f)
                S.dpi = 96.0f;
            return 0;

        default:
            break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool RegisterOverlayClass()
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ARKOverlayWindow";

    if (!RegisterClassExW(&wc))
    {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS)
            return false;
    }

    return true;
}

int IniGetInt(const wchar_t* path, const wchar_t* key, int def)
{
    return GetPrivateProfileIntW(L"overlay", key, def, path);
}

std::wstring IniGetString(const wchar_t* path, const wchar_t* key, const wchar_t* def)
{
    wchar_t buf[256] = {};
    GetPrivateProfileStringW(L"overlay", key, def, buf, 256, path);
    return buf;
}

float IniGetFloat(const wchar_t* path, const wchar_t* key, float def)
{
    wchar_t defBuf[64] = {};
    swprintf_s(defBuf, L"%f", def);

    std::wstring s = IniGetString(path, key, defBuf);

    float v = static_cast<float>(wcstod(s.c_str(), nullptr));
    return IsFinite(v) ? v : def;
}

void IniWriteInt(const wchar_t* path, const wchar_t* key, int value)
{
    wchar_t buf[32] = {};
    swprintf_s(buf, L"%d", value);
    WritePrivateProfileStringW(L"overlay", key, buf, path);
}

void IniWriteFloat(const wchar_t* path, const wchar_t* key, float value)
{
    wchar_t buf[64] = {};
    swprintf_s(buf, L"%f", value);
    WritePrivateProfileStringW(L"overlay", key, buf, path);
}

void IniWriteString(const wchar_t* path, const wchar_t* key, const wchar_t* value)
{
    WritePrivateProfileStringW(L"overlay", key, value, path);
}

std::wstring ColorToString(uint32_t argb)
{
    wchar_t buf[16] = {};
    swprintf_s(buf, L"#%08X", argb);
    return buf;
}

uint32_t ParseColor(const std::wstring& input, uint32_t def)
{
    if (input.empty())
        return def;

    std::wstring s = input;

    if (s[0] == L'#')
        s.erase(0, 1);

    if (s.size() == 6)
        s = L"FF" + s;

    if (s.size() != 8)
        return def;

    uint32_t v = 0;

    for (wchar_t c : s)
    {
        v <<= 4;

        if (c >= L'0' && c <= L'9')
            v += static_cast<uint32_t>(c - L'0');
        else if (c >= L'a' && c <= L'f')
            v += static_cast<uint32_t>(10 + c - L'a');
        else if (c >= L'A' && c <= L'F')
            v += static_cast<uint32_t>(10 + c - L'A');
        else
            return def;
    }

    return v;
}

} // anonymous namespace

bool ao::overlayIsInitialized()
{
    return S.hwnd != nullptr;
}

bool ao::overlayInit(const RECT& windowRect)
{
    if (S.hwnd)
    {
        overlayResize(windowRect);
        return true;
    }

    EnablePerMonitorDpiV2();

    if (!EnsureGdiplus())
        return false;

    if (!RegisterOverlayClass())
        return false;

    LONG w = windowRect.right - windowRect.left;
    LONG h = windowRect.bottom - windowRect.top;

    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    S.hwnd = CreateWindowExW(
        0,
        L"ARKOverlayWindow",
        L"arkoverlay",
        WS_POPUP,
        windowRect.left,
        windowRect.top,
        w,
        h,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr
    );

    if (!S.hwnd)
        return false;

    LONG_PTR ex = GetWindowLongPtrW(S.hwnd, GWL_EXSTYLE);

    ex &= ~static_cast<LONG_PTR>(WS_EX_APPWINDOW);
    ex |= static_cast<LONG_PTR>(
        WS_EX_LAYERED |
        WS_EX_TOPMOST |
        WS_EX_NOACTIVATE |
        WS_EX_TOOLWINDOW
    );

    SetWindowLongPtrW(S.hwnd, GWL_EXSTYLE, ex);

    SetWindowPos(
        S.hwnd,
        HWND_TOPMOST,
        windowRect.left,
        windowRect.top,
        w,
        h,
        SWP_NOACTIVATE | SWP_FRAMECHANGED
    );

    ex = GetWindowLongPtrW(S.hwnd, GWL_EXSTYLE);
    ex |= static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
    SetWindowLongPtrW(S.hwnd, GWL_EXSTYLE, ex);

    SetWindowPos(
        S.hwnd,
        HWND_TOPMOST,
        windowRect.left,
        windowRect.top,
        w,
        h,
        SWP_NOACTIVATE | SWP_FRAMECHANGED
    );

    S.dpi = GetWindowDpi(S.hwnd);

    if (!CreateSurface(static_cast<int>(w), static_cast<int>(h)))
    {
        overlayShutdown();
        return false;
    }

    ShowWindow(S.hwnd, SW_SHOWNOACTIVATE);

    return true;
}

void ao::overlayHide()
{
    if (!S.hwnd)
        return;

    if (IsWindowVisible(S.hwnd))
        ShowWindow(S.hwnd, SW_HIDE);
}

void ao::overlayShow()
{
    if (!S.hwnd)
        return;

    if (!IsWindowVisible(S.hwnd))
        ShowWindow(S.hwnd, SW_SHOWNOACTIVATE);
}

void ao::overlayResize(const RECT& newRect)
{
    if (!S.hwnd)
        return;

    LONG w = newRect.right - newRect.left;
    LONG h = newRect.bottom - newRect.top;

    if (w <= 0 || h <= 0)
    {
        overlayHide();
        return;
    }

    S.dpi = GetWindowDpi(S.hwnd);

    SetWindowPos(
        S.hwnd,
        HWND_TOPMOST,
        newRect.left,
        newRect.top,
        w,
        h,
        SWP_NOACTIVATE | SWP_FRAMECHANGED
    );

    if (static_cast<int>(w) != S.w || static_cast<int>(h) != S.h)
    {
        CreateSurface(static_cast<int>(w), static_cast<int>(h));
    }
}

void ao::overlayShutdown()
{
    if (S.font)
    {
        delete S.font;
        S.font = nullptr;
    }

    if (S.sfCenter)
    {
        delete S.sfCenter;
        S.sfCenter = nullptr;
    }

    if (S.sfLeft)
    {
        delete S.sfLeft;
        S.sfLeft = nullptr;
    }

    DestroySurface();

    if (S.memDC)
    {
        DeleteDC(S.memDC);
        S.memDC = nullptr;
    }

    if (S.hwnd)
    {
        DestroyWindow(S.hwnd);
        S.hwnd = nullptr;
    }

    if (S.gdiStarted)
    {
        GdiplusShutdown(S.gdiToken);
        S.gdiStarted = false;
        S.gdiToken = 0;
    }
}

bool ao::overlayDraw(
    const Target* targetList,
    int targetCount,
    const Camera& camera,
    const Config& config)
{
    if (!S.hwnd || !S.surfaceGraphics)
        return false;

    if (!IsWindowVisible(S.hwnd))
        return false;

    EnsureFont(config.fontScale);
    EnsureStringFormats();

    Graphics* g = S.surfaceGraphics;

    g->Clear(Color(0, 0, 0, 0));

    const float uiScale = (S.dpi > 1.0f) ? (S.dpi / 96.0f) : 1.0f;
    const float penWidth = std::max(1.0f, uiScale);
    const float focal = FocalLength(camera);

    float lineH = S.font ? S.font->GetHeight(g) : 12.0f;
    if (!(lineH > 1.0f))
        lineH = 12.0f;

    const float widthFactor = std::max(0.01f, config.widthFactor);
    const float maxRange = std::max(0.0f, config.maxRange);

    static float fps = 0.0f;
    static bool fpsInit = false;
    static std::chrono::steady_clock::time_point lastTime;

    auto now = std::chrono::steady_clock::now();

    if (!fpsInit)
    {
        fpsInit = true;
        lastTime = now;
    }
    else
    {
        float dt = std::chrono::duration<float>(now - lastTime).count();
        if (dt > 0.0f)
            fps = fps * 0.9f + (1.0f / dt) * 0.1f;

        lastTime = now;
    }

    if (fps > 999.0f)
        fps = 999.0f;

    const float maxScreenDim =
        static_cast<float>(std::max(1, std::max(camera.screenW, camera.screenH))) * 2.0f;

    if (targetList && targetCount > 0)
    {
        for (int i = 0; i < targetCount; ++i)
        {
            const Target& t = targetList[i];

            if (t.behindCamera)
                continue;

            if (t.isTurret && !config.enableTurrets)
                continue;

            if (config.playersOnly && !t.isPlayer)
                continue;

            float dist = t.distance;

            if (!(dist > 0.0f))
                dist = Distance(camera.pos, t.worldPos);

            if (!IsFinite(dist))
                continue;

            if (dist > maxRange)
                continue;

            float cx = 0.0f;
            float cy = 0.0f;
            float depth = 0.0f;

            if (!WorldToScreen(camera, focal, t.worldPos, cx, cy, depth))
                continue;

            float wpx = t.boxW * focal / depth * widthFactor;
            float hpx = t.boxH * focal / depth;

            if (!IsFinite(wpx) || !IsFinite(hpx))
                continue;

            wpx = Clamp(wpx, 1.0f, maxScreenDim);
            hpx = Clamp(hpx, 1.0f, maxScreenDim);

            const float x = cx - wpx * 0.5f;
            const float y = cy - hpx * 0.5f;

            const uint32_t baseArgb =
                t.isTurret ? config.turretColor :
                (t.isPlayer ? config.playerColor : config.dinoColor);

            Color baseColor(baseArgb);

            if (config.enableBoxes)
            {
                Pen pen(baseColor, penWidth);

                g->DrawRectangle(&pen, x, y, wpx, hpx);
                g->DrawLine(&pen, cx, y, cx, y + hpx);
            }

            // Label: name (+ owner/tribe) (+ level) or turret + ammo.
            if (config.enableNames && S.font && S.sfCenter)
            {
                wchar_t label[192] = {};
                int n = 0;

                if (t.isTurret)
                {
                    n = swprintf_s(label, L"%s",
                        t.name[0] ? t.name : L"Turret");

                    if (n > 0 && config.showAmmo && t.ammo >= 0)
                        swprintf_s(label + n, 192 - n, L" | Ammo %d", t.ammo);
                }
                else
                {
                    n = swprintf_s(label, L"%s",
                        t.name[0] ? t.name : (t.isPlayer ? L"Player" : L"Unknown"));

                    if (n > 0 && t.ownerName[0])
                        n += swprintf_s(label + n, 192 - n, L" (%s)", t.ownerName);

                    if (n > 0 && config.showLevel && t.level >= 0)
                        swprintf_s(label + n, 192 - n, L" Lvl %d", t.level);
                }

                if (n > 0 && label[0])
                {
                    float textW = std::max(wpx, 200.0f * uiScale);
                    float nameY = y - lineH - 2.0f * uiScale;

                    if (nameY < 0.0f)
                        nameY = y + hpx + 2.0f * uiScale;

                    RectF nameRect(
                        cx - textW * 0.5f,
                        nameY,
                        textW,
                        lineH
                    );

                    SolidBrush nameBrush(baseColor);

                    g->DrawString(
                        label,
                        -1,
                        S.font,
                        nameRect,
                        S.sfCenter,
                        &nameBrush
                    );
                }
            }

            if (config.enableHealth && t.maxHealth > 0.0f && IsFinite(t.maxHealth))
            {
                float frac = t.health / t.maxHealth;

                if (!(frac >= 0.0f))
                    frac = 0.0f;
                if (frac > 1.0f)
                    frac = 1.0f;

                const float barW = std::max(3.0f, 3.0f * uiScale);
                const float gap = 3.0f * uiScale;

                float barX = x + wpx + gap;

                if (barX + barW > static_cast<float>(camera.screenW))
                    barX = x - barW - gap;

                const float barY = y;
                const float barH = hpx;

                Color bgColor(140, 0, 0, 0);
                SolidBrush bgBrush(bgColor);

                g->FillRectangle(&bgBrush, barX, barY, barW, barH);

                if (frac > 0.0f)
                {
                    const float fillH = barH * frac;
                    const float fillY = barY + barH - fillH;

                    Color fillColor(config.healthBarColor);
                    SolidBrush fillBrush(fillColor);

                    g->FillRectangle(&fillBrush, barX, fillY, barW, fillH);
                }

                Color borderColor(220, 0, 0, 0);
                Pen borderPen(borderColor, 1.0f);

                g->DrawRectangle(&borderPen, barX, barY, barW, barH);
            }

            if (config.enableDistance && S.font && S.sfCenter)
            {
                wchar_t distBuf[64] = {};
                swprintf_s(distBuf, L"%.0fm", dist);

                float textW = std::max(wpx, 120.0f * uiScale);

                RectF distRect(
                    cx - textW * 0.5f,
                    y + hpx + 2.0f * uiScale,
                    textW,
                    lineH
                );

                SolidBrush distBrush(Color(230, 255, 255, 255));

                g->DrawString(
                    distBuf,
                    -1,
                    S.font,
                    distRect,
                    S.sfCenter,
                    &distBrush
                );
            }
        }
    }

    // HUD: FPS.
    if (config.showFps && S.font && S.sfLeft)
    {
        wchar_t fpsBuf[32] = {};
        swprintf_s(fpsBuf, L"FPS %3.0f", fps);

        RectF fpsRect(
            4.0f * uiScale,
            4.0f * uiScale,
            300.0f * uiScale,
            lineH
        );

        SolidBrush fpsBrush(Color(255, 255, 255, 255));

        g->DrawString(
            fpsBuf,
            -1,
            S.font,
            fpsRect,
            S.sfLeft,
            &fpsBrush
        );
    }

    // HUD: your coordinates (cm -> m).
    if (config.showCoords && S.font && S.sfLeft)
    {
        wchar_t buf[128] = {};
        swprintf_s(buf, L"X:%.1f Y:%.1f Z:%.1f",
            camera.playerPos.x / 100.0f,
            camera.playerPos.y / 100.0f,
            camera.playerPos.z / 100.0f);

        RectF coordRect(
            4.0f * uiScale,
            4.0f * uiScale + lineH * 1.3f,
            400.0f * uiScale,
            lineH
        );

        SolidBrush coordBrush(Color(255, 255, 255, 255));

        g->DrawString(
            buf,
            -1,
            S.font,
            coordRect,
            S.sfLeft,
            &coordBrush
        );
    }

    g->Flush();

    Present();

    return true;
}

void ao::loadConfig(const wchar_t* path, Config& cfg)
{
    cfg = Config{};

    if (!path)
        return;

    const bool fileMissing =
        (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES);

    cfg.enableBoxes =
        IniGetInt(path, L"enableBoxes", cfg.enableBoxes ? 1 : 0) != 0;

    cfg.enableNames =
        IniGetInt(path, L"enableNames", cfg.enableNames ? 1 : 0) != 0;

    cfg.enableHealth =
        IniGetInt(path, L"enableHealth", cfg.enableHealth ? 1 : 0) != 0;

    cfg.enableDistance =
        IniGetInt(path, L"enableDistance", cfg.enableDistance ? 1 : 0) != 0;

    cfg.playersOnly =
        IniGetInt(path, L"playersOnly", cfg.playersOnly ? 1 : 0) != 0;

    cfg.showFps =
        IniGetInt(path, L"showFps", cfg.showFps ? 1 : 0) != 0;

    cfg.showLevel =
        IniGetInt(path, L"showLevel", cfg.showLevel ? 1 : 0) != 0;

    cfg.showCoords =
        IniGetInt(path, L"showCoords", cfg.showCoords ? 1 : 0) != 0;

    cfg.showAmmo =
        IniGetInt(path, L"showAmmo", cfg.showAmmo ? 1 : 0) != 0;

    cfg.enableTurrets =
        IniGetInt(path, L"enableTurrets", cfg.enableTurrets ? 1 : 0) != 0;

    cfg.maxRange =
        IniGetFloat(path, L"maxRange", cfg.maxRange);

    cfg.widthFactor =
        IniGetFloat(path, L"widthFactor", cfg.widthFactor);

    cfg.fontScale =
        IniGetFloat(path, L"fontScale", cfg.fontScale);

    std::wstring defPlayer = ColorToString(cfg.playerColor);
    std::wstring defDino = ColorToString(cfg.dinoColor);
    std::wstring defHealth = ColorToString(cfg.healthBarColor);
    std::wstring defTurret = ColorToString(cfg.turretColor);

    cfg.playerColor =
        ParseColor(IniGetString(path, L"playerColor", defPlayer.c_str()), cfg.playerColor);

    cfg.dinoColor =
        ParseColor(IniGetString(path, L"dinoColor", defDino.c_str()), cfg.dinoColor);

    cfg.healthBarColor =
        ParseColor(IniGetString(path, L"healthBarColor", defHealth.c_str()), cfg.healthBarColor);

    cfg.turretColor =
        ParseColor(IniGetString(path, L"turretColor", defTurret.c_str()), cfg.turretColor);

    if (fileMissing)
        saveConfig(path, cfg);
}

void ao::saveConfig(const wchar_t* path, const Config& cfg)
{
    if (!path)
        return;

    IniWriteInt(path, L"enableBoxes", cfg.enableBoxes ? 1 : 0);
    IniWriteInt(path, L"enableNames", cfg.enableNames ? 1 : 0);
    IniWriteInt(path, L"enableHealth", cfg.enableHealth ? 1 : 0);
    IniWriteInt(path, L"enableDistance", cfg.enableDistance ? 1 : 0);
    IniWriteInt(path, L"playersOnly", cfg.playersOnly ? 1 : 0);
    IniWriteInt(path, L"showFps", cfg.showFps ? 1 : 0);
    IniWriteInt(path, L"showLevel", cfg.showLevel ? 1 : 0);
    IniWriteInt(path, L"showCoords", cfg.showCoords ? 1 : 0);
    IniWriteInt(path, L"showAmmo", cfg.showAmmo ? 1 : 0);
    IniWriteInt(path, L"enableTurrets", cfg.enableTurrets ? 1 : 0);

    IniWriteFloat(path, L"maxRange", cfg.maxRange);
    IniWriteFloat(path, L"widthFactor", cfg.widthFactor);
    IniWriteFloat(path, L"fontScale", cfg.fontScale);

    IniWriteString(path, L"playerColor", ColorToString(cfg.playerColor).c_str());
    IniWriteString(path, L"dinoColor", ColorToString(cfg.dinoColor).c_str());
    IniWriteString(path, L"healthBarColor", ColorToString(cfg.healthBarColor).c_str());
    IniWriteString(path, L"turretColor", ColorToString(cfg.turretColor).c_str());
}

//-----------------------------------------------------------------------------
// Demo
//-----------------------------------------------------------------------------

namespace {

HWND g_demoHost = nullptr;
HWND g_demoButton = nullptr;
ao::Config g_demoConfig;

LRESULT CALLBACK DemoHostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);

    switch (msg)
    {
        case WM_CREATE:
        {
            g_demoButton = CreateWindowExW(
                0,
                L"BUTTON",
                L"Exit / Click-through test",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                20, 20, 240, 36,
                hwnd,
                reinterpret_cast<HMENU>(101),
                GetModuleHandleW(nullptr),
                nullptr
            );

            return 0;
        }

        case WM_COMMAND:
        {
            if (LOWORD(wParam) == 101)
            {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }

        case WM_KEYDOWN:
        {
            if (wParam == VK_ESCAPE)
            {
                DestroyWindow(hwnd);
                return 0;
            }

            if (wParam == VK_F5)
            {
                ao::loadConfig(L".\\overlay.ini", g_demoConfig);
                return 0;
            }

            break;
        }

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);

            RECT rc;
            GetClientRect(hwnd, &rc);

            FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH)));

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(220, 220, 220));

            DrawTextW(
                dc,
                L"Overlay demo background. Click the button through the overlay.",
                -1,
                &rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE
            );

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY:
        {
            PostQuitMessage(0);
            return 0;
        }

        default:
            break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool RegisterDemoHostClass()
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DemoHostWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH));
    wc.lpszClassName = L"ARKOverlayDemoHost";

    if (!RegisterClassExW(&wc))
    {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS)
            return false;
    }

    return true;
}

} // anonymous namespace

bool ao::overlayDemo(const RECT* initialRect)
{
    loadConfig(L".\\overlay.ini", g_demoConfig);

    EnablePerMonitorDpiV2();

    if (!RegisterDemoHostClass())
        return false;

    int winW = 1280;
    int winH = 720;

    if (initialRect)
    {
        winW = std::max(320L, initialRect->right - initialRect->left);
        winH = std::max(240L, initialRect->bottom - initialRect->top);
    }

    g_demoHost = CreateWindowExW(
        0,
        L"ARKOverlayDemoHost",
        L"ARK Overlay Demo - Esc exits - F5 reloads overlay.ini",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        winW,
        winH,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr
    );

    if (!g_demoHost)
        return false;

    if (initialRect)
    {
        SetWindowPos(
            g_demoHost,
            nullptr,
            initialRect->left,
            initialRect->top,
            winW,
            winH,
            SWP_NOZORDER | SWP_NOACTIVATE
        );
    }

    ShowWindow(g_demoHost, SW_SHOW);
    UpdateWindow(g_demoHost);

    RECT client = {};
    GetClientRect(g_demoHost, &client);

    POINT topLeft{ 0, 0 };
    ClientToScreen(g_demoHost, &topLeft);

    RECT overlayRect;
    overlayRect.left = topLeft.x;
    overlayRect.top = topLeft.y;
    overlayRect.right = topLeft.x + client.right;
    overlayRect.bottom = topLeft.y + client.bottom;

    if (!ao::overlayInit(overlayRect))
    {
        DestroyWindow(g_demoHost);

        MSG m;
        while (PeekMessageW(&m, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE))
        {
        }

        return false;
    }

    std::vector<ao::Target> targets(4);

    // Wild small dino.
    wcscpy_s(targets[0].name, L"Compy");
    targets[0].ownerName[0] = 0;
    targets[0].isPlayer = false;
    targets[0].isTurret = false;
    targets[0].boxW = 0.7f;
    targets[0].boxH = 0.9f;
    targets[0].health = 20.0f;
    targets[0].maxHealth = 20.0f;
    targets[0].level = 7;
    targets[0].ammo = -1;

    // Tamed large dino with owner.
    wcscpy_s(targets[1].name, L"Rex");
    wcscpy_s(targets[1].ownerName, L"TribeOfLlama");
    targets[1].isPlayer = false;
    targets[1].isTurret = false;
    targets[1].boxW = 3.5f;
    targets[1].boxH = 5.5f;
    targets[1].health = 500.0f;
    targets[1].maxHealth = 1000.0f;
    targets[1].level = 143;
    targets[1].ammo = -1;

    // Player.
    wcscpy_s(targets[2].name, L"Player_\u0410\u0440\u043A");
    targets[2].ownerName[0] = 0;
    targets[2].isPlayer = true;
    targets[2].isTurret = false;
    targets[2].boxW = 0.65f;
    targets[2].boxH = 1.8f;
    targets[2].health = 75.0f;
    targets[2].maxHealth = 100.0f;
    targets[2].level = 105;
    targets[2].ammo = -1;

    // Auto turret with ammo.
    wcscpy_s(targets[3].name, L"Auto Turret");
    targets[3].ownerName[0] = 0;
    targets[3].isPlayer = false;
    targets[3].isTurret = true;
    targets[3].boxW = 0.6f;
    targets[3].boxH = 0.9f;
    targets[3].health = 1800.0f;
    targets[3].maxHealth = 1800.0f;
    targets[3].level = -1;
    targets[3].ammo = 40;

    RECT lastOverlay = overlayRect;

    auto start = std::chrono::steady_clock::now();

    // Config hot-reload watcher.
    auto iniWriteTime = []() -> ULONGLONG {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(L".\\overlay.ini", GetFileExInfoStandard, &fad))
        {
            ULARGE_INTEGER u;
            u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
            u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
            return u.QuadPart;
        }
        return 0;
    };

    ULONGLONG lastIniTime = iniWriteTime();
    auto lastIniCheck = std::chrono::steady_clock::now();

    bool quit = false;

    while (!quit)
    {
        MSG msg;

        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                quit = true;
                break;
            }

            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (quit)
            break;

        if (!IsWindow(g_demoHost))
            break;

        if (IsIconic(g_demoHost))
        {
            ao::overlayHide();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        RECT nowClient = {};
        GetClientRect(g_demoHost, &nowClient);

        POINT nowTopLeft{ 0, 0 };
        ClientToScreen(g_demoHost, &nowTopLeft);

        RECT currentOverlay;
        currentOverlay.left = nowTopLeft.x;
        currentOverlay.top = nowTopLeft.y;
        currentOverlay.right = nowTopLeft.x + nowClient.right;
        currentOverlay.bottom = nowTopLeft.y + nowClient.bottom;

        if (!EqualRect(&currentOverlay, &lastOverlay))
        {
            ao::overlayResize(currentOverlay);
            lastOverlay = currentOverlay;
        }

        ao::overlayShow();

        {
            auto nowCheck = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    nowCheck - lastIniCheck).count() >= 500)
            {
                lastIniCheck = nowCheck;
                ULONGLONG t2 = iniWriteTime();
                if (t2 != lastIniTime)
                {
                    lastIniTime = t2;
                    ao::loadConfig(L".\\overlay.ini", g_demoConfig);
                }
            }
        }

        float t = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - start
        ).count();

        targets[0].worldPos.x = sinf(t * 1.1f) * 2.0f;
        targets[0].worldPos.y = 0.0f;
        targets[0].worldPos.z = 8.0f + sinf(t * 0.7f) * 2.0f;

        targets[1].worldPos.x = cosf(t * 0.35f) * 12.0f;
        targets[1].worldPos.y = 0.0f;
        targets[1].worldPos.z = 80.0f + 12.0f * sinf(t * 0.21f);
        targets[1].health = 500.0f + 500.0f * sinf(t * 0.4f);

        targets[2].worldPos.x = 3.0f * sinf(t * 0.9f);
        targets[2].worldPos.y = 0.0f;
        targets[2].worldPos.z = 25.0f + 6.0f * cosf(t * 0.37f);
        targets[2].health = 50.0f + 50.0f * sinf(t * 1.7f);

        targets[3].worldPos.x = -5.0f + 1.0f * sinf(t * 0.5f);
        targets[3].worldPos.y = 0.0f;
        targets[3].worldPos.z = 15.0f;
        targets[3].ammo = static_cast<int>(20.0f + 20.0f * sinf(t * 0.8f));

        for (auto& tgt : targets)
        {
            tgt.behindCamera = false;
            tgt.headPos = tgt.worldPos;
            tgt.headPos.y += tgt.boxH * 0.5f;
        }

        ao::Camera cam;
        cam.pos = { 0.0f, 0.0f, 0.0f };
        cam.forward = { 0.0f, 0.0f, 1.0f };
        cam.right = { 1.0f, 0.0f, 0.0f };
        cam.up = { 0.0f, 1.0f, 0.0f };
        cam.fovDegrees = 90.0f;
        cam.screenW = lastOverlay.right - lastOverlay.left;
        cam.screenH = lastOverlay.bottom - lastOverlay.top;

        // Fake "your position" for the coords HUD (cm).
        cam.playerPos.x = 1500.0f + 500.0f * sinf(t * 0.25f);
        cam.playerPos.y = 120.0f;
        cam.playerPos.z = -3000.0f + 800.0f * cosf(t * 0.2f);

        ao::overlayDraw(
            targets.data(),
            static_cast<int>(targets.size()),
            cam,
            g_demoConfig
        );

        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    ao::overlayShutdown();

    return true;
}

//-----------------------------------------------------------------------------
// Screen-space draw path (matrix / arkmath projection)
//-----------------------------------------------------------------------------
bool ao::overlayDrawScreen(
    const ScreenTarget* targetList,
    int targetCount,
    int screenW,
    int screenH,
    const Vec3& playerPos,
    const Config& config)
{
    if (!S.hwnd || !S.surfaceGraphics)
        return false;
    if (!IsWindowVisible(S.hwnd))
        return false;

    EnsureFont(config.fontScale);
    EnsureStringFormats();

    Graphics* g = S.surfaceGraphics;
    g->Clear(Color(0, 0, 0, 0));

    const float uiScale = (S.dpi > 1.0f) ? (S.dpi / 96.0f) : 1.0f;
    const float penWidth = std::max(1.0f, uiScale);
    float lineH = S.font ? S.font->GetHeight(g) : 12.0f;
    if (!(lineH > 1.0f))
        lineH = 12.0f;

    static float fps = 0.0f;
    static bool fpsInit = false;
    static std::chrono::steady_clock::time_point lastTime;
    auto now = std::chrono::steady_clock::now();
    if (!fpsInit) { fpsInit = true; lastTime = now; }
    else {
        float dt = std::chrono::duration<float>(now - lastTime).count();
        if (dt > 0.0f) fps = fps * 0.9f + (1.0f / dt) * 0.1f;
        lastTime = now;
    }
    if (fps > 999.0f) fps = 999.0f;

    const float sw = static_cast<float>(screenW);
    const float sh = static_cast<float>(screenH);
    const float maxRange = std::max(0.0f, config.maxRange);

    if (targetList)
    {
        for (int i = 0; i < targetCount; ++i)
        {
            const ScreenTarget& t = targetList[i];

            if (t.isTurret && !config.enableTurrets) continue;
            if (config.playersOnly && !t.isPlayer) continue;
            if (t.distance > maxRange) continue;

            const float cx = t.screenX;
            const float cy = t.screenY;
            const float wpx = t.boxW;
            const float hpx = t.boxH;

            if (!IsFinite(cx) || !IsFinite(cy) || !(wpx > 0.0f) || !(hpx > 0.0f))
                continue;

            if (cx < -wpx || cy < -hpx || cx > sw + wpx || cy > sh + hpx)
                continue;

            const float x = cx - wpx * 0.5f;
            const float y = cy - hpx * 0.5f;

            const uint32_t baseArgb =
                t.isTurret ? config.turretColor :
                (t.isPlayer ? config.playerColor : config.dinoColor);
            Color baseColor(baseArgb);

            if (config.enableBoxes)
            {
                Pen pen(baseColor, penWidth);
                g->DrawRectangle(&pen, x, y, wpx, hpx);
                g->DrawLine(&pen, cx, y, cx, y + hpx);
            }

            if (config.enableNames && S.font && S.sfCenter)
            {
                wchar_t label[192] = {};
                int n = 0;
                if (t.isTurret)
                {
                    n = swprintf_s(label, L"%s", t.name[0] ? t.name : L"Turret");
                    if (n > 0 && config.showAmmo && t.ammo >= 0)
                        swprintf_s(label + n, 192 - n, L" | Ammo %d", t.ammo);
                }
                else
                {
                    n = swprintf_s(label, L"%s",
                        t.name[0] ? t.name : (t.isPlayer ? L"Player" : L"Unknown"));
                    if (n > 0 && t.ownerName[0])
                        n += swprintf_s(label + n, 192 - n, L" (%s)", t.ownerName);
                    if (n > 0 && config.showLevel && t.level >= 0)
                        swprintf_s(label + n, 192 - n, L" Lvl %d", t.level);
                }
                if (n > 0 && label[0])
                {
                    float textW = std::max(wpx, 200.0f * uiScale);
                    float nameY = y - lineH - 2.0f * uiScale;
                    if (nameY < 0.0f)
                        nameY = y + hpx + 2.0f * uiScale;
                    RectF nameRect(cx - textW * 0.5f, nameY, textW, lineH);
                    SolidBrush nameBrush(baseColor);
                    g->DrawString(label, -1, S.font, nameRect, S.sfCenter, &nameBrush);
                }
            }

            if (config.enableHealth && t.maxHealth > 0.0f && IsFinite(t.maxHealth))
            {
                float frac = t.health / t.maxHealth;
                if (!(frac >= 0.0f)) frac = 0.0f;
                if (frac > 1.0f) frac = 1.0f;
                const float barW = std::max(3.0f, 3.0f * uiScale);
                const float gap = 3.0f * uiScale;
                float barX = x + wpx + gap;
                if (barX + barW > sw) barX = x - barW - gap;
                Color bgColor(140, 0, 0, 0);
                SolidBrush bgBrush(bgColor);
                g->FillRectangle(&bgBrush, barX, y, barW, hpx);
                if (frac > 0.0f)
                {
                    const float fillH = hpx * frac;
                    Color fillColor(config.healthBarColor);
                    SolidBrush fillBrush(fillColor);
                    g->FillRectangle(&fillBrush, barX, y + hpx - fillH, barW, fillH);
                }
                Color borderColor(220, 0, 0, 0);
                Pen borderPen(borderColor, 1.0f);
                g->DrawRectangle(&borderPen, barX, y, barW, hpx);
            }

            if (config.enableDistance && S.font && S.sfCenter)
            {
                wchar_t distBuf[64] = {};
                swprintf_s(distBuf, L"%.0fm", t.distance);
                float textW = std::max(wpx, 120.0f * uiScale);
                RectF distRect(cx - textW * 0.5f, y + hpx + 2.0f * uiScale, textW, lineH);
                SolidBrush distBrush(Color(230, 255, 255, 255));
                g->DrawString(distBuf, -1, S.font, distRect, S.sfCenter, &distBrush);
            }
        }
    }

    if (config.showFps && S.font && S.sfLeft)
    {
        wchar_t fpsBuf[32] = {};
        swprintf_s(fpsBuf, L"FPS %3.0f", fps);
        RectF fpsRect(4.0f * uiScale, 4.0f * uiScale, 300.0f * uiScale, lineH);
        SolidBrush fpsBrush(Color(255, 255, 255, 255));
        g->DrawString(fpsBuf, -1, S.font, fpsRect, S.sfLeft, &fpsBrush);
    }

    if (config.showCoords && S.font && S.sfLeft)
    {
        wchar_t buf[128] = {};
        swprintf_s(buf, L"X:%.1f Y:%.1f Z:%.1f",
            playerPos.x / 100.0f, playerPos.y / 100.0f, playerPos.z / 100.0f);
        RectF coordRect(4.0f * uiScale, 4.0f * uiScale + lineH * 1.3f, 400.0f * uiScale, lineH);
        SolidBrush coordBrush(Color(255, 255, 255, 255));
        g->DrawString(buf, -1, S.font, coordRect, S.sfLeft, &coordBrush);
    }

    g->Flush();
    Present();
    return true;
}