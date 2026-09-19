#include "camera.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>
#include <deque>

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

static float g_accM[16] = {};
static bool  g_accValid = false;
static bool  g_accWasValid = false;
static int   g_accFreeze = 0;
static CameraState g_accCam{};
static bool  g_accCamValid = false;

static float   g_vcM[16] = {};
static bool    g_vcHeld = false;
static int     g_vcHoldLeft = 0;
static uint64_t g_vcBase = 0;
static uint32_t g_vcOff = 0;
static int     g_vcFail = 0;

void cameraResetRoute()
{
    g_route = VpRoute::None;
    g_accValid = false; g_accWasValid = false; g_accFreeze = 0; g_accCamValid = false; g_vcBase = 0; g_vcHeld = false; g_vcHoldLeft = 0;
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
    
    // v35 elimination: reject impossible FOVs (Main cam is usually 70-110 degrees)
    const float s0 = sqrtf(c.m[0] * c.m[0] + c.m[1] * c.m[1] + c.m[2] * c.m[2]);
    float fov = 90.0f;
    if (s0 > 0.05f && s0 < 64.0f)
        fov = 2.0f * atanf(1.0f / s0) * 180.0f / kPi;
    if (fov < 50.0f || fov > 130.0f) return -1000; // Eliminate fake matrices

    if (anchorValid && anchorDist(c, anchor) <= kPawnAnchorMaxCm) s += 100;
    
    // v35 elimination: Strict Aspect Ratio match (much stronger signal than size proximity)
    if (c.resOk && c.w >= 320 && c.h >= 240)
    {
        float aspectC = (float)c.w / (float)c.h;
        float aspectW = (float)fbW / (float)fbH;
        if (fabsf(aspectC - aspectW) < 0.05f) s += 100;
        else if (abs(c.w - fbW) < 96 && abs(c.h - fbH) < 96) s += 50;
    }
    
    // v35 elimination: yaw-only matrices are typically 2D UI or shadow casters -> penalize them
    if (yawOnlyMatrix(c.m)) s -= 50;
    
    // v35 elimination: DO NOT give a massive bonus for being active.
    // A stationary main camera must not lose to a moving background camera.
    if (c.active) s += 5;
    
    return s;
}

// === V37-REGISTRY-START ===
void cameraResetRoute();
struct CamGroup
{
    uint64_t p = 0;
    int      rootIdx = -1;
    uint32_t off = 0;
    int      groupId = 0;
    float    m[16] = {};
    float    fov = 90.0f;
    bool     fovLocked = false;
    int      w = 0, h = 0;
    bool     resOk = false;
    uint64_t lastSeen = 0;
    int      seen = 0;
    int      chosen = 0;
};
static std::vector<CamGroup> g_groups;
static int      g_nextGroupId = 1;
static int      g_forcedGroup = 0;
static uint64_t g_groupTick  = 0;
static int      g_lastFbW = 0, g_lastFbH = 0;
static std::deque<float> g_selFovTrail;
static uint64_t g_lastDiagWrite = 0;

