#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/ui/Popup.hpp>
#include <vector>
#include <chrono>
#include <algorithm>

using namespace geode::prelude;

namespace {
    constexpr float TICK = 0.25f;

    constexpr uint64_t typeBit(GameObjectType type) {
        return 1ull << static_cast<int>(type);
    }

    constexpr uint64_t COLLIDING_TYPES = 
        typeBit(GameObjectType::Solid) | 
        typeBit(GameObjectType::Hazard) | 
        typeBit(GameObjectType::Slope) |
        typeBit(GameObjectType::CollisionObject) | 
        typeBit(GameObjectType::AnimatedHazard);

    constexpr uint64_t INTERACT_TYPES =
        typeBit(GameObjectType::YellowJumpPad) | typeBit(GameObjectType::PinkJumpPad) | 
        typeBit(GameObjectType::RedJumpPad) | typeBit(GameObjectType::GravityPad) | 
        typeBit(GameObjectType::SpiderPad) | typeBit(GameObjectType::YellowJumpRing) | 
        typeBit(GameObjectType::PinkJumpRing) | typeBit(GameObjectType::RedJumpRing) | 
        typeBit(GameObjectType::GravityRing) | typeBit(GameObjectType::GreenRing) | 
        typeBit(GameObjectType::DropRing) | typeBit(GameObjectType::CustomRing) | 
        typeBit(GameObjectType::DashRing) | typeBit(GameObjectType::GravityDashRing) | 
        typeBit(GameObjectType::SpiderOrb);

    struct ObjSnapshot {
        GameObject* object;
        bool isActivated;
        EffectGameObject* effect;
        bool activated;
    };

    struct GhostState {
        PlayerObject* ghost = nullptr;
        CCLayer* ghostLayer = nullptr;
        bool simulating = false;
        bool ghostDied = false;
        bool held = false;
        std::vector<ObjSnapshot> snapshots;
        std::unordered_set<GameObject*> usedObjects;
        float tickLength = TICK;
    } g_ghost;

    struct AutoplayController {
        bool active = false;
        bool takeOver = false;
        int lookaheadHorizon = 80;
        
        int attemptTick = 0;
        bool buttonState = false;
        
        std::vector<int> plannedFlips;
        int verifyAtTick = -1;
        int planSafeUntil = -1;

        std::vector<std::pair<int, bool>> attemptFlips;
        std::vector<std::pair<int, bool>> knownFlips;
        int knownUntilTick = 0;
        size_t replayIdx = 0;
        int bestReachedTick = 0;
        std::vector<std::pair<int, bool>> bestFlips;

        CCLabelBMFont* tagLabel = nullptr;
        CCDrawNode* trailNode = nullptr;
        std::vector<CCPoint> trailPoints;

        void resetForNewAttempt() {
            attemptTick = 0;
            buttonState = false;
            plannedFlips.clear();
            attemptFlips.clear();
            trailPoints.clear();
            verifyAtTick = -1;
            planSafeUntil = -1;
            replayIdx = 0;
        }

        void clearLearnedMemory() {
            knownFlips.clear();
            bestFlips.clear();
            knownUntilTick = 0;
            bestReachedTick = 0;
            replayIdx = 0;
        }
    } g_autoplay;

    void startAutoplay(PlayLayer* layer);
    void stopAutoplay(PlayLayer* layer);

    bool isGhost(PlayerObject* p) {
        return p && p == g_ghost.ghost;
    }

    bool isFlyingMode(PlayerObject* p) {
        return p && (p->m_isDart || p->m_isBird || p->m_isSwing || p->m_isShip);
    }

    const char* getModeName(PlayerObject* p) {
        if (!p) return "Unknown";
        if (p->m_isDart) return "Wave";
        if (p->m_isShip) return "Ship";
        if (p->m_isBird) return "UFO";
        if (p->m_isBall) return "Ball";
        if (p->m_isRobot) return "Robot";
        if (p->m_isSpider) return "Spider";
        if (p->m_isSwing) return "Swing";
        return "Cube";
    }

