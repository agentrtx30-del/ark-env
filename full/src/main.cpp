#include "arkoverlay.h"
#include "common.h"
#include "log.h"
#include "memory.h"
#include "offsets.h"
#include "process.h"
#include "camera.h"
#include "actors.h"
#include "targets.h"
#include "projection.h"
#include "selfcheck.h"
#include "nametable.h"
#include "classifier.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cmath>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>
#include <unordered_map>

namespace ao = arkoverlay;

// ---------------------------------------------------------------------------
// Flags
// ---------------------------------------------------------------------------
struct RuntimeFlags
{
    bool demo = false;
    bool selfCheck = false;
    bool dumpOnly = false;
    bool overlayEnabled = true;
    int  dumpFrames = 0;
};

static void parseArgs(int argc, char** argv, RuntimeFlags& f)
{
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--demo"))            f.demo = true;
        else if (!strcmp(argv[i], "--selfcheck"))  f.selfCheck = true;
        else if (!strcmp(argv[i], "--dump-only"))  f.dumpOnly = true;
        else if (!strcmp(argv[i], "--no-overlay")) f.overlayEnabled = false;
        else if (!strncmp(argv[i], "--dump-frames=", 14)) f.dumpFrames = atoi(argv[i] + 14);
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static bool accessSelfTest(const WinMemory& mem, uint64_t b)
{
    uint16_t mz = 0;
    return mem.read(b, mz) && mz == 0x5A4D;
}

static bool updateOverlayRect(HWND w, RECT& last, bool& init)
{
    if (!w || !IsWindow(w)) return false;
    if (IsIconic(w)) { ao::overlayHide(); return false; }
    RECT c{};
    if (!GetClientRect(w, &c)) return false;
    POINT tl{ 0, 0 };
    if (!ClientToScreen(w, &tl)) return false;
    RECT r{ tl.x, tl.y, tl.x + c.right, tl.y + c.bottom };
    if (r.right <= r.left || r.bottom <= r.top) { ao::overlayHide(); return false; }
    if (!init)
    {
        if (!ao::overlayInit(r)) return false;
        last = r;
        init = true;
        return true;
    }
    if (!EqualRect(&r, &last)) { ao::overlayResize(r); last = r; }
    ao::overlayShow();
    return true;
}

static bool plausiblePos(const Vec3f& p)
{
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
    const float ax = fabsf(p.x), ay = fabsf(p.y), az = fabsf(p.z);
    if (ax > 5.0e6f || ay > 5.0e6f || az > 5.0e6f) return false;
    return (ax + ay + az) > 10000.0f;
}

static Vec3f vecNorm(Vec3f v)
{
    const float l = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    if (l < 1e-6f) return Vec3f{ 0.0f, 0.0f, 0.0f };
    return Vec3f{ v.x / l, v.y / l, v.z / l };
}

static Vec3f vecCross(const Vec3f& a, const Vec3f& b)
{
    return Vec3f{ a.y * b.z - a.z * b.y,
                  a.z * b.x - a.x * b.z,
                  a.x * b.y - a.y * b.x };
}

// Validated component position read (discovery / recovery paths).
static bool readComponentPos(const WinMemory& mem, const OffsetProfile& off,
                             uint64_t actor, Vec3f& out, int* srcUsed = nullptr)
{
    const uint32_t compPtrOffs[2] = { off.capsuleOffset, 0x170u };
    for (int k = 0; k < 2; ++k)
    {
        uint64_t c = 0;
        if (!readPtr(mem, actor + compPtrOffs[k], c)) continue;
        float radius = 0.0f;
        if (!mem.read(c + 0x124, radius)) continue;
        if (!std::isfinite(radius) || radius <= 0.0f || radius > 1500.0f) continue;
        Vec3f v{};
        if (!mem.read(c + 0xF0, v)) continue;
        if (!plausiblePos(v)) continue;
        out = v;
        if (srcUsed) *srcUsed = k + 1;
        return true;
    }
    if (srcUsed) *srcUsed = 0;
    return false;
}

static uint64_t validatedComponent(const WinMemory& mem, const OffsetProfile& off, uint64_t actor)
{
    const uint32_t compPtrOffs[2] = { off.capsuleOffset, 0x170u };
    for (int k = 0; k < 2; ++k)
    {
        uint64_t c = 0;
        if (!readPtr(mem, actor + compPtrOffs[k], c)) continue;
        float radius = 0.0f;
        if (!mem.read(c + 0x124, radius)) continue;
        if (!std::isfinite(radius) || radius <= 0.0f || radius > 1500.0f) continue;
        return c;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Component OWNER offset auto-discovery (pooled-component gate).
// ---------------------------------------------------------------------------
static uint32_t g_ownerOff = 0;

static uint32_t discoverOwnerOff(const WinMemory& mem, uint64_t comp, uint64_t actor)
{
    if (g_ownerOff)
    {
        uint64_t o = 0;
        if (readPtr(mem, comp + g_ownerOff, o) && o == actor) return g_ownerOff;
        g_ownerOff = 0;
    }
    for (uint32_t off = 0x18; off <= 0x78; off += 8)
    {
        uint64_t o = 0;
        if (!readPtr(mem, comp + off, o)) continue;
        if (o == actor)
        {
            g_ownerOff = off;
            return off;
        }
    }
    return 0;
}

static bool ownerOk(const WinMemory& mem, uint64_t comp, uint64_t actor, uint32_t ownerOff)
{
    if (!ownerOff || !comp) return true;
    uint64_t o = 0;
    return readPtr(mem, comp + ownerOff, o) && o == actor;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------
static FILE* g_dumpFile = nullptr;
static int   g_dumpFramesLeft = 0;
static uint64_t g_gridDumped[4] = { 0, 0, 0, 0 };
static int   g_gridCount = 0;
static FILE* g_evtFile = nullptr;
static uint64_t g_frame = 0;

static void dumpOpen()
{
    if (!g_dumpFile)
    {
        fopen_s(&g_dumpFile, ".\\work\\debug_frames.txt", "w");
        if (g_dumpFile) logInfo("debug dump -> work\\debug_frames.txt");
    }
}
static void dumpClose()
{
    if (g_dumpFile) { fclose(g_dumpFile); g_dumpFile = nullptr; }
}
static void evtOpen()
{
    if (!g_evtFile) fopen_s(&g_evtFile, ".\\work\\debug_events.txt", "w");
}
static void evtClose()
{
    if (g_evtFile) { fclose(g_evtFile); g_evtFile = nullptr; }
}
static void evt(const char* kind, uint64_t ptr, int extra)
{
    if (!g_evtFile) return;
    fprintf(g_evtFile, "E f=%llu %s ptr=%016llX n=%d\n",
            (unsigned long long)g_frame, kind,
            (unsigned long long)ptr, extra);
    fflush(g_evtFile);
}

// ---------------------------------------------------------------------------
// Lock-on tracker
// ---------------------------------------------------------------------------
struct Tracked
{
    uint64_t ptr = 0;
    uint64_t classPtr = 0;
    uint64_t compPtr = 0;
    uint32_t ownerOff = 0;
    ClassInfo cls;
    bool hasCls = false;

    enum class State { Pending, Locked, Dying };
    State state = State::Pending;
    bool removeMe = false;

    int confirm = 0;
    int blind = 0;
    int dying = 0;
    int jumpVotes = 0;
    int pinFail = 0;
    int refresh = 0;
    int pooled = 0;
    bool capsOk = false;

    bool havePos = false;
    Vec3f meas;                      // capsule CENTER (authoritative)
    Vec3f draw;                      // smoothed CENTER used for rendering
    Vec3f jumpCand;

    float radius = 50.0f;
    float halfHeight = 100.0f;
    float health = 0.0f;
    float maxHealth = 0.0f;
    int level = -1;
};

static const float kAcceptStepCm   = 500.0f;
static const int   kConfirmNeed    = 3;
static const int   kBlindPending   = 20;
static const int   kBlindLocked    = 30;
static const int   kDyingFrames    = 15;
static const int   kPinFailNeed    = 10;
static const int   kJumpVotesNeed  = 10;
static const float kLerp           = 0.40f;

static void absorb(Tracked& t, const Vec3f& p)
{
    if (!t.havePos)
    {
        t.meas = p;
        t.draw = p;
        t.havePos = true;
        t.confirm = 1;
        return;
    }
    const float d = distanceCm(t.meas, p);
    if (d < kAcceptStepCm)
    {
        t.meas = p;
        t.jumpVotes = 0;
        if (t.confirm < kConfirmNeed) ++t.confirm;
    }
    else if (t.jumpVotes > 0 && distanceCm(t.jumpCand, p) < 500.0f)
    {
        if (++t.jumpVotes >= kJumpVotesNeed)
        {
            t.meas = p;
            t.jumpVotes = 0;
            t.confirm = 1;
        }
    }
    else
    {
        t.jumpCand = p;
        t.jumpVotes = 1;
    }
}

static void updateTarget(const WinMemory& mem, const OffsetProfile& off, Tracked& t)
{
    // ---- identity pin, double-read tear protection ----
    uint64_t cp = 0;
    bool pinOk = readPtr(mem, t.ptr + 0x10, cp) && cp == t.classPtr;
    if (!pinOk)
    {
        uint64_t cp2 = 0;
        if (readPtr(mem, t.ptr + 0x10, cp2) && cp2 == t.classPtr) pinOk = true;
    }
    if (!pinOk)
    {
        ++t.blind;
        if (++t.pinFail >= kPinFailNeed)
        {
            t.removeMe = true;
            evt("REMOVE_PIN", t.ptr, t.pinFail);
        }
        return;
    }
    t.pinFail = 0;

    // ---- OWNERSHIP gate: pooled/recycled component? ----
    bool owned = ownerOk(mem, t.compPtr, t.ptr, t.ownerOff);
    if (!owned)
    {
        ++t.pooled;
        const uint64_t c2 = validatedComponent(mem, off, t.ptr);
        if (c2)
        {
            if (!t.ownerOff) t.ownerOff = discoverOwnerOff(mem, c2, t.ptr);
            if (ownerOk(mem, c2, t.ptr, t.ownerOff))
            {
                t.compPtr = c2;
                owned = true;
                evt("RECOMP", t.ptr, t.pooled);
            }
        }
        if (!owned)
        {
            ++t.blind;
            if (t.blind > kBlindLocked && t.state == Tracked::State::Locked)
            {
                t.state = Tracked::State::Dying;
                t.dying = 0;
                evt("DYING", t.ptr, t.blind);
            }
            return;
        }
    }

    // ---- position from owned component (capsule CENTER) ----
    Vec3f pos{};
    bool posOk = false;
    if (t.compPtr)
    {
        Vec3f v{};
        if (mem.read(t.compPtr + 0xF0, v) && plausiblePos(v))
        {
            pos = v;
            posOk = true;
        }
    }
    if (!posOk)
    {
        int src = 0;
        if (readComponentPos(mem, off, t.ptr, pos, &src))
        {
            t.compPtr = validatedComponent(mem, off, t.ptr);
            if (!t.ownerOff && t.compPtr) t.ownerOff = discoverOwnerOff(mem, t.compPtr, t.ptr);
            posOk = true;
        }
    }

    // ---- slow refresh: re-validate component + sizes ----
    if (++t.refresh >= 30)
    {
        t.refresh = 0;
        const uint64_t c = validatedComponent(mem, off, t.ptr);
        if (c && ownerOk(mem, c, t.ptr, t.ownerOff))
        {
            t.compPtr = c;
            float r = 0.0f, hh = 0.0f;
            if (mem.read(c + 0x124, r) && mem.read(c + 0x12c, hh) &&
                std::isfinite(r) && r > 0.0f && r < 1500.0f &&
                std::isfinite(hh) && hh > 0.0f && hh < 3000.0f)
            {
                t.radius = r;
                t.halfHeight = hh;
                t.capsOk = true;
            }
        }
    }

    // ---- auxiliary fields ----
    LogicalTarget lt;
    const bool fieldsOk = readLogicalTarget(mem, off, t.ptr, lt);
    if (fieldsOk)
    {
        if (lt.capsuleOk)
        {
            t.radius = lt.capsuleRadius;
            t.halfHeight = lt.halfHeight;
            t.capsOk = true;
        }
        if (lt.maxHealth > 0.0f)
        {
            t.health = lt.health;
            t.maxHealth = lt.maxHealth;
        }
        if (lt.level >= 0)
            t.level = lt.level;
    }

    if (posOk || fieldsOk) t.blind = 0;
    else ++t.blind;

    if (posOk) absorb(t, pos);

    const Tracked::State prev = t.state;
    switch (t.state)
    {
    case Tracked::State::Pending:
        if (t.havePos && t.confirm >= kConfirmNeed)
            t.state = Tracked::State::Locked;
        else if (t.blind > kBlindPending)
        {
            t.removeMe = true;
            evt("REMOVE_BLIND_PENDING", t.ptr, t.blind);
        }
        break;
    case Tracked::State::Locked:
        if (t.blind > kBlindLocked)
        {
            t.state = Tracked::State::Dying;
            t.dying = 0;
        }
        break;
    case Tracked::State::Dying:
        if (t.blind == 0)
            t.state = Tracked::State::Locked;
        else if (++t.dying > kDyingFrames)
        {
            t.removeMe = true;
            evt("REMOVE_DYING", t.ptr, t.dying);
        }
        break;
    }
    if (prev != t.state)
    {
        if (t.state == Tracked::State::Locked) evt("LOCK", t.ptr, t.confirm);
        if (t.state == Tracked::State::Dying)  evt("DYING", t.ptr, t.blind);
    }
    if (t.removeMe && prev == Tracked::State::Locked)
        evt("REMOVE_LOCKED", t.ptr, t.blind);

    if (t.state != Tracked::State::Pending && t.havePos)
    {
        t.draw.x += (t.meas.x - t.draw.x) * kLerp;
        t.draw.y += (t.meas.y - t.draw.y) * kLerp;
        t.draw.z += (t.meas.z - t.draw.z) * kLerp;
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    RuntimeFlags flags;
    parseArgs(argc, argv, flags);
    if (flags.demo)
        return ao::overlayDemo(nullptr) ? 0 : 1;

    logInit(L".\\work\\ark-full.log");
    logInfo("ark-full starting (lock-on tracker v11: geometric basis + center anchor)");
    logInfo("hotkeys: F8 = toggle overlay, F9 = debug dump 300 frames, L = exit");

    OffsetProfile offsets = makeDefaultOffsetProfile();
    if (!offsets.valid) { logError("offset profile invalid"); return 1; }

    std::unordered_map<uint64_t, std::wstring> idmap;
    int n = loadNameTable(L".\\work\\idmap.tsv", idmap);
    if (n <= 0) n = loadNameTable(L".\\work\\idmap.txt", idmap);
    bool namesOk = n > 1000;
    logInfo("name table: %d entries (%s)", n, namesOk ? "ok" : "MISSING");

    ao::Config cfg;
    ao::loadConfig(L".\\overlay.ini", cfg);
    bool running = true;

    while (running)
    {
        DWORD pid = 0;
        if (!findProcessByName(offsets.gameModule.c_str(), pid))
        {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        logInfo("found ARK pid=%lu", pid);

        WinMemory mem;
        if (!mem.open(pid))
        {
            logError("OpenProcess failed, error=%lu", mem.lastOpenError());
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        uint64_t base = 0;
        if (!findModuleBase(mem.nativeHandle(), offsets.gameModule.c_str(), base))
        {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        if (!accessSelfTest(mem, base))
        {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        logInfo("attach success: base=0x%016llX", base);

        RuntimeRoots roots;
        roots.shooterBase = base;
        roots.gworldAddress = base + offsets.gworldRva;
        roots.engineGlobalAddress = base + offsets.engineGlobalRva;

        if (flags.selfCheck)
        {
            runSelfCheck(mem, roots, offsets, 32, &idmap);
            break;
        }

        evtOpen();
        if (flags.dumpFrames > 0)
        {
            dumpOpen();
            g_dumpFramesLeft = flags.dumpFrames;
        }
        g_gridCount = 0;
        g_gridDumped[0] = g_gridDumped[1] = g_gridDumped[2] = g_gridDumped[3] = 0;
        g_ownerOff = 0;

        HWND gameWindow = nullptr;
        if (!findGameWindow(pid, gameWindow))
        {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        bool overlayInit = false;
        RECT lastRect{};
        g_frame = 0;
        uint64_t lastWorld = 0;
        int rebuilds = 0;

        float anchor[3] = { 0.0f, 0.0f, 0.0f };
        bool anchorValid = false;
        uint64_t ownPawnPtr = 0;

        ActorArrayState cachedActors;
        std::vector<Tracked> tracked;
        std::vector<ao::Target> overlayTargets;
        overlayTargets.reserve(64);

        CameraState stableCam{};
        bool haveStable = false;
        int camJumpVotes = 0;
        Vec3f camJumpCand{};
        int soloReads = 0;

        ao::Camera lastOverlayCam{};
        bool haveLastDraw = false;

        while (running)
        {
            ++g_frame;

            if (GetAsyncKeyState(VK_F8) & 1)
            {
                flags.overlayEnabled = !flags.overlayEnabled;
                logInfo("overlay toggled: %s", flags.overlayEnabled ? "on" : "off");
            }
            if (GetAsyncKeyState(VK_F9) & 1)
            {
                if (g_dumpFramesLeft > 0)
                {
                    g_dumpFramesLeft = 0;
                    logInfo("debug dump stopped");
                }
                else
                {
                    dumpOpen();
                    g_dumpFramesLeft = 300;
                    logInfo("debug dump started (300 frames)");
                }
            }
            if (GetAsyncKeyState('L') & 1)
            {
                logInfo("L pressed — exiting");
                running = false;
                break;
            }

            if (!processAlive(pid) || !IsWindow(gameWindow))
            {
                logInfo("game exited; detaching");
                break;
            }

            if (!updateOverlayRect(gameWindow, lastRect, overlayInit))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(33));
                continue;
            }

            int fbW = static_cast<int>(lastRect.right - lastRect.left);
            int fbH = static_cast<int>(lastRect.bottom - lastRect.top);

            // ===========================================================
            // Camera: triple-read consensus (torn-matrix protection)
            // ===========================================================
            CameraState cs[3];
            ao::Camera  oc[3];
            bool okFlag[3] = { false, false, false };
            for (int i = 0; i < 3; ++i)
            {
                if (!readCameraState(mem, roots, offsets, cs[i], fbW, fbH,
                                     anchorValid ? anchor : nullptr))
                    continue;
                ao::Camera tmp{};
                if (!makeOverlayCamera(cs[i], tmp))
                    continue;
                oc[i] = tmp;
                okFlag[i] = true;
            }

            int useIdx = -1;
            if (okFlag[0] && okFlag[1] && !memcmp(&oc[0], &oc[1], sizeof(ao::Camera)))
                useIdx = 0;
            else if (okFlag[1] && okFlag[2] && !memcmp(&oc[1], &oc[2], sizeof(ao::Camera)))
                useIdx = 1;
            else if (okFlag[0] && okFlag[2] && !memcmp(&oc[0], &oc[2], sizeof(ao::Camera)))
                useIdx = 0;

            ao::Camera ocam;
            if (useIdx >= 0)
            {
                soloReads = 0;
                const CameraState& cand = cs[useIdx];
                if (!haveStable)
                {
                    stableCam = cand; ocam = oc[useIdx];
                    haveStable = true; lastOverlayCam = ocam; haveLastDraw = true;
                    camJumpVotes = 0;
                }
                else
                {
                    const float d = distanceCm(stableCam.cameraPos, cand.cameraPos);
                    if (d < 500.0f)
                    {
                        stableCam = cand; ocam = oc[useIdx];
                        lastOverlayCam = ocam; haveLastDraw = true; camJumpVotes = 0;
                    }
                    else if (camJumpVotes > 0 &&
                             distanceCm(camJumpCand, cand.cameraPos) < 200.0f)
                    {
                        stableCam = cand; ocam = oc[useIdx];
                        lastOverlayCam = ocam; haveLastDraw = true; camJumpVotes = 0;
                    }
                    else
                    {
                        camJumpCand = cand.cameraPos;
                        camJumpVotes = 1;
                        ocam = lastOverlayCam;
                    }
                }
            }
            else
            {
                bool anyOk = okFlag[0] || okFlag[1] || okFlag[2];
                if (anyOk && ++soloReads > 3)
                {
                    soloReads = 0;
                    useIdx = okFlag[0] ? 0 : (okFlag[1] ? 1 : 2);
                    const CameraState& cand = cs[useIdx];
                    if (!haveStable)
                    {
                        stableCam = cand; ocam = oc[useIdx];
                        haveStable = true; lastOverlayCam = ocam; haveLastDraw = true;
                    }
                    else if (distanceCm(stableCam.cameraPos, cand.cameraPos) < 500.0f)
                    {
                        stableCam = cand; ocam = oc[useIdx];
                        lastOverlayCam = ocam; haveLastDraw = true;
                    }
                    else
                    {
                        ocam = lastOverlayCam;
                    }
                }
                else if (haveStable && haveLastDraw)
                {
                    ocam = lastOverlayCam;
                }
                else
                {
                    ao::overlayHide();
                    std::this_thread::sleep_for(std::chrono::milliseconds(33));
                    continue;
                }
            }

            // ===========================================================
            // Actors
            // ===========================================================
            ActorArrayState cur;
            bool actorsOk = readActorArrayState(mem, roots, offsets, cur);

            if (actorsOk)
            {
                if (lastWorld != 0 && cur.worldPtr != lastWorld)
                {
                    logInfo("world pointer changed; clearing tracker");
                    tracked.clear();
                    overlayTargets.clear();
                    anchorValid = false;
                    haveStable = false;
                    cachedActors.valid = false;
                    ownPawnPtr = 0;
                }
                lastWorld = cur.worldPtr;

                if (!cachedActors.valid || (g_frame % 2 == 0) ||
                    cachedActors.levelPtr != cur.levelPtr ||
                    cachedActors.dataArray != cur.dataArray)
                {
                    cachedActors = cur;
                }

                const bool discover = (g_frame % 30 == 0) || tracked.empty();
                if (discover && cachedActors.valid)
                {
                    const int scan = cachedActors.count < 4096
                                     ? cachedActors.count : 4096;
                    for (int i = 0; i < scan && tracked.size() < 96; ++i)
                    {
                        uint64_t ap = 0;
                        if (!readPtr(mem, cachedActors.dataArray + (uint64_t)i * 8, ap))
                            continue;

                        bool known = false;
                        for (const Tracked& t : tracked)
                            if (t.ptr == ap) { known = true; break; }
                        if (known) continue;

                        LogicalTarget lt;
                        if (!readLogicalTarget(mem, offsets, ap, lt)) continue;
                        if (!lt.capsuleOk) continue;

                        Vec3f p{};
                        int src = 0;
                        if (!readComponentPos(mem, offsets, ap, p, &src)) continue;

                        uint64_t classPtr = 0;
                        if (!readPtr(mem, ap + 0x10, classPtr)) continue;

                        Tracked t{};
                        t.ptr = ap;
                        t.classPtr = classPtr;
                        t.compPtr = validatedComponent(mem, offsets, ap);
                        if (!t.compPtr) continue;
                        t.ownerOff = discoverOwnerOff(mem, t.compPtr, ap);
                        if (t.ownerOff && !ownerOk(mem, t.compPtr, ap, t.ownerOff))
                            continue;
                        t.capsOk = lt.capsuleOk;
                        if (namesOk)
                        {
                            t.cls = classifyActor(mem, idmap, ap);
                            t.hasCls = true;
                            if (t.cls.kind != ClassKind::Dino &&
                                t.cls.kind != ClassKind::Player)
                                continue;
                        }
                        absorb(t, p);
                        t.radius = lt.capsuleRadius;
                        t.halfHeight = lt.halfHeight;
                        tracked.push_back(t);
                        evt("NEW", ap, (int)tracked.size());
                    }
                    ++rebuilds;

                    if (namesOk && rebuilds >= 4)
                    {
                        bool any = false;
                        for (const Tracked& t : tracked)
                            if (t.hasCls) { any = true; break; }
                        if (!any && cachedActors.count > 200)
                        {
                            namesOk = false;
                            logWarn("classifier found nothing; structural fallback");
                        }
                    }
                }

                for (Tracked& t : tracked)
                    updateTarget(mem, offsets, t);

                tracked.erase(
                    std::remove_if(tracked.begin(), tracked.end(),
                        [](const Tracked& t) { return t.removeMe; }),
                    tracked.end());

                if (!anchorValid)
                {
                    const Tracked* bestP = nullptr;
                    const Tracked* bestD = nullptr;
                    for (const Tracked& t : tracked)
                    {
                        if (t.state == Tracked::State::Pending || !t.havePos)
                            continue;
                        if (t.hasCls && t.cls.kind == ClassKind::Player)
                        {
                            bestP = &t;
                            break;
                        }
                        if (!bestD) bestD = &t;
                    }
                    const Tracked* b = bestP ? bestP : bestD;
                    if (b)
                    {
                        anchor[0] = b->meas.x;
                        anchor[1] = b->meas.y;
                        anchor[2] = b->meas.z;
                        anchorValid = true;
                    }
                }

                // -------------------------------------------------------
                // Own pawn: ANY locked target within 3 m of the view origin
                // (third-person own pawn sits ~0.3 m away); hold while < 6 m.
                // -------------------------------------------------------
                {
                    bool curPresent = false;
                    float curD = 1e9f;
                    for (const Tracked& t : tracked)
                    {
                        if (t.ptr != ownPawnPtr) continue;
                        curPresent = (t.state == Tracked::State::Locked && t.havePos);
                        if (curPresent) curD = distanceMeters(ocam.pos, t.draw);
                        break;
                    }
                    if (curPresent && curD > 6.0f) curPresent = false;
                    if (!curPresent)
                    {
                        uint64_t best = 0;
                        float bestD = 3.0f;
                        for (const Tracked& t : tracked)
                        {
                            if (t.state != Tracked::State::Locked || !t.havePos) continue;
                            const float d = distanceMeters(ocam.pos, t.draw);
                            if (d < bestD) { bestD = d; best = t.ptr; }
                        }
                        if (best != ownPawnPtr)
                        {
                            ownPawnPtr = best;   // 0 if none nearby
                            evt("OWN", ownPawnPtr, (int)(bestD * 10.0f));
                        }
                    }
                }

                // -------------------------------------------------------
                // GEOMETRIC BASIS OVERRIDE.
                // camera.cpp's position + FOV are trusted; its row/column
                // basis assignment is not. Third-person view direction IS
                // the camera->own-pawn direction; up = world Z; zero roll.
                // -------------------------------------------------------
                {
                    Vec3f aim{};
                    bool haveAim = false;
                    if (ownPawnPtr)
                    {
                        for (const Tracked& t : tracked)
                            if (t.ptr == ownPawnPtr && t.havePos)
                            { aim = t.draw; haveAim = true; }
                    }
                    if (!haveAim)
                    {
                        float bestD = 1e9f;
                        for (const Tracked& t : tracked)
                        {
                            if (t.state != Tracked::State::Locked || !t.havePos) continue;
                            const float d = distanceCm(ocam.pos, t.draw);
                            if (d < bestD) { bestD = d; aim = t.draw; haveAim = true; }
                        }
                    }
                    if (haveAim)
                    {
                        Vec3f f{ aim.x - ocam.pos.x,
                                 aim.y - ocam.pos.y,
                                 aim.z - ocam.pos.z };
                        const float fl = sqrtf(f.x * f.x + f.y * f.y + f.z * f.z);
                        if (fl > 1.0f)
                        {
                            f.x /= fl; f.y /= fl; f.z /= fl;
                            const Vec3f wUp{ 0.0f, 0.0f, 1.0f };
                            Vec3f r = vecCross(f, wUp);
                            const float rl = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z);
                            if (rl > 1e-3f)
                            {
                                r.x /= rl; r.y /= rl; r.z /= rl;
                                const Vec3f u = vecCross(r, f);
                                ocam.forward = f;
                                ocam.right   = r;
                                ocam.up      = u;
                            }
                        }
                    }
                }

                // -------------------------------------------------------
                // Draw list: overlay CENTERS boxes on worldPos, so feed the
                // capsule CENTER. Sizes in cm (same units as positions).
                // -------------------------------------------------------
                overlayTargets.clear();
                for (const Tracked& t : tracked)
                {
                    if (t.state != Tracked::State::Locked || !t.havePos)
                        continue;
                    if (t.ptr == ownPawnPtr)
                        continue;

                    const bool isPlayer = t.hasCls &&
                                          (t.cls.kind == ClassKind::Player);

                    float r = t.radius, hh = t.halfHeight;
                    if (!(r > 15.0f && r < 500.0f))   r = isPlayer ? 40.0f : 60.0f;
                    if (!(hh > 30.0f && hh < 600.0f)) hh = isPlayer ? 90.0f : 120.0f;

                    ao::Target at{};
                    at.worldPos = t.draw;          // CENTER (overlay centers on it)
                    at.boxW = r * 2.0f;
                    at.boxH = hh * 2.0f;
                    at.health = t.health;
                    at.maxHealth = t.maxHealth;
                    at.level = t.level;
                    at.ammo = -1;
                    at.isPlayer = isPlayer;
                    at.isTurret = false;
                    at.behindCamera = false;
                    at.distance = distanceMeters(ocam.pos, t.draw);

                    const wchar_t* nm = t.hasCls ? t.cls.label.c_str() : nullptr;
                    wcsncpy_s(at.name,
                              (nm && nm[0]) ? nm :
                              (isPlayer ? L"Player" : L"Target"),
                              _TRUNCATE);

                    overlayTargets.push_back(at);
                }

                std::sort(overlayTargets.begin(), overlayTargets.end(),
                    [](const ao::Target& a, const ao::Target& b)
                    { return a.distance < b.distance; });
                if (overlayTargets.size() > 64)
                    overlayTargets.resize(64);

            } // end if (actorsOk)

            // ===========================================================
            // Draw
            // ===========================================================
            if (flags.overlayEnabled && !flags.dumpOnly)
            {
                ao::overlayDraw(overlayTargets.data(),
                                static_cast<int>(overlayTargets.size()),
                                ocam, cfg);
            }
            else
            {
                ao::overlayHide();
            }

            // ===========================================================
            // Debug dump (F9) + component grids
            // ===========================================================
            if (g_dumpFile && g_dumpFramesLeft > 0)
            {
                --g_dumpFramesLeft;
                fprintf(g_dumpFile,
                    "FRAME %llu fb=%dx%d cam=(%.1f,%.1f,%.1f) fov=%.1f tracked=%zu drawn=%zu own=%016llX ownerOff=0x%X\n",
                    (unsigned long long)g_frame, fbW, fbH,
                    stableCam.cameraPos.x, stableCam.cameraPos.y, stableCam.cameraPos.z,
                    ocam.fovDegrees, tracked.size(), overlayTargets.size(),
                    (unsigned long long)ownPawnPtr, g_ownerOff);

                for (const Tracked& t : tracked)
                {
                    Vec3f comp{};
                    int src = 0;
                    const bool haveComp = readComponentPos(mem, offsets, t.ptr, comp, &src);

                    fprintf(g_dumpFile,
                        "  T ptr=%016llX comp=%016llX own=%u st=%d cf=%d bl=%d pf=%d pl=%d caps=%d\n"
                        "    meas=(%.1f,%.1f,%.1f) draw=(%.1f,%.1f,%.1f)\n"
                        "    comp=(%.1f,%.1f,%.1f)%s r=%.1f hh=%.1f hp=%.1f/%.1f lvl=%d dist=%.1f\n",
                        (unsigned long long)t.ptr, (unsigned long long)t.compPtr,
                        t.ownerOff, (int)t.state, t.confirm, t.blind, t.pinFail,
                        t.pooled, t.capsOk ? 1 : 0,
                        t.meas.x, t.meas.y, t.meas.z,
                        t.draw.x, t.draw.y, t.draw.z,
                        comp.x, comp.y, comp.z, haveComp ? "" : "(fail)",
                        t.radius, t.halfHeight, t.health, t.maxHealth, t.level,
                        distanceMeters(ocam.pos, t.draw));

                    if (g_gridCount < 4)
                    {
                        bool seen = false;
                        for (int g = 0; g < g_gridCount; ++g)
                            if (g_gridDumped[g] == t.ptr) { seen = true; break; }
                        if (!seen && t.compPtr)
                        {
                            fprintf(g_dumpFile,
                                "    GRID ptr=%016llX comp=%016llX\n",
                                (unsigned long long)t.ptr, (unsigned long long)t.compPtr);
                            for (uint32_t o = 0x100; o <= 0x180; o += 0x10)
                            {
                                float row[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                                mem.readBytes(t.compPtr + o, row, sizeof(row));
                                fprintf(g_dumpFile,
                                    "      +%03X %14.3f %14.3f %14.3f %14.3f\n",
                                    o, row[0], row[1], row[2], row[3]);
                            }
                            g_gridDumped[g_gridCount++] = t.ptr;
                        }
                    }
                }
                fflush(g_dumpFile);
                if (g_dumpFramesLeft == 0) logInfo("debug dump finished");
            }

            // ===========================================================
            // Status
            // ===========================================================
            if (g_frame % 60 == 0)
            {
                int locked = 0;
                for (const Tracked& t : tracked)
                    if (t.state == Tracked::State::Locked) ++locked;
                printf("[status] frame=%llu tracked=%zu locked=%d drawn=%zu "
                       "names=%d camStable=%d ownerOff=0x%X dump=%d overlay=%s\n",
                       (unsigned long long)g_frame,
                       tracked.size(),
                       locked,
                       overlayTargets.size(),
                       namesOk ? 1 : 0,
                       haveStable ? 1 : 0,
                       g_ownerOff,
                       g_dumpFramesLeft,
                       flags.overlayEnabled ? "ON" : "OFF");
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        } // end inner loop

        ao::overlayHide();
        ao::overlayShutdown();
        logInfo("detached");
        std::this_thread::sleep_for(std::chrono::seconds(2));

    } // end outer loop

    dumpClose();
    evtClose();
    logShutdown();
    return 0;
}