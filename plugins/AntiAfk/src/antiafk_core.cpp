#include "antiafk_core.hpp"

#include <cmath>

namespace antiafk {
namespace {

// (int)floor(g_warningInterval - idle + 0.999f) of the original.
int RemainingSeconds(int warningInterval, float idle) {
	return static_cast<int>(std::floor(static_cast<float>(warningInterval) - idle + 0.999f));
}

// The message the original picks for an idle warning / countdown: the last warning announces the
// final punishment, the earlier ones announce the suicide.
ActionKind WarningMessageKind(const Settings& settings, const State& state) {
	if (state.warnCount == settings.maxWarnings - 1) {
		return settings.kickInsteadOfSpec ? ActionKind::CountdownKick : ActionKind::CountdownSpec;
	}
	return ActionKind::CountdownKill;
}

float LengthSquared(const plg::vec3& v) {
	return v.x * v.x + v.y * v.y + v.z * v.z;
}

// AngleBetweenDirsDeg() of the original.
float AngleBetweenDirsDeg(const plg::vec3& a, const plg::vec3& b) {
	float dot = a.x * b.x + a.y * b.y + a.z * b.z;
	if (dot > 1.0f) {
		dot = 1.0f;
	}
	if (dot < -1.0f) {
		dot = -1.0f;
	}
	return std::acos(dot) * (180.0f / static_cast<float>(M_PI));
}

} // namespace

void ResetState(State& state, float curtime, bool clearAim) {
	state.afkStart = curtime;
	state.warnedOnce = false;
	state.lastCountdown = -1;
	if (clearAim) {
		state.haveAimDir = false;
	}
}

TickOutcome EvaluateTick(State& state, const Settings& settings, float curtime, bool inWarmup, bool isAlive,
						 bool movingKeys, const plg::vec3* aimDir) {
	TickOutcome outcome;

	// The original's `continue` paths: an AFK check is meaningless during warmup and while dead.
	if (inWarmup) {
		ResetState(state, curtime, true);
		return outcome;
	}
	if (!isAlive) {
		ResetState(state, curtime, true);
		return outcome;
	}

	// DirFromTrace(); an unusable direction behaves exactly like a trace that returned no start/end
	// difference.
	plg::vec3 curDir{0.0f, 0.0f, 0.0f};
	bool haveDir = false;
	if (aimDir != nullptr && LengthSquared(*aimDir) >= 1e-8f && !std::isnan(LengthSquared(*aimDir)) &&
		!std::isinf(LengthSquared(*aimDir))) {
		const float inv = 1.0f / std::sqrt(LengthSquared(*aimDir));
		curDir.x = aimDir->x * inv;
		curDir.y = aimDir->y * inv;
		curDir.z = aimDir->z * inv;
		haveDir = true;
	}

	bool isAiming = false;
	if (haveDir) {
		if (!state.haveAimDir) {
			state.lastAimDir = curDir;
			state.haveAimDir = true;
		}
		const float deltaAim = AngleBetweenDirsDeg(state.lastAimDir, curDir);
		isAiming = deltaAim >= settings.aimThreshDeg;
	}

	if (movingKeys || isAiming) {
		ResetState(state, curtime); // NOTE: not the clearAim form, like the original
	}
	if (haveDir) {
		state.lastAimDir = curDir;
	}

	const float idle = curtime - state.afkStart;

	// Countdown during the last five seconds before the warning interval.
	if (idle >= static_cast<float>(settings.warningInterval) - 5.0f && idle < static_cast<float>(settings.warningInterval)) {
		const int remain = RemainingSeconds(settings.warningInterval, idle);
		if (state.lastCountdown != remain) {
			outcome.actions.push_back(Action{WarningMessageKind(settings, state), remain, 0});
			state.lastCountdown = remain;
		}
	}

	// The first warning.
	if (idle >= static_cast<float>(settings.initialWarningTime) && idle < static_cast<float>(settings.warningInterval) &&
		!state.warnedOnce) {
		const int remain = RemainingSeconds(settings.warningInterval, idle);
		outcome.actions.push_back(Action{WarningMessageKind(settings, state), remain, 0});
		state.warnedOnce = true;
	}

	// The punishment.
	if (idle >= static_cast<float>(settings.warningInterval)) {
		if (state.warnCount < settings.maxWarnings - 1) {
			if (isAlive) {
				outcome.actions.push_back(Action{ActionKind::Suicide, 0, 0});
			}
			++state.warnCount;
			outcome.actions.push_back(Action{ActionKind::WarningKill, state.warnCount, settings.maxWarnings});
			state.lastCountdown = -1;
			state.afkStart = curtime;
			state.warnedOnce = false;
		} else {
			if (settings.kickInsteadOfSpec) {
				outcome.actions.push_back(Action{ActionKind::Kick, 0, 0});
			} else {
				outcome.actions.push_back(Action{ActionKind::Spec, 0, 0});
				outcome.actions.push_back(Action{ActionKind::SpecFinal, 0, 0});
			}
			outcome.eraseState = true;
		}
	}

	return outcome;
}

} // namespace antiafk