    void sweepGhostLayer() {
        if (!g_ghost.ghostLayer) return;
        auto* children = g_ghost.ghostLayer->getChildren();
        while (children && children->count() > 1) {
            auto* node = static_cast<CCNode*>(children->lastObject());
            if (node != g_ghost.ghost) {
                g_ghost.ghostLayer->removeChild(node, true);
            } else {
                break;
            }
        }
    }

    void copyState(PlayerObject* ghost, PlayerObject* real) {
        ghost->setVisible(false);
        ghost->resetTouchedRings(true);
        ghost->copyAttributes(real);
        ghost->setScale(real->getScale());
        ghost->setContentSize(real->getContentSize());
        ghost->m_width = real->m_width;
        ghost->m_height = real->m_height;
        ghost->m_gravityMod = real->m_gravityMod;
        ghost->m_isOnGround = real->m_isOnGround;
        ghost->m_isUpsideDown = real->m_isUpsideDown;
        ghost->m_vehicleSize = real->m_vehicleSize;
        ghost->m_playerSpeed = real->m_playerSpeed;
        ghost->m_isDashing = real->m_isDashing;
        ghost->m_isShip = real->m_isShip;
        ghost->m_isBird = real->m_isBird;
        ghost->m_isBall = real->m_isBall;
        ghost->m_isDart = real->m_isDart;
        ghost->m_isRobot = real->m_isRobot;
        ghost->m_isSpider = real->m_isSpider;
        ghost->m_isSwing = real->m_isSwing;
        ghost->m_yVelocity = real->m_yVelocity;
        ghost->m_fallSpeed = real->m_fallSpeed;
        ghost->m_position = real->getPosition();
        ghost->setPosition(real->getPosition());
        ghost->m_lastPosition = real->m_lastPosition;
    }

    void snapshotObject(GameObject* obj) {
        if (!obj || g_ghost.usedObjects.contains(obj)) return;
        g_ghost.usedObjects.insert(obj);
        ObjSnapshot s { obj, obj->m_isActivated, nullptr, false };
        if (auto* effect = typeinfo_cast<EffectGameObject*>(obj)) {
            s.effect = effect;
            s.activated = effect->m_activated;
        }
        g_ghost.snapshots.push_back(s);
    }

    void restoreObjects() {
        for (auto const& s : g_ghost.snapshots) {
            s.object->m_isActivated = s.isActivated;
            if (s.effect) {
                s.effect->m_activated = s.activated;
            }
        }
        g_ghost.snapshots.clear();
        g_ghost.usedObjects.clear();
    }

    PlayerObject* getOrCreateGhost(PlayLayer* layer) {
        if (!g_ghost.ghostLayer) {
            g_ghost.ghostLayer = CCLayer::create();
            g_ghost.ghostLayer->retain();
        }
        if (!g_ghost.ghost) {
            g_ghost.ghost = PlayerObject::create(1, 1, layer, g_ghost.ghostLayer, true);
            g_ghost.ghost->retain();
            g_ghost.ghost->setVisible(false);
            g_ghost.ghostLayer->addChild(g_ghost.ghost);
        }
        return g_ghost.ghost;
    }

    void setGhostButton(bool down) {
        g_ghost.held = down;
        if (down) {
            g_ghost.ghost->pushButton(PlayerButton::Jump);
        } else {
            g_ghost.ghost->releaseButton(PlayerButton::Jump);
        }
    }

    int simulatePlan(PlayLayer* layer, PlayerObject* real, std::vector<int> const& relativeFlips, int horizon) {
        auto* ghost = getOrCreateGhost(layer);
        g_ghost.simulating = true;
        g_ghost.ghostDied = false;
        copyState(ghost, real);
        setGhostButton(g_autoplay.buttonState);

        float dt = g_ghost.tickLength <= 0.001f ? TICK : g_ghost.tickLength;
        int survived = 0;

        for (int tick = 0; tick < horizon; ++tick) {
            for (int flipTick : relativeFlips) {
                if (flipTick == tick) {
                    setGhostButton(!g_ghost.held);
                }
            }

            ghost->update(dt);
            ghost->updateRotation(dt);
            if (g_ghost.ghostDied) break;

            layer->checkCollisions(ghost, dt, false);
            if (g_ghost.ghostDied) break;

            ++survived;
        }

        restoreObjects();
        ghost->stopAllActions();
        g_ghost.simulating = false;
        return survived;
    }

