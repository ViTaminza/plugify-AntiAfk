// AntiAfk unit tests — the engine-independent AFK state machine (the timing, countdown, warning
// and punishment sequence of the original's CheckAfkTick).
//
// Run by ../../test.sh (inside the Steam Runtime image, cwd = build/tests).

#include "antiafk_core.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

antiafk::Settings DefaultSettings() {
	antiafk::Settings settings;
	settings.initialWarningTime = 15;
	settings.warningInterval = 35;
	settings.maxWarnings = 3;
	settings.kickInsteadOfSpec = false;
	settings.aimThreshDeg = 1.0f;
	return settings;
}

std::vector<antiafk::ActionKind> Kinds(const antiafk::TickOutcome& outcome) {
	std::vector<antiafk::ActionKind> kinds;
	for (const antiafk::Action& action : outcome.actions) {
		kinds.push_back(action.kind);
	}
	return kinds;
}

// One second of standing still at time t.
antiafk::TickOutcome IdleTick(antiafk::State& state, const antiafk::Settings& settings, float t) {
	return antiafk::EvaluateTick(state, settings, t, false, true, false, nullptr);
}

void TestFirstWarningAndCountdown() {
	const antiafk::Settings settings = DefaultSettings();
	antiafk::State state;
	state.afkStart = 0.0f;

	Check(IdleTick(state, settings, 14.0f).actions.empty(), "no warning before initial_warning_time");

	const antiafk::TickOutcome warning = IdleTick(state, settings, 15.0f);
	Check(warning.actions.size() == 1, "one message at initial_warning_time");
	Check(Kinds(warning)[0] == antiafk::ActionKind::CountdownKill, "the first warning is the kill warning");
	Check(warning.actions[0].a == 20, "remaining seconds of the first warning (35-15)");
	Check(state.warnedOnce, "the first warning is remembered");

	Check(IdleTick(state, settings, 16.0f).actions.empty(), "the first warning is not repeated");

	Check(IdleTick(state, settings, 29.5f).actions.empty(), "no countdown before interval-5");
	const antiafk::TickOutcome countdown = IdleTick(state, settings, 30.2f);
	Check(countdown.actions.size() == 1, "countdown starts in the last five seconds");
	Check(countdown.actions[0].a == 5, "countdown shows the remaining seconds");
	Check(IdleTick(state, settings, 30.5f).actions.empty(), "the same countdown second is not repeated");
	const antiafk::TickOutcome last = IdleTick(state, settings, 34.5f);
	Check(last.actions.size() == 1 && last.actions[0].a == 1, "countdown counts down to one second");
}

void TestPunishmentEscalation() {
	const antiafk::Settings settings = DefaultSettings();
	antiafk::State state;
	state.afkStart = 0.0f;

	const antiafk::TickOutcome punish = IdleTick(state, settings, 35.0f);
	Check(punish.actions.size() == 2, "idle >= warning_interval: suicide + warning message");
	Check(Kinds(punish)[0] == antiafk::ActionKind::Suicide, "the suicide comes first");
	Check(Kinds(punish)[1] == antiafk::ActionKind::WarningKill, "then the AFK_WarningKill message");
	Check(punish.actions[1].a == 1 && punish.actions[1].b == 3, "warnCount/maxWarnings are passed");
	Check(!punish.eraseState, "no state erase after a soft punishment");
	Check(state.warnCount == 1, "warnCount incremented");
	Check(state.afkStart == 35.0f, "the idle timer restarts after the punishment");
	Check(state.lastCountdown == -1 && !state.warnedOnce, "countdown/warning state reset");

	// Second soft punishment.
	const antiafk::TickOutcome second = IdleTick(state, settings, 70.0f);
	Check(second.actions.size() == 2 && second.actions[1].a == 2, "second soft punishment (2/3)");

	// warnCount is now max-1: the countdown / first warning switch to the final-punishment texts.
	state.afkStart = 100.0f;
	const antiafk::TickOutcome finalWarning = IdleTick(state, settings, 115.0f);
	Check(Kinds(finalWarning)[0] == antiafk::ActionKind::CountdownSpec,
		  "the last warning announces the move to spectators");

	const antiafk::TickOutcome final = IdleTick(state, settings, 135.0f);
	Check(Kinds(final) == (std::vector<antiafk::ActionKind>{antiafk::ActionKind::Spec, antiafk::ActionKind::SpecFinal}),
		  "the final punishment moves the player to spectators and prints AFK_SpecFinal");
	Check(final.eraseState, "the slot state is forgotten after the final punishment");
}

void TestKickInsteadOfSpec() {
	antiafk::Settings settings = DefaultSettings();
	settings.kickInsteadOfSpec = true;
	antiafk::State state;
	state.warnCount = settings.maxWarnings - 1;
	state.afkStart = 0.0f;

	const antiafk::TickOutcome final = IdleTick(state, settings, 35.0f);
	Check(Kinds(final) == (std::vector<antiafk::ActionKind>{antiafk::ActionKind::Kick}),
		  "kick_instead_of_spec: the final punishment kicks and prints nothing");
	Check(final.eraseState, "the slot state is forgotten after the kick");

	antiafk::State second;
	second.warnCount = settings.maxWarnings - 1;
	second.afkStart = 0.0f;
	const antiafk::TickOutcome told = IdleTick(second, settings, 15.0f);
	Check(Kinds(told)[0] == antiafk::ActionKind::CountdownKick, "the last warning announces the kick");
}

