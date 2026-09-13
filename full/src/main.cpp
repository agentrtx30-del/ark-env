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
};

static void parseArgs(int argc, char** argv, RuntimeFlags& f)
{
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--demo"))       f.demo = true;
        else if (!strcmp(argv[i], "--selfcheck"))  f.selfCheck = true;
        else if (!strcmp(argv[i], "--dump-only"))   f.dumpOnly = true;
        else if (!strcmp(argv[i], "--no-overlay"))  f.overlayEnabled = false;
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

// ---------------------------------------------------------------------------
// Position: component (capsule+0xF0) is AUTHORITATIVE.
// Direct (actor+0xC00) is a LAST RESORT only — it reads stale/zero in this
// build and was the root cause of the persistent flicker.
// ---------------------------------------------------------------------------
static bool readComponentPos(const WinMemory& mem, const OffsetProfile& off,
                             uint64_t actor, Vec3f& out)
{
    const uint32_t compPtrOffs[2] = { off.capsuleOffset, 0x170u };
    for (int k = 0; k < 2; ++k)
    {
        uint64_t c = 0;
        if (!readPtr(mem, actor + compPtrOffs[k], c)) continue;
        Vec3f v{};
        if (!mem.read(c + 0xF0, v)) continue;
        if (!plausiblePos(v)) continue;
        out = v;
        return true;
    }
    // last resort: direct actor location
    Vec3f d{};
    if (mem.read(actor + off.locationOffset, d) && plausiblePos(d))
    {
        out = d;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Lock-on tracker
// ---------------------------------------------------------------------------
struct Tracked
{
    uint64_t ptr = 0;
    uint64_t classPtr = 0;          // identity pin (actor+0x10)
    ClassInfo cls;
    bool hasCls = false;

    enum class State { Pending, Locked, Dying };
    State state = State::Pending;
    bool removeMe = false;

    int confirm = 0;                 // consecutive agreeing reads
    int blind = 0;                   // consecutive frames with no usable read
    int dying = 0;
    int jumpVotes = 0;

    bool havePos = false;
    Vec3f meas;                      // last accepted measured position
    Vec3f draw;                      // smoothed position used for rendering
    Vec3f jumpCand;

    float radius = 50.0f;
    float halfHeight = 100.0f;
    float health = 0.0f;
    float maxHealth = 0.0f;
    int level = -1;
};

static const float kAcceptStepCm = 2500.0f;   // max plausible move per read
static const int   kConfirmNeed  = 3;          // reads to lock
static const int   kBlindPending = 20;         // frames before dropping a pending target
static const int   kBlindLocked  = 90;         // ~3 s freeze before dying
static const int   kDyingFrames  = 30;
static const float kLerp         = 0.40f;      // smoothing factor

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
        // Two agreeing far reads: accept as real teleport/fast travel.
        t.meas = p;
        t.jumpVotes = 0;
        t.confirm = 1;
    }
    else
    {
        t.jumpCand = p;
        t.jumpVotes = 1;
        // ignore this read — keeps the frozen position
    }
}

static void updateTarget(const WinMemory& mem, const OffsetProfile& off, Tracked& t)
{
    // Identity pin: if the class pointer at actor+0x10 changed, UE recycled
    // this address for a different object. Drop it immediately.
    uint64_t cp = 0;
    if (!readPtr(mem, t.ptr + 0x10, cp) || cp != t.classPtr)
    {
        t.removeMe = true;
        return;
    }

    // Read fields (health, capsule, level).
    LogicalTarget lt;
    const bool alive = readLogicalTarget(mem, off, t.ptr, lt);

    // Read position from component (authoritative).
    Vec3f pos{};
    const bool posOk = readComponentPos(mem, off, t.ptr, pos);

    if (alive)
    {
        t.blind = 0;
        if (lt.capsuleOk)
        {
            t.radius = lt.capsuleRadius;
            t.halfHeight = lt.halfHeight;
        }
        if (lt.maxHealth > 0.0f)
        {
            t.health = lt.health;
            t.maxHealth = lt.maxHealth;
        }
        if (lt.level >= 0)
            t.level = lt.level;
    }
    else
    {
        ++t.blind;
    }

    if (posOk) absorb(t, pos);
    // posOk == false: position is frozen at last good value (no jump, no flicker).

    switch (t.state)
    {
    case Tracked::State::Pending:
        if (t.havePos && t.confirm >= kConfirmNeed)
            t.state = Tracked::State::Locked;
        else if (t.blind > kBlindPending)
            t.removeMe = true;
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
            t.removeMe = true;
        break;
    }

    // Smooth follow: drawn position glides toward measured position.
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
    logInfo("ark-full starting (lock-on tracker v4)");
    logInfo("hotkeys: F8 = toggle overlay, L = exit");

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

    // ===================================================================
    // Outer loop: watch for game process
    // ===================================================================
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

        HWND gameWindow = nullptr;
        if (!findGameWindow(pid, gameWindow))
        {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        // ---------------------------------------------------------------
        // Per-session state
        // ---------------------------------------------------------------
        bool overlayInit = false;
        RECT lastRect{};
        uint64_t frame = 0;
        uint64_t lastWorld = 0;
        int rebuilds = 0;

        float anchor[3] = { 0.0f, 0.0f, 0.0f };
        bool anchorValid = false;

        ActorArrayState cachedActors;
        std::vector<Tracked> tracked;
        std::vector<ao::Target> overlayTargets;
        overlayTargets.reserve(64);

        // Camera stability state: keep last good camera, vote on jumps.
        CameraState stableCam{};
        bool haveStable = false;
        int camJumpVotes = 0;
        Vec3f camJumpCand{};
        int hardFail = 0;

        // Last drawn overlay (redrawn on actor-read failure instead of blanking).
        ao::Camera lastOverlayCam{};
        bool haveLastDraw = false;

        // ===============================================================
        // Inner loop: per-frame
        // ===============================================================
        while (running)
        {
            ++frame;

            // ---- hotkeys ----
            if (GetAsyncKeyState(VK_F8) & 1)
            {
                flags.overlayEnabled = !flags.overlayEnabled;
                logInfo("overlay toggled: %s", flags.overlayEnabled ? "on" : "off");
            }
            if (GetAsyncKeyState('L') & 1)
            {
                logInfo("L pressed — exiting");
                running = false;
                break;
            }

            // ---- process / window alive? ----
            if (!processAlive(pid) || !IsWindow(gameWindow))
            {
                logInfo("game exited; detaching");
                break;
            }

            // ---- overlay window ----
            if (!updateOverlayRect(gameWindow, lastRect, overlayInit))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(33));
                continue;
            }

            int fbW = static_cast<int>(lastRect.right - lastRect.left);
            int fbH = static_cast<int>(lastRect.bottom - lastRect.top);

            // ===========================================================
            // Camera: read + stability gate (jump voting)
            // ===========================================================
            CameraState camRead;
            bool camOk = readCameraState(mem, roots, offsets, camRead, fbW, fbH,
                                         anchorValid ? anchor : nullptr);
            if (camOk)
            {
                hardFail = 0;
                if (!haveStable)
                {
                    stableCam = camRead;
                    haveStable = true;
                    camJumpVotes = 0;
                }
                else
                {
                    const float d = distanceCm(stableCam.cameraPos, camRead.cameraPos);
                    if (d < 500.0f)
                    {
                        // Normal: accept.
                        stableCam = camRead;
                        camJumpVotes = 0;
                    }
                    else if (camJumpVotes > 0 &&
                             distanceCm(camJumpCand, camRead.cameraPos) < 200.0f)
                    {
                        // Two agreeing far reads: real teleport/fast travel.
                        stableCam = camRead;
                        camJumpVotes = 0;
                    }
                    else
                    {
                        // Torn matrix or single-frame garbage: ignore, keep stable.
                        camJumpCand = camRead.cameraPos;
                        camJumpVotes = 1;
                    }
                }
            }
            else
            {
                // Read failed entirely: keep stable cam for up to ~1.5 s.
                if (haveStable && ++hardFail <= 45)
                {
                    // stableCam stays as-is
                }
                else
                {
                    ao::overlayHide();
                    std::this_thread::sleep_for(std::chrono::milliseconds(33));
                    continue;
                }
            }

            // Convert to overlay camera.
            ao::Camera ocam;
            if (!makeOverlayCamera(stableCam, ocam))
            {
                // If conversion fails but we have a previous good one, reuse it.
                if (haveLastDraw)
                    ocam = lastOverlayCam;
                else
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(33));
                    continue;
                }
            }
            else
            {
                lastOverlayCam = ocam;
                haveLastDraw = true;
            }

            // ===========================================================
            // Actor array: read but NEVER blank overlay on failure
            // ===========================================================
            ActorArrayState cur;
            bool actorsOk = readActorArrayState(mem, roots, offsets, cur);

            if (actorsOk)
            {
                // World change detection.
                if (lastWorld != 0 && cur.worldPtr != lastWorld)
                {
                    logInfo("world pointer changed; clearing tracker");
                    tracked.clear();
                    overlayTargets.clear();
                    anchorValid = false;
                    haveStable = false;
                    cachedActors.valid = false;
                }
                lastWorld = cur.worldPtr;

                if (!cachedActors.valid || (frame % 2 == 0) ||
                    cachedActors.levelPtr != cur.levelPtr ||
                    cachedActors.dataArray != cur.dataArray)
                {
                    cachedActors = cur;
                }

                // -------------------------------------------------------
                // Discovery: slow scan, classify once, pin identity
                // -------------------------------------------------------
                const bool discover = (frame % 30 == 0) || tracked.empty();
                if (discover && cachedActors.valid)
                {
                    const int scan = cachedActors.count < 4096
                                     ? cachedActors.count : 4096;
                    for (int i = 0; i < scan && tracked.size() < 96; ++i)
                    {
                        uint64_t ap = 0;
                        if (!readPtr(mem, cachedActors.dataArray + (uint64_t)i * 8, ap))
                            continue;

                        // Already tracked?
                        bool known = false;
                        for (const Tracked& t : tracked)
                            if (t.ptr == ap) { known = true; break; }
                        if (known) continue;

                        // Must have a capsule (real pawn gate).
                        LogicalTarget lt;
                        if (!readLogicalTarget(mem, offsets, ap, lt)) continue;
                        if (!lt.capsuleOk) continue;

                        // Must have a plausible component position.
                        Vec3f p{};
                        if (!readComponentPos(mem, offsets, ap, p)) continue;

                        // Must have an identity pin.
                        uint64_t classPtr = 0;
                        if (!readPtr(mem, ap + 0x10, classPtr)) continue;

                        // Classify: only Dino/Player enter the tracker.
                        Tracked t{};
                        t.ptr = ap;
                        t.classPtr = classPtr;
                        if (namesOk)
                        {
                            t.cls = classifyActor(mem, idmap, ap);
                            t.hasCls = true;
                            if (t.cls.kind != ClassKind::Dino &&
                                t.cls.kind != ClassKind::Player)
                                continue;   // PlayerStarts, triggers, etc. never enter
                        }
                        absorb(t, p);
                        t.radius = lt.capsuleRadius;
                        t.halfHeight = lt.halfHeight;
                        tracked.push_back(t);
                    }
                    ++rebuilds;

                    // Fallback: if classifier rejects everything, disable it.
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

                // -------------------------------------------------------
                // Per-target update + garbage collection
                // -------------------------------------------------------
                for (Tracked& t : tracked)
                    updateTarget(mem, offsets, t);

                tracked.erase(
                    std::remove_if(tracked.begin(), tracked.end(),
                        [](const Tracked& t) { return t.removeMe; }),
                    tracked.end());

                // -------------------------------------------------------
                // Anchor: prefer a Locked player pawn, else any Locked dino.
                // -------------------------------------------------------
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
                // Build draw list: ONLY Locked targets, smoothed positions
                // -------------------------------------------------------
                overlayTargets.clear();
                for (const Tracked& t : tracked)
                {
                    if (t.state != Tracked::State::Locked || !t.havePos)
                        continue;

                    const bool isPlayer = t.hasCls &&
                                          (t.cls.kind == ClassKind::Player);

                    ao::Target at{};
                    at.worldPos = t.draw;
                    at.boxW = t.radius * 2.0f;
                    at.boxH = t.halfHeight * 2.0f;
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

                // Sort by distance, cap at 64.
                std::sort(overlayTargets.begin(), overlayTargets.end(),
                    [](const ao::Target& a, const ao::Target& b)
                    { return a.distance < b.distance; });
                if (overlayTargets.size() > 64)
                    overlayTargets.resize(64);

            } // end if (actorsOk)
            // If actor read failed: overlayTargets keeps its LAST contents
            // (no blanking — the draw list from the previous good frame persists).

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
            // Status log
            // ===========================================================
            if (frame % 60 == 0)
            {
                int locked = 0;
                for (const Tracked& t : tracked)
                    if (t.state == Tracked::State::Locked) ++locked;
                printf("[status] frame=%llu tracked=%zu locked=%d drawn=%zu "
                       "names=%d camStable=%d overlay=%s\n",
                       frame,
                       tracked.size(),
                       locked,
                       overlayTargets.size(),
                       namesOk ? 1 : 0,
                       haveStable ? 1 : 0,
                       flags.overlayEnabled ? "ON" : "OFF");
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        } // end inner loop

        ao::overlayHide();
        ao::overlayShutdown();
        logInfo("detached");
        std::this_thread::sleep_for(std::chrono::seconds(2));

    } // end outer loop

    logShutdown();
    return 0;
}