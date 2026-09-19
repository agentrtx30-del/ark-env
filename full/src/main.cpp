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

// Forward declarations for manual matrix selector (v36)
void cameraForceNextCandidate();
void cameraResetCandidateForce();
int  cameraGetForcedCandidate();
int  cameraGetCandidateCount();

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
#include <deque>
#include <string>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

namespace ao = arkoverlay;

struct RuntimeFlags
{
    bool demo = false;
    bool selfCheck = false;
    bool dumpOnly = false;
    bool overlayEnabled = true;
    bool camTrace = false;
    bool findCam = false;
    int  dumpFrames = 0;
    int  hz = 250;
};

static void parseArgs(int argc, char** argv, RuntimeFlags& f)
{
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--demo"))            f.demo = true;
        else if (!strcmp(argv[i], "--selfcheck"))  f.selfCheck = true;
        else if (!strcmp(argv[i], "--dump-only"))  f.dumpOnly = true;
        else if (!strcmp(argv[i], "--no-overlay")) f.overlayEnabled = false;
        else if (!strcmp(argv[i], "--camtrace"))   f.camTrace = true;
        else if (!strcmp(argv[i], "--findcam"))    f.findCam = true;
        else if (!strncmp(argv[i], "--dump-frames=", 14)) f.dumpFrames = atoi(argv[i] + 14);
        else if (!strncmp(argv[i], "--hz=", 5))          f.hz = atoi(argv[i] + 5);
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
// Diagnostics + SENDME
// ---------------------------------------------------------------------------
static FILE* g_dumpFile = nullptr;
static int   g_dumpFramesLeft = 0;
static FILE* g_evtFile = nullptr;
static FILE* g_camTrace = nullptr;
static uint64_t g_frame = 0;

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

static std::deque<std::string> g_badTrace;
static std::deque<std::string> g_badSnap;
static std::deque<std::string> g_statusLines;
static std::string g_findcamLine = "findcam: (not run)";
static uint64_t g_ticks = 0, g_badTicks = 0, g_longestFreeze = 0;
static unsigned g_camJumps = 0, g_ownFlips = 0;
static float g_fovSeen[8]; static int g_fovSeenN = 0;
static Vec3f g_lastAcceptedPos{ 0, 0, 0 };
static bool  g_haveLastAccepted = false;
static bool  g_prevBad = false;
static uint64_t g_prevOwnPtr = 0;

struct Tracked;

static void writeSendMe(const char* reason)
{
    FILE* f = nullptr;
    fopen_s(&f, ".\\work\\SENDME.txt", "w");
    if (!f) return;
    fprintf(f, "# ark-full SENDME (%s) - paste this WHOLE file, nothing else\n", reason);
    fprintf(f, "# ticks=%llu bad=%llu longestFreeze=%llu camJumps=%u ownFlips=%u\n",
            (unsigned long long)g_ticks, (unsigned long long)g_badTicks,
            (unsigned long long)g_longestFreeze, g_camJumps, g_ownFlips);
    fprintf(f, "# fovsSeen:");
    for (int i = 0; i < g_fovSeenN; ++i) fprintf(f, " %.1f", g_fovSeen[i]);
    fprintf(f, "\n%s\n", g_findcamLine.c_str());
    fprintf(f, "# - last status lines -\n");
    for (size_t i = 0; i < g_statusLines.size(); ++i) fprintf(f, "%s\n", g_statusLines[i].c_str());
    fprintf(f, "# - anomaly trace (!! = abnormal tick), last %zu -\n", g_badTrace.size());
    for (size_t i = 0; i < g_badTrace.size(); ++i) fprintf(f, "%s\n", g_badTrace[i].c_str());
    fprintf(f, "# - anomaly episode snapshots, last %zu -\n", g_badSnap.size());
    for (size_t i = 0; i < g_badSnap.size(); ++i) fprintf(f, "%s\n", g_badSnap[i].c_str());
    fprintf(f, "# --- end ---\n");
    fclose(f);
    logInfo("SENDME written -> work\\SENDME.txt (%s)", reason);
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
    int stale = 0;

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

static void captureSnapshot(uint64_t frame, const ao::Camera& ocam, bool ocamOk,
                            const std::vector<Tracked>& tracked, int rej, int frz)
{
    char buf[256];
    sprintf_s(buf, "SNAP f=%llu cam=(%.0f,%.0f,%.0f) fov=%.1f ok=%d rej=%d frz=%d tracked=%zu",
              (unsigned long long)frame, ocam.pos.x, ocam.pos.y, ocam.pos.z,
              ocam.fovDegrees, ocamOk ? 1 : 0, rej, frz, tracked.size());
    std::string s = buf;
    int n = 0;
    for (size_t i = 0; i < tracked.size(); ++i)
    {
        if (n++ >= 12) { s += "\n  ...(rest omitted)"; break; }
        const Tracked& t = tracked[i];
        sprintf_s(buf, "\n  T %016llX st=%d meas=(%.0f,%.0f,%.0f) r=%.0f hh=%.0f",
                  (unsigned long long)t.ptr, (int)t.state,
                  t.meas.x, t.meas.y, t.meas.z, t.radius, t.halfHeight);
        s += buf;
    }
    g_badSnap.push_back(s);
    while (g_badSnap.size() > 2) g_badSnap.pop_front();
}

static const float kAcceptStepCm   = 500.0f;
static const int   kConfirmNeed    = 3;
static const int   kBlindPending   = 20;
static const int   kBlindLocked    = 8;
static const int   kDyingFrames    = 15;
static const int   kPinFailNeed    = 10;
static const int   kJumpVotesNeed  = 10;
static const float kDrawMaxDistCm  = 25000.0f;
static const int   kDrawCap        = 40;
static const int   kFreezeHide     = 30;   // v32: hide only on real stalls
static const int   kRouteBreaker   = 300;  // v32: clear locked route after 300 frozen ticks

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
                t.stale = 0; t.jumpVotes = 0; t.havePos = false; t.confirm = 0;
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
        {
            t.compPtr = validatedComponent(mem, off, t.ptr);
            if (!t.ownerOff && t.compPtr) t.ownerOff = discoverOwnerOff(mem, t.compPtr, t.ptr);
            posOk = true;
        }
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

    if (posOk || fieldsOk) { t.blind = 0; t.stale = 0; }
    else                   { ++t.blind; ++t.stale; }

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
        t.draw = t.meas;
    }
}

