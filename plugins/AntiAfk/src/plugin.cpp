// AntiAfk — Plugify C++ port of the Metamod plugin Anti-Afk (ABKAM), now on the native s2sdk API
// (docs/PLUGIN_STANDARD.md §7: no pisexcompat, no guard library, no own wrapper library).
//
// Behaviour kept identical (the engine-free state machine lives in antiafk_core.cpp and is
// unit-tested by antiafk_core_tests):
//   * settings.ini of <configs>/AntiAfk drives the warning times, the warning count, the
//     "kick instead of spectate" flag, the aim-movement threshold and debug_log;
//   * a one-second check (accumulated from OnPluginUpdate, never an s2sdk timer) walks every
//     in-game, non-bot, non-SourceTV, non-spectator human slot: during warmup and while dead (and
//     after round_start) the idle timer resets, moving a WASD key or turning the view by >=
//     aim_move_threshold_deg resets it too;
//   * idle >= warning_interval: suicide (warnCount < max_warnings - 1) with the "AFK_WarningKill"
//     message, otherwise the final punishment - kick or move to spectators - after which the slot
//     forgets its state;
//   * "round_start" enables the checks and resets every idle timer; "cs_win_panel_match"/"game_end"
//     disable them and clear the warning counters (the original's three HookEvent calls);
//   * all messages go through antiafk.phrases.txt (AFK_Prefix prefix + the message, formatted
//     exactly like the original's snprintf of the phrase) and through s2sdk::PrintToChat with the
//     cs2-menus colour tags turned into their control bytes.
//
// Engine access that Metamod had and s2sdk does not:
//   * g_pGameRules->m_bWarmupPeriod() -> s2sdk::GetGameRules() and the map-gated cached schema
//     offset of CCSGameRules::m_bWarmupPeriod, read with GetEntData2 on the rules object pointer
//     (which is not an entity handle; PITFALLS C6/C8);
//   * pPawn->m_pMovementServices()->m_nButtons().m_pButtonStates()[0] -> s2sdk::GetClientButtons()
//     (the same movement-services button array);
//   * g_pPlayers->RayTrace() + DirFromTrace: the trace *direction* was only ever the view
//     direction, so the port uses s2sdk::GetClientForwardVector() (normalized like DirFromTrace;
//     an unusable vector counts as "no direction");
//   * m_iTeamNum() -> the controller's CBaseEntity::m_iTeamNum (1 byte) at a map-gated cached
//     schema offset; s2sdk's GetClientTeam walks the pawn and warns without one, so it is only the
//     fallback (PITFALLS C5);
//   * m_bPawnIsAlive() -> s2sdk::IsClientAlive();
//   * engine->DisconnectClient(..., NETWORK_DISCONNECT_KICKED) -> s2sdk::KickClient(..., Kicked)
//     with the empty internal message of the old port; g_pPlayers->CommitSuicide(slot, false, true)
//     -> s2sdk::ForcePlayerSuicide(slot, false, true); g_pPlayers->ChangeTeam(slot, 1) ->
//     s2sdk::ChangeClientTeam(slot, CSTeam::Spectator);
//   * StartupServer/OnGetGameRules -> the map gate below. The original created a 1 s timer there; the
//     port accumulates the same 1 s interval from OnPluginUpdate instead (the host pumps updates
//     with a map up) and deliberately never registers an s2sdk timer - see "Tick cadence" below.
//
// Safety rules from handoff/weaponskins/PITFALLS.md, implemented here directly:
//   * A1: OnPluginStart never returns Failed (that stops every Plugify plugin); every failure is
//     logged and the feature is disabled.
//   * A7/C12: OnPluginEnd unhooks the four events and unregisters the map listeners (there is no
//     s2sdk timer to kill), so `plg reload` leaves nothing that points into the unloaded library.
//     A failed hook (false/exception) is remembered and simply not unhooked.
//   * B1/B2: the map gate. No client, entity or schema call is made before the first map and in the
//     window between OnMapEnd and the next OnMapStart. Readiness is granted by OnMapStart, and by
//     the first OnPluginUpdate for a plugin loaded into a running map (only when no OnMapEnd was
//     seen since OnPluginStart); OnMapEnd drops it. OnMapEnd only clears state - it never queries a
//     client.
//   * B4/C9: the 1 s cadence is accumulated from OnPluginUpdate, so it pauses with the map gate and
//     with the checks (round_start/match end); the accumulator is reset to 0 on a gate latch, map
//     start/end, a reload and every pause, and a large update delta is turned into at most one tick
//     (fmod drops the whole missed intervals) - there is no unbounded catch-up and no timer that
//     could survive the map or the plugin unload.
//   * C2: HasClient() = map ready, 0 <= slot < 64, IsClientConnected && IsClientInGame &&
//     !IsClientSourceTV && !IsFakeClient. ResolveController() adds a non-null, IsValidEntPointer-
//     valid controller pointer and a controller handle that is neither 0 (the world) nor -1
//     (INVALID_EHANDLE_INDEX) and that the engine's IsValidEntHandle accepts - PITFALLS C1: a
//     *valid* CEntityHandle may be negative, so its sign is never used to reject it.
//   * C8: GetSchemaOffset is called only while the gate is open and only successful offsets
//     (>= 0) are cached; the cache is dropped in OnPluginEnd so a reload resolves it again.
//   * C11: the original registers no console command, convar or say listener, and neither does
//     this port - there is no command ownership to take over or remove.
//   * C13: the four game events are hooked as Pre in OnPluginStart. "player_disconnect" resolves
//     its "userid" field with s2sdk::GetEventPlayerSlot (the field is player-typed); no entity is
//     looked up in an event handler, so an event arriving during a changelevel is safe. Every
//     callback answers Continue (Handled would swallow the event for the engine and other plugins).
//
// Tick cadence (why no s2sdk timer):
//   s2sdk's TimerSystem::RunFrame() holds a non-recursive mutex while it invokes a timer callback
//   (ref/s2sdk-src/src/core/timer_system.cpp). The AFK tick calls ForcePlayerSuicide(),
//   ChangeClientTeam() and KickClient(); those can synchronously fire game events whose handlers in
//   other plugins create or kill s2sdk timers, which would lock the same mutex on the same thread
//   and deadlock the server. The check is therefore driven by an accumulator in OnPluginUpdate: a
//   plain host callback, no timer lock held, at most one Tick() per update, the same 1 s interval.
//
// Slot reuse / map change (never punish the wrong player):
//   * every slot entry stores the player's identity: the controller pointer, the controller's
//     CEntityHandle (index | serial << 15 - the generation) and the SteamID64 when available. A
//     tick whose identity differs from the stored one starts a fresh state (afkStart = curtime,
//     warnCount = 0) instead of inheriting the previous occupant's warnings;
//   * EvaluateTick works on a copy of the state, and the result is committed only after the gate
//     and the identity have been re-checked in the same tick. A slot that changed hands between
//     the read and the punishment is dropped without printing or acting, so a deferred decision
//     can never suicide/kick/move the new occupant;
//   * OnMapEnd and OnMapStart clear all AFK state, and the "player_disconnect" event erases the
//     slot again, so no warning counter survives a level change or a reconnect.
//
// Config and phrases (production files, no conversion):
//   * the exact pisexcompat candidate order is kept: the raw cwd
//     "addons/configs/AntiAfk/settings.ini" / "antiafk.phrases.txt" first, then the plugin's
//     config, translations and data directories; the first existing file is authoritative even
//     when malformed (no silent switch to a decoy), read with the plugin-local KV1 reader in
//     kv1.hpp;
//   * the phrase language is the chain pisexcompat used: <configs>/core.cfg "ServerLang" first,
//     then s2sdk::GetServerLanguage, then "en"; Source language names ("russian", "english", ...)
//     are normalized to the codes of the phrase file;
//   * messages are colourized with the same tag table pisexcompat used (text.hpp).
//
// Fail soft everywhere: missing bindings, a missing/malformed config or phrases file, a missing
// schema offset and an exception in a callback are logged and skip the affected feature only.