    int planSearch(PlayLayer* layer, PlayerObject* real, std::vector<int>& currentRelPlan, int depth, int horizon, std::vector<int>& bestPlan, int& bestSurvived) {
        int survived = simulatePlan(layer, real, currentRelPlan, horizon);
        if (survived > bestSurvived) {
            bestSurvived = survived;
            bestPlan = currentRelPlan;
        }
        if (survived >= horizon || depth <= 0) {
            return survived;
        }

        bool holdMode = isFlyingMode(real);
        int lastFlip = currentRelPlan.empty() ? -1 : currentRelPlan.back();
        int stepBack = holdMode ? 4 : 8;

        for (int b = 2; b < survived - lastFlip; b += stepBack) {
            int flipTick = survived - b;
            if (flipTick <= lastFlip) break;

            currentRelPlan.push_back(flipTick);
            if (!holdMode) {
                currentRelPlan.push_back(flipTick + 4);
            }

            int res = planSearch(layer, real, currentRelPlan, depth - 1, horizon, bestPlan, bestSurvived);
            
            currentRelPlan.pop_back();
            if (!holdMode) currentRelPlan.pop_back();

            if (res >= horizon) return res;
        }

        return bestSurvived;
    }

    void applyAutoplayButton(PlayLayer* layer, bool down) {
        if (g_autoplay.buttonState == down) return;
        g_autoplay.buttonState = down;
        g_autoplay.attemptFlips.push_back({ g_autoplay.attemptTick, down });

        if (down) {
            layer->m_player1->pushButton(PlayerButton::Jump);
        } else {
            layer->m_player1->releaseButton(PlayerButton::Jump);
        }
    }

    void drawAutoplayTrail() {
        if (!g_autoplay.trailNode) return;
        g_autoplay.trailNode->clear();
        auto const& pts = g_autoplay.trailPoints;
        for (size_t i = 1; i < pts.size(); ++i) {
            if (ccpDistance(pts[i - 1], pts[i]) < 120.f) {
                g_autoplay.trailNode->drawSegment(pts[i - 1], pts[i], 1.2f, { 0.2f, 1.f, 0.5f, 0.9f });
            }
        }
    }

    void stopAutoplay(PlayLayer* layer) {
        if (!g_autoplay.active) return;
        g_autoplay.active = false;

        if (layer && layer->m_player1) {
            applyAutoplayButton(layer, false);
        }
        if (g_autoplay.tagLabel) {
            g_autoplay.tagLabel->removeFromParent();
            g_autoplay.tagLabel = nullptr;
        }
        if (g_autoplay.trailNode) {
            g_autoplay.trailNode->removeFromParent();
            g_autoplay.trailNode = nullptr;
        }
    }

    void startAutoplay(PlayLayer* layer) {
        if (!layer || layer->m_isPlatformer) return;

        layer->m_isTestMode = true;
        g_autoplay.active = true;
        g_autoplay.resetForNewAttempt();

        auto win = CCDirector::get()->getWinSize();
        g_autoplay.tagLabel = CCLabelBMFont::create("AUTOPLAY", "bigFont.fnt");
        g_autoplay.tagLabel->setScale(0.45f);
        g_autoplay.tagLabel->setColor({ 255, 215, 90 });
        g_autoplay.tagLabel->setOpacity(220);
        g_autoplay.tagLabel->setPosition({ win.width / 2.f, win.height - 25.f });
        layer->addChild(g_autoplay.tagLabel, 1000);

        g_autoplay.trailNode = CCDrawNode::create();
        layer->m_objectLayer->addChild(g_autoplay.trailNode, 999);
    }