static float fovFromMatrix(const float m[16])
{
    const float s0 = sqrtf(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
    if (s0 > 0.05f && s0 < 64.0f)
        return 2.0f * atanf(1.0f / s0) * 180.0f / kPi;
    return 90.0f;
}
static CamGroup* findGroup(uint64_t p)
{
    for (size_t i = 0; i < g_groups.size(); ++i)
        if (g_groups[i].p == p) return &g_groups[i];
    return nullptr;
}
static void writeCamDiag(bool force)
{
    if (!force && g_groupTick - g_lastDiagWrite < 15) return;
    g_lastDiagWrite = g_groupTick;
    FILE* f = nullptr;
    fopen_s(&f, ".\\work\\camdiag.txt", "w");
    if (!f) return;
    fprintf(f, "# camdiag tick=%llu forcedGroupId=G%d window=%dx%d liveGroups=%d\n",
            (unsigned long long)g_groupTick, g_forcedGroup, g_lastFbW, g_lastFbH,
            (int)g_groups.size());
    fprintf(f, "# v39 direct: base=0x%llX off=0x%X fail=%d held=%d\n",
            (unsigned long long)g_vcBase, g_vcOff, g_vcFail, g_vcHeld ? 1 : 0);
    fprintf(f, "# id  ptr              root off      res      fov   seen  age  pos                       chosen\n");
    for (size_t i = 0; i < g_groups.size(); ++i)
    {
        const CamGroup& g = g_groups[i];
        const char* rn = (g.rootIdx==0)?"LP":(g.rootIdx==1?"GI":"ENG");
        fprintf(f, "G%-3d %016llX %-3s +0x%-6X %4dx%-4d %5.1f %5d %4llu (%.0f,%.0f,%.0f) %5d%s\n",
                g.groupId, (unsigned long long)g.p, rn, g.off, g.w, g.h, g.fov,
                g.seen, (unsigned long long)(g_groupTick - g.lastSeen),
                g.m[12], g.m[13], g.m[14], g.chosen,
                (g.groupId==g_forcedGroup)?"  <== SELECTED":"");
    }
    fprintf(f, "# selected-group raw fov trail (flashing = big jumps):");
    for (size_t i = 0; i < g_selFovTrail.size(); ++i) fprintf(f, " %.1f", g_selFovTrail[i]);
    fprintf(f, "\n");
    fclose(f);
}
void cameraForceNextCandidate()
{
    int n = (int)g_groups.size();
    if (n <= 0) return;
    int pos = -1;
    for (int i = 0; i < n; ++i) if (g_groups[i].groupId == g_forcedGroup) { pos = i; break; }
    pos = (pos + 1) % n;
    g_forcedGroup = g_groups[pos].groupId;
    cameraResetRoute();
    printf("[info] Camera selector: group %d/%d (stable id G%d)\n", pos + 1, n, g_forcedGroup);
}
void cameraResetCandidateForce()
{
    if (g_forcedGroup != 0) printf("[info] Camera selector: reset to auto\n");
    g_forcedGroup = 0;
    cameraResetRoute();
}
int cameraGetForcedCandidate()
{
    for (int i = 0; i < (int)g_groups.size(); ++i)
        if (g_groups[i].groupId == g_forcedGroup) return i + 1;
    return 0;
}
int cameraGetCandidateCount() { return (int)g_groups.size(); }
// === V37-REGISTRY-END ===


// === V38-START ===

bool camFrustumOK(const Vec3f& w, float extraDeg)
{
    if (!g_accCamValid) return true;
    float dx = w.x - g_accCam.cameraPos.x;
    float dy = w.y - g_accCam.cameraPos.y;
    float dz = w.z - g_accCam.cameraPos.z;
    float L  = sqrtf(dx*dx + dy*dy + dz*dz);
    if (L < 1.0f) return true;
    float c = (dx*g_accCam.forward.x + dy*g_accCam.forward.y + dz*g_accCam.forward.z) / L;
    float half = g_accCam.fovDegrees * 0.5f + extraDeg;
    if (half > 89.0f) half = 89.0f;
    return c >= cosf(half * kPi / 180.0f);
}

static bool orientSaneCam(const float* m)
{
    return m[10] > 0.25f && fabsf(m[6]) < 0.50f;   // up.z and right.z
}
// === V38-END ===

// === V39-START ===

static bool vcRead(const WinMemory& mem, const OffsetProfile& off, uint64_t base, uint32_t vo,
                   int fbW, int fbH, const float* anchor, bool anchorValid, Candidate& c)
{
    if (!base || !vo) return false;
    uint64_t p = 0;
    if (!readPtr(mem, base + vo, p)) return false;
    float m[16];
    if (!readMatrixTearSafe(mem, p + off.viewMatrixOffset, m)) return false;
    c = Candidate{};
    c.root = "direct"; c.rootIdx = 9; c.off = vo; c.p = p;
    memcpy(c.m, m, sizeof(m));
    c.resOk = resFor(mem, off, p, c.w, c.h);
    if (!c.resOk || c.w < 320 || c.h < 240) return false;
    if (fabsf((float)c.w / (float)c.h - (float)fbW / (float)fbH) > 0.06f) return false;
    if (!(m[10] > 0.25f) || fabsf(m[6]) > 0.50f) return false;
    if (anchorValid)
    {
        float d = fabsf(m[12]-anchor[0]) + fabsf(m[13]-anchor[1]) + fabsf(m[14]-anchor[2]);
        if (d > 2500.0f) return false;
    }
    return true;
}

static bool directVcTick(const WinMemory& mem, const OffsetProfile& off,
                         uint64_t lp, uint64_t gi, uint64_t eng,
                         const float* anchor, bool anchorValid, int fbW, int fbH,
                         Candidate& out)
{
    if (g_vcBase)
    {
        Candidate c{};
        if (vcRead(mem, off, g_vcBase, g_vcOff, fbW, fbH, anchor, anchorValid, c))
        {
            g_vcFail = 0;
            memcpy(g_vcM, c.m, sizeof(g_vcM));
            g_vcHeld = true; g_vcHoldLeft = 45;
            out = c;
            return true;
        }
        if (++g_vcFail < 45) return false;      // caller holds last good matrix
        g_vcBase = 0; g_vcFail = 0;             // route died -> re-probe
    }
    const uint64_t bases[3] = { lp, gi, eng };
    const uint32_t vos[4] = { off.viewportClientOffset, 0x1e0u, 0x208u, 0x78u };
    for (int bi = 0; bi < 3; ++bi)
        for (int vi = 0; vi < 4; ++vi)
        {
            Candidate c{};
            if (vcRead(mem, off, bases[bi], vos[vi], fbW, fbH, anchor, anchorValid, c))
            {
                g_vcBase = bases[bi]; g_vcOff = vos[vi]; g_vcFail = 0;
                memcpy(g_vcM, c.m, sizeof(g_vcM));
                g_vcHeld = true; g_vcHoldLeft = 45;
                printf("[info] v39 direct viewport lock: base%d+0x%X\n", bi, vos[vi]);
                out = c;
                return true;
            }
        }
    return false;
}
// === V39-END ===

static bool resolveViewportClient(
    const WinMemory& mem, const OffsetProfile& offsets,
    uint64_t localPlayer, uint64_t gameInstance, uint64_t engine,
    const float* anchor, bool anchorValid, int fbW, int fbH, Candidate& out)
{
    // ---- v39: deterministic direct viewport-client read (bypasses the scan) ----
    {
        Candidate dc{};
        if (directVcTick(mem, offsets, localPlayer, gameInstance, engine,
                         anchor, anchorValid, fbW, fbH, dc))
        {
            ++g_groupTick;
            out = dc;
            writeCamDiag(false);
            return true;
        }
        if (g_vcHeld && g_vcHoldLeft > 0)
        {
            --g_vcHoldLeft;
            Candidate hold{};
            hold.root = "vchold"; hold.rootIdx = 9; hold.off = g_vcOff; hold.p = 0;
            memcpy(hold.m, g_vcM, sizeof(hold.m));
            hold.resOk = true; hold.w = fbW; hold.h = fbH; hold.score = 999;
            out = hold;
            writeCamDiag(false);
            return true;
        }
    }
    std::vector<Candidate> cands;
    collect(mem, offsets, "localPlayer", 0, localPlayer, 0x28, 0x4000, cands);
    if (cands.empty())
        collect(mem, offsets, "gameInstance", 1, gameInstance, 0x28, 0x10000, cands);
    if (cands.empty())
        collect(mem, offsets, "engine", 2, engine, 0x28, 0x8000, cands);

    ++g_groupTick;
    g_lastFbW = fbW; g_lastFbH = fbH;

    if (cands.empty()) { cameraResetRoute(); writeCamDiag(false); return false; }

    // registry = DIAGNOSTIC ONLY now (selection uses continuity, not pointers)
    for (size_t i = 0; i < cands.size(); ++i)
    {
        const Candidate& c = cands[i];
        CamGroup* g = findGroup(c.p);
        if (!g)
        {
            if (g_groups.size() >= 400) continue;
            g_groups.push_back(CamGroup());
            g = &g_groups.back();
            g->p = c.p; g->groupId = g_nextGroupId++;
        }
        g->rootIdx = c.rootIdx; g->off = c.off;
        memcpy(g->m, c.m, sizeof(g->m));
        g->resOk = c.resOk; g->w = c.w; g->h = c.h;
        g->lastSeen = g_groupTick; g->seen++;
        float f = fovFromMatrix(c.m);
        if (!g->fovLocked && f > 30.0f && f < 160.0f) { g->fov = f; g->fovLocked = true; }
        else if (g->fovLocked && fabsf(f - g->fov) <= 15.0f) g->fov = g->fov*0.8f + f*0.2f;
    }
    for (int i = (int)g_groups.size() - 1; i >= 0; --i)
        if (g_groupTick - g_groups[i].lastSeen > 240)
            g_groups.erase(g_groups.begin() + i);

    // ---- v38 CONTINUITY LOCK ----
    // 1) TRACK: accept only a candidate that moved smoothly from last accepted matrix.
    //    Old ring-buffer snapshots jump meters between frames -> rejected here.
    if (g_accValid)
    {
        int best = -1; float bestCost = 1e30f;
        for (size_t i = 0; i < cands.size(); ++i)
        {
            const float* m = cands[i].m;
            float dt = fabsf(m[12]-g_accM[12]) + fabsf(m[13]-g_accM[13]) + fabsf(m[14]-g_accM[14]);
            if (dt > 300.0f) continue;                       // >3m in one tick = snapshot, not camera
            if (anchorValid)
            {
                float da = fabsf(m[12]-anchor[0]) + fabsf(m[13]-anchor[1]) + fabsf(m[14]-anchor[2]);
                if (da > 2500.0f) continue;                  // locked onto stale snapshot -> drop
            }
            float d0 = m[0]*g_accM[0]+m[1]*g_accM[1]+m[2]*g_accM[2];
            float d1 = m[4]*g_accM[4]+m[5]*g_accM[5]+m[6]*g_accM[6];
            float d2 = m[8]*g_accM[8]+m[9]*g_accM[9]+m[10]*g_accM[10];
            float md = d0 < d1 ? d0 : d1; if (d2 < md) md = d2;
            if (md < 0.985f) continue;                       // rotation snapped = not our camera
            float cost = dt + 300.0f*(1.0f-md);
            if (cost < bestCost) { bestCost = cost; best = (int)i; }
        }
        if (best >= 0)
        {
            memcpy(g_accM, cands[best].m, sizeof(g_accM));
            g_accFreeze = 0;
            out = cands[best];
            writeCamDiag(false);
            return true;
        }
        // 2) FREEZE-HOLD: keep last good matrix instead of flipping to a snapshot
        if (g_accFreeze < 120)
        {
            ++g_accFreeze;
            Candidate hold{};
            hold.root = "hold"; hold.rootIdx = -1; hold.off = 0; hold.p = 0;
            memcpy(hold.m, g_accM, sizeof(hold.m));
            hold.resOk = true; hold.w = fbW; hold.h = fbH;
            hold.score = 999; hold.active = false;
            out = hold;
            writeCamDiag(false);
            return true;
        }
        g_accValid = false;   // blind too long -> reacquire below
    }

    // 3) ACQUIRE
    {
        int best = -1; float bestD = 1e30f;
        // 3a) anchor known: candidate glued to your own pawn (<=9m), sane orientation
        if (anchorValid)
        {
            for (size_t i = 0; i < cands.size(); ++i)
            {
                const float* m = cands[i].m;
                if (!orientSaneCam(m)) continue;
                float d = fabsf(m[12]-anchor[0]) + fabsf(m[13]-anchor[1]) + fabsf(m[14]-anchor[2]);
                if (d > 900.0f) continue;
                if (d < bestD) { bestD = d; best = (int)i; }
            }
        }
        // 3b) BOOTSTRAP (own pawn not known yet -> no anchor): scored pick,
        //     same behaviour as the old auto mode, so startup can lock at all.
        if (best < 0)
        {
            int bestScore = 49;
            for (size_t i = 0; i < cands.size(); ++i)
            {
                if (!orientSaneCam(cands[i].m)) continue;
                if (!cands[i].resOk) continue;
                int sc = scoreCandidate(cands[i], anchor, anchorValid, fbW, fbH);
                if (sc > bestScore) { bestScore = sc; best = (int)i; }
            }
        }
        if (best >= 0)
        {
            memcpy(g_accM, cands[best].m, sizeof(g_accM));
            g_accValid = true; g_accWasValid = true; g_accFreeze = 0;
            out = cands[best];
            writeCamDiag(false);
            return true;
        }
    }

    // 4) last resort: hold previous matrix briefly so overlay never flips to garbage
    if (g_accWasValid && g_accFreeze < 600)
    {
        ++g_accFreeze;
        Candidate hold{};
        hold.root = "hold"; memcpy(hold.m, g_accM, sizeof(hold.m));
        hold.resOk = true; hold.w = fbW; hold.h = fbH; hold.score = 999;
        out = hold;
        writeCamDiag(false);
        return true;
    }
    writeCamDiag(false);
    return false;
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
    g_accCam = out; g_accCamValid = true;
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