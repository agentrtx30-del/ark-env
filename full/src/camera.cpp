#include "camera.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

static bool g_verbose = false;
void cameraDebugSetVerbose(bool verbose) { g_verbose = verbose; }

static bool finiteMatrix(const float m[16])
{
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i])) return false;
    return true;
}

static bool readResolution(const WinMemory& mem, uint64_t address, int32_t& w, int32_t& h)
{
    if (!mem.read(address, w)) return false;
    if (!mem.read(address + 4, h)) return false;
    return (w >= 16 && h >= 16 && w <= 32768 && h <= 32768);
}

static int matrixSignature(const float m[16])
{
    if (!finiteMatrix(m)) return 0;
    const float magLast = fabsf(m[12]) + fabsf(m[13]) + fabsf(m[14]);
    const bool colOK =
        fabsf(m[3]) < 1e-2f && fabsf(m[7]) < 1e-2f &&
        fabsf(m[11]) < 1e-2f && fabsf(m[15] - 1.0f) < 1e-2f;
    bool rowsOK = true;
    for (int r = 0; r < 3; ++r)
    {
        const float a = m[r * 4], b = m[r * 4 + 1], c = m[r * 4 + 2];
        const float L = sqrtf(a * a + b * b + c * c);
        if (!(L > 0.2f && L < 8.0f)) { rowsOK = false; break; }
    }
    if (colOK && rowsOK && magLast > 1e4f && magLast < 5e7f) return 1;
    bool anyLarge = false;
    for (int i = 0; i < 16; ++i)
        if (fabsf(m[i]) > 1e3f) { anyLarge = true; break; }
    if (anyLarge && magLast > 1e3f && magLast < 1e8f) return 2;
    return 0;
}

struct Candidate
{
    const char* root = "";
    uint32_t off = 0;
    int sig = 0;
    bool resOk = false;
    int w = 0, h = 0;
    uint64_t p = 0;
    float m[16] = {};
};

static std::vector<Candidate> g_cands;

void cameraDebugDumpCandidates()
{
    if (g_cands.empty())
    {
        printf("  (no matrix-like candidates found near any root)\n");
        return;
    }
    for (const Candidate& c : g_cands)
    {
        printf("  cand %-12s +0x%-5X sig=%d res=%s", c.root, c.off, c.sig, c.resOk ? "yes" : "no");
        if (c.resOk) printf(" %dx%d", c.w, c.h);
        printf("\n      lastrow = %.1f %.1f %.1f   m15=%.3f\n", c.m[12], c.m[13], c.m[14], c.m[15]);
    }
}

static bool resFor(const WinMemory& mem, const OffsetProfile& off, uint64_t p, int& w, int& h)
{
    uint64_t vp = 0;
    if (readPtr(mem, p + off.viewportOffset, vp) &&
        readResolution(mem, vp + off.resolutionOffset, w, h)) return true;
    if (off.altResolutionViewportOffset && off.altResolutionOffset)
    {
        uint64_t avp = 0;
        if (readPtr(mem, p + off.altResolutionViewportOffset, avp) &&
            readResolution(mem, avp + off.altResolutionOffset, w, h)) return true;
    }
    if (off.altResolutionDirect && readResolution(mem, p + off.altResolutionDirect, w, h)) return true;
    return false;
}

static float anchorDist(const Candidate& c, const float* a)
{
    return fabsf(c.m[12] - a[0]) + fabsf(c.m[13] - a[1]) + fabsf(c.m[14] - a[2]);
}

static void collect(const WinMemory& mem, const OffsetProfile& off, const char* root,
                    uint64_t base, uint32_t lo, uint32_t hi, std::vector<Candidate>& out)
{
    for (uint32_t o = lo; o <= hi; o += 8)
    {
        uint64_t p = 0;
        if (!readPtr(mem, base + o, p)) continue;
        float m[16];
        if (!mem.readBytes(p + off.viewMatrixOffset, m, sizeof(m))) continue;
        const int sig = matrixSignature(m);
        if (sig != 1) continue;                 // only true camera->world transforms
        Candidate c{};
        c.root = root; c.off = o; c.sig = sig; c.p = p;
        memcpy(c.m, m, sizeof(m));
        c.resOk = resFor(mem, off, p, c.w, c.h);
        if (!c.resOk) continue;
        if (g_cands.size() < 12) g_cands.push_back(c);
        out.push_back(c);
    }
}