#include "antiafk_core.hpp"
#include "kv1.hpp"
#include "text.hpp"

#include <plg/plugin.hpp>
#include <plg/source_location.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <exception>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

constexpr const char* kLogTag = "[AntiAfk]";

// s2sdk player slots are 0..63 (the engine's MAXPLAYERS); every s2sdk client export indexes its
// client list with the slot, so the range is checked before the first call.
constexpr int32_t kMaxSlots = 64;

// TEAM_SPECTATOR of the original (CS_TEAM_SPECTATOR).
constexpr int32_t kTeamSpectator = static_cast<int32_t>(s2sdk::CSTeam::Spectator);

// IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT (hl2sdk in_buttons.h).
constexpr uint64_t kMoveButtons = 0x8ULL | 0x10ULL | 0x200ULL | 0x400ULL;

// The original's one-second CheckAfkTick interval. The first tick is a full interval after the
// gate/round_start, and a large update delta never produces more than one tick (PITFALLS C9/B4, no
// unbounded catch-up).
constexpr double kTickIntervalSeconds = 1.0;

// The HEAD port loaded its files through pisexcompat, whose paths.cpp tried the raw cwd path
// first and only then the plugin's Plugify directories. The candidate lists below restore that
// exact order (see "config / phrases paths" further down).
constexpr const char* kPluginDir = "AntiAfk";
constexpr const char* kTranslationsDir = "translations";
constexpr const char* kConfigRawPath = "addons/configs/AntiAfk/settings.ini";
constexpr const char* kConfigFileName = "settings.ini";
constexpr const char* kPhrasesRawPath = "antiafk.phrases.txt";
constexpr const char* kCoreConfigRelativePath = "core.cfg";

// --- logging -------------------------------------------------------------------------------------

void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogError(std::string_view message) {
	LogMessage(message, plg::Severity::Error);
}

std::string ToStd(const plg::string& text) {
	return std::string(std::string_view(text));
}

// --- settings / phrases (the original's globals) -------------------------------------------------

antiafk::Settings g_settings;
antiafk::PhraseMap g_phrases;

void Dbg(std::string_view message) {
	if (!g_settings.debugLog) {
		return;
	}
	LogMessage(std::string(kLogTag) + " " + std::string(message), plg::Severity::Debug);
}

// --- per-slot AFK state (the original's g_Afk) ---------------------------------------------------

// The identity of the player a slot entry belongs to. controllerHandle is a CEntityHandle, i.e. it
// carries the entity's serial (generation); a reused slot gets a new handle. SteamID64 is an extra
// check for the rare case where the engine recycles the controller object with a new serial wrap.
struct PlayerIdentity {
	bool valid = false;
	void* controllerPointer = nullptr;
	int32_t controllerHandle = -1;
	uint64_t steamId64 = 0;
};

struct SlotEntry {
	antiafk::State afk;
	PlayerIdentity identity;
};

std::unordered_map<int32_t, SlotEntry> g_afk;

// --- PITFALLS B1/B2: the map gate ----------------------------------------------------------------

bool g_mapReady = false;           // client/entity/schema calls are safe right now
bool g_mapStartSeen = false;       // an OnMapStart arrived since OnPluginStart / the last OnMapEnd
bool g_mapEndSeen = false;         // an OnMapEnd arrived since OnPluginStart
bool g_mapListenersRegistered = false;

// --- state of the plugin itself ------------------------------------------------------------------

bool g_canCheckAfk = false; // the original's g_canCheckAfk (round_start enables the checks)
bool g_loggedChecksOff = false;

// Seconds accumulated since the last AFK check. Driven by OnPluginUpdate only (never an s2sdk
// timer; see "Tick cadence" in the header).
double g_tickAccumulator = 0.0;

int32_t g_warmupOffset = -1; // CCSGameRules::m_bWarmupPeriod, resolved once a map is up
int32_t g_teamOffset = -1;   // CBaseEntity::m_iTeamNum, resolved once a map is up

// Event hooks / map listeners that OnPluginEnd has to take back (PITFALLS A7).
bool g_roundStartHooked = false;
bool g_matchEndHooked = false;
bool g_gameEndHooked = false;
bool g_playerDisconnectHooked = false;

