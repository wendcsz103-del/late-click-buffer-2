#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <chrono>
#include <deque>

using namespace geode::prelude;

namespace {
    struct Held { bool down; int button; bool p1; int waited; };

    std::deque<Held> g_held;
    bool g_inSim = false;       // true while we are testing a click
    bool g_simDead = false;     // set when the test run kills the player
    bool g_delivering = false;  // true while we send a held click ourselves

    constexpr float STEP = 1.f / 240.f;

    bool enabled() {
        return Mod::get()->getSettingValue<bool>("enabled");
    }

    // Returns true if clicking now survives `horizon` ticks.
    // Uses the game's own death flag. Restores the level afterwards.
    bool survives(GJBaseGameLayer* gl, int button, bool p1) {
        auto pl = PlayLayer::get();
        if (!pl || static_cast<GJBaseGameLayer*>(pl) != gl) return true;

        int horizon = (int)Mod::get()->getSettingValue<int64_t>("horizon");
        double budget = Mod::get()->getSettingValue<double>("budget-ms");

        auto cp = pl->createCheckpoint();
        if (!cp) return true;

        auto t0 = std::chrono::steady_clock::now();
        g_inSim = true;
        g_simDead = false;

        gl->handleButton(true, button, p1);
        bool timedOut = false;
        for (int i = 0; i < horizon && !g_simDead; ++i) {
            gl->processCommands(STEP, false, false);
            if (gl->m_player1 && gl->m_player1->m_isDead) {
                g_simDead = true;
                break;
            }
            if ((i & 7) == 7) {
                double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                if (ms > budget) { timedOut = true; break; }
            }
        }

        bool died = g_simDead;
        g_inSim = false;
        g_simDead = false;

        pl->loadFromCheckpoint(cp);

        if (timedOut) return true; // out of time, send the click now
        return !died;
    }
}

class $modify(LCBBase, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        // Only buffer the jump button, and only when the mod is on and we are not testing.
        if (button != 1 || g_inSim || g_delivering || !enabled()) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        // Keep order: if a click is already held, queue this event behind it.
        if (!g_held.empty()) {
            g_held.push_back({down, button, isPlayer1, 0});
            return;
        }

        if (!down || survives(this, button, isPlayer1)) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        g_held.push_back({down, button, isPlayer1, 0});
    }

    void processCommands(float dt, bool halfTick, bool lastTick) {
        if (!g_inSim && !g_held.empty()) {
            if (!enabled()) {
                g_held.clear();
            } else {
                auto& h = g_held.front();
                int maxWait = (int)Mod::get()->getSettingValue<int64_t>("max-wait");
                bool go = !h.down || ++h.waited > maxWait
                          || survives(this, h.button, h.p1);
                if (go) {
                    Held ev = h;
                    g_held.pop_front();
                    g_delivering = true;
                    GJBaseGameLayer::handleButton(ev.down, ev.button, ev.p1);
                    g_delivering = false;
                }
            }
        }
        GJBaseGameLayer::processCommands(dt, halfTick, lastTick);
    }
};