enum class VpRoute { None, LocalPlayer, GameInstance, Engine };
static VpRoute g_route = VpRoute::None;
static uint32_t g_routeOffset = 0;

static uint64_t routeSource(VpRoute r, uint64_t lp, uint64_t gi, uint64_t eng)
{
    return r == VpRoute::LocalPlayer ? lp : (r == VpRoute::GameInstance ? gi : eng);
}

// STICKY selection: keep the current viewport client forever unless it stops
// reading sane, drifts >300 m from the pawn anchor, or another candidate is
// >5 m closer to the anchor. No time-based re-picks => no periodic flicker.
static bool resolveViewportClient(
    const WinMemory& mem, const OffsetProfile& offsets,
    uint64_t localPlayer, uint64_t gameInstance, uint64_t engine,
    const float* anchor, bool anchorValid, Candidate& out)
{
    std::vector<Candidate> cands;
    collect(mem, offsets, "localPlayer", localPlayer, 0x28, 0x4000, cands);
    if (cands.empty())
        collect(mem, offsets, "gameInstance", gameInstance, 0x28, 0x10000, cands);
    if (cands.empty())
        collect(mem, offsets, "engine", engine, 0x28, 0x8000, cands);
    if (cands.empty())
    {
        g_route = VpRoute::None;
        return false;
    }
    if (anchorValid)
    {
        std::sort(cands.begin(), cands.end(),
            [&](const Candidate& a, const Candidate& b)
            { return anchorDist(a, anchor) < anchorDist(b, anchor); });
    }
    const Candidate& bestNew = cands[0];

    if (g_route != VpRoute::None)
    {
        const uint64_t src = routeSource(g_route, localPlayer, gameInstance, engine);
        uint64_t p = 0;
        if (readPtr(mem, src + g_routeOffset, p))
        {
            float m[16];
            if (mem.readBytes(p + offsets.viewMatrixOffset, m, sizeof(m)) &&
                matrixSignature(m) == 1)
            {
                Candidate cur{};
                cur.root = "cache"; cur.off = g_routeOffset; cur.sig = 1; cur.p = p;
                memcpy(cur.m, m, sizeof(m));
                cur.resOk = resFor(mem, offsets, p, cur.w, cur.h);
                if (cur.resOk)
                {
                    bool switchAway = false;
                    if (anchorValid)
                    {
                        const float dCur = anchorDist(cur, anchor);
                        const float dNew = anchorDist(bestNew, anchor);
                        switchAway = (dCur > 30000.0f) || (dNew + 500.0f < dCur);
                    }
                    if (!switchAway)
                    {
                        out = cur;
                        return true;
                    }
                }
            }
        }
    }

    // Adopt the best candidate.
    g_route = strcmp(bestNew.root, "localPlayer") == 0 ? VpRoute::LocalPlayer :
              strcmp(bestNew.root, "gameInstance") == 0 ? VpRoute::GameInstance :
              VpRoute::Engine;
    g_routeOffset = bestNew.off;
    if (g_verbose)
        printf("    DISCOVERED viewportClient at %s+0x%X (sig=%d res=yes)\n",
               bestNew.root, bestNew.off, bestNew.sig);
    out = bestNew;
    return true;
}

static void dumpHop(const char* name, const WinMemory& mem, uint64_t addr)
{
    if (!g_verbose) return;
    uint64_t raw = 0;
    const bool ok = mem.read(addr, raw);
    printf("    FAIL %-12s @0x%016llX raw=0x%016llX read=%d sane=%d\n",
           name, addr, raw, ok ? 1 : 0, (ok && isSaneUserPointer(raw)) ? 1 : 0);
}