static int countLocked(const std::vector<Tracked>& tracked)
{
    int n = 0;
    for (size_t i = 0; i < tracked.size(); ++i)
        if (tracked[i].state == Tracked::State::Locked) ++n;
    return n;
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

    timeBeginPeriod(1);
    int periodMs = 1000 / (flags.hz > 0 ? flags.hz : 250);
    if (periodMs < 1) periodMs = 1;
    if (periodMs > 33) periodMs = 33;

    logInit(L".\\work\\ark-full.log");
    logInfo("ark-full starting (lock-on tracker v32: scored route lock, no oscillation)");
    logInfo("hotkeys: F8 overlay, F9 full dump, F10 write SENDME now, L exit (writes SENDME)");
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
            std::this_thread::sleep_for(std::chrono::milliseconds(periodMs * 8));
            continue;
        }
        logInfo("found ARK pid=%lu", pid);

        WinMemory mem;
        if (!mem.open(pid))
        {
            logError("OpenProcess failed, error=%lu", mem.lastOpenError());
            std::this_thread::sleep_for(std::chrono::milliseconds(periodMs * 8));
            continue;
        }

        uint64_t base = 0;
        if (!findModuleBase(mem.nativeHandle(), offsets.gameModule.c_str(), base))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(periodMs * 8));
            continue;
        }
        if (!accessSelfTest(mem, base))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(periodMs * 8));
            continue;
        }
        logInfo("attach success: base=0x%016llX", base);

        RuntimeRoots roots;
        roots.shooterBase = base;
        roots.gworldAddress = base + offsets.gworldRva;
        roots.engineGlobalAddress = base + offsets.engineGlobalRva;

        if (offsets.pcmOffset == 0 || offsets.povOffset == 0)
        {
            std::string report;
            if (runFindCamera(mem, roots, offsets, idmap, report))
            {
                g_findcamLine = "findcam: OK " + report;
                logInfo("findcam: %s", report.c_str());
            }
            else
            {
                g_findcamLine = "findcam: FAILED (" + report + ") - scored route lock active";
                logWarn("findcam: %s (scored route lock active)", report.c_str());
            }
        }
        else
        {
            char b[128];
            sprintf_s(b, "findcam: cached pcm=+0x%X pov=+0x%X", offsets.pcmOffset, offsets.povOffset);
            g_findcamLine = b;
        }
        if (flags.findCam)
        {
            printf("%s\n", g_findcamLine.c_str());
            writeSendMe("findcam");
            break;
        }

        if (flags.selfCheck)
        {
            runSelfCheck(mem, roots, offsets, 32, &idmap);
            break;
        }

        evtOpen();
        if (flags.camTrace && !g_camTrace)
            fopen_s(&g_camTrace, ".\\work\\camtrace.txt", "w");
        if (flags.dumpFrames > 0)
        {
            if (!g_dumpFile)
            {
                fopen_s(&g_dumpFile, ".\\work\\debug_frames.txt", "w");
                if (g_dumpFile) logInfo("debug dump -> work\\debug_frames.txt");
            }
            g_dumpFramesLeft = flags.dumpFrames;
        }
        g_ownerOff = 0;

        HWND gameWindow = nullptr;
        if (!findGameWindow(pid, gameWindow))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(periodMs * 8));
            continue;
        }

        bool overlayInit = false;
        RECT lastRect{};
        g_frame = 0;
        uint64_t lastWorld = 0;
        int rebuilds = 0;

        // v32 sticky own-pawn anchor
        uint64_t ownPawnPtr = 0;
        Vec3f  ownPawnPos{ 0.0f, 0.0f, 0.0f };
        uint64_t ownLastOk = 0;
        bool ownEver = false;

        Vec3f trustedPos{ 0.0f, 0.0f, 0.0f };
        float trustedFov = 0.0f;
        int   freezeStreak = 0;

        ActorArrayState cachedActors;
        std::vector<Tracked> tracked;
        std::vector<ao::Target> overlayTargets;
        overlayTargets.reserve(64);

        bool haveStable = false;
        ao::Camera lastOverlayCam{};
        bool haveLastDraw = false;

        auto rateMark = std::chrono::steady_clock::now();
        uint64_t rateFrames = 0;
        double measHz = 0.0;

        while (running)
        {
            ++g_frame;
            ++rateFrames;

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
                    if (!g_dumpFile)
                    {
                        fopen_s(&g_dumpFile, ".\\work\\debug_frames.txt", "w");
                        if (g_dumpFile) logInfo("debug dump -> work\\debug_frames.txt");
                    }
                    g_dumpFramesLeft = 300;
                    logInfo("debug dump started (300 frames)");
                }
            }
            if (GetAsyncKeyState(VK_F10) & 1)
                writeSendMe("F10");
            if (GetAsyncKeyState(VK_F11) & 1)
            {
                cameraForceNextCandidate();
            }
            if (GetAsyncKeyState(VK_F12) & 1)
            {
                cameraResetCandidateForce();
            }
            if (GetAsyncKeyState('L') & 1)
            {
                logInfo("L pressed - exiting");
                writeSendMe("L-key");
                running = false;
                break;
            }

            if (!processAlive(pid) || !IsWindow(gameWindow))
            {
                logInfo("game exited; detaching");
                writeSendMe("game-exit");
                break;
            }

            if (!updateOverlayRect(gameWindow, lastRect, overlayInit))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(periodMs));
                continue;
            }

            int fbW = static_cast<int>(lastRect.right - lastRect.left);
            int fbH = static_cast<int>(lastRect.bottom - lastRect.top);

            // 1) Tracker
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
                    ownPawnPtr = 0; ownEver = false;
                    cameraResetRoute();
                    freezeStreak = 0;
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
                        for (size_t k = 0; k < tracked.size(); ++k)
                            if (tracked[k].ptr == ap) { known = true; break; }
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
                        for (size_t k = 0; k < tracked.size(); ++k)
                            if (tracked[k].hasCls) { any = true; break; }
                        if (!any && cachedActors.count > 200)
                        {
                            namesOk = false;
                            logWarn("classifier found nothing; structural fallback");
                        }
                    }
                }

                for (size_t i = 0; i < tracked.size(); ++i)
                    updateTarget(mem, offsets, tracked[i]);

                tracked.erase(
                    std::remove_if(tracked.begin(), tracked.end(),
                        [](const Tracked& t){ return t.removeMe; }),
                    tracked.end());
            }

            const int lockedCount = countLocked(tracked);

            // 2) Camera: scored locked route (camera.cpp v32). Accept = route
            //    read sane. No per-tick distance gates => no oscillation.
            const bool anchorValid = ownEver && (g_frame - ownLastOk) <= 500;
            const float* anchorPtr = anchorValid ? &ownPawnPos.x : nullptr;

            CameraState csA;
            ao::Camera  candA;
            const bool okA = readCameraState(mem, roots, offsets, csA, fbW, fbH, anchorPtr) &&
                             makeOverlayCamera(csA, candA);
            const bool saneA = okA && orientSane(candA) && sizeSane(candA, fbW, fbH);

            ao::Camera ocam;
            bool ocamOk = false;

            if (saneA)
            {
                ocam = candA;
                haveStable = true;
                trustedPos = candA.pos;
                trustedFov = candA.fovDegrees;
                freezeStreak = 0;
                lastOverlayCam = ocam;
                haveLastDraw = true;
                ocamOk = true;

                const float rf = floorf(candA.fovDegrees * 2.0f + 0.5f) / 2.0f;
                bool seen = false;
                for (int i = 0; i < g_fovSeenN; ++i) if (g_fovSeen[i] == rf) seen = true;
                if (!seen && g_fovSeenN < 8) g_fovSeen[g_fovSeenN++] = rf;
                if (g_haveLastAccepted && distanceCm(candA.pos, g_lastAcceptedPos) > 3000.0f)
                {
                    ++g_camJumps;
                    captureSnapshot(g_frame, candA, true, tracked, 0, 0);
                }
                g_lastAcceptedPos = candA.pos;
                g_haveLastAccepted = true;

                // v32: own pawn acquired ONLY from a live (non-frozen) camera,
                // nearest Player-class locked target within 8 m; sticky 2 s.
                uint64_t best = 0;
                float bestD = 800.0f;
                for (size_t i = 0; i < tracked.size(); ++i)
                {
                    const Tracked& t = tracked[i];
                    if (!t.hasCls || t.cls.kind != ClassKind::Player) continue;
                    if (t.state != Tracked::State::Locked || !t.havePos) continue;
                    const float d = distanceCm(t.draw, ocam.pos);
                    if (d < bestD) { bestD = d; best = t.ptr; }
                }
                if (best)
                {
                    if (best != ownPawnPtr) { ++g_ownFlips; ownPawnPtr = best; }
                    for (size_t i = 0; i < tracked.size(); ++i)
                        if (tracked[i].ptr == best && tracked[i].havePos) ownPawnPos = tracked[i].meas;
                    ownLastOk = g_frame;
                    ownEver = true;
                }
            }
            else
            {
                ++freezeStreak;
                if (freezeStreak > kRouteBreaker)
                {
                    logInfo("route breaker after %d frozen frames", freezeStreak);
                    cameraResetRoute();
                    freezeStreak = 0;
                }
                if (haveStable && haveLastDraw)
                {
                    ocam = lastOverlayCam;
                    ocamOk = true;
                }
            }

            if (g_camTrace && (g_frame % 5 == 0))
            {
                fprintf(g_camTrace,
                    "T f=%llu okA=%d sane=%d anchor=%d frz=%d fov=%.1f campos=(%.0f,%.0f,%.0f)\n",
                    (unsigned long long)g_frame, okA ? 1 : 0, saneA ? 1 : 0,
                    anchorValid ? 1 : 0, freezeStreak,
                    ocamOk ? ocam.fovDegrees : 0.0f,
                    ocamOk ? ocam.pos.x : 0.0f, ocamOk ? ocam.pos.y : 0.0f, ocamOk ? ocam.pos.z : 0.0f);
                fflush(g_camTrace);
            }

            // SENDME instrumentation
            ++g_ticks;
            {
                const bool bad = !ocamOk || freezeStreak > 0;
                if (bad)
                {
                    ++g_badTicks;
                    if ((uint64_t)freezeStreak > g_longestFreeze) g_longestFreeze = freezeStreak;
                    char lb[256];
                    sprintf_s(lb,
                        "!! f=%llu okA=%d sane=%d anchor=%d frz=%d fov=%.1f",
                        (unsigned long long)g_frame, okA ? 1 : 0, saneA ? 1 : 0,
                        anchorValid ? 1 : 0, freezeStreak,
                        ocamOk ? ocam.fovDegrees : 0.0f);
                    g_badTrace.push_back(lb);
                    while (g_badTrace.size() > 40) g_badTrace.pop_front();
                    if (!g_prevBad)
                        captureSnapshot(g_frame, ocam, ocamOk, tracked, 0, freezeStreak);
                }
                g_prevBad = bad;
            }

            // 3) Draw list (culled) or hide when stalled
            bool drew = false;
            if (ocamOk && actorsOk && freezeStreak <= kFreezeHide)
            {
                overlayTargets.clear();
                for (size_t i = 0; i < tracked.size(); ++i)
                {
                    const Tracked& t = tracked[i];
                    if (t.state != Tracked::State::Locked || !t.havePos) continue;
                    if (t.stale > 2) continue;
                    if (ownPawnPtr && t.ptr == ownPawnPtr) continue;
                    const float dcm = distanceCm(ocam.pos, t.draw);
                    if (dcm > kDrawMaxDistCm) continue;

                    const bool isPlayer = t.hasCls && (t.cls.kind == ClassKind::Player);

                    float r = t.radius, hh = t.halfHeight;
                    if (!(r > 15.0f && r < 500.0f))   r = isPlayer ? 40.0f : 60.0f;
                    if (!(hh > 30.0f && hh < 600.0f)) hh = isPlayer ? 90.0f : 120.0f;

                                        // v38: cull targets behind / outside the live camera frustum
                    if (!camFrustumOK(t.draw, 10.0f)) continue;

                    // v40 draw filter: players ALWAYS; creatures only level > 130
                    if (!isPlayer && !(t.level > 130)) continue;
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
                    at.distance = dcm / 100.0f;

                    const wchar_t* nm = t.hasCls ? t.cls.label.c_str() : nullptr;
                    wcsncpy_s(at.name,
                              (nm && nm[0]) ? nm :
                              (isPlayer ? L"Player" : L"Target"),
                              _TRUNCATE);

                    overlayTargets.push_back(at);
                }

                std::sort(overlayTargets.begin(), overlayTargets.end(),
                    [](const ao::Target& a, const ao::Target& b) { return a.distance < b.distance; });
                if (overlayTargets.size() > kDrawCap)
                    overlayTargets.resize(kDrawCap);

                if (flags.overlayEnabled && !flags.dumpOnly)
                {
                    ao::overlayDraw(overlayTargets.data(),
                                    static_cast<int>(overlayTargets.size()),
                                    ocam, cfg);
                    drew = true;
                }
            }
            if (!drew) ao::overlayHide();

            // 4) Optional full dump (F9)
            if (g_dumpFile && g_dumpFramesLeft > 0)
            {
                --g_dumpFramesLeft;
                fprintf(g_dumpFile,
                    "FRAME %llu fb=%dx%d cam=(%.1f,%.1f,%.1f) fov=%.1f sw=%d sh=%d tracked=%zu drawn=%zu own=%016llX frz=%d\n"
                    "  F=(%.3f,%.3f,%.3f) R=(%.3f,%.3f,%.3f) U=(%.3f,%.3f,%.3f)\n",
                    (unsigned long long)g_frame, fbW, fbH,
                    ocamOk ? ocam.pos.x : 0.0f, ocamOk ? ocam.pos.y : 0.0f, ocamOk ? ocam.pos.z : 0.0f,
                    ocamOk ? ocam.fovDegrees : 0.0f,
                    ocamOk ? ocam.screenW : 0, ocamOk ? ocam.screenH : 0,
                    tracked.size(), overlayTargets.size(),
                    (unsigned long long)ownPawnPtr, freezeStreak,
                    ocamOk ? ocam.forward.x : 0.0f, ocamOk ? ocam.forward.y : 0.0f, ocamOk ? ocam.forward.z : 0.0f,
                    ocamOk ? ocam.right.x : 0.0f, ocamOk ? ocam.right.y : 0.0f, ocamOk ? ocam.right.z : 0.0f,
                    ocamOk ? ocam.up.x : 0.0f, ocamOk ? ocam.up.y : 0.0f, ocamOk ? ocam.up.z : 0.0f);

                for (size_t i = 0; i < tracked.size(); ++i)
                {
                    const Tracked& t = tracked[i];
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
                fflush(g_dumpFile);
                if (g_dumpFramesLeft == 0) logInfo("debug dump finished");
            }

            // 5) Status
            if (g_frame % 60 == 0)
            {
                const auto now = std::chrono::steady_clock::now();
                const double secs = std::chrono::duration<double>(now - rateMark).count();
                if (secs > 0.05) measHz = static_cast<double>(rateFrames) / secs;
                rateMark = now;
                rateFrames = 0;

                int cIdx = cameraGetForcedCandidate();
                int cCnt = cameraGetCandidateCount();
                char camStr[32];
                if (cIdx < 0) sprintf_s(camStr, "auto/%d", cCnt);
                else sprintf_s(camStr, "F%d/%d", cIdx + 1, cCnt);

                char sb[320];
                sprintf_s(sb,
                    "[status] frame=%llu hz=%.0f tracked=%zu locked=%d drawn=%zu "
                    "names=%d fov=%.1f frz=%d own=%d cam=%s overlay=%s",
                    (unsigned long long)g_frame,
                    measHz,
                    tracked.size(),
                    lockedCount,
                    overlayTargets.size(),
                    namesOk ? 1 : 0,
                    trustedFov,
                    freezeStreak,
                    ownPawnPtr ? 1 : 0,
                    camStr,
                    flags.overlayEnabled ? "ON" : "OFF");
                printf("%s\n", sb);
                g_statusLines.push_back(sb);
                while (g_statusLines.size() > 12) g_statusLines.pop_front();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(periodMs));
        } // end inner loop

        ao::overlayHide();
        ao::overlayShutdown();
        writeSendMe("detach");
        logInfo("detached");
        std::this_thread::sleep_for(std::chrono::seconds(2));

    } // end outer loop

    if (g_dumpFile) { fclose(g_dumpFile); g_dumpFile = nullptr; }
    evtClose();
    if (g_camTrace) { fclose(g_camTrace); g_camTrace = nullptr; }
    timeEndPeriod(1);
    logShutdown();
    return 0;
}