// --- config / phrases paths ----------------------------------------------------------------------
//
// `pisex::KeyValues::LoadFromFile("addons/configs/AntiAfk/settings.ini")` and
// `pisex::LoadPhrasesFile("antiafk.phrases.txt")` resolved their file through
// libs/pisexcompat/src/paths.cpp. Its order was:
//
//   settings: raw cwd "addons/configs/AntiAfk/settings.ini", then
//             <configs>/AntiAfk/settings.ini, <configs>/AntiAfk/translations/settings.ini,
//             <data>/AntiAfk/settings.ini;
//   phrases:  raw cwd "antiafk.phrases.txt", then
//             <configs>/AntiAfk/translations/antiafk.phrases.txt,
//             <configs>/AntiAfk/antiafk.phrases.txt.
//
// The first existing candidate is authoritative, exactly like FirstExisting() + LoadFromFile(): a
// malformed first file fails the load and the caller keeps its defaults, it never falls through to
// a later candidate. Without GetConfigsDir/GetDataDir the roots are empty, but the raw cwd path is
// still first and the relative plugin paths ("AntiAfk/...") are still tried - pisexcompat built
// those with JoinPath("", name), so a missing host never hides a raw file.

std::string JoinPath(std::string_view a, std::string_view b) {
	if (a.empty()) {
		return std::string(b);
	}
	if (b.empty()) {
		return std::string(a);
	}
	std::string out(a);
	if (out.back() != '/') {
		out.push_back('/');
	}
	if (b.front() == '/') {
		b.remove_prefix(1);
	}
	out.append(b);
	return out;
}

std::string HostConfigsRoot() {
	if (plg::GetConfigsDir == nullptr) {
		return {};
	}
	return ToStd(plg::GetConfigsDir());
}

std::string HostDataRoot() {
	if (plg::GetDataDir == nullptr) {
		return {};
	}
	return ToStd(plg::GetDataDir());
}

std::string PluginConfigDir() {
	return JoinPath(HostConfigsRoot(), kPluginDir);
}

std::string PluginTranslationsDir() {
	return JoinPath(PluginConfigDir(), kTranslationsDir);
}

std::string PluginDataDir() {
	return JoinPath(HostDataRoot(), kPluginDir);
}

// <plg::GetConfigsDir()>/<relative>; "" when the plugin runs without the Plugify host. Only the
// shared core.cfg still uses this: pisexcompat read it directly under GetConfigsDir() and did not
// search the raw Metamod path for it.
std::string ConfigPath(std::string_view relative) {
	const std::string root = HostConfigsRoot();
	if (root.empty()) {
		return {};
	}
	return JoinPath(root, relative);
}

bool FileExists(const std::string& path) {
	if (path.empty()) {
		return false;
	}
	std::ifstream file(path, std::ios::binary);
	return file.good();
}

std::string FirstExisting(const std::vector<std::string>& candidates) {
	for (const std::string& candidate : candidates) {
		if (FileExists(candidate)) {
			return candidate;
		}
	}
	return {};
}

std::vector<std::string> SettingsCandidates() {
	std::vector<std::string> candidates;
	candidates.emplace_back(kConfigRawPath);
	candidates.push_back(JoinPath(PluginConfigDir(), kConfigFileName));
	candidates.push_back(JoinPath(PluginTranslationsDir(), kConfigFileName));
	candidates.push_back(JoinPath(PluginDataDir(), kConfigFileName));
	return candidates;
}

std::vector<std::string> PhrasesCandidates() {
	std::vector<std::string> candidates;
	candidates.emplace_back(kPhrasesRawPath);
	candidates.push_back(JoinPath(PluginTranslationsDir(), kPhrasesRawPath));
	candidates.push_back(JoinPath(PluginConfigDir(), kPhrasesRawPath));
	return candidates;
}

// The first existing candidate, or the most likely location for the error log when none exists -
// pisexcompat's ResolvePath()/ResolveTranslationsPath() fallback (candidates[1]).
std::string ResolveFirstExisting(const std::vector<std::string>& candidates) {
	const std::string existing = FirstExisting(candidates);
	if (!existing.empty()) {
		return existing;
	}
	if (candidates.size() > 1) {
		return candidates[1];
	}
	return candidates.empty() ? std::string() : candidates.front();
}

// --- phrase language (the pisexcompat chain, kept locally) ----------------------------------------

// Source language names -> the codes the phrase files use (pisexcompat's NormalizeLanguage, the
// same table cs2-menus had). Unknown names are returned unchanged.
std::string NormalizeLanguage(std::string_view name) {
	std::string lower;
	lower.reserve(name.size());
	for (const char c : name) {
		lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	}
	if (lower == "russian" || lower == "ru") {
		return "ru";
	}
	if (lower == "english" || lower == "en") {
		return "en";
	}
	if (lower == "ukrainian" || lower == "ua" || lower == "uk") {
		return "ua";
	}
	if (lower == "portuguese" || lower == "pt-br" || lower == "brazilian") {
		return "pt-BR";
	}
	if (lower == "spanish" || lower == "es") {
		return "es";
	}
	if (lower == "german" || lower == "de") {
		return "de";
	}
	if (lower == "french" || lower == "fr") {
		return "fr";
	}
	if (lower == "polish" || lower == "pl") {
		return "pl";
	}
	if (lower == "turkish" || lower == "tr") {
		return "tr";
	}
	if (lower == "schinese" || lower == "zh-cn") {
		return "zh-CN";
	}
	return std::string(name);
}

// The Metamod core setting pisexcompat read first: <configs>/core.cfg "ServerLang" ("ru",
// "russian", ...). "" when the file is missing, malformed or has no ServerLang.
std::string CoreConfigLanguage() {
	const std::string path = ConfigPath(kCoreConfigRelativePath);
	if (path.empty() || !FileExists(path)) {
		return {};
	}
	const std::optional<antiafk::KvNode> root = antiafk::LoadKv1File(path);
	if (!root) {
		return {};
	}
	return std::string(root->GetString("ServerLang", ""));
}

