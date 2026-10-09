// AntiAfk — Plugify port of the Metamod plugin Anti-Afk (ABKAM)
// (ref/src/Anti-Afk/AntiAfk.cpp).
//
// Behaviour kept identical (the state machine itself lives in antiafk_core.cpp and is unit-tested):
//   * settings.ini of addons/configs/AntiAfk drives the warning times, the warning count, the
//     "kick instead of spectate" flag, the aim-movement threshold and debug_log;
//   * a one-second timer (created once a map runs) walks every in-game, non-bot, non-spectator
//     slot: during warmup and while dead (and after round_start) the idle timer resets, moving a
//     WASD key or turning the view by >= aim_move_threshold_deg resets it too;
//   * idle >= warning_interval: suicide (warnCount < max_warnings - 1) with the "AFK_WarningKill"
//     message, otherwise the final punishment - kick or move to spectators - after which the slot
//     forgets its state;
//   * "round_start" enables the checks and resets every timer; "cs_win_panel_match"/"game_end"
//     disable them and clear the warning counters (the original's three HookEvent calls);
//   * all messages go through antiafk.phrases.txt (AFK_Prefix prefix + the message, formatted
//     exactly like the original's snprintf of the phrase).
//
// Port notes (engine access that Metamod had and s2sdk does not):
//   * g_pGameRules->m_bWarmupPeriod() -> s2sdk GetGameRules() + the cached schema offset of
//     CCSGameRules::m_bWarmupPeriod (read with GetEntData2 on the rules object pointer, which is
//     not an entity handle);
//   * pPawn->m_pMovementServices()->m_nButtons().m_pButtonStates()[0] -> s2sdk GetClientButtons()
//     (the same movement-services button array);
//   * g_pPlayers->RayTrace() + DirFromTrace: the trace *direction* was only ever the view
//     direction, so the port uses s2sdk GetClientForwardVector() (normalized like DirFromTrace;
//     an unusable vector counts as "no direction");
//   * m_iTeamNum() -> pisex::g_pPlayers->GetTeam(), m_bPawnIsAlive() -> pisex::g_pPlayers->IsAlive();
//   * engine->DisconnectClient(..., NETWORK_DISCONNECT_KICKED) -> s2sdk KickClient(..., Kicked);
//   * StartupServer/OnGetGameRules -> the pisexcompat deferred startup callback (a map is
//     guaranteed to run there, so the one-second timer is created safely).

#include "antiafk_core.hpp"

#include <pisex/pisex.hpp>

#include <plg/plugin.hpp>
#include <plg/source_location.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

constexpr const char* kLogTag = "[AntiAfk]";
constexpr int kMaxSlots = 64;

// TEAM_SPECTATOR of the original (CS_TEAM_SPECTATOR).
constexpr int kTeamSpectator = 1;

// IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT (hl2sdk in_buttons.h).
constexpr uint64_t kMoveButtons = 0x8ULL | 0x10ULL | 0x200ULL | 0x400ULL;

constexpr const char* kConfigPath = "addons/configs/AntiAfk/settings.ini";
constexpr const char* kPhrasesFile = "antiafk.phrases.txt";

constexpr float kTickInterval = 1.0f;

void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

class AntiAfkPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

	// round_start / cs_win_panel_match / game_end (the three event hooks of the original).
	void OnRoundStart();
	void OnMatchEnd(const char* eventName);
	// The one-second timer body (CheckAfkTick of the original).
	void Tick();
	// Creates the repeating one-second timer once a map runs (the original's OnGetGameRules).
	void StartTimer();

	void Dbg(std::string_view message);

private:
	void LoadConfig();
	void LoadPhrases();
	// A line with the AFK_Prefix prefix, printed like the original's PrintChat().
	void PrintLine(int iSlot, std::string_view message);
	// Formats the phrase with the snprintf arguments of the original and prints it.
	void PrintPhrase(int iSlot, const char* key, const char* fallback, int a = 0, int b = 0, bool twoArgs = false);
	// m_bWarmupPeriod of the current game rules; false while the rules object is unavailable.
	bool InWarmup();

	antiafk::Settings m_settings;
	pisex::PhraseMap m_phrases;
	std::unordered_map<int, antiafk::State> m_afk;

	bool m_canCheckAfk = false;
	pisex::CTimer* m_timer = nullptr;
	int m_warmupOffset = -1;
	bool m_loggedChecksOff = false;
};

