#include "camera.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>

static bool g_verbose = false;
void cameraDebugSetVerbose(bool verbose) { g_verbose = verbose; }

static const float kPawnAnchorMaxCm = 3000.0f;
static const float kPi = 3.14159265358979f;

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

static bool readMatrixTearSafe(const WinMemory& mem, uint64_t addr, float out[16])
{
    float a[16], b[16];
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        if (!mem.readBytes(addr, a, sizeof(a))) return false;
        if (!mem.readBytes(addr, b, sizeof(b))) return false;
        if (memcmp(a, b, sizeof(a)) == 0)
        {
            if (matrixSignature(a) != 1) return false;
            memcpy(out, a, sizeof(a));
            return true;
        }
    }
    return false;
}

static bool yawOnlyMatrix(const float m[16])
{
    return fabsf(m[2]) < 1e-3f && fabsf(m[6]) < 1e-3f &&
           fabsf(m[8]) < 1e-3f && fabsf(m[9]) < 1e-3f &&
           fabsf(m[10] - 1.0f) < 1e-3f;
}

// ---------------------------------------------------------------------------
// POV (definitive) path
// ---------------------------------------------------------------------------
struct PovInfo
{
    Vec3f loc;
    Vec3f rot;
    float fov;
};

static bool readPovTearSafe(const WinMemory& mem, uint64_t addr, PovInfo& out)
{
    unsigned char a[28], b[28];
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        if (!mem.readBytes(addr, a, sizeof(a))) return false;
        if (!mem.readBytes(addr, b, sizeof(b))) return false;
        if (memcmp(a, b, sizeof(a)) == 0)
        {
            memcpy(&out.loc, a, 12);
            memcpy(&out.rot, a + 12, 12);
            memcpy(&out.fov, a + 24, 4);
            return std::isfinite(out.loc.x) && std::isfinite(out.loc.y) && std::isfinite(out.loc.z) &&
                   std::isfinite(out.rot.x) && std::isfinite(out.rot.y) && std::isfinite(out.rot.z) &&
                   out.fov > 5.0f && out.fov < 170.0f &&
                   out.rot.x > -89.9f && out.rot.x < 89.9f;
        }
    }
    return false;
}

static bool povPlausible(const PovInfo& p)
{
    const float ax = fabsf(p.loc.x), ay = fabsf(p.loc.y), az = fabsf(p.loc.z);
    if (ax > 5.0e6f || ay > 5.0e6f || az > 5.0e6f) return false;
    return (ax + ay + az) > 10000.0f;
}

static void buildCameraFromPov(const PovInfo& pov, int fbW, int fbH, CameraState& out)
{
    const float pitch = pov.rot.x * kPi / 180.0f;
    const float yaw   = pov.rot.y * kPi / 180.0f;
    const float cp = cosf(pitch), sp = sinf(pitch);
    const float cy = cosf(yaw),   sy = sinf(yaw);

    Vec3f F = { cp * cy, cp * sy, sp };
    Vec3f R = { sy, -cy, 0.0f };
    Vec3f U = { F.y * R.z - F.z * R.y,
                F.z * R.x - F.x * R.z,
                F.x * R.y - F.y * R.x };

    out = {};
    out.valid = true;
    out.viewW = fbW;
    out.viewH = fbH;
    out.projectionLike = false;
    out.cameraPos = pov.loc;
    out.forward = F;
    out.right = R;
    out.up = U;
    out.fovDegrees = pov.fov;
    memset(out.matrix16, 0, sizeof(out.matrix16));
    out.matrix16[0] = out.matrix16[5] = out.matrix16[10] = out.matrix16[15] = 1.0f;
}

// v34: NO class-pointer sanity check here — it was rejecting the one good
// offset combo and killing findcam/POV on this build.
static bool readLocalPlayer(const WinMemory& mem, const RuntimeRoots& roots,
                            const OffsetProfile& off, uint64_t& lpOut)
{
    uint64_t engine = 0;
    if (!readPtr(mem, roots.engineGlobalAddress, engine)) return false;
    const uint32_t giOffs[2] = { off.gameInstanceOffset, off.altGameInstanceOffset };
    const uint32_t lpOffs[2] = { off.localPlayersOffset, off.altLocalPlayersOffset };
    for (int i = 0; i < 2; ++i)
    {
        if (giOffs[i] == 0) continue;
        uint64_t gi = 0;
        if (!readPtr(mem, engine + giOffs[i], gi)) continue;
        for (int j = 0; j < 2; ++j)
        {
            if (lpOffs[j] == 0) continue;
            uint64_t lpArr = 0;
            if (!readPtr(mem, gi + lpOffs[j], lpArr)) continue;
            int32_t cnt = 0;
            if (!mem.read(lpArr + 8, cnt) || cnt <= 0 || cnt > 4) continue;
            uint64_t lpData = 0;
            if (!readPtr(mem, lpArr, lpData)) continue;
            uint64_t lp = 0;
            if (!readPtr(mem, lpData, lp)) continue;
            lpOut = lp;
            return true;
        }
    }
    return false;
}

