#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJGameLevel.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <algorithm>
#include <cmath>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Level cache: every solid / hazard / ring, sorted by left edge, built once
// when the level loads so the per-frame scan is a cheap binary search.
// kind: 0 = solid, 1 = hazard, 2 = ring/orb
// ---------------------------------------------------------------------------
struct Obst {
	float l, r, cy, h;
	int kind;
};

struct Params {
	float jumpLead = 0.14f;   // seconds before an obstacle that we react
	float shipMargin = 40.f;  // units of clearance when steering
};

static std::vector<Obst> s_obst;
static Params s_params;

// bot state
static bool s_holding = false;
static float s_tapTimer = -1.f;
static float s_cooldown = 0.f;
static float s_lastTapX = -1.f;

// auto safe mode: set the moment the bot touches the controls in an attempt,
// cleared on the next reset. While set, nothing from this attempt is saved.
static bool s_tainted = false;
static bool s_notified = false;

// AI state
static int s_deathsSinceAsk = 0;
static bool s_inFlight = false;
static async::TaskHolder<web::WebResponse> s_listener;

static bool cfgBool(char const* key) {
	return Mod::get()->getSettingValue<bool>(key);
}

static char const* modeName(PlayerObject* p) {
	if (p->m_isShip) return "ship";
	if (p->m_isBird) return "ufo";
	if (p->m_isBall) return "ball";
	if (p->m_isDart) return "wave";
	if (p->m_isRobot) return "robot";
	if (p->m_isSpider) return "spider";
	if (p->m_isSwing) return "swing";
	return "cube";
}

// ---------------------------------------------------------------------------
// Input helpers
// ---------------------------------------------------------------------------
static void tap(GJBaseGameLayer* g, float duration) {
	if (s_holding) return;
	g->m_player1->pushButton(PlayerButton::Jump);
	s_holding = true;
	s_tapTimer = duration;
	s_lastTapX = g->m_player1->getPositionX();
}

static void setHold(GJBaseGameLayer* g, bool want) {
	if (want && !s_holding) {
		g->m_player1->pushButton(PlayerButton::Jump);
		s_holding = true;
		s_tapTimer = -1.f; // steering mode: no auto release
	}
	else if (!want && s_holding) {
		g->m_player1->releaseButton(PlayerButton::Jump);
		s_holding = false;
	}
}

static void releaseIfDue(GJBaseGameLayer* g, float dt) {
	if (s_holding && s_tapTimer > 0.f) {
		s_tapTimer -= dt;
		if (s_tapTimer <= 0.f) {
			g->m_player1->releaseButton(PlayerButton::Jump);
			s_holding = false;
		}
	}
}

// ---------------------------------------------------------------------------
// Obstacle queries
// ---------------------------------------------------------------------------
static std::vector<Obst>::const_iterator firstAtOrAfter(float x) {
	return std::lower_bound(s_obst.begin(), s_obst.end(), x, [](Obst const& o, float v) {
		return o.l < v;
	});
}

// first solid/hazard ahead of the player (within `reach`) that overlaps the
// player's vertical band
static bool blockingAhead(float px, float py, float reach) {
	for (auto it = firstAtOrAfter(px - 5.f); it != s_obst.end() && it->l <= px + reach; ++it) {
		if (it->kind == 2) continue;
		if (std::fabs(it->cy - py) < 10.f + it->h / 2.f) return true;
	}
	return false;
}

static bool ringNearby(float px, float py) {
	for (auto it = firstAtOrAfter(px - 15.f); it != s_obst.end() && it->l <= px + 25.f; ++it) {
		if (it->kind == 2 && std::fabs(it->cy - py) < 45.f) return true;
	}
	return false;
}

// where the safe corridor's middle is, for ship/wave/ufo steering
static float corridorTarget(float px, float py, float lookahead) {
	float ceilY = py + 90.f;
	float floorY = py - 90.f;
	for (auto it = firstAtOrAfter(px); it != s_obst.end() && it->l <= px + lookahead; ++it) {
		if (it->kind == 2) continue;
		if (it->cy > py) ceilY = std::min(ceilY, it->cy - it->h / 2.f);
		else floorY = std::max(floorY, it->cy + it->h / 2.f);
	}
	float margin = s_params.shipMargin;
	float lo = floorY + margin * 0.5f;
	float hi = ceilY - margin * 0.5f;
	if (lo > hi) return (floorY + ceilY) / 2.f;
	return std::clamp(py, lo, hi) == py ? py : (py < lo ? lo : hi);
}

