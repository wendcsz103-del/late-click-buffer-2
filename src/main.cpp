// Late Click Buffer - GD 2.2081
// A click is held until the first tick where clicking keeps the player alive.
// "Alive" is decided by running the real game physics forward from a checkpoint
// snapshot, then restoring the snapshot.
#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include <chrono>
#include <deque>

using namespace geode::prelude;

namespace {
    struct Ev { bool down; int button; bool p1; int waited = 0; };

    std::deque<Ev> g_queue;
    bool g_sim = false;      // running a prediction
    bool g_simDied = false;
    bool g_simDone = false;  // level ended during prediction (counts as survived)
    bool g_deliver = false;  // we are sending a held event ourselves
    bool g_on = true;

    constexpr float STEP = 1.f / 240.f;

    PlayLayer* pl() { return PlayLayer::get(); }

    // true = clicking on the current tick survives `horizon` ticks
    bool clickSurvives(GJBaseGameLayer* gl, int button, bool p1) {
        auto layer = pl();
        if (!layer || (GJBaseGameLayer*)layer != gl) return true;

        int horizon = (int)Mod::get()->getSettingValue<int64_t>("horizon");
        double budget = Mod::get()->getSettingValue<double>("budget-ms");

        auto cp = layer->createCheckpoint();
        if (!cp) return true;
        cp->retain();

        auto t0 = std::chrono::steady_clock::now();
        g_sim = true; g_simDied = false; g_simDone = false;

        gl->handleButton(true, button, p1);
        bool timedOut = false;
        for (int i = 0; i < horizon && !g_simDied && !g_simDone; ++i) {
            gl->processCommands(STEP, false, i == horizon - 1);
            if (gl->m_player1 && gl->m_player1->m_isDead) g_simDied = true;
            if ((i & 7) == 7) {
                double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                if (ms > budget) { timedOut = true; break; }
            }
        }
        bool died = g_simDied;
        g_sim = false;
        layer->loadFromCheckpoint(cp);
        cp->release();

        if (timedOut) return true; // out of time: fail open, click now
        return !died;
    }

    bool active(GJBaseGameLayer* gl) {
        if (!g_on || g_sim || g_deliver) return false;
        if (!Mod::get()->getSettingValue<bool>("enabled")) return false;
        auto layer = pl();
        if (!layer || (GJBaseGameLayer*)layer != gl) return false;
        return !layer->m_isPaused && !layer->m_levelEndAnimationStarted;
    }
}

class $modify(LCBBase, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        // only the jump button is buffered
        if (button != 1 || !active(this)) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        if (!g_queue.empty()) { // keep order behind a held click
            g_queue.push_back({down, button, isPlayer1});
            return;
        }
        if (!down || clickSurvives(this, button, isPlayer1)) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        g_queue.push_back({down, button, isPlayer1});
    }

    void processCommands(float dt, bool halfTick, bool lastTick) {
        if (!g_sim && !g_queue.empty()) {
            if (!active(this)) {
                g_queue.clear();
            } else {
                auto& e = g_queue.front();
                int maxWait = (int)Mod::get()->getSettingValue<int64_t>("max-wait");
                bool go = !e.down || ++e.waited > maxWait
                          || clickSurvives(this, e.button, e.p1);
                if (go) {
                    Ev ev = e;
                    g_queue.pop_front();
                    g_deliver = true;
                    GJBaseGameLayer::handleButton(ev.down, ev.button, ev.p1);
                    g_deliver = false;
                }
            }
        }
        GJBaseGameLayer::processCommands(dt, halfTick, lastTick);
    }
};

class $modify(LCBPlay, PlayLayer) {
    void destroyPlayer(PlayerObject* p, GameObject* o) {
        if (g_sim) { g_simDied = true; return; }
        g_queue.clear();
        PlayLayer::destroyPlayer(p, o);
    }
    void playEndAnimationToPos(CCPoint pos) {
        if (g_sim) { g_simDone = true; return; }
        PlayLayer::playEndAnimationToPos(pos);
    }
    void levelComplete() {
        if (g_sim) { g_simDone = true; return; }
        PlayLayer::levelComplete();
    }
    void resetLevel() { g_queue.clear(); PlayLayer::resetLevel(); }
    void onQuit() { g_queue.clear(); PlayLayer::onQuit(); }
};

class $modify(LCBAudio, FMODAudioEngine) {
    int playEffect(gd::string path, float speed, float unk, float volume) {
        if (g_sim && Mod::get()->getSettingValue<bool>("mute-sim")) return 0;
        return FMODAudioEngine::playEffect(path, speed, unk, volume);
    }
};