static bool readObjectName(const WinMemory& mem,
                           const std::unordered_map<uint64_t, std::wstring>& idmap,
                           uint64_t obj, std::wstring& nameOut)
{
    uint64_t cls = 0;
    if (!readPtr(mem, obj + 0x10, cls)) return false;
    uint32_t idx = 0;
    if (!mem.read(cls + 0x18, idx)) return false;
    auto it = idmap.find((uint64_t)idx);
    if (it == idmap.end()) return false;
    nameOut = it->second;
    return true;
}

bool runFindCamera(const WinMemory& mem, const RuntimeRoots& roots, OffsetProfile& off,
                   const std::unordered_map<uint64_t, std::wstring>& idmap,
                   std::string& report)
{
    char buf[512];
    uint64_t lp = 0;
    if (!readLocalPlayer(mem, roots, off, lp)) { report = "no local player"; return false; }

    uint64_t ctrl = 0;
    if (off.playerControllerOffset && readPtr(mem, lp + off.playerControllerOffset, ctrl))
    {
        uint64_t cls = 0;
        if (!readPtr(mem, ctrl + 0x10, cls)) ctrl = 0;
    }
    else ctrl = 0;
    if (!ctrl)
    {
        for (uint32_t o = 0x18; o <= 0x120 && !ctrl; o += 8)
        {
            uint64_t c = 0;
            if (!readPtr(mem, lp + o, c)) continue;
            std::wstring nm;
            if (!idmap.empty())
            {
                if (!readObjectName(mem, idmap, c, nm)) continue;
                if (nm.find(L"PlayerController") == std::wstring::npos) continue;
            }
            else
            {
                uint64_t cls = 0;
                if (!readPtr(mem, c + 0x10, cls)) continue;
            }
            ctrl = c;
            off.playerControllerOffset = o;
        }
    }
    if (!ctrl) { report = "no controller"; return false; }

    uint64_t pcm = 0;
    if (off.pcmOffset && readPtr(mem, ctrl + off.pcmOffset, pcm))
    {
        uint64_t cls = 0;
        if (!readPtr(mem, pcm + 0x10, cls)) pcm = 0;
    }
    else pcm = 0;
    if (!pcm)
    {
        for (uint32_t o = 0x280; o <= 0x480 && !pcm; o += 8)
        {
            uint64_t p = 0;
            if (!readPtr(mem, ctrl + o, p)) continue;
            std::wstring nm;
            if (!idmap.empty())
            {
                if (!readObjectName(mem, idmap, p, nm)) continue;
                if (nm.find(L"PlayerCameraManager") == std::wstring::npos) continue;
            }
            else
            {
                uint64_t cls = 0;
                if (!readPtr(mem, p + 0x10, cls)) continue;
            }
            pcm = p;
            off.pcmOffset = o;
        }
    }
    if (!pcm) { report = "no player camera manager"; return false; }

    bool found = false;
    uint32_t povOff = 0;
    PovInfo pov{};
    for (uint32_t o = 0x0; o <= 0x2800 && !found; o += 0x8)
    {
        float f = 0.0f;
        if (!mem.read(pcm + o + 0x18, f)) continue;
        if (!(f > 5.0f && f < 170.0f)) continue;
        PovInfo p{};
        if (!readPovTearSafe(mem, pcm + o, p)) continue;
        if (!povPlausible(p)) continue;
        bool stable = true;
        Vec3f prev = p.loc;
        for (int s = 0; s < 8 && stable; ++s)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            PovInfo q{};
            if (!readPovTearSafe(mem, pcm + o, q)) { stable = false; break; }
            if (memcmp(&q.fov, &p.fov, 4) != 0) stable = false;
            else
            {
                const float dx = q.loc.x - prev.x, dy = q.loc.y - prev.y, dz = q.loc.z - prev.z;
                if (sqrtf(dx * dx + dy * dy + dz * dz) > 1500.0f) stable = false;
                prev = q.loc;
            }
        }
        if (!stable) continue;
        found = true;
        povOff = o;
        pov = p;
    }
    if (!found) { report = "no stable POV struct"; return false; }

    off.povOffset = povOff;

    FILE* f = nullptr;
    fopen_s(&f, ".\\work\\findcam.json", "w");
    if (f)
    {
        fprintf(f, "{ \"playerControllerOffset\": \"0x%X\", \"pcmOffset\": \"0x%X\", \"povOffset\": \"0x%X\" }\n",
                off.playerControllerOffset, off.pcmOffset, off.povOffset);
        fclose(f);
    }
    sprintf_s(buf, "ctrl=+0x%X pcm=+0x%X pov=+0x%X loc=(%.0f,%.0f,%.0f) fov=%.1f",
              off.playerControllerOffset, off.pcmOffset, off.povOffset,
              pov.loc.x, pov.loc.y, pov.loc.z, pov.fov);
    report = buf;
    return true;
}

