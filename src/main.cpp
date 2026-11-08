#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

using namespace geode::prelude;

namespace {
struct Obstacle {
    float left;
    float centerY;
    float height;
};

std::vector<Obstacle> s_obstacles;
float s_cooldown = 0.f;
float s_tapTime = 0.f;
bool s_holdingJump = false;

bool isEnabled() {
    return Mod::get()->getSettingValue<bool>("enabled");
}

void releaseJump(PlayLayer* layer) {
    if (s_holdingJump && layer && layer->m_player1) {
        layer->m_player1->releaseButton(PlayerButton::Jump);
    }
    s_holdingJump = false;
    s_tapTime = 0.f;
}

void cacheObstacles(PlayLayer* layer) {
    s_obstacles.clear();
    if (!layer->m_objects) {
        log::warn("Autopilot: level has no object list");
        return;
    }

    for (auto* object : CCArrayExt<GameObject*>(layer->m_objects)) {
        if (!object) continue;
        switch (object->m_objectType) {
            case GameObjectType::Solid:
            case GameObjectType::Slope:
            case GameObjectType::Hazard: {
                auto rect = object->getObjectRect();
                s_obstacles.push_back({rect.getMinX(), rect.getMidY(), rect.size.height});
                break;
            }
            default:
                break;
        }
    }

    std::sort(s_obstacles.begin(), s_obstacles.end(), [](auto const& a, auto const& b) {
        return a.left < b.left;
    });
    log::info("Autopilot cached {} obstacles", s_obstacles.size());
}

void autopilotStep(PlayLayer* layer, float dt) {
    auto player = layer ? layer->m_player1 : nullptr;
    if (!player || player->m_isDead || !isEnabled()) {
        releaseJump(layer);
        return;
    }

    if (s_holdingJump) {
        s_tapTime -= dt;
        if (s_tapTime <= 0.f) releaseJump(layer);
    }
    s_cooldown = std::max(0.f, s_cooldown - dt);

    // This initial controller supports cube/robot-style grounded jumps.
    // Flying and other modes need separate movement logic.
    if (player->m_isShip || player->m_isDart || player->m_isBird ||
        player->m_isBall || player->m_isSpider || player->m_isSwing ||
        !player->m_isOnGround || s_cooldown > 0.f || s_holdingJump) {
        return;
    }

    float px = player->getPositionX();
    float py = player->getPositionY();
    float speed = std::max(0.f, player->m_playerSpeed * 355.f);
    float jumpLead = static_cast<float>(Mod::get()->getSettingValue<double>("jump-lead"));
    float reach = std::clamp(speed * jumpLead, 24.f, 180.f);

    for (auto const& obstacle : s_obstacles) {
        if (obstacle.left < px - 8.f) continue;
        if (obstacle.left > px + reach) break;
        if (std::fabs(obstacle.centerY - py) > obstacle.height * 0.5f + 24.f) continue;

        player->pushButton(PlayerButton::Jump);
        s_holdingJump = true;
        s_tapTime = 0.06f;
        s_cooldown = 0.18f;
        log::info("Autopilot jump at x={:.1f}", px);
        return;
    }
}
} // namespace

class $modify(AutopilotPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        cacheObstacles(this);
        s_cooldown = 0.f;
        s_tapTime = 0.f;
        s_holdingJump = false;
    }

    void update(float dt) {
        PlayLayer::update(dt);
        autopilotStep(this, dt);
    }

    void resetLevel() {
        releaseJump(this);
        s_cooldown = 0.f;
        PlayLayer::resetLevel();
    }
};