    void executeAutoplayTick(PlayLayer* layer, PlayerObject* p1) {
        if (!g_autoplay.active || p1->m_isDead) return;

        int currentTick = g_autoplay.attemptTick++;
        if (currentTick % 4 == 0) {
            g_autoplay.trailPoints.push_back(p1->getPosition());
            drawAutoplayTrail();
        }

        if (currentTick < g_autoplay.knownUntilTick && g_autoplay.replayIdx < g_autoplay.knownFlips.size()) {
            while (g_autoplay.replayIdx < g_autoplay.knownFlips.size() && 
                   g_autoplay.knownFlips[g_autoplay.replayIdx].first <= currentTick) {
                if (g_autoplay.knownFlips[g_autoplay.replayIdx].first == currentTick) {
                    applyAutoplayButton(layer, g_autoplay.knownFlips[g_autoplay.replayIdx].second);
                }
                ++g_autoplay.replayIdx;
            }
            return;
        }

        while (!g_autoplay.plannedFlips.empty() && g_autoplay.plannedFlips.front() <= currentTick) {
            g_autoplay.plannedFlips.erase(g_autoplay.plannedFlips.begin());
            applyAutoplayButton(layer, !g_autoplay.buttonState);
        }

        if (currentTick < g_autoplay.verifyAtTick) return;

        int horizon = isFlyingMode(p1) ? (g_autoplay.lookaheadHorizon - 20) : g_autoplay.lookaheadHorizon;
        std::vector<int> candidatePlan;
        std::vector<int> bestPlan;
        int bestSurvived = 0;

        planSearch(layer, p1, candidatePlan, 4, horizon, bestPlan, bestSurvived);

        if (!bestPlan.empty()) {
            g_autoplay.plannedFlips.clear();
            for (int rel : bestPlan) {
                g_autoplay.plannedFlips.push_back(currentTick + rel);
            }
        }

        g_autoplay.planSafeUntil = currentTick + bestSurvived;
        g_autoplay.verifyAtTick = currentTick + (bestSurvived >= horizon ? 8 : 1);

        sweepGhostLayer();
    }
}

// ==========================================
//           AUTOPLAY CONTROL GUI
// ==========================================
class AutoplayMenu : public geode::Popup<> {
protected:
    CCLabelBMFont* m_statusLabel = nullptr;
    CCLabelBMFont* m_statsLabel = nullptr;
    CCLabelBMFont* m_horizonLabel = nullptr;
    ButtonSprite* m_toggleBtnSprite = nullptr;
    ButtonSprite* m_takeoverBtnSprite = nullptr;