AntiAfkPlugin g_antiAfkPlugin;

// --- configuration / phrases -------------------------------------------------------------------

void AntiAfkPlugin::LoadConfig() {
	pisex::KeyValues kv("AntiAfk");
	if (!kv.LoadFromFile(kConfigPath)) {
		LogMessage(std::string(kLogTag) + " failed to load " + kConfigPath + " - keeping the built-in defaults",
				   plg::Severity::Error);
		return;
	}
	m_settings.initialWarningTime = static_cast<int>(kv.GetFloat("initial_warning_time", 15.0f));
	m_settings.warningInterval = static_cast<int>(kv.GetFloat("warning_interval", 35.0f));
	m_settings.maxWarnings = kv.GetInt("max_warnings", 3);
	m_settings.kickInsteadOfSpec = kv.GetInt("kick_instead_of_spec", 0) != 0;
	m_settings.aimThreshDeg = kv.GetFloat("aim_move_threshold_deg", 1.0f);
	m_settings.debugLog = kv.GetInt("debug_log", 1) != 0;
	Dbg("config loaded: warn=" + std::to_string(m_settings.initialWarningTime) + ", interval=" +
		std::to_string(m_settings.warningInterval) + ", max=" + std::to_string(m_settings.maxWarnings) + ", kick=" +
		std::to_string(static_cast<int>(m_settings.kickInsteadOfSpec)) + ", aim=" +
		std::to_string(m_settings.aimThreshDeg) + ", dbg=" + std::to_string(static_cast<int>(m_settings.debugLog)));
}

void AntiAfkPlugin::LoadPhrases() {
	m_phrases.clear();
	if (!pisex::LoadPhrasesFile(kPhrasesFile, m_phrases)) {
		LogMessage(std::string(kLogTag) + " failed to load " + kPhrasesFile + " - using the built-in message texts",
				   plg::Severity::Error);
	}
}

namespace {

const char* PhraseOf(const pisex::PhraseMap& phrases, const char* key, const char* fallback) {
	const auto it = phrases.find(key);
	return it != phrases.end() && !it->second.empty() ? it->second.c_str() : fallback;
}

} // namespace

void AntiAfkPlugin::PrintLine(int iSlot, std::string_view message) {
	const char* prefix = PhraseOf(m_phrases, "AFK_Prefix", "");
	if (prefix != nullptr && prefix[0] != '\0') {
		pisex::g_pUtils->PrintToChat(iSlot, "%s %.*s", prefix, static_cast<int>(message.size()), message.data());
	} else {
		pisex::g_pUtils->PrintToChat(iSlot, "%.*s", static_cast<int>(message.size()), message.data());
	}
}

void AntiAfkPlugin::PrintPhrase(int iSlot, const char* key, const char* fallback, int a, int b, bool twoArgs) {
	const char* format = PhraseOf(m_phrases, key, fallback);
	const std::string body =
			twoArgs ? pisex::FormatString(format, a, b) : pisex::FormatString(format, a);
	PrintLine(iSlot, body);
}

void AntiAfkPlugin::Dbg(std::string_view message) {
	if (!m_settings.debugLog) {
		return;
	}
	LogMessage(std::string(kLogTag) + " " + std::string(message), plg::Severity::Debug);
}

// --- engine reads ------------------------------------------------------------------------------

bool AntiAfkPlugin::InWarmup() {
	// s2sdk::GetGameRules() reads the engine's *current* rules global (it is null between maps), so
	// the pointer is fetched on every use. Caching it would keep a dangling pointer after a map
	// change (the original re-fetched its g_pGameRules from the OnGetGameRules hook for the same
	// reason); only the schema offset is cached.
	if (!__s2sdk_GetGameRules || !__s2sdk_GetEntData2) {
		return false;
	}
	if (m_warmupOffset < 0) {
		m_warmupOffset = pisex::SchemaOffset("CCSGameRules", "m_bWarmupPeriod");
	}
	if (m_warmupOffset < 0) {
		return false;
	}
	try {
		void* gameRules = s2sdk::GetGameRules();
		if (gameRules == nullptr) {
			return false;
		}
		// CCSGameRules is not an entity: the offset of GetSchemaOffset is read directly on the
		// rules object pointer (m_bWarmupPeriod is one byte).
		return s2sdk::GetEntData2(gameRules, m_warmupOffset, 1) != 0;
	} catch (...) {
		return false;
	}
}