static bool tryChain(
    const WinMemory& mem, const RuntimeRoots& roots, const OffsetProfile& offsets,
    uint32_t giOff, uint32_t lpOff, int fallbackW, int fallbackH,
    const float* anchor, bool anchorValid, CameraState& out)
{
    if (g_verbose) printf("  try gi=+0x%X lp=+0x%X\n", giOff, lpOff);
    uint64_t engine = 0;
    if (!readPtr(mem, roots.engineGlobalAddress, engine)) { dumpHop("engine", mem, roots.engineGlobalAddress); return false; }
    uint64_t gameInstance = 0;
    if (!readPtr(mem, engine + giOff, gameInstance)) { dumpHop("gameInstance", mem, engine + giOff); return false; }
    const uint64_t lpArray = gameInstance + lpOff;
    uint64_t lpData = 0;
    if (!readPtr(mem, lpArray, lpData)) { dumpHop("lpData", mem, lpArray); return false; }
    int32_t lpCount = 0;
    if (!mem.read(lpArray + 8, lpCount) || lpCount <= 0 || lpCount > 4)
    { if (g_verbose) printf("    FAIL lpCount     count=%d\n", lpCount); return false; }
    uint64_t localPlayer = 0;
    if (!readPtr(mem, lpData, localPlayer)) { dumpHop("localPlayer", mem, lpData); return false; }
    if (g_verbose) printf("    localPlayer    = 0x%016llX\n", localPlayer);

    Candidate cand;
    if (!resolveViewportClient(mem, offsets, localPlayer, gameInstance, engine,
                               anchor, anchorValid, cand))
    { dumpHop("viewportClnt", mem, localPlayer + offsets.viewportClientOffset); return false; }
    if (g_verbose) printf("    viewportClient = 0x%016llX sig=%d\n", cand.p, cand.sig);

    int32_t w = 0, h = 0;
    if (cand.resOk) { w = cand.w; h = cand.h; }
    else if (fallbackW >= 16 && fallbackH >= 16) { w = fallbackW; h = fallbackH; }
    else return false;

    if (g_verbose)
    {
        printf("    matrix pos     = %.1f %.1f %.1f\n", cand.m[12], cand.m[13], cand.m[14]);
        printf("    matrix row0    = %.3f %.3f %.3f %.3f\n", cand.m[0], cand.m[1], cand.m[2], cand.m[3]);
        printf("    matrix row3    = %.1f %.1f %.1f %.3f\n", cand.m[12], cand.m[13], cand.m[14], cand.m[15]);
    }
    out.valid = true;
    out.viewW = w; out.viewH = h;
    memcpy(out.matrix16, cand.m, sizeof(out.matrix16));
    out.projectionLike = false;
    out.cameraPos = { cand.m[12], cand.m[13], cand.m[14] };
    out.forward   = { cand.m[0],  cand.m[1],  cand.m[2]  };
    out.right     = { cand.m[4],  cand.m[5],  cand.m[6]  };
    out.up        = { cand.m[8],  cand.m[9],  cand.m[10] };
    const float s0 = sqrtf(cand.m[0] * cand.m[0] + cand.m[1] * cand.m[1] + cand.m[2] * cand.m[2]);
    if (s0 > 0.05f && s0 < 64.0f)
        out.fovDegrees = 2.0f * atanf(1.0f / s0) * 180.0f / 3.14159265358979f;
    else
        out.fovDegrees = 90.0f;
    return true;
}

bool readCameraState(
    const WinMemory& mem, const RuntimeRoots& roots, const OffsetProfile& offsets,
    CameraState& out, int fallbackW, int fallbackH, const float* anchor)
{
    out = {};
    g_cands.clear();
    const bool anchorValid = (anchor != nullptr);
    const uint32_t gi[2] = { offsets.gameInstanceOffset, offsets.altGameInstanceOffset };
    const uint32_t lp[2] = { offsets.localPlayersOffset, offsets.altLocalPlayersOffset };
    for (int i = 0; i < 2; ++i)
    {
        if (gi[i] == 0) continue;
        if (i == 1 && gi[1] == gi[0]) break;
        for (int j = 0; j < 2; ++j)
        {
            if (lp[j] == 0) continue;
            if (j == 1 && lp[1] == lp[0]) break;
            if (tryChain(mem, roots, offsets, gi[i], lp[j], fallbackW, fallbackH,
                         anchor, anchorValid, out))
                return true;
        }
    }
    return false;
}