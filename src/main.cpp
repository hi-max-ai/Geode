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