// --- the tick -----------------------------------------------------------------------------------

void AntiAfkPlugin::Tick() {
	if (!m_canCheckAfk) {
		if (!m_loggedChecksOff) {
			m_loggedChecksOff = true;
			Dbg("tick: checks OFF (waiting for round_start)");
		}
		return;
	}
	m_loggedChecksOff = false;
	if (!pisex::IsGameReady()) {
		return;
	}

	const bool inWarmup = InWarmup();
	const antiafk::Settings settings = m_settings;
	const pisex::GlobalVars* globals = pisex::GetCGlobalVars();
	const float curtime = globals != nullptr ? globals->curtime : 0.0f;

	for (int slot = 0; slot < kMaxSlots; ++slot) {
		try {
			if (!pisex::g_pPlayers->IsInGame(slot) || pisex::g_pPlayers->IsFakeClient(slot)) {
				continue;
			}
			// m_iTeamNum() == TEAM_SPECTATOR
			if (pisex::g_pPlayers->GetTeam(slot) == kTeamSpectator) {
				continue;
			}

			// m_bPawnIsAlive()
			const bool isAlive = pisex::g_pPlayers->IsAlive(slot);

			// Both reads hang off the pawn: the original only touched them when isAlive, and
			// EvaluateTick() ignores both values when !isAlive, so skipping the reads then is
			// behaviour-identical (and keeps the pawn-less connect window, where s2sdk logs
			// "invalid player pawn", out of these calls).
			bool movingKeys = false;
			plg::vec3 aimDir{0.0f, 0.0f, 0.0f};
			const plg::vec3* aim = nullptr;
			if (isAlive) {
				// pPawn->m_pMovementServices()->m_nButtons().m_pButtonStates()[0]
				if (__s2sdk_GetClientButtons) {
					const uint64_t buttons = s2sdk::GetClientButtons(slot, 0);
					movingKeys = (buttons & kMoveButtons) != 0;
				}

				// The trace direction of the original is the view direction.
				if (__s2sdk_GetClientForwardVector) {
					aimDir = s2sdk::GetClientForwardVector(slot);
					aim = &aimDir;
				}
			}

			auto it = m_afk.find(slot);
			if (it == m_afk.end()) {
				antiafk::State fresh;
				fresh.afkStart = curtime;
				it = m_afk.emplace(slot, fresh).first;
				Dbg("init slot " + std::to_string(slot) + " (afkStart=" + std::to_string(fresh.afkStart) + ")");
			}
			antiafk::State& state = it->second;

			const antiafk::TickOutcome outcome =
					antiafk::EvaluateTick(state, settings, curtime, inWarmup, isAlive, movingKeys, aim);

			for (const antiafk::Action& action : outcome.actions) {
				switch (action.kind) {
				case antiafk::ActionKind::CountdownKick:
					PrintPhrase(slot, "AFK_CountdownKick",
								"Вы не двигаетесь, и через %d секунд вас кикнут за AFK.", action.a);
					break;
				case antiafk::ActionKind::CountdownSpec:
					PrintPhrase(slot, "AFK_CountdownSpec",
								"Вы не двигаетесь, и через %d секунд вас переведут в наблюдатели.", action.a);
					break;
				case antiafk::ActionKind::CountdownKill:
					PrintPhrase(slot, "AFK_CountdownKill",
								"Вы не двигаетесь, и через %d секунд вас убьют за AFK.", action.a);
					break;
				case antiafk::ActionKind::WarningKill:
					PrintPhrase(slot, "AFK_WarningKill", "Предупреждение %d/%d: вы убиты за AFK.", action.a, action.b,
								true);
					Dbg("slot " + std::to_string(slot) + ": soft punish (kill), warnCount=" + std::to_string(action.a));
					break;
				case antiafk::ActionKind::SpecFinal:
					PrintPhrase(slot, "AFK_SpecFinal", "Вы были переведены в наблюдатели из-за AFK.");
					Dbg("slot " + std::to_string(slot) + ": final punish -> SPEC");
					break;
				case antiafk::ActionKind::Suicide:
					pisex::g_pPlayers->CommitSuicide(slot, false, true);
					break;
				case antiafk::ActionKind::Spec:
					pisex::g_pPlayers->ChangeTeam(slot, kTeamSpectator);
					break;
				case antiafk::ActionKind::Kick:
					if (__s2sdk_KickClient) {
						s2sdk::KickClient(slot, s2sdk::NetworkDisconnectionReason::Kicked, plg::string());
						Dbg("slot " + std::to_string(slot) + ": final punish -> KICK");
					}
					break;
				}
			}
			if (outcome.eraseState) {
				m_afk.erase(slot);
			}
		} catch (const std::exception& ex) {
			LogMessage(std::string(kLogTag) + " tick of slot " + std::to_string(slot) + " failed: " + ex.what(),
					   plg::Severity::Error);
		} catch (...) {
			LogMessage(std::string(kLogTag) + " tick of slot " + std::to_string(slot) + " failed", plg::Severity::Error);
		}
	}
}