// ---------------------------------------------------------------------------
// Fallback: scored, route-locked matrix scan with process-of-elimination
// ---------------------------------------------------------------------------
struct Candidate
{
    const char* root = "";
    int rootIdx = 0;
    uint32_t off = 0;
    bool resOk = false;
    int w = 0, h = 0;
    uint64_t p = 0;
    float m[16] = {};
    int score = 0;
    bool active = false;
};

static std::vector<Candidate> g_cands;

void cameraDebugDumpCandidates()
{
    if (g_cands.empty())
    {
        printf("  (no matrix-like candidates found near any root)\n");
        return;
    }
    for (size_t i = 0; i < g_cands.size(); ++i)
    {
        const Candidate& c = g_cands[i];
        printf("  cand %-12s +0x%-5X res=%s score=%d active=%d", c.root, c.off, c.resOk ? "yes" : "no", c.score, c.active ? 1 : 0);
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

// v34 activity table: last-seen matrix per (root,offset). A group whose matrix
// never changes is a dead/unused viewport -> eliminated in favour of live ones.
struct MatHist { int rootIdx; uint32_t off; float m[16]; bool valid; };
static MatHist g_hist[24];
static int g_histN = 0;

static MatHist* histGet(int rootIdx, uint32_t off)
{
    for (int i = 0; i < g_histN; ++i)
        if (g_hist[i].rootIdx == rootIdx && g_hist[i].off == off) return &g_hist[i];
    if (g_histN >= 24) return nullptr;
    MatHist* h = &g_hist[g_histN++];
    h->rootIdx = rootIdx; h->off = off; h->valid = false;
    return h;
}

static void collect(const WinMemory& mem, const OffsetProfile& off, const char* root,
                    int rootIdx, uint64_t base, uint32_t lo, uint32_t hi, std::vector<Candidate>& out)
{
    for (uint32_t o = lo; o <= hi; o += 8)
    {
        uint64_t p = 0;
        if (!readPtr(mem, base + o, p)) continue;
        float m[16];
        if (!readMatrixTearSafe(mem, p + off.viewMatrixOffset, m)) continue;
        Candidate c{};
        c.root = root; c.rootIdx = rootIdx; c.off = o; c.p = p;
        memcpy(c.m, m, sizeof(m));
        c.resOk = resFor(mem, off, p, c.w, c.h);
        if (!c.resOk) continue;
        if (c.w < 320 || c.h < 240) continue;
        MatHist* h = histGet(rootIdx, o);
        if (h)
        {
            c.active = !h->valid || memcmp(h->m, m, sizeof(m)) != 0;
            memcpy(h->m, m, sizeof(m));
            h->valid = true;
        }
        if (g_cands.size() < 12) g_cands.push_back(c);
        out.push_back(c);
    }
}

enum class VpRoute { None, LocalPlayer, GameInstance, Engine };
static VpRoute g_route = VpRoute::None;
static uint32_t g_routeOffset = 0;
static int g_switchVotes = 0;
static int g_lockedReadFail = 0;
static int g_lockedIdle = 0;
static float g_lockedMat[16];
static bool g_lockedMatValid = false;

void cameraResetRoute()
{
    g_route = VpRoute::None;
    g_routeOffset = 0;
    g_switchVotes = 0;
    g_lockedReadFail = 0;
    g_lockedIdle = 0;
    g_lockedMatValid = false;
}

static int routeRootIdx(VpRoute r) { return r == VpRoute::LocalPlayer ? 0 : (r == VpRoute::GameInstance ? 1 : 2); }

static uint64_t routeSource(VpRoute r, uint64_t lp, uint64_t gi, uint64_t eng)
{
    return r == VpRoute::LocalPlayer ? lp : (r == VpRoute::GameInstance ? gi : eng);
}

static int scoreCandidate(const Candidate& c, const float* anchor, bool anchorValid,
                          int fbW, int fbH)
{
    int s = 0;
    if (anchorValid && anchorDist(c, anchor) <= kPawnAnchorMaxCm) s += 100;
    if (c.resOk && c.w >= 320 && c.h >= 240 &&
        abs(c.w - fbW) < 96 && abs(c.h - fbH) < 96) s += 50;
    if (yawOnlyMatrix(c.m)) s += 25;
    if (c.active) s += 50;                 // v34: live groups outrank frozen ones
    return s;
}

static bool resolveViewportClient(
    const WinMemory& mem, const OffsetProfile& offsets,
    uint64_t localPlayer, uint64_t gameInstance, uint64_t engine,
    const float* anchor, bool anchorValid, int fbW, int fbH, Candidate& out)
{
    std::vector<Candidate> cands;
    collect(mem, offsets, "localPlayer", 0, localPlayer, 0x28, 0x4000, cands);
    if (cands.empty())
        collect(mem, offsets, "gameInstance", 1, gameInstance, 0x28, 0x10000, cands);
    if (cands.empty())
        collect(mem, offsets, "engine", 2, engine, 0x28, 0x8000, cands);
    if (cands.empty())
    {
        cameraResetRoute();
        return false;
    }

    for (size_t i = 0; i < cands.size(); ++i)
        cands[i].score = scoreCandidate(cands[i], anchor, anchorValid, fbW, fbH);

    size_t bestI = 0;
    for (size_t i = 1; i < cands.size(); ++i)
    {
        if (cands[i].score > cands[bestI].score) bestI = i;
        else if (cands[i].score == cands[bestI].score && anchorValid &&
                 anchorDist(cands[i], anchor) < anchorDist(cands[bestI], anchor)) bestI = i;
    }
    Candidate& bestNew = cands[bestI];
    bool anyActiveRival = false;
    for (size_t i = 0; i < cands.size(); ++i)
        if (cands[i].active && (int)i != (int)bestI) anyActiveRival = true;
    if (bestNew.active) anyActiveRival = true;

    if (g_route != VpRoute::None)
    {
        const uint64_t src = routeSource(g_route, localPlayer, gameInstance, engine);
        uint64_t p = 0;
        Candidate cur{};
        bool curOk = false;
        if (readPtr(mem, src + g_routeOffset, p))
        {
            float m[16];
            if (readMatrixTearSafe(mem, p + offsets.viewMatrixOffset, m))
            {
                cur.root = "cache"; cur.rootIdx = routeRootIdx(g_route);
                cur.off = g_routeOffset; cur.p = p;
                memcpy(cur.m, m, sizeof(m));
                cur.resOk = resFor(mem, offsets, p, cur.w, cur.h);
                MatHist* h = histGet(cur.rootIdx, cur.off);
                cur.active = !h->valid || memcmp(h->m, m, sizeof(m)) != 0;
                if (h) { memcpy(h->m, m, sizeof(m)); h->valid = true; }
                cur.score = scoreCandidate(cur, anchor, anchorValid, fbW, fbH);
                g_lockedReadFail = 0;
                g_lockedIdle = (g_lockedMatValid && memcmp(g_lockedMat, m, sizeof(m)) == 0)
                               ? g_lockedIdle + 1 : 0;
                memcpy(g_lockedMat, m, sizeof(m));
                g_lockedMatValid = true;
                curOk = true;
            }
        }
        if (!curOk)
        {
            // v34: torn/failed read of the lock => reuse last-good matrix for up
            // to 30 ticks instead of falling through and re-locking randomly.
            ++g_lockedReadFail;
            if (g_lockedReadFail <= 30 && g_lockedMatValid)
            {
                cur.root = "cache"; cur.rootIdx = routeRootIdx(g_route);
                cur.off = g_routeOffset; cur.p = p;
                memcpy(cur.m, g_lockedMat, sizeof(cur.m));
                cur.resOk = true; cur.w = fbW; cur.h = fbH;
                cur.active = false;
                cur.score = scoreCandidate(cur, anchor, anchorValid, fbW, fbH);
                curOk = true;
            }
        }

        if (curOk)
        {
            const bool dead = (g_lockedIdle > 300 && anyActiveRival);  // elimination
            const bool healthy = !dead &&
                ((cur.score >= 100) || (!anchorValid && cur.score >= 25));
            if (healthy)
            {
                if (bestNew.score > cur.score + 50)
                {
                    if (++g_switchVotes >= 30) { g_switchVotes = 0; /* adopt bestNew */ }
                    else { out = cur; return true; }
                }
                else { g_switchVotes = 0; out = cur; return true; }
            }
            else
            {
                if (++g_switchVotes >= 10) { g_switchVotes = 0; /* adopt bestNew */ }
                else { out = cur; return true; }
            }
        }
        else if (g_lockedMatValid && g_lockedReadFail <= 30)
        {
            Candidate stale{};
            stale.root = "cache"; stale.rootIdx = routeRootIdx(g_route);
            stale.off = g_routeOffset;
            memcpy(stale.m, g_lockedMat, sizeof(stale.m));
            stale.resOk = true; stale.w = fbW; stale.h = fbH; stale.active = false;
            stale.score = scoreCandidate(stale, anchor, anchorValid, fbW, fbH);
            out = stale;
            return true;
        }
        else
        {
            if (++g_switchVotes >= 10) { g_switchVotes = 0; /* adopt bestNew */ }
            else if (g_lockedMatValid)
            {
                Candidate stale{};
                stale.root = "cache"; stale.rootIdx = routeRootIdx(g_route);
                stale.off = g_routeOffset;
                memcpy(stale.m, g_lockedMat, sizeof(stale.m));
                stale.resOk = true; stale.w = fbW; stale.h = fbH; stale.active = false;
                stale.score = scoreCandidate(stale, anchor, anchorValid, fbW, fbH);
                out = stale;
                return true;
            }
        }
    }

    g_route = strcmp(bestNew.root, "localPlayer") == 0 ? VpRoute::LocalPlayer :
              strcmp(bestNew.root, "gameInstance") == 0 ? VpRoute::GameInstance :
              VpRoute::Engine;
    g_routeOffset = bestNew.off;
    g_switchVotes = 0;
    g_lockedReadFail = 0;
    g_lockedIdle = 0;
    memcpy(g_lockedMat, bestNew.m, sizeof(g_lockedMat));
    g_lockedMatValid = true;
    if (g_verbose)
        printf("    LOCKED viewportClient at %s+0x%X score=%d active=%d\n",
               bestNew.root, bestNew.off, bestNew.score, bestNew.active ? 1 : 0);
    out = bestNew;
    return true;
}

static bool tryChain(
    const WinMemory& mem, const RuntimeRoots& roots, const OffsetProfile& offsets,
    uint32_t giOff, uint32_t lpOff, int fallbackW, int fallbackH,
    const float* anchor, bool anchorValid, CameraState& out)
{
    uint64_t engine = 0;
    if (!readPtr(mem, roots.engineGlobalAddress, engine)) return false;
    uint64_t gameInstance = 0;
    if (!readPtr(mem, engine + giOff, gameInstance)) return false;
    const uint64_t lpArray = gameInstance + lpOff;
    uint64_t lpData = 0;
    if (!readPtr(mem, lpArray, lpData)) return false;
    int32_t lpCount = 0;
    if (!mem.read(lpArray + 8, lpCount) || lpCount <= 0 || lpCount > 4) return false;
    uint64_t localPlayer = 0;
    if (!readPtr(mem, lpData, localPlayer)) return false;

    Candidate cand;
    if (!resolveViewportClient(mem, offsets, localPlayer, gameInstance, engine,
                               anchor, anchorValid, fallbackW, fallbackH, cand))
        return false;

    int32_t w = 0, h = 0;
    if (cand.resOk) { w = cand.w; h = cand.h; }
    else if (fallbackW >= 16 && fallbackH >= 16) { w = fallbackW; h = fallbackH; }
    else return false;

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
        out.fovDegrees = 2.0f * atanf(1.0f / s0) * 180.0f / kPi;
    else
        out.fovDegrees = 90.0f;
    return true;
}

bool readCameraState(
    const WinMemory& mem, const RuntimeRoots& roots, const OffsetProfile& offsets,
    CameraState& out, int fallbackW, int fallbackH, const float* anchor)
{
    out = {};

    if (offsets.pcmOffset && offsets.povOffset && offsets.playerControllerOffset)
    {
        uint64_t lp = 0, ctrl = 0, pcm = 0;
        if (readLocalPlayer(mem, roots, offsets, lp) &&
            readPtr(mem, lp + offsets.playerControllerOffset, ctrl) &&
            readPtr(mem, ctrl + offsets.pcmOffset, pcm))
        {
            PovInfo pov{};
            if (readPovTearSafe(mem, pcm + offsets.povOffset, pov) && povPlausible(pov))
            {
                buildCameraFromPov(pov, fallbackW, fallbackH, out);
                return true;
            }
        }
    }

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