// ---------------------------------------------------------------------------
// The bot: one decision per physics step
// ---------------------------------------------------------------------------
static void botStep(GJBaseGameLayer* g, float dt) {
	s_tainted = true;
	if (!s_notified) {
		s_notified = true;
		Notification::create("Autopilot on: safe mode, progress won't save", NotificationIcon::Info)->show();
	}

	auto p = g->m_player1;
	float px = p->getPositionX();
	float py = p->getPositionY();
	float speed = p->m_playerSpeed * 355.f; // units/sec, approximate
	float lead = speed * s_params.jumpLead;

	releaseIfDue(g, dt);
	s_cooldown -= dt;

	bool upsideDown = p->m_isUpsideDown;

	// --- steering modes: ship, wave ---------------------------------------
	if (p->m_isShip || p->m_isDart) {
		float target = corridorTarget(px, py, speed * 0.35f);
		bool wantUp = (py + p->m_yVelocity * 0.12f) < target;
		if (upsideDown) wantUp = !wantUp;
		setHold(g, wantUp);
		return;
	}

	// --- ufo: pulse taps to hop toward the corridor -------------------------
	if (p->m_isBird) {
		float target = corridorTarget(px, py, speed * 0.35f);
		bool wantUp = py < target;
		if (upsideDown) wantUp = !wantUp;
		if (wantUp && s_cooldown <= 0.f) {
			tap(g, 0.05f);
			s_cooldown = 0.12f;
		}
		return;
	}

	// --- tap modes: cube, robot, ball, spider, swing -------------------------
	bool grounded = p->m_isOnGround;

	// rings work in the air for cube-like modes
	if (!grounded && ringNearby(px, py) && s_cooldown <= 0.f) {
		tap(g, 0.05f);
		s_cooldown = 0.15f;
		return;
	}

	if (blockingAhead(px, py, lead) && s_cooldown <= 0.f) {
		bool canAct = grounded || p->m_isBall || p->m_isSpider || p->m_isSwing;
		if (canAct) {
			tap(g, p->m_isRobot ? 0.12f : 0.05f);
			s_cooldown = 0.2f;
		}
	}
}

// ---------------------------------------------------------------------------
// Gemini tuner
// ---------------------------------------------------------------------------
static std::string nearbyText(float px, float py) {
	std::string out;
	int count = 0;
	for (auto it = firstAtOrAfter(px - 120.f); it != s_obst.end() && it->l <= px + 80.f && count < 12; ++it, ++count) {
		char const* k = it->kind == 0 ? "solid" : it->kind == 1 ? "hazard" : "ring";
		out += fmt::format("[{} dx={:.0f} dy={:.0f} w={:.0f} h={:.0f}] ", k, it->l - px, it->cy - py, it->r - it->l, it->h);
	}
	return out.empty() ? "none" : out;
}

static void applyAiReply(matjson::Value const& root) {
	auto text = root["candidates"][0]["content"]["parts"][0]["text"].asString();
	if (text.isErr()) {
		log::warn("Gemini reply had no text");
		return;
	}
	auto parsed = matjson::parse(text.unwrap());
	if (parsed.isErr()) {
		log::warn("Gemini reply was not valid JSON: {}", text.unwrap());
		return;
	}
	auto v = parsed.unwrap();
	auto jl = v["jump_lead"].asDouble();
	if (jl.isOk()) s_params.jumpLead = std::clamp(static_cast<float>(jl.unwrap()), 0.04f, 0.4f);
	auto sm = v["ship_margin"].asDouble();
	if (sm.isOk()) s_params.shipMargin = std::clamp(static_cast<float>(sm.unwrap()), 10.f, 100.f);
	auto note = v["note"].asString();
	log::info("Gemini tuned: jump_lead={:.3f} ship_margin={:.0f} ({})",
		s_params.jumpLead, s_params.shipMargin, note.isOk() ? note.unwrap() : "no note");
}

static void askGemini(std::string prompt) {
	auto key = Mod::get()->getSettingValue<std::string>("api-key");
	if (key.empty()) {
		log::warn("Use Gemini tuning is on but no API key is set");
		return;
	}
	auto model = Mod::get()->getSettingValue<std::string>("model");
	auto url = fmt::format("https://generativelanguage.googleapis.com/v1beta/models/{}:generateContent", model);

	matjson::Value part;
	part["text"] = prompt;
	auto parts = matjson::Value::array();
	parts.push(part);
	matjson::Value content;
	content["parts"] = parts;
	auto contents = matjson::Value::array();
	contents.push(content);

	matjson::Value body;
	body["contents"] = contents;
	body["generationConfig"]["responseMimeType"] = "application/json";
	body["generationConfig"]["temperature"] = 0.2;

	s_inFlight = true;

	auto req = web::WebRequest();
	req.header("x-goog-api-key", key);
	req.bodyJSON(body);
	req.timeout(std::chrono::seconds(30));

	// Geode v5 async: the callback runs on the main thread when the response arrives
	s_listener.spawn(
		req.post(url),
		[](web::WebResponse res) {
			s_inFlight = false;
			if (!res.ok()) {
				log::warn("Gemini request failed, HTTP {}", res.code());
				return;
			}
			auto json = res.json();
			if (json.isErr()) {
				log::warn("Gemini response was not JSON");
				return;
			}
			applyAiReply(json.unwrap());
		}
	);
}