void TestResets() {
	const antiafk::Settings settings = DefaultSettings();
	antiafk::State state;
	state.afkStart = 0.0f;
	state.warnCount = 2;
	state.warnedOnce = true;

	// Warmup resets the idle timer and the aim state.
	const antiafk::TickOutcome warmup = antiafk::EvaluateTick(state, settings, 10.0f, true, true, false, nullptr);
	Check(warmup.actions.empty(), "warmup: no actions");
	Check(state.afkStart == 10.0f && !state.warnedOnce, "warmup: the timer and the warning flag reset");
	Check(state.warnCount == 2, "warmup keeps warnCount (like the original)");
	Check(!state.haveAimDir, "warmup clears the aim direction");

	// Dead players reset as well.
	state.afkStart = 20.0f;
	const antiafk::TickOutcome dead = antiafk::EvaluateTick(state, settings, 30.0f, false, false, false, nullptr);
	Check(dead.actions.empty(), "dead: no actions");
	Check(state.afkStart == 30.0f, "dead: the timer resets");

	// Movement keys reset the idle timer but do not clear the aim direction.
	state.haveAimDir = true;
	state.afkStart = 40.0f;
	const antiafk::TickOutcome keys = antiafk::EvaluateTick(state, settings, 50.0f, false, true, true, nullptr);
	Check(keys.actions.empty(), "movement: no actions");
	Check(state.afkStart == 50.0f, "movement: the timer resets");
	Check(state.haveAimDir, "movement keeps the aim direction");
}

plg::vec3 Unit(float x, float y, float z) {
	plg::vec3 v;
	v.x = x;
	v.y = y;
	v.z = z;
	return v;
}

// The aim tests must not trip the warning thresholds: the idle timer starts at 0 and the
// warning/interval values are pushed far out, so the only observable effect is the reset.
antiafk::Settings AimSettings() {
	antiafk::Settings settings = DefaultSettings();
	settings.initialWarningTime = 100000;
	settings.warningInterval = 200000;
	return settings;
}

void TestAimMovement() {
	const antiafk::Settings settings = AimSettings();
	antiafk::State state;
	state.afkStart = 0.0f;

	// First direction: becomes the reference, no movement yet.
	plg::vec3 forward = Unit(1.0f, 0.0f, 0.0f);
	Check(antiafk::EvaluateTick(state, settings, 10.0f, false, true, false, &forward).actions.empty(),
		  "the first direction is only recorded");
	Check(state.haveAimDir, "the first direction is kept");
	Check(state.afkStart == 0.0f, "recording the first direction is not activity");

	// A 0.5 degree turn is below the threshold.
	plg::vec3 tiny = Unit(1.0f, 0.0087f, 0.0f);
	Check(antiafk::EvaluateTick(state, settings, 20.0f, false, true, false, &tiny).actions.empty(),
		  "a small turn prints nothing");
	Check(state.afkStart == 0.0f, "a 0.5 degree turn does not reset the idle timer");

	// A 5 degree turn resets the idle timer.
	plg::vec3 turned = Unit(1.0f, 0.0875f, 0.0f);
	Check(antiafk::EvaluateTick(state, settings, 30.0f, false, true, false, &turned).actions.empty(),
		  "aim movement itself prints nothing");
	Check(state.afkStart == 30.0f, "a >= threshold turn resets the idle timer");

	// An unusable direction behaves like "no trace direction": the idle timer keeps running and
	// the last recorded direction stays.
	const antiafk::TickOutcome none = antiafk::EvaluateTick(state, settings, 40.0f, false, true, false, nullptr);
	Check(none.actions.empty(), "a missing direction prints nothing");
	Check(state.afkStart == 30.0f, "a missing direction is not activity");
	Check(state.haveAimDir, "the last direction is kept");
}

void TestAimOff() {
	antiafk::Settings settings = AimSettings();
	settings.aimThreshDeg = 0.0f; // the original's ">= 0" makes every valid direction "activity"
	antiafk::State state;
	state.afkStart = 0.0f;
	plg::vec3 forward = Unit(0.0f, 1.0f, 0.0f);
	antiafk::EvaluateTick(state, settings, 10.0f, false, true, false, &forward);
	state.afkStart = 5.0f;
	antiafk::EvaluateTick(state, settings, 20.0f, false, true, false, &forward);
	Check(state.afkStart == 20.0f, "aim_move_threshold_deg = 0 treats every tick as activity");
}

} // namespace

int main() {
	TestFirstWarningAndCountdown();
	TestPunishmentEscalation();
	TestKickInsteadOfSpec();
	TestResets();
	TestAimMovement();
	TestAimOff();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