    bool setup() override {
        this->setTitle("Autoplay Control");
        auto win = m_mainLayer->getContentSize();

        m_statusLabel = CCLabelBMFont::create("", "bigFont.fnt");
        m_statusLabel->setScale(0.45f);
        m_statusLabel->setPosition({ win.width / 2.f, win.height - 50.f });
        m_mainLayer->addChild(m_statusLabel);

        auto* menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        m_mainLayer->addChild(menu);

        m_toggleBtnSprite = ButtonSprite::create("Toggle Autoplay");
        auto* toggleBtn = CCMenuItemSpriteExtra::create(
            m_toggleBtnSprite, this, menu_selector(AutoplayMenu::onToggleAutoplay)
        );
        toggleBtn->setPosition({ win.width / 2.f, win.height - 85.f });
        menu->addChild(toggleBtn);

        m_takeoverBtnSprite = ButtonSprite::create("Takeover: OFF");
        auto* takeoverBtn = CCMenuItemSpriteExtra::create(
            m_takeoverBtnSprite, this, menu_selector(AutoplayMenu::onToggleTakeover)
        );
        takeoverBtn->setPosition({ win.width / 2.f - 65.f, win.height - 125.f });
        menu->addChild(takeoverBtn);

        auto* clearMemorySprite = ButtonSprite::create("Reset Memory");
        auto* clearBtn = CCMenuItemSpriteExtra::create(
            clearMemorySprite, this, menu_selector(AutoplayMenu::onClearMemory)
        );
        clearBtn->setPosition({ win.width / 2.f + 65.f, win.height - 125.f });
        menu->addChild(clearBtn);

        auto* horizonTitle = CCLabelBMFont::create("Lookahead:", "bigFont.fnt");
        horizonTitle->setScale(0.35f);
        horizonTitle->setPosition({ win.width / 2.f - 40.f, win.height - 160.f });
        m_mainLayer->addChild(horizonTitle);

        m_horizonLabel = CCLabelBMFont::create("", "goldFont.fnt");
        m_horizonLabel->setScale(0.45f);
        m_horizonLabel->setPosition({ win.width / 2.f + 25.f, win.height - 160.f });
        m_mainLayer->addChild(m_horizonLabel);

        auto* minusSpr = ButtonSprite::create("-");
        minusSpr->setScale(0.7f);
        auto* minusBtn = CCMenuItemSpriteExtra::create(minusSpr, this, menu_selector(AutoplayMenu::onHorizonMinus));
        minusBtn->setPosition({ win.width / 2.f + 60.f, win.height - 160.f });
        menu->addChild(minusBtn);

        auto* plusSpr = ButtonSprite::create("+");
        plusSpr->setScale(0.7f);
        auto* plusBtn = CCMenuItemSpriteExtra::create(plusSpr, this, menu_selector(AutoplayMenu::onHorizonPlus));
        plusBtn->setPosition({ win.width / 2.f + 85.f, win.height - 160.f });
        menu->addChild(plusBtn);

        auto* statsCard = CCScale9Sprite::create("square02_small.png");
        statsCard->setContentSize({ win.width - 40.f, 42.f });
        statsCard->setOpacity(110);
        statsCard->setPosition({ win.width / 2.f, 32.f });
        m_mainLayer->addChild(statsCard);

        m_statsLabel = CCLabelBMFont::create("", "chatFont.fnt");
        m_statsLabel->setScale(0.6f);
        m_statsLabel->setPosition({ win.width / 2.f, 32.f });
        m_mainLayer->addChild(m_statsLabel);

        this->updateUI();
        this->scheduleUpdate();

        return true;
    }

    void update(float dt) override {
        updateUI();
    }

    void updateUI() {
        auto* playLayer = GameManager::get()->m_playLayer;

        if (g_autoplay.active) {
            m_statusLabel->setString("Status: ACTIVE (Test Mode)");
            m_statusLabel->setColor({ 100, 255, 120 });
            m_toggleBtnSprite->setString("Stop Autoplay");
        } else {
            m_statusLabel->setString("Status: INACTIVE");
            m_statusLabel->setColor({ 255, 120, 120 });
            m_toggleBtnSprite->setString("Start Autoplay");
        }

        m_takeoverBtnSprite->setString(g_autoplay.takeOver ? "Takeover: ON" : "Takeover: OFF");
        m_horizonLabel->setString(fmt::format("{}t", g_autoplay.lookaheadHorizon).c_str());

        if (playLayer && playLayer->m_player1) {
            m_statsLabel->setString(fmt::format(
                "Mode: {} | Tick: {} | Known: {}t | Best: {}t",
                getModeName(playLayer->m_player1),
                g_autoplay.attemptTick,
                g_autoplay.knownUntilTick,
                g_autoplay.bestReachedTick
            ).c_str());
        } else {
            m_statsLabel->setString("Open a level to play with Autoplay.");
        }
    }

    void onToggleAutoplay(CCObject*) {
        auto* playLayer = GameManager::get()->m_playLayer;
        if (!playLayer) {
            FLAlertLayer::create("Autoplay", "Open a level first to enable Autoplay.", "OK")->show();
            return;
        }

        if (g_autoplay.active) {
            stopAutoplay(playLayer);
        } else {
            startAutoplay(playLayer);
        }
        updateUI();
    }

    void onToggleTakeover(CCObject*) {
        g_autoplay.takeOver = !g_autoplay.takeOver;
        updateUI();
    }

    void onClearMemory(CCObject*) {
        g_autoplay.clearLearnedMemory();
        FLAlertLayer::create("Autoplay", "Learned memory cleared for this session.", "OK")->show();
        updateUI();
    }