static void onDeath(PlayLayer* pl) {
	s_deathsSinceAsk++;
	if (!cfgBool("use-ai") || s_inFlight) return;
	if (s_deathsSinceAsk < Mod::get()->getSettingValue<int64_t>("ai-every-n-deaths")) return;
	s_deathsSinceAsk = 0;

	auto p = pl->m_player1;
	float px = p->getPositionX();
	float py = p->getPositionY();
	float speed = p->m_playerSpeed * 355.f;
	float lastTapAgo = s_lastTapX < 0.f ? -1.f : px - s_lastTapX;

	auto prompt = fmt::format(
		"You tune a Geometry Dash autoplay bot. The bot just died.\n"
		"State at death: mode={}, speed={:.0f} units/s, x={:.0f}, y={:.0f}, onGround={}, upsideDown={}.\n"
		"Last input was {:.0f} units before the death point (-1 means none).\n"
		"Nearby objects relative to the player: {}\n"
		"Current params: jump_lead={:.3f} seconds (how long before reaching an obstacle the bot reacts), ship_margin={:.0f} units.\n"
		"If the bot probably reacted too late, raise jump_lead; if too early, lower it. For ship/wave deaths adjust ship_margin.\n"
		"Reply ONLY with JSON: {{\"jump_lead\": number between 0.04 and 0.4, \"ship_margin\": number between 10 and 100, \"note\": short string}}",
		modeName(p), speed, px, py, p->m_isOnGround, p->m_isUpsideDown,
		lastTapAgo, nearbyText(px, py), s_params.jumpLead, s_params.shipMargin
	);
	askGemini(prompt);
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
class $modify(APGameLayer, GJBaseGameLayer) {
	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		auto pl = PlayLayer::get();
		if (pl && this == static_cast<GJBaseGameLayer*>(pl)
			&& cfgBool("enabled")
			&& !m_player1->m_isDead
			&& !s_obst.empty()) {
			botStep(this, dt);
		}
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
	}
};

class $modify(APPlayLayer, PlayLayer) {
	void setupHasCompleted() {
		PlayLayer::setupHasCompleted();

		s_obst.clear();
		for (auto* obj : CCArrayExt<GameObject*>(m_objects)) {
			int kind = -1;
			switch (obj->m_objectType) {
				case GameObjectType::Solid:
				case GameObjectType::Slope:
					kind = 0; break;
				case GameObjectType::Hazard:
					kind = 1; break;
				case GameObjectType::YellowJumpRing:
				case GameObjectType::PinkJumpRing:
				case GameObjectType::RedJumpRing:
				case GameObjectType::GravityRing:
				case GameObjectType::GreenRing:
				case GameObjectType::DropRing:
				case GameObjectType::CustomRing:
				case GameObjectType::DashRing:
					kind = 2; break;
				default: break;
			}
			if (kind < 0) continue;
			auto r = obj->getObjectRect();
			s_obst.push_back({ r.getMinX(), r.getMaxX(), r.getMidY(), r.size.height, kind });
		}
		std::sort(s_obst.begin(), s_obst.end(), [](Obst const& a, Obst const& b) { return a.l < b.l; });

		s_params.jumpLead = static_cast<float>(Mod::get()->getSettingValue<double>("jump-lead"));
		s_params.shipMargin = static_cast<float>(Mod::get()->getSettingValue<double>("ship-margin"));
		s_deathsSinceAsk = 0;
		s_inFlight = false;
		s_tainted = false;
		s_notified = false;
		log::info("Autopilot cached {} objects", s_obst.size());
	}

	void resetLevel() {
		PlayLayer::resetLevel();
		s_holding = false;
		s_tapTimer = -1.f;
		s_cooldown = 0.f;
		s_tainted = false; // each attempt is judged on its own
	}

	// Auto safe mode: if the bot touched this attempt, finish it as a test run
	// so stats, coins and the completion don't get saved or submitted.
	void levelComplete() {
		bool prev = m_isTestMode;
		if (s_tainted) m_isTestMode = true;
		PlayLayer::levelComplete();
		m_isTestMode = prev;
	}

	void destroyPlayer(PlayerObject* player, GameObject* obj) {
		bool wasDead = m_player1->m_isDead;
		PlayLayer::destroyPlayer(player, obj);
		if (!wasDead && player == m_player1 && cfgBool("enabled")) {
			onDeath(this);
		}
	}
};

// Auto safe mode, second layer: never write percentages (normal or practice)
// for an attempt the bot touched.
class $modify(APGameLevel, GJGameLevel) {
	void savePercentage(int percent, bool isPracticeMode, int clicks, int attempts, bool isChkValid) {
		if (s_tainted) return;
		GJGameLevel::savePercentage(percent, isPracticeMode, clicks, attempts, isChkValid);
	}
};
