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

static bool orientSane(const ao::Camera& c)
{
    return fabsf(c.right.z) < 0.50f && c.up.z > 0.25f;
}

static bool sizeSane(const ao::Camera& c, int fbW, int fbH)
{
    if (c.screenW < 320 || c.screenH < 240) return false;
    const int dw = c.screenW - fbW;
    const int dh = c.screenH - fbH;
    return (dw > -96 && dw < 96 && dh > -96 && dh < 96);
}

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
    if (g_dumpFile) { fflush(g_dumpFile); fclose(g_dumpFile); g_dumpFile = nullptr; }
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
    Vec3f meas;
    Vec3f draw;
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
static const int   kJumpVotesNeed  = 8;
static const float kOwnPickCm      = 300.0f;
static const float kFovContDeg     = 10.0f;   // v24: real cam quantizes 84.5..90.9

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
            posOk = true;   // v23: use for this frame, keep compPtr unchanged
    }

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
        t.draw = t.meas;   // zero-lag
}

static int countLocked(const std::vector<Tracked>& tracked)
{
    int n = 0;
    for (const Tracked& t : tracked)
        if (t.state == Tracked::State::Locked) ++n;
    return n;
}

static const Vec3f& ocamSafe(const ao::Camera& cam, bool have)
{
    static const Vec3f zero{ 0.0f, 0.0f, 0.0f };
    return have ? cam.pos : zero;
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
    logInfo("ark-full starting (lock-on tracker v24: fov-tolerant gate + cheap dump)");
    logInfo("hotkeys: F8 = toggle overlay, F9 = debug dump 300 frames, L = exit");
    {
        char cwd[MAX_PATH] = { 0 };
        if (GetCurrentDirectoryA(MAX_PATH, cwd)) logInfo("cwd=%s", cwd);
    }

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

        uint64_t ownPawnPtr = 0;
        Vec3f  ownPawnPos{ 0.0f, 0.0f, 0.0f };

        Vec3f trustedPos{ 0.0f, 0.0f, 0.0f };
        float trustedFov = 0.0f;
        bool  haveTrusted = false;
        int   rejectStreak = 0;

        ActorArrayState cachedActors;
        std::vector<Tracked> tracked;
        std::vector<ao::Target> overlayTargets;
        std::vector<uint64_t> ptrBuf;
        overlayTargets.reserve(64);

        CameraState stableCam{};
        bool haveStable = false;

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
                    if (g_dumpFile) fflush(g_dumpFile);
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
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
                continue;
            }

            int fbW = static_cast<int>(lastRect.right - lastRect.left);
            int fbH = static_cast<int>(lastRect.bottom - lastRect.top);

            // ===========================================================
            // 1) Tracker (camera-independent).
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
                    cachedActors.valid = false;
                    ownPawnPtr = 0;
                    haveTrusted = false;
                    rejectStreak = 0;
                }
                lastWorld = cur.worldPtr;

                if (!cachedActors.valid || (g_frame % 2 == 0) ||
                    cachedActors.levelPtr != cur.levelPtr ||
                    cachedActors.dataArray != cur.dataArray)
                {
                    cachedActors = cur;
                }

                const bool discover = (g_frame % 30 == 0) || tracked.empty();
                if (discover && cachedActors.valid && cachedActors.count > 0)
                {
                    const int scan = cachedActors.count < 4096
                                     ? cachedActors.count : 4096;
                    ptrBuf.resize((size_t)scan);
                    if (mem.readBytes(cachedActors.dataArray, ptrBuf.data(),
                                      (size_t)scan * 8))
                    {
                        for (int i = 0; i < scan && tracked.size() < 96; ++i)
                        {
                            const uint64_t ap = ptrBuf[i];
                            if (ap < 0x10000) continue;

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
            }

            const int lockedCount = countLocked(tracked);

            // ===========================================================
            // 2) Own pawn: held while your Player actor stays locked.
            // ===========================================================
            {
                bool present = false;
                if (ownPawnPtr)
                {
                    for (const Tracked& t : tracked)
                    {
                        if (t.ptr != ownPawnPtr) continue;
                        present = (t.state == Tracked::State::Locked && t.havePos);
                        if (present) ownPawnPos = t.draw;
                        break;
                    }
                }
                if (!present)
                {
                    uint64_t best = 0;
                    float bestD = kOwnPickCm;
                    const Vec3f& ref = ocamSafe(lastOverlayCam, haveLastDraw);
                    for (const Tracked& t : tracked)
                    {
                        if (!t.hasCls || t.cls.kind != ClassKind::Player) continue;
                        if (t.state != Tracked::State::Locked || !t.havePos) continue;
                        const float d = distanceCm(t.draw, ref);
                        if (d < bestD) { bestD = d; best = t.ptr; }
                    }
                    if (!best && ownPawnPtr)
                    {
                        for (const Tracked& t : tracked)
                        {
                            if (!t.hasCls || t.cls.kind != ClassKind::Player) continue;
                            if (t.state != Tracked::State::Locked || !t.havePos) continue;
                            if (distanceCm(t.draw, ownPawnPos) < kOwnPickCm) { best = t.ptr; break; }
                        }
                    }
                    if (best)
                    {
                        ownPawnPtr = best;
                        for (const Tracked& t : tracked)
                            if (t.ptr == best && t.havePos) ownPawnPos = t.draw;
                        evt("OWN", best, 1);
                    }
                    else
                    {
                        ownPawnPtr = 0;
                    }
                }
            }

            float anchor[3] = { 0.0f, 0.0f, 0.0f };
            const float* anchorPtr = nullptr;
            if (ownPawnPtr)
            {
                anchor[0] = ownPawnPos.x;
                anchor[1] = ownPawnPos.y;
                anchor[2] = ownPawnPos.z;
                anchorPtr = anchor;
            }

            // ===========================================================
            // 3) Camera: single read + continuity gates (v24 fov tolerance).
            // ===========================================================
            CameraState csNow;
            ao::Camera  cand;
            const bool candOk =
                readCameraState(mem, roots, offsets, csNow, fbW, fbH, anchorPtr) &&
                makeOverlayCamera(csNow, cand);

            ao::Camera ocam;
            bool ocamOk = false;

            if (candOk && orientSane(cand) && sizeSane(cand, fbW, fbH))
            {
                const float fov = cand.fovDegrees;
                bool accept = false;

                if (!haveTrusted)
                {
                    // v24 bootstrap: must also sit next to a locked actor.
                    if (fov > 60.0f && fov < 120.0f)
                    {
                        for (const Tracked& t : tracked)
                        {
                            if (t.state == Tracked::State::Locked && t.havePos &&
                                distanceCm(cand.pos, t.draw) < 1500.0f)
                            {
                                accept = true;
                                break;
                            }
                        }
                    }
                }
                else
                {
                    const float dTrusted = distanceCm(cand.pos, trustedPos);
                    const float dFov = fabsf(fov - trustedFov);
                    if (dTrusted < 1500.0f && dFov < kFovContDeg)
                        accept = true;
                    else if (ownPawnPtr &&
                             distanceCm(cand.pos, ownPawnPos) < 1500.0f &&
                             dFov < 25.0f)
                        accept = true;   // teleport / respawn
                }

                if (accept)
                {
                    ocam = cand;
                    stableCam = csNow;
                    haveStable = true;
                    trustedPos = cand.pos;
                    trustedFov = cand.fovDegrees;
                    haveTrusted = true;
                    rejectStreak = 0;
                    lastOverlayCam = ocam;
                    haveLastDraw = true;
                    ocamOk = true;
                }
            }

            if (!ocamOk)
            {
                ++rejectStreak;
                if (rejectStreak > 90)
                {
                    logInfo("camera re-bootstrap after prolonged rejection");
                    haveTrusted = false;
                    rejectStreak = 0;
                }
                if (haveStable && haveLastDraw)
                {
                    ocam = lastOverlayCam;   // freeze; never scatter
                    ocamOk = true;
                }
            }
            if (!ocamOk)
            {
                if (g_frame % 60 == 0)
                    printf("[status] CAM-FAIL frame=%llu locked=%d rej=%d\n",
                           (unsigned long long)g_frame, lockedCount, rejectStreak);
                if (g_dumpFile && g_dumpFramesLeft > 0)
                {
                    fprintf(g_dumpFile, "CAMFAIL f=%llu locked=%d rej=%d cand=%d\n",
                            (unsigned long long)g_frame, lockedCount,
                            rejectStreak, candOk ? 1 : 0);
                }
                ao::overlayHide();
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
                continue;
            }

            // ===========================================================
            // 4) Draw list + render
            // ===========================================================
            if (actorsOk)
            {
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
                    at.worldPos = t.draw;
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
            }

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
            // 5) Debug dump: every 2nd frame, <=16 targets, rare flush.
            // ===========================================================
            if (g_dumpFile && g_dumpFramesLeft > 0)
            {
                --g_dumpFramesLeft;
                if ((g_frame & 1) == 0)
                {
                    fprintf(g_dumpFile,
                        "FRAME %llu fb=%dx%d cam=(%.1f,%.1f,%.1f) fov=%.1f sw=%d sh=%d tracked=%zu drawn=%zu own=%016llX rej=%d\n"
                        "  F=(%.3f,%.3f,%.3f) R=(%.3f,%.3f,%.3f) U=(%.3f,%.3f,%.3f)\n",
                        (unsigned long long)g_frame, fbW, fbH,
                        ocam.pos.x, ocam.pos.y, ocam.pos.z,
                        ocam.fovDegrees, ocam.screenW, ocam.screenH,
                        tracked.size(), overlayTargets.size(),
                        (unsigned long long)ownPawnPtr, rejectStreak,
                        ocam.forward.x, ocam.forward.y, ocam.forward.z,
                        ocam.right.x, ocam.right.y, ocam.right.z,
                        ocam.up.x, ocam.up.y, ocam.up.z);

                    int dumped = 0;
                    for (const Tracked& t : tracked)
                    {
                        if (dumped++ >= 16) break;
                        Vec3f comp{};
                        int src = 0;
                        const bool haveComp = readComponentPos(mem, offsets, t.ptr, comp, &src);
                        fprintf(g_dumpFile,
                            "  T ptr=%016llX st=%d bl=%d pf=%d caps=%d\n"
                            "    meas=(%.1f,%.1f,%.1f) draw=(%.1f,%.1f,%.1f)\n"
                            "    comp=(%.1f,%.1f,%.1f)%s r=%.1f hh=%.1f dist=%.1f\n",
                            (unsigned long long)t.ptr, (int)t.state, t.blind, t.pinFail,
                            t.capsOk ? 1 : 0,
                            t.meas.x, t.meas.y, t.meas.z,
                            t.draw.x, t.draw.y, t.draw.z,
                            comp.x, comp.y, comp.z, haveComp ? "" : "(fail)",
                            t.radius, t.halfHeight,
                            distanceMeters(ocam.pos, t.draw));
                    }
                    if ((g_frame & 15) == 0)
                        fflush(g_dumpFile);
                }
                if (g_dumpFramesLeft == 0)
                {
                    fflush(g_dumpFile);
                    logInfo("debug dump finished");
                }
            }

            // ===========================================================
            // 6) Status
            // ===========================================================
            if (g_frame % 60 == 0)
            {
                printf("[status] frame=%llu tracked=%zu locked=%d drawn=%zu "
                       "names=%d camStable=%d fov=%.1f rej=%d own=%d overlay=%s\n",
                       (unsigned long long)g_frame,
                       tracked.size(),
                       lockedCount,
                       overlayTargets.size(),
                       namesOk ? 1 : 0,
                       haveStable ? 1 : 0,
                       trustedFov,
                       rejectStreak,
                       ownPawnPtr ? 1 : 0,
                       flags.overlayEnabled ? "ON" : "OFF");
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(8));  // ~120 Hz
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