// arkesp.dll: in-process ESP for ARK Store (UWP).
// Same data pipeline as the external tool (world -> level -> actors -> pawns,
// camera from viewport client matrix), rendered through the game's own D3D11
// Present via D2D. SEH everywhere; a bad read disables drawing, never crashes.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <psapi.h>
#include <d3d11.h>
#include <d2d1.h>
#include <dwrite.h>
#include <cmath>
#include <cstdio>
#include <string>
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

// ---- verified offsets for 1.212.962.2 (same table as external tool) ----
static const uint64_t RVA_GWORLD = 0x43a5758, RVA_ENGINE = 0x438e270;
static const uint32_t OFF_GI = 0x5f48, OFF_LP = 0xf0, OFF_VC = 0x1e0, OFF_MAT = 0x60;
static const uint32_t OFF_LVL = 0xf8, OFF_ACT = 0x88, OFF_LOC = 0xc00, OFF_CAPS = 0x268;
static const uint32_t OFF_CAPR = 0x124, OFF_CAPH = 0x12c, OFF_HP = 0x964, OFF_MHP = 0x968;
static const uint32_t OFF_STAT = 0xd10, OFF_DLVL = 0x6cc;
static const wchar_t* EXPECTED_VERSION = L"1.212.962.2";

static FILE* g_log = nullptr;
static void dlog(const char* fmt, ...)
{
    if (!g_log) return;
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fprintf(g_log, "\n"); fflush(g_log);
}

static bool g_active = false;
static bool g_stop = false;

