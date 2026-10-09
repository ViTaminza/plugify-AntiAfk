// AntiAfk — engine-independent core: the per-player AFK state machine of the original
// (ref/src/Anti-Afk/AntiAfk.cpp, CheckAfkTick) with the engine calls turned into actions.
//
// The original interleaved the decision logic (timers, countdown, warnings, escalation) with
// hl2sdk/schema reads. Here every engine read is an argument and every engine write is an action
// the plugin executes, so the timing behaviour is unit-testable (tests/antiafk_core_tests.cpp)
// while the plugin keeps the exact call order of the original.

#pragma once

#include <cstdint>
#include <vector>

#include <plg/numerics.hpp>

namespace antiafk {

// The [AntiAfk] settings.ini keys (types as in the original's globals).
struct Settings {
	int initialWarningTime = 15;   // initial_warning_time
	int warningInterval = 35;	   // warning_interval
	int maxWarnings = 3;		   // max_warnings
	bool kickInsteadOfSpec = false; // kick_instead_of_spec
	float aimThreshDeg = 1.0f;	   // aim_move_threshold_deg
	bool debugLog = true;		   // debug_log
};

// The original's PlayerAfkState (g_Afk[slot]).
struct State {
	float afkStart = 0.0f;
	bool warnedOnce = false;
	int lastCountdown = -1;
	int warnCount = 0;

	plg::vec3 lastAimDir{0.0f, 0.0f, 0.0f};
	bool haveAimDir = false;
};

// What the plugin has to do for one slot in one tick (the prints of the original).
enum class ActionKind {
	CountdownKick, // print AFK_CountdownKick with %d = "a"
	CountdownSpec, // print AFK_CountdownSpec with %d = "a"
	CountdownKill, // print AFK_CountdownKill with %d = "a"
	WarningKill,   // print AFK_WarningKill with %d/%d = "a"/"b"
	SpecFinal,	   // print AFK_SpecFinal (final punishment was "move to spectators")
	Suicide,	   // g_pPlayers->CommitSuicide(slot, false, true)
	Kick,		   // engine->DisconnectClient(..., NETWORK_DISCONNECT_KICKED)
	Spec		   // g_pPlayers->ChangeTeam(slot, 1)
};

struct Action {
	ActionKind kind;
	int a = 0;
	int b = 0;
};

struct TickOutcome {
	std::vector<Action> actions;
	// The original's g_Afk.erase(slot) after the final punishment.
	bool eraseState = false;
};

// Resets the idle timer (the original's ResetAfk). `clearAim` = the warmup/dead/round-start form.
void ResetState(State& state, float curtime, bool clearAim = false);

// One CheckAfkTick() iteration for a single in-game, non-bot, non-spectator slot.
//
//   curtime      gpGlobals->curtime
//   inWarmup     g_pGameRules->m_bWarmupPeriod()
//   isAlive      controller's m_bPawnIsAlive()
//   movingKeys   WASD bits of the movement services' button state
//   aimDir       current aim direction from the eye trace; nullptr when the direction could not be
//                computed (the original's DirFromTrace returning false)
//
// warmup and "not alive" reset the state and end the tick (as in the original's `continue`), and
// movement/aim movement reset the idle timer before the message/punishment blocks run.
TickOutcome EvaluateTick(State& state, const Settings& settings, float curtime, bool inWarmup, bool isAlive,
						 bool movingKeys, const plg::vec3* aimDir);

} // namespace antiafk