// --- events / lifecycle --------------------------------------------------------------------------

void AntiAfkPlugin::OnRoundStart() {
	m_canCheckAfk = true;
	for (auto& [slot, state] : m_afk) {
		antiafk::ResetState(state, pisex::GetCGlobalVars()->curtime, true);
		(void)slot;
	}
	Dbg("round_start: checks ON; reset all AFK timers");
}

void AntiAfkPlugin::OnMatchEnd(const char* eventName) {
	m_canCheckAfk = false;
	for (auto& [slot, state] : m_afk) {
		state.warnCount = 0;
		(void)slot;
	}
	Dbg(std::string(eventName != nullptr ? eventName : "match end") + ": checks OFF");
}

void AntiAfkPlugin::StartTimer() {
	if (m_timer != nullptr) {
		return;
	}
	m_timer = pisex::g_pUtils->CreateTimer(kTickInterval, []() -> float {
		g_antiAfkPlugin.Tick();
		return kTickInterval;
	});
	Dbg("timer created (1.0s)");
}

plg::PluginResult AntiAfkPlugin::OnPluginStart() {
	try {
		pisex::Init("AntiAfk");
		LoadConfig();
		LoadPhrases();

		pisex::g_pUtils->HookEvent("round_start", [](const char*, pisex::IGameEvent*, bool) {
			g_antiAfkPlugin.OnRoundStart();
		});
		pisex::g_pUtils->HookEvent("cs_win_panel_match", [](const char* name, pisex::IGameEvent*, bool) {
			g_antiAfkPlugin.OnMatchEnd(name);
		});
		pisex::g_pUtils->HookEvent("game_end", [](const char* name, pisex::IGameEvent*, bool) {
			g_antiAfkPlugin.OnMatchEnd(name);
		});

		// The original created its timer from OnGetGameRules; the deferred startup callback runs on
		// the first update with a map, which is the same guarantee.
		pisex::g_pUtils->StartupServer([]() {
			g_antiAfkPlugin.StartTimer();
		});
	} catch (const std::exception& ex) {
		LogMessage(std::string(kLogTag) + " OnPluginStart failed: " + ex.what(), plg::Severity::Error);
	} catch (...) {
		LogMessage(std::string(kLogTag) + " OnPluginStart failed with an unknown exception", plg::Severity::Error);
	}
	return {}; // never Failed: that stops every plugify plugin
}

plg::PluginResult AntiAfkPlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	(void)dt;
	try {
		pisex::Poll();
	} catch (const std::exception& ex) {
		LogMessage(std::string(kLogTag) + " update failed: " + ex.what(), plg::Severity::Error);
	} catch (...) {
		LogMessage(std::string(kLogTag) + " update failed with an unknown exception", plg::Severity::Error);
	}
	return {};
}

plg::PluginResult AntiAfkPlugin::OnPluginEnd() {
	try {
		m_afk.clear();
		pisex::Shutdown();
	} catch (const std::exception& ex) {
		LogMessage(std::string(kLogTag) + " OnPluginEnd failed: " + ex.what(), plg::Severity::Error);
	} catch (...) {
		LogMessage(std::string(kLogTag) + " OnPluginEnd failed with an unknown exception", plg::Severity::Error);
	}
	return {};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_antiAfkPlugin)