// ---- SEH-guarded in-process reads ----
static bool rd(uint64_t a, void* o, size_t n)
{
    __try { memcpy(o, (void*)a, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool rdP(uint64_t a, uint64_t& p)
{
    if (!rd(a, &p, 8)) return false;
    return p > 0x10000 && p < 0x7FFFFFFFFFFF && !(p & 7);
}

// ---- pipeline state ----
struct Tgt { float x, y, z, r, hh, hp, mhp; int lvl; };
static Tgt g_tgts[96];
static int g_ntgt = 0;
static float g_px, g_py, g_pz;          // camera pos
static float g_fw[3], g_rt3[3], g_up[3]; // normalized axes
static float g_fov = 90.0f;
static bool g_cam = false;
static int g_frame = 0;

static void norm3(float v[3])
{
    const float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (l > 1e-6f) { v[0]/=l; v[1]/=l; v[2]/=l; }
}

static void collect()   // identical chain logic, in-process
{
    g_ntgt = 0; g_cam = false;
    __try
    {
        const uint64_t base = (uint64_t)GetModuleHandleW(L"ShooterGame.exe");
        if (!base) return;
        uint64_t world = 0, engine = 0, gi = 0, lpa = 0, lp = 0, vc = 0;
        int32_t cnt = 0;
        if (!rdP(base + RVA_GWORLD, world)) return;
        if (!rdP(base + RVA_ENGINE, engine)) return;
        if (!rdP(engine + OFF_GI, gi)) return;
        if (!rdP(gi + OFF_LP, lpa)) return;
        if (!rd(lpa + 8, &cnt, 4) || cnt <= 0 || cnt > 4) return;
        if (!rdP(lpa, lp)) return;
        if (!rdP(lp + OFF_VC, vc)) return;
        float m[16];
        if (!rd(vc + OFF_MAT, m, sizeof(m))) return;
        g_px = m[12]; g_py = m[13]; g_pz = m[14];
        g_fw[0]=m[0]; g_fw[1]=m[1]; g_fw[2]=m[2];
        g_rt3[0]=m[4]; g_rt3[1]=m[5]; g_rt3[2]=m[6];
        g_up[0]=m[8]; g_up[1]=m[9]; g_up[2]=m[10];
        norm3(g_fw); norm3(g_rt3); norm3(g_up);
        const float s0 = sqrtf(m[0]*m[0]+m[1]*m[1]+m[2]*m[2]);
        if (s0 > 0.05f && s0 < 64.0f)
            g_fov = 2.0f * atanf(1.0f / s0) * 180.0f / 3.14159265358979323846f;
        g_cam = true;

        uint64_t level = 0, data = 0;
        int32_t n = 0;
        if (!rdP(world + OFF_LVL, level)) return;
        if (!rdP(level + OFF_ACT, data)) return;
        if (!rd(level + OFF_ACT + 8, &n, 4) || n <= 0 || n > 200000) return;

        for (int i = 0; i < n && g_ntgt < 96; ++i)
        {
            uint64_t a = 0;
            if (!rdP(data + (uint64_t)i * 8, a)) continue;
            Tgt t{};
            float loc[3], cloc[3];
            bool hD = false, hC = false;
            if (rd(a + OFF_LOC, loc, 12))
                hD = fabsf(loc[0]) < 1e8f && (fabsf(loc[0])+fabsf(loc[1])+fabsf(loc[2])) > 1.0f;
            uint64_t caps = 0;
            if (rdP(a + OFF_CAPS, caps) && rd(caps + 0xF0, cloc, 12))
                hC = fabsf(cloc[0]) < 1e8f && (fabsf(cloc[0])+fabsf(cloc[1])+fabsf(cloc[2])) > 1.0f;
            if (!hD && !hC) continue;
            if (hD && hC)
            {
                const float dx=loc[0]-cloc[0], dy=loc[1]-cloc[1], dz=loc[2]-cloc[2];
                if (dx*dx+dy*dy+dz*dz > 2000.0f*2000.0f) hD = false; // consensus
            }
            if (hC) { t.x=cloc[0]; t.y=cloc[1]; t.z=cloc[2]; }
            else    { t.x=loc[0];  t.y=loc[1];  t.z=loc[2]; }
            if (!rdP(a + OFF_CAPS, caps)) continue;
            if (!rd(caps + OFF_CAPR, &t.r, 4) || !rd(caps + OFF_CAPH, &t.hh, 4)) continue;
            if (!(t.r > 0 && t.r < 1500 && t.hh > 0 && t.hh < 3000)) continue;
            rd(a + OFF_HP, &t.hp, 4);
            rd(a + OFF_MHP, &t.mhp, 4);
            uint64_t st = 0;
            if (rdP(a + OFF_STAT, st))
            {
                int32_t lv = -1;
                if (rd(st + OFF_DLVL, &lv, 4) && lv >= 0 && lv < 1000000) t.lvl = lv;
            }
            g_tgts[g_ntgt++] = t;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_ntgt = 0; g_cam = false; }
}

// ---- D3D11 Present hook + D2D rendering ----
typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *ResizeFn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static PresentFn g_origPresent = nullptr;
static ResizeFn  g_origResize = nullptr;
static IDXGISwapChain* g_dummySC = nullptr;
static ID2D1Factory* g_d2d = nullptr;
static IDWriteFactory* g_dw = nullptr;
static ID2D1RenderTarget* g_rt = nullptr;
static ID2D1SolidColorBrush* g_br = nullptr;
static IDWriteTextFormat* g_tf = nullptr;

static void releaseRT()
{
    if (g_br) { g_br->Release(); g_br = nullptr; }
    if (g_rt) { g_rt->Release(); g_rt = nullptr; }
}

static void ensureRT(IDXGISwapChain* sc)
{
    if (g_rt) return;
    if (!g_d2d) D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d);
    if (!g_dw) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw);
    if (!g_d2d || !g_dw) return;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) return;
    IDXGISurface* surf = nullptr;
    if (SUCCEEDED(back->QueryInterface(__uuidof(IDXGISurface), (void**)&surf)))
    {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_IGNORE));
        g_d2d->CreateDxgiSurfaceRenderTarget(surf, &props, &g_rt);
        surf->Release();
    }
    back->Release();
    if (g_rt)
    {
        g_rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &g_br);
        if (!g_tf)
            g_dw->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13, L"en-us", &g_tf);
        g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
}

static void drawScene()
{
    if (!g_cam || !g_br || !g_tf || g_ntgt == 0) return;
    const D2D1_SIZE_F sz = g_rt->GetSize();
    const float focal = (sz.height * 0.5f) / tanf(g_fov * 3.14159265358979323846f / 360.0f);
    wchar_t buf[64];
    for (int i = 0; i < g_ntgt; ++i)
    {
        const Tgt& t = g_tgts[i];
        const float dx = t.x-g_px, dy = t.y-g_py, dz = t.z-g_pz;
        const float depth = dx*g_fw[0]+dy*g_fw[1]+dz*g_fw[2];
        if (!(depth > 1.0f)) continue;
        const float cx = dx*g_rt3[0]+dy*g_rt3[1]+dz*g_rt3[2];
        const float cy = dx*g_up[0]+dy*g_up[1]+dz*g_up[2];
        const float sx = sz.width*0.5f + (cx/depth)*focal;
        const float sy = sz.height*0.5f - (cy/depth)*focal;
        const float hpx = (t.hh*2.0f) * focal / depth;
        const float wpx = (t.r*2.0f) * focal / depth;
        if (!(hpx > 2.0f) || hpx > sz.height*2.0f) continue;
        g_br->SetColor(D2D1::ColorF(0.2f, 0.9f, 0.3f, 1.0f));
        g_rt->DrawRectangle(D2D1::RectF(sx-wpx*0.5f, sy-hpx*0.5f, sx+wpx*0.5f, sy+hpx*0.5f), g_br, 1.5f);
        swprintf_s(buf, L"Lvl %d  %.0fm  %.0f/%.0f", t.lvl,
                   sqrtf(dx*dx+dy*dy+dz*dz)/100.0f, t.hp, t.mhp);
        g_rt->DrawText(buf, (UINT32)wcslen(buf), g_tf,
                       D2D1::RectF(sx-120, sy-hpx*0.5f-18, sx+120, sy-hpx*0.5f-2), g_br);
    }
}

static HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT a, UINT b)
{
    __try
    {
        if (sc != g_dummySC && g_active)
        {
            if ((++g_frame % 2) == 0) collect();
            ensureRT(sc);
            if (g_rt)
            {
                // NOTE: no Clear() - we draw ON TOP of the game's frame.
                g_rt->BeginDraw();
                drawScene();
                g_rt->EndDraw();
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_active = false;
        dlog("SEH in Present; disabling drawing");
    }
    return g_origPresent(sc, a, b);
}

static HRESULT STDMETHODCALLTYPE hkResize(IDXGISwapChain* sc, UINT c, UINT w, UINT h, DXGI_FORMAT f, UINT fl)
{
    if (sc != g_dummySC) releaseRT();
    return g_origResize(sc, c, w, h, f, fl);
}

static LRESULT CALLBACK DummyWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{ return DefWindowProcW(h, m, w, l); }

static DWORD WINAPI mainThread(void*)
{
    // Log next to the DLL (LocalState) - readable from your account.
    wchar_t path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)mainThread, &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring logp = path;
    logp = logp.substr(0, logp.rfind(L'\\')) + L"\\arkesp.log";
    { std::string a(logp.begin(), logp.end()); fopen_s(&g_log, a.c_str(), "a"); }

    // Version gate: refuse on mismatch.
    {
        UINT32 n = 0;
        if (GetPackageFullName(GetCurrentProcess(), &n, nullptr) == ERROR_INSUFFICIENT_BUFFER && n > 0)
        {
            std::vector<wchar_t> b(n);
            if (GetPackageFullName(GetCurrentProcess(), &n, b.data()) == ERROR_SUCCESS)
            {
                std::wstring f = b.data();
                const size_t u1 = f.find(L'_'), u2 = f.find(L'_', u1 + 1);
                const std::wstring ver = (u2 != std::wstring::npos) ? f.substr(u1+1, u2-u1-1) : f;
                if (ver != EXPECTED_VERSION)
                {
                    dlog("version mismatch %ls != %ls; refusing to activate", ver.c_str(), EXPECTED_VERSION);
                    g_stop = true;
                    return 1;
                }
            }
        }
    }
    dlog("arkesp active, waiting for d3d11");

    for (int i = 0; i < 120 && !GetModuleHandleW(L"d3d11.dll"); ++i) Sleep(500);
    if (!GetModuleHandleW(L"d3d11.dll")) { dlog("no d3d11.dll; abort"); return 2; }

    // Dummy (message-only) window + device to reach the shared swapchain vtable.
    WNDCLASSW wc{};
    wc.lpfnWndProc = DummyWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"arkesp_msg";
    RegisterClassW(&wc);
    HWND hw = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!hw) { dlog("message window failed err=%lu", GetLastError()); return 3; }

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 64; sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hw;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL flv;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &sd, &g_dummySC, &dev, &flv, &ctx);
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &sd, &g_dummySC, &dev, &flv, &ctx);
    if (FAILED(hr) || !g_dummySC) { dlog("dummy device failed hr=0x%08lX", (unsigned long)hr); return 3; }

    void** vt = *(void***)g_dummySC;
    g_origPresent = (PresentFn)vt[8];
    g_origResize = (ResizeFn)vt[13];
    DWORD oldProt = 0;
    VirtualProtect(&vt[8], sizeof(void*), PAGE_READWRITE, &oldProt);
    vt[8] = (void*)hkPresent;
    VirtualProtect(&vt[8], sizeof(void*), oldProt, &oldProt);
    VirtualProtect(&vt[13], sizeof(void*), PAGE_READWRITE, &oldProt);
    vt[13] = (void*)hkResize;
    VirtualProtect(&vt[13], sizeof(void*), oldProt, &oldProt);
    dlog("present hooked at %p", g_origPresent);

    g_active = true;
    while (!g_stop) Sleep(1000);   // uninject story: restart the game
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(GetModuleHandleW(nullptr));
        HANDLE t = CreateThread(nullptr, 0, mainThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}