// s2sdk's ServerLanguage (envs/plugins/s2sdk/settings.jsonc); available before the first map.
std::string ServerLanguage() {
	if (__s2sdk_GetServerLanguage == nullptr) {
		return {};
	}
	try {
		return ToStd(s2sdk::GetServerLanguage());
	} catch (...) {
		return {};
	}
}

// The language the phrase table is resolved for: <configs>/core.cfg "ServerLang" first, then
// s2sdk's ServerLanguage, then "en" - the chain pisexcompat's LoadPhrasesFile(..., nullptr) used.
std::string PhrasesLanguage() {
	if (const std::string core = CoreConfigLanguage(); !core.empty()) {
		return NormalizeLanguage(core);
	}
	if (const std::string server = ServerLanguage(); !server.empty()) {
		return NormalizeLanguage(server);
	}
	return "en";
}

// --- config / phrases loading --------------------------------------------------------------------

void LoadConfig() {
	// First existing wins. If that file is malformed, stop here with the defaults: trying the next
	// candidate would silently switch to different kick/time settings, which the old loader never
	// did (LoadFromFile returned false and the caller used its defaults).
	const std::string path = ResolveFirstExisting(SettingsCandidates());
	const std::optional<antiafk::KvNode> root = antiafk::LoadKv1File(path);
	if (!root) {
		LogError(std::string(kLogTag) + " failed to load " + path + " - keeping the built-in defaults");
		return;
	}
	// The original's (int)GetFloat() / GetInt() reads, including their defaults.
	g_settings.initialWarningTime = static_cast<int>(root->GetFloat("initial_warning_time", 15.0f));
	g_settings.warningInterval = static_cast<int>(root->GetFloat("warning_interval", 35.0f));
	g_settings.maxWarnings = root->GetInt("max_warnings", 3);
	g_settings.kickInsteadOfSpec = root->GetInt("kick_instead_of_spec", 0) != 0;
	g_settings.aimThreshDeg = root->GetFloat("aim_move_threshold_deg", 1.0f);
	g_settings.debugLog = root->GetInt("debug_log", 1) != 0;
	Dbg("config loaded: warn=" + std::to_string(g_settings.initialWarningTime) +
		", interval=" + std::to_string(g_settings.warningInterval) +
		", max=" + std::to_string(g_settings.maxWarnings) +
		", kick=" + std::to_string(static_cast<int>(g_settings.kickInsteadOfSpec)) +
		", aim=" + std::to_string(g_settings.aimThreshDeg) +
		", dbg=" + std::to_string(static_cast<int>(g_settings.debugLog)));
}

void LoadPhrases() {
	g_phrases.clear();
	// Same first-existing-authoritative rule as LoadConfig(): a malformed raw phrases file must
	// not fall through to a valid plugin decoy and silently change every message.
	const std::string path = ResolveFirstExisting(PhrasesCandidates());
	const std::optional<antiafk::KvNode> root = antiafk::LoadKv1File(path);
	if (!root) {
		LogError(std::string(kLogTag) + " failed to load " + path + " - using the built-in message texts");
		return;
	}
	const std::string language = PhrasesLanguage();
	g_phrases = antiafk::ResolvePhrases(*root, language);
	Dbg(std::to_string(g_phrases.size()) + " phrases loaded from " + path + " (language \"" + language + "\")");
}

// The phrase of a key, or "" when the key is missing/empty (the original's g_Phrases.find()).
const std::string& PhraseText(const char* key) {
	static const std::string empty;
	if (key == nullptr) {
		return empty;
	}
	const auto it = g_phrases.find(key);
	if (it == g_phrases.end() || it->second.empty()) {
		return empty;
	}
	return it->second;
}

// --- printing (the original's PrintChat) ---------------------------------------------------------

// "<AFK_Prefix> <message>" colorized, exactly like pisexcompat's PrintToChat(prefix + " " + body)
// after the port dropped the old leading space. The caller has already validated the slot and the
// map gate; this function only adds the print binding check (like pisex's CanPrint()).
void PrintLine(int32_t slot, std::string_view message) {
	if (!g_mapReady || slot < 0 || slot >= kMaxSlots || __s2sdk_PrintToChat == nullptr) {
		return;
	}
	const std::string& prefix = PhraseText("AFK_Prefix");
	std::string line;
	if (!prefix.empty()) {
		line.reserve(prefix.size() + 1 + message.size());
		line.append(prefix);
		line.push_back(' ');
	} else {
		line.reserve(message.size());
	}
	line.append(message);
	try {
		s2sdk::PrintToChat(slot, plg::string(antiafk::Colorize(line)));
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " PrintToChat failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " PrintToChat failed");
	}
}

// Formats the phrase with the snprintf arguments of the original and prints it.
void PrintPhrase(int32_t slot, const char* key, const char* fallback, int a = 0, int b = 0, bool twoArgs = false) {
	const std::string& phrase = PhraseText(key);
	const char* format = phrase.empty() ? fallback : phrase.c_str();
	const std::string body = twoArgs ? antiafk::FormatTwo(format, a, b) : antiafk::FormatOne(format, a);
	PrintLine(slot, body);
}

// --- PITFALLS C8: map-gated schema offsets -------------------------------------------------------

// -1 while the gate is closed (GetSchemaOffset segfaults before the first map), and a failed
// lookup is not cached, so a field that was not available yet is retried instead of poisoned.
int32_t LookupSchemaOffset(const char* className, const char* fieldName) {
	if (!g_mapReady || __s2sdk_GetSchemaOffset == nullptr) {
		return -1;
	}
	try {
		return s2sdk::GetSchemaOffset(plg::string(className), plg::string(fieldName));
	} catch (...) {
		return -1;
	}
}

// --- PITFALLS C2: client / controller guards -----------------------------------------------------

// Map ready, slot in range and a connected, in-game, non-SourceTV, non-bot client. This is the
// original's IsInGame()+IsFakeClient() pair with the explicit SourceTV/connected checks
// PLUGIN_STANDARD §7 asks for.
bool HasClient(int32_t slot) {
	if (!g_mapReady || slot < 0 || slot >= kMaxSlots) {
		return false;
	}
	if (__s2sdk_IsClientConnected == nullptr || __s2sdk_IsClientInGame == nullptr ||
		__s2sdk_IsClientSourceTV == nullptr || __s2sdk_IsFakeClient == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(slot) && s2sdk::IsClientInGame(slot) &&
			   !s2sdk::IsClientSourceTV(slot) && !s2sdk::IsFakeClient(slot);
	} catch (...) {
		return false;
	}
}

// The slot's controller pointer and its CEntityHandle, both validated. -1/false whenever the slot
// has no usable controller. PITFALLS C1: only 0 (the world) and -1 (INVALID_EHANDLE_INDEX) are
// rejected by hand; a valid handle may be negative once the serial bits reach the sign bit, and
// the engine's IsValidEntHandle is the authority on the rest.
bool ResolveController(int32_t slot, void*& pointer, int32_t& handle) {
	pointer = nullptr;
	handle = -1;
	if (!HasClient(slot)) {
		return false;
	}
	if (__s2sdk_PlayerSlotToEntPointer == nullptr || __s2sdk_PlayerSlotToEntHandle == nullptr ||
		__s2sdk_IsValidEntHandle == nullptr) {
		return false;
	}
	try {
		void* controller = s2sdk::PlayerSlotToEntPointer(slot);
		if (controller == nullptr) {
			return false;
		}
		if (__s2sdk_IsValidEntPointer != nullptr && !s2sdk::IsValidEntPointer(controller)) {
			return false;
		}
		const int32_t controllerHandle = s2sdk::PlayerSlotToEntHandle(slot);
		if (controllerHandle == 0 || controllerHandle == -1 || !s2sdk::IsValidEntHandle(controllerHandle)) {
			return false;
		}
		pointer = controller;
		handle = controllerHandle;
		return true;
	} catch (...) {
		return false;
	}
}

// The original never asked for a bot's SteamID (PITFALLS B5); HasClient already excluded bots.
uint64_t ClientSteamId(int32_t slot) {
	if (__s2sdk_GetClientSteamID64 == nullptr) {
		return 0;
	}
	try {
		return s2sdk::GetClientSteamID64(slot);
	} catch (...) {
		return 0;
	}
}

bool SameIdentity(const PlayerIdentity& a, const PlayerIdentity& b) {
	if (!a.valid || !b.valid) {
		return false;
	}
	if (a.controllerPointer != b.controllerPointer || a.controllerHandle != b.controllerHandle) {
		return false;
	}
	// A 0 SteamID is "not available here" (e.g. a client that is still authenticating), not a
	// different player; the handle/generation check above already decides that case.
	if (a.steamId64 != 0 && b.steamId64 != 0 && a.steamId64 != b.steamId64) {
		return false;
	}
	return true;
}

// Re-checked right before a tick commits its state and before any destructive action: the map
// gate may have closed (changelevel) and the slot may have been recycled to a new controller while
// the tick was running.
bool StillSamePlayer(int32_t slot, const PlayerIdentity& identity) {
	if (!g_mapReady || !identity.valid) {
		return false;
	}
	void* controller = nullptr;
	int32_t handle = -1;
	if (!ResolveController(slot, controller, handle)) {
		return false;
	}
	if (controller != identity.controllerPointer || handle != identity.controllerHandle) {
		return false;
	}
	const uint64_t steamId = ClientSteamId(slot);
	if (identity.steamId64 != 0 && steamId != 0 && steamId != identity.steamId64) {
		return false;
	}
	return true;
}

// --- engine reads (the original's schema/movement reads) -----------------------------------------

float CurrentTime() {
	if (!g_mapReady || __s2sdk_GetGameTime == nullptr) {
		return 0.0f;
	}
	try {
		return s2sdk::GetGameTime();
	} catch (...) {
		return 0.0f;
	}
}

bool InWarmup(void* gameRules) {
	// The rules object pointer is re-fetched on every tick: s2sdk::GetGameRules() reads the engine's
	// *current* global (it is null between maps), and caching it would keep a dangling pointer after
	// a changelevel. Only the schema offset is cached (PITFALLS B2/C8).
	if (!g_mapReady || gameRules == nullptr || __s2sdk_GetEntData2 == nullptr) {
		return false;
	}
	if (g_warmupOffset < 0) {
		g_warmupOffset = LookupSchemaOffset("CCSGameRules", "m_bWarmupPeriod");
	}
	if (g_warmupOffset < 0) {
		return false;
	}
	try {
		// CCSGameRules is not an entity: the offset is read directly on the rules object pointer
		// (m_bWarmupPeriod is one byte).
		return s2sdk::GetEntData2(gameRules, g_warmupOffset, 1) != 0;
	} catch (...) {
		return false;
	}
}

int32_t TeamOf(int32_t slot, void* controller) {
	// The team lives on the controller (the original's m_iTeamNum()). Read it at the cached schema
	// offset, as pisexcompat's GetPlayerTeam did; s2sdk's GetClientTeam is only the fallback for a
	// missing offset because it walks the pawn and warns for a client without one (PITFALLS C5).
	if (!g_mapReady || controller == nullptr) {
		return 0;
	}
	if (g_teamOffset < 0) {
		g_teamOffset = LookupSchemaOffset("CBaseEntity", "m_iTeamNum");
	}
	if (g_teamOffset >= 0 && __s2sdk_GetEntData2 != nullptr) {
		try {
			return static_cast<int32_t>(s2sdk::GetEntData2(controller, g_teamOffset, 1) & 0xFF);
		} catch (...) {
			return 0;
		}
	}
	// The pisexcompat fallback: PawnExists() -> s2sdk's pawn-based team read. Only used when the
	// schema offset is unavailable; GetClientPawn needs the controller that was validated above.
	if (__s2sdk_GetClientPawn != nullptr && __s2sdk_GetClientTeam != nullptr) {
		try {
			if (s2sdk::GetClientPawn(slot) != nullptr) {
				return static_cast<int32_t>(s2sdk::GetClientTeam(slot));
			}
		} catch (...) {
			return 0;
		}
	}
	return 0;
}

bool ClientAlive(int32_t slot) {
	// The original's m_bPawnIsAlive() / pisexcompat's IsAlive(): s2sdk::IsClientAlive dereferences
	// the pawn, so it is only called after the controller checks above.
	if (__s2sdk_IsClientAlive == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientAlive(slot);
	} catch (...) {
		return false;
	}
}

bool MovingKeys(int32_t slot) {
	if (__s2sdk_GetClientButtons == nullptr) {
		return false;
	}
	try {
		return (s2sdk::GetClientButtons(slot, 0) & kMoveButtons) != 0;
	} catch (...) {
		return false;
	}
}

bool ClientForward(int32_t slot, plg::vec3& out) {
	if (__s2sdk_GetClientForwardVector == nullptr) {
		return false;
	}
	try {
		out = s2sdk::GetClientForwardVector(slot);
		return true;
	} catch (...) {
		return false;
	}
}

// --- the tick (the original's CheckAfkTick) ------------------------------------------------------

// The stored entry for a slot, reset to a fresh timer when the slot now belongs to another player.
SlotEntry& EntryFor(int32_t slot, const PlayerIdentity& identity, float curtime) {
	const auto found = g_afk.find(slot);
	if (found == g_afk.end()) {
		SlotEntry entry;
		entry.afk.afkStart = curtime;
		entry.identity = identity;
		Dbg("init slot " + std::to_string(slot) + " (afkStart=" + std::to_string(curtime) + ")");
		return g_afk.emplace(slot, entry).first->second;
	}
	if (!SameIdentity(found->second.identity, identity)) {
		// The slot changed hands without a player_disconnect (or that event was lost): do not carry
		// the previous occupant's warnCount/warnedOnce into the new player.
		found->second = SlotEntry{};
		found->second.afk.afkStart = curtime;
		found->second.identity = identity;
		Dbg("slot " + std::to_string(slot) + ": player changed -> fresh AFK state");
	}
	return found->second;
}

void ApplyAction(int32_t slot, const antiafk::Action& action) {
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
		PrintPhrase(slot, "AFK_WarningKill", "Предупреждение %d/%d: вы убиты за AFK.", action.a, action.b, true);
		Dbg("slot " + std::to_string(slot) + ": soft punish (kill), warnCount=" + std::to_string(action.a));
		break;
	case antiafk::ActionKind::SpecFinal:
		PrintPhrase(slot, "AFK_SpecFinal", "Вы были переведены в наблюдатели из-за AFK.");
		Dbg("slot " + std::to_string(slot) + ": final punish -> SPEC");
		break;
	case antiafk::ActionKind::Suicide:
		if (__s2sdk_ForcePlayerSuicide != nullptr) {
			try {
				s2sdk::ForcePlayerSuicide(slot, false, true);
			} catch (...) {
			}
		}
		break;
	case antiafk::ActionKind::Spec:
		if (__s2sdk_ChangeClientTeam != nullptr) {
			try {
				s2sdk::ChangeClientTeam(slot, s2sdk::CSTeam::Spectator);
			} catch (...) {
			}
		}
		break;
	case antiafk::ActionKind::Kick:
		if (__s2sdk_KickClient != nullptr) {
			try {
				// The old port kicked with an empty internal message and NETWORK_DISCONNECT_KICKED.
				s2sdk::KickClient(slot, s2sdk::NetworkDisconnectionReason::Kicked, plg::string());
			} catch (...) {
			}
			Dbg("slot " + std::to_string(slot) + ": final punish -> KICK");
		}
		break;
	}
}

void TickSlot(int32_t slot, float curtime, bool inWarmup) {
	if (!HasClient(slot)) {
		// A slot with no connected human player must not keep a warnCount: the player_disconnect
		// event normally erases it, but the event can be missed or arrive with a wrong slot, and a
		// stale counter would let the next occupant escalate straight to the final punishment.
		g_afk.erase(slot);
		return;
	}
	void* controller = nullptr;
	int32_t controllerHandle = -1;
	if (!ResolveController(slot, controller, controllerHandle)) {
		// Same for a slot without a valid controller entity: fail closed instead of carrying the
		// state across a broken/absent controller.
		g_afk.erase(slot);
		return;
	}
	const PlayerIdentity identity{true, controller, controllerHandle, ClientSteamId(slot)};

	// A spectator is never checked (the original's isSpec continue) - before any state is created.
	if (TeamOf(slot, controller) == kTeamSpectator) {
		return;
	}

	const bool isAlive = ClientAlive(slot);

	// Both reads hang off the pawn: the original only touched them when isAlive, and EvaluateTick()
	// ignores both values when !isAlive, so skipping the reads then is behaviour-identical (and
	// keeps the pawn-less connect window out of these calls).
	bool movingKeys = false;
	plg::vec3 aimDir{0.0f, 0.0f, 0.0f};
	const plg::vec3* aim = nullptr;
	if (isAlive) {
		movingKeys = MovingKeys(slot);
		if (ClientForward(slot, aimDir)) {
			aim = &aimDir;
		}
	}

	SlotEntry& entry = EntryFor(slot, identity, curtime);

	// Evaluate on a copy: the state is committed only after the gate and the identity are
	// re-validated (see below), so a recycled slot never inherits the decision of its predecessor.
	antiafk::State trial = entry.afk;
	const antiafk::TickOutcome outcome =
			antiafk::EvaluateTick(trial, g_settings, curtime, inWarmup, isAlive, movingKeys, aim);

	if (!StillSamePlayer(slot, identity)) {
		// The map is going away or the slot now has another player: drop the pending decision
		// (no print, no suicide/kick/spec) and the state, so the next tick starts clean.
		g_afk.erase(slot);
		return;
	}

	entry.afk = trial;
	for (const antiafk::Action& action : outcome.actions) {
		ApplyAction(slot, action);
	}
	if (outcome.eraseState) {
		g_afk.erase(slot);
	}
}