    void onHorizonMinus(CCObject*) {
        g_autoplay.lookaheadHorizon = std::max(40, g_autoplay.lookaheadHorizon - 10);
        updateUI();
    }

    void onHorizonPlus(CCObject*) {
        g_autoplay.lookaheadHorizon = std::min(160, g_autoplay.lookaheadHorizon + 10);
        updateUI();
    }

public:
    static inline AutoplayMenu* s_instance = nullptr;

    static AutoplayMenu* create() {
        auto ret = new AutoplayMenu();
        if (ret && ret->initAnchored(360.f, 230.f, "GJ_square01.png")) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    ~AutoplayMenu() override {
        if (s_instance == this) {
            s_instance = nullptr;
        }
    }

    static void toggle() {
        if (s_instance) {
            s_instance->onClose(nullptr);
            s_instance = nullptr;
        } else {
            s_instance = AutoplayMenu::create();
            if (s_instance) {
                s_instance->show();
            }
        }
    }
};

// ==========================================
//                 HOOKS
// ==========================================

class $modify(CCKeyboardDispatcher) {
    void dispatchKeyboardMSG(enumKeyCodes key, bool down, bool isKeyRepeat) {
        if (down && !isKeyRepeat) {
            if (key == enumKeyCodes::KEY_GraveAccent || 
                key == static_cast<enumKeyCodes>(0xC0) || 
                key == static_cast<enumKeyCodes>(96)) {
                AutoplayMenu::toggle();
                return;
            }
        }
        CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, isKeyRepeat);
    }
};

class $modify(PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        g_autoplay.resetForNewAttempt();
        return true;
    }

    void onQuit() {
        stopAutoplay(this);
        PlayLayer::onQuit();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        if (g_autoplay.active) {
            g_autoplay.resetForNewAttempt();
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g_ghost.simulating && isGhost(player)) {
            g_ghost.ghostDied = true;
            return;
        }

        if (g_autoplay.active && player == m_player1 && !player->m_isDead) {
            int reached = g_autoplay.attemptTick;
            if (reached > g_autoplay.bestReachedTick) {
                g_autoplay.bestReachedTick = reached;
                g_autoplay.bestFlips = g_autoplay.attemptFlips;
            }
            g_autoplay.knownUntilTick = std::max(0, g_autoplay.bestReachedTick - 30);
            g_autoplay.knownFlips.clear();
            for (auto const& f : g_autoplay.bestFlips) {
                if (f.first < g_autoplay.knownUntilTick) {
                    g_autoplay.knownFlips.push_back(f);
                }
            }
        }

        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        if (g_autoplay.active) {
            log::info("Autoplay successfully beat the level in test mode!");
        }
        PlayLayer::levelComplete();
    }
};

class $modify(PlayerObject) {
    void update(float dt) {
        if (g_ghost.simulating) {
            PlayerObject::update(dt);
            return;
        }

        auto* layer = GameManager::get()->m_playLayer;
        if (layer && this == layer->m_player1) {
            g_ghost.tickLength = dt;
            if (g_autoplay.active) {
                executeAutoplayTick(layer, this);
            }
        }

        PlayerObject::update(dt);
    }
};

class $modify(GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        if (g_autoplay.active && !g_ghost.simulating) {
            if (!g_autoplay.takeOver) {
                return;
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int objectCount, float dt) {
        if (!g_ghost.simulating) {
            GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
            return;
        }

        static gd::vector<GameObject*> relevant;
        relevant.clear();

        for (int i = 0; i < objectCount; ++i) {
            auto* obj = (*objects)[i];
            if (!obj) continue;

            auto type = static_cast<unsigned>(obj->m_objectType);
            uint64_t bit = type < 64 ? (1ull << type) : 0;
            if ((COLLIDING_TYPES | INTERACT_TYPES) & bit) {
                relevant.push_back(obj);
            }
        }

        GJBaseGameLayer::collisionCheckObjects(player, &relevant, static_cast<int>(relevant.size()), dt);
    }

    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (g_ghost.simulating) {
            if (!isGhost(player) || !object) return false;
            snapshotObject(object);
            return true;
        }
        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
    }
};