void Tick() {
	// PITFALLS B2: between OnMapEnd and the next OnMapStart s2sdk's client list is torn down, and
	// PITFALLS B1: before the first map it does not exist at all. Both are covered by the gate.
	if (!g_mapReady) {
		return;
	}
	if (!g_canCheckAfk) {
		if (!g_loggedChecksOff) {
			g_loggedChecksOff = true;
			Dbg("tick: checks OFF (waiting for round_start)");
		}
		return;
	}
	g_loggedChecksOff = false;

	if (__s2sdk_GetGameTime == nullptr || __s2sdk_GetGameRules == nullptr || __s2sdk_GetEntData2 == nullptr) {
		return;
	}
	void* gameRules = nullptr;
	float curtime = 0.0f;
	try {
		// The original bailed out of the whole tick when the game-rules object was unavailable
		// (`if (!g_pGameRules) return;`); the port keeps that (audit L3).
		gameRules = s2sdk::GetGameRules();
		if (gameRules == nullptr) {
			return;
		}
		curtime = s2sdk::GetGameTime();
	} catch (...) {
		return;
	}
	const bool inWarmup = InWarmup(gameRules);
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		try {
			TickSlot(slot, curtime, inWarmup);
		} catch (const std::exception& ex) {
			LogError(std::string(kLogTag) + " tick of slot " + std::to_string(slot) + " failed: " + ex.what());
		} catch (...) {
			LogError(std::string(kLogTag) + " tick of slot " + std::to_string(slot) + " failed");
		}
	}
}

// --- map gate listeners (PITFALLS B1/B2) ---------------------------------------------------------

// s2sdk listener callbacks are plain function pointers called on the game thread; they must not
// throw and OnMapEnd must not touch a client (only state + logging).

void OnMapStartListener() {
	try {
		// From here on the clients and entities of the new map are in place.
		g_mapReady = true;
		g_mapStartSeen = true;
		g_mapEndSeen = false;
		g_tickAccumulator = 0.0;
		// Never carry an AFK timer/warning count across a level change: the slot->player mapping
		// of the old map is gone, and a new occupant must not inherit anything.
		g_afk.clear();
		Dbg("map start: gate open, AFK state cleared");
	} catch (...) {
		// An s2sdk listener must not throw into the engine.
	}
}

void OnMapEndListener() {
	try {
		// PITFALLS B2: from here until the next map start every s2sdk client query can crash. This
		// handler only drops state.
		g_mapReady = false;
		g_mapStartSeen = false;
		g_mapEndSeen = true;
		g_tickAccumulator = 0.0;
		g_afk.clear();
		Dbg("map end: gate closed, AFK state cleared");
	} catch (...) {
		// An s2sdk listener must not throw into the engine.
	}
}

void UnregisterMapListeners();

void RegisterMapListeners() {
	if (g_mapListenersRegistered) {
		return;
	}
	if (__s2sdk_OnMapStart_Register == nullptr || __s2sdk_OnMapEnd_Register == nullptr) {
		LogError(std::string(kLogTag) +
				 " the s2sdk map listeners are unavailable - every client call stays refused");
		return;
	}
	// Set the bookkeeping before the calls: if the second registration throws after the first one
	// succeeded, UnregisterMapListeners() below still has to take the first one back.
	g_mapListenersRegistered = true;
	try {
		s2sdk::OnMapStart_Register(&OnMapStartListener);
		s2sdk::OnMapEnd_Register(&OnMapEndListener);
	} catch (...) {
		LogError(std::string(kLogTag) + " registering the s2sdk map listeners failed");
		UnregisterMapListeners();
	}
}

void UnregisterMapListeners() {
	if (!g_mapListenersRegistered) {
		return;
	}
	g_mapListenersRegistered = false;
	if (__s2sdk_OnMapStart_Unregister != nullptr) {
		try {
			s2sdk::OnMapStart_Unregister(&OnMapStartListener);
		} catch (...) {
		}
	}
	if (__s2sdk_OnMapEnd_Unregister != nullptr) {
		try {
			s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
		} catch (...) {
		}
	}
}

// --- event hooks (the original's three HookEvent calls + the disconnect guard) --------------------

s2sdk::ResultType OnRoundStartEvent(const plg::string& eventName, void* event, bool dontBroadcast) {
	(void)eventName;
	(void)event;
	(void)dontBroadcast;
	try {
		g_canCheckAfk = true;
		g_tickAccumulator = 0.0; // the first check after enabling is a full interval later
		const float now = CurrentTime();
		for (auto& [slot, entry] : g_afk) {
			antiafk::ResetState(entry.afk, now, true);
			(void)slot;
		}
		Dbg("round_start: checks ON; reset all AFK timers");
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " round_start handler failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " round_start handler failed");
	}
	// Continue: a Pre hook result >= Handled would swallow the event for the engine and others.
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnMatchEndEvent(const plg::string& eventName, void* event, bool dontBroadcast) {
	(void)event;
	(void)dontBroadcast;
	try {
		g_canCheckAfk = false;
		g_tickAccumulator = 0.0; // paused: do not accumulate a catch-up tick
		for (auto& [slot, entry] : g_afk) {
			entry.afk.warnCount = 0;
			(void)slot;
		}
		const std::string name = eventName.empty() ? std::string("match end") : ToStd(eventName);
		Dbg(name + ": checks OFF");
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " match end handler failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " match end handler failed");
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnPlayerDisconnectEvent(const plg::string& eventName, void* event, bool dontBroadcast) {
	(void)eventName;
	(void)dontBroadcast;
	try {
		if (event == nullptr || __s2sdk_GetEventPlayerSlot == nullptr) {
			return s2sdk::ResultType::Continue;
		}
		// The skill of the original: "userid" is a player-typed event field, so it must be resolved
		// with GetEventPlayerSlot (PITFALLS C13), not read as an int and used as a slot.
		const int32_t slot = s2sdk::GetEventPlayerSlot(event, plg::string("userid"));
		if (slot >= 0 && slot < kMaxSlots) {
			// The slot lost its player: his AFK state (including the warning count) must not carry
			// over to the next occupant.
			g_afk.erase(slot);
			Dbg("slot " + std::to_string(slot) + ": state dropped on disconnect");
		}
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " player_disconnect handler failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " player_disconnect handler failed");
	}
	return s2sdk::ResultType::Continue;
}

bool HookOne(const char* name, s2sdk::EventCallback callback) {
	if (__s2sdk_HookEvent == nullptr) {
		LogError(std::string(kLogTag) + " the s2sdk event bindings are unavailable - '" + name + "' is not hooked");
		return false;
	}
	try {
		const s2sdk::EventHookError result = s2sdk::HookEvent(plg::string(name), callback, s2sdk::HookMode::Pre);
		if (result == s2sdk::EventHookError::Okay) {
			return true;
		}
		LogError(std::string(kLogTag) + " hooking '" + name + "' failed - that feature is disabled");
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " hooking '" + name + "' failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " hooking '" + name + "' failed");
	}
	return false;
}

void UnhookOne(const char* name, s2sdk::EventCallback callback, bool& hooked) {
	if (!hooked) {
		return;
	}
	hooked = false;
	if (__s2sdk_UnhookEvent == nullptr) {
		return;
	}
	try {
		s2sdk::UnhookEvent(plg::string(name), callback, s2sdk::HookMode::Pre);
	} catch (...) {
	}
}

void HookEvents() {
	if (!g_roundStartHooked) {
		g_roundStartHooked = HookOne("round_start", &OnRoundStartEvent);
	}
	if (!g_matchEndHooked) {
		g_matchEndHooked = HookOne("cs_win_panel_match", &OnMatchEndEvent);
	}
	if (!g_gameEndHooked) {
		g_gameEndHooked = HookOne("game_end", &OnMatchEndEvent);
	}
	if (!g_playerDisconnectHooked) {
		g_playerDisconnectHooked = HookOne("player_disconnect", &OnPlayerDisconnectEvent);
	}
}

void UnhookEvents() {
	UnhookOne("round_start", &OnRoundStartEvent, g_roundStartHooked);
	UnhookOne("cs_win_panel_match", &OnMatchEndEvent, g_matchEndHooked);
	UnhookOne("game_end", &OnMatchEndEvent, g_gameEndHooked);
	UnhookOne("player_disconnect", &OnPlayerDisconnectEvent, g_playerDisconnectHooked);
}

// --- plugin lifecycle ----------------------------------------------------------------------------

class AntiAfkPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override {
		try {
			// A reload starts from a clean sheet; OnPluginEnd already unregistered everything. These
			// calls are no-ops when nothing is registered and keep a host that calls Start twice from
			// leaking a hook/listener.
			UnhookEvents();
			UnregisterMapListeners();
			g_afk.clear();
			g_canCheckAfk = false;
			g_loggedChecksOff = false;
			g_mapReady = false;
			g_mapStartSeen = false;
			g_mapEndSeen = false;
			g_tickAccumulator = 0.0;
			g_warmupOffset = -1;
			g_teamOffset = -1;

			// File I/O only: no client, entity or schema call happens in OnPluginStart (PITFALLS B1).
			LoadConfig();
			LoadPhrases();

			RegisterMapListeners();
			HookEvents();
		} catch (const std::exception& ex) {
			LogError(std::string(kLogTag) + " OnPluginStart failed: " + ex.what());
		} catch (...) {
			LogError(std::string(kLogTag) + " OnPluginStart failed with an unknown exception");
		}
		return {}; // never Failed: that stops every plugify plugin (PITFALLS A1)
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override {
		try {
			// PITFALLS B1: a plugin loaded into a running map never saw OnMapStart; its first update
			// is the "a map is up" signal. An OnMapEnd seen since OnPluginStart means the server is
			// between maps, so the latch waits for the next OnMapStart instead.
			if (!g_mapReady && g_mapListenersRegistered && (g_mapStartSeen || !g_mapEndSeen)) {
				g_mapReady = true;
				// The first check is a full interval after the map is usable, not immediately.
				g_tickAccumulator = 0.0;
				Dbg("map gate opened by the first update");
			}

			// Tick cadence (header): accumulate the host's delta; no s2sdk timer is involved. While
			// the gate is closed or the checks are off (before round_start / after match end) the
			// accumulator is dropped, so resuming never fires an immediate catch-up tick.
			if (!g_mapReady || !g_canCheckAfk) {
				g_tickAccumulator = 0.0;
				return {};
			}
			const double seconds = std::chrono::duration<double>(dt).count();
			if (!(seconds > 0.0) || !std::isfinite(seconds)) {
				return {};
			}
			g_tickAccumulator += seconds;
			if (g_tickAccumulator < kTickIntervalSeconds) {
				return {};
			}
			// At most one check per update. A long delta (server hitch, map load, host throttling)
			// drops the whole missed intervals instead of replaying them: no unbounded catch-up.
			g_tickAccumulator = std::fmod(g_tickAccumulator, kTickIntervalSeconds);
			Tick();
		} catch (const std::exception& ex) {
			LogError(std::string(kLogTag) + " update failed: " + ex.what());
		} catch (...) {
			LogError(std::string(kLogTag) + " update failed with an unknown exception");
		}
		return {};
	}

	plg::PluginResult OnPluginEnd() override {
		try {
			// PITFALLS A7: every hook/listener points into this library, which is about to go away;
			// `plg reload` must be able to load it again without duplicates. There is no s2sdk timer
			// to kill (the tick lives in OnPluginUpdate).
			UnhookEvents();
			UnregisterMapListeners();

			g_afk.clear();
			g_canCheckAfk = false;
			g_loggedChecksOff = false;
			g_mapReady = false;
			g_mapStartSeen = false;
			g_mapEndSeen = true;
			g_tickAccumulator = 0.0;
			// PITFALLS C8: a reload resolves the offsets again instead of reusing this instance's.
			g_warmupOffset = -1;
			g_teamOffset = -1;
		} catch (const std::exception& ex) {
			LogError(std::string(kLogTag) + " OnPluginEnd failed: " + ex.what());
		} catch (...) {
			LogError(std::string(kLogTag) + " OnPluginEnd failed with an unknown exception");
		}
		return {};
	}
};

AntiAfkPlugin g_antiAfkPlugin;

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_antiAfkPlugin)
