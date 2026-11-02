// anti_afk_plugin_tests — the plugin-level test of the pure-s2sdk AntiAfk port
// (docs/PLUGIN_STANDARD.md §7: no pisexcompat, no guard library).
//
// The plugin TU is compiled into this test (`#include "../src/plugin.cpp"`), so the full plugin
// code path is exercised; every __s2sdk_* binding slot is a recording fake and the files the plugin
// reads come from a temporary <configs> root. Covered:
//
//   1. the plugin-local KV1 reader and the shipped production settings.ini / antiafk.phrases.txt
//      (tests/data copies): comments, BOM, malformed files, the loader's atoi/strtof semantics, the
//      truncating (int)GetFloat() of the two time settings, and the phrase "en" fallback; the exact
//      pisexcompat candidate order (raw cwd first, then plugin config/translations/data; the first
//      existing file is authoritative even when malformed, and a missing host still sees raw);
//   2. the phrase language chain pisexcompat had: <configs>/core.cfg "ServerLang" first, then
//      s2sdk::GetServerLanguage, then "en", with the cs2-menus {TAG} colour bytes;
//   3. PITFALLS B1/B2 — the map gate: no client call before the first map, between OnMapEnd and
//      the next OnMapStart, in the map-change window when the plugin was reloaded, or when the
//      map listeners are unavailable; the first-update latch for a plugin loaded mid-map;
//   4. the update-driven cadence (deliberately no s2sdk timer: TimerSystem::RunFrame holds a
//      non-recursive mutex across the callback): one check per full 1 s accumulated in
//      OnPluginUpdate, one check at most per update, no unbounded catch-up after a large delta,
//      and the accumulator dropped by pause/map end/reload so resuming cannot fire immediately;
//   5. the whole anti-AFK behaviour through the update-driven tick: the first warning, the countdown,
//      the suicide + AFK_WarningKill escalation, the final move-to-spectators + AFK_SpecFinal, the
//      kick_instead_of_spec path, the WASD and aim-movement resets, warmup/death resets and the
//      spectator skip;
//   6. PITFALLS C2/C1 — connected / in-game / SourceTV / bot / controller-pointer / controller-
//      handle guards (0 and -1 rejected, a valid negative CEntityHandle accepted, an engine
//      rejection refused) and the per-slot exception isolation;
//   7. slot reuse, player_disconnect (GetEventPlayerSlot on "userid") and a level change never
//      inherit warnCount or punish the new occupant: the identity (controller handle/generation +
//      pointer + SteamID64) is re-checked before the tick commits or acts;
//   8. A7 — teardown: the four Pre event hooks are removed with the same callbacks, the map
//      listeners are unregistered, the gate closes, the state and the schema offsets are dropped,
//      a reload registers everything exactly once again, a stale update cannot reach a client, and
//      the s2sdk timer API is never touched.
//
// No pisexcompat, no Plugify host, no engine. Run by ../test.sh (build/tests/antiafk_plugin_tests).

#include "../src/plugin.cpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef ANTIAFK_TEST_DATA_DIR
#define ANTIAFK_TEST_DATA_DIR ""
#endif

namespace {

// ---------------------------------------------------------------------------------------------
// checks
// ---------------------------------------------------------------------------------------------

int g_checks = 0;
int g_failures = 0;

void Section(const char* name) {
	std::printf("-- %s\n", name);
}

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

void CheckEqInt(int64_t actual, int64_t expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected %lld, got %lld)\n", what.c_str(),
					static_cast<long long>(expected), static_cast<long long>(actual));
	}
}

void CheckEqStr(std::string_view actual, std::string_view expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected '%s', got '%s')\n", what.c_str(), std::string(expected).c_str(),
					std::string(actual).c_str());
	}
}

void CheckNear(float actual, float expected, const std::string& what) {
	++g_checks;
	if (std::fabs(actual - expected) > 1e-5f) {
		++g_failures;
		std::printf("FAIL: %s (expected %f, got %f)\n", what.c_str(), static_cast<double>(expected),
					static_cast<double>(actual));
	}
}

// Colour control bytes of the cs2-menus table. Built from char codes so no following hex digit can
// be swallowed by a \x escape.
const std::string kRed(1, '\x02');
const std::string kDefault(1, '\x01');
const std::string kPrefix = kRed + "[AFK]" + kDefault;

// ---------------------------------------------------------------------------------------------
// the temp <configs> root and the shipped test data
// ---------------------------------------------------------------------------------------------

std::filesystem::path TempRoot() {
	return std::filesystem::temp_directory_path() / "antiafk_plugin_tests_work";
}

std::filesystem::path ConfigRoot() {
	return TempRoot() / "configs";
}

std::filesystem::path DataRoot() {
	return TempRoot() / "data";
}

std::filesystem::path Cwd() {
	return std::filesystem::current_path();
}

std::string PathString(const std::filesystem::path& path) {
	return path.string();
}

void WriteFile(const std::filesystem::path& path, std::string_view content) {
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file.write(content.data(), static_cast<std::streamsize>(content.size()));
	const bool ok = file.good();
	file.close();
	Check(ok, "write " + path.string());
}

void RemoveFile(const std::filesystem::path& path) {
	std::error_code ec;
	std::filesystem::remove(path, ec);
}

std::string ReadFileOrEmpty(const std::string& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return {};
	}
	return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// The checked-in copies of the production files. CMake bakes in the absolute source path; when the
// binary is started on the host instead, the path relative to build/tests (the working directory
// test.sh uses) and to the repository root still point at the same file.
std::string ShippedPath(const std::string& relative) {
	std::string configured = ANTIAFK_TEST_DATA_DIR;
	if (!configured.empty()) {
		configured += "/";
		configured += relative;
		if (std::filesystem::exists(configured)) {
			return configured;
		}
	}
	for (const char* prefix : {"../../", "", "../"}) {
		const std::filesystem::path candidate =
				std::filesystem::current_path() / prefix / "plugins/AntiAfk/tests/data" / relative;
		if (std::filesystem::exists(candidate)) {
			return candidate.lexically_normal().string();
		}
	}
	return {};
}

bool CopyShipped(const std::string& relative, const std::filesystem::path& destination) {
	const std::string source = ShippedPath(relative);
	if (source.empty()) {
		return false;
	}
	const std::string content = ReadFileOrEmpty(source);
	if (content.empty()) {
		return false;
	}
	WriteFile(destination, content);
	return true;
}

void UseProductionFiles() {
	const bool settings = CopyShipped("AntiAfk/settings.ini", ConfigRoot() / "AntiAfk/settings.ini");
	const bool phrases =
			CopyShipped("AntiAfk/translations/antiafk.phrases.txt",
						ConfigRoot() / "AntiAfk/translations/antiafk.phrases.txt");
	Check(settings, "the shipped production settings.ini is available");
	Check(phrases, "the shipped production antiafk.phrases.txt is available");
}

std::string SettingsText(int initialWarningTime, int warningInterval, int maxWarnings, bool kickInsteadOfSpec,
						 float aimThresholdDeg = 1.0f, bool debugLog = false) {
	std::string text = "\"AntiAfk\"\n{\n";
	text += "    \"initial_warning_time\" \"" + std::to_string(initialWarningTime) + "\"\n";
	text += "    \"warning_interval\" \"" + std::to_string(warningInterval) + "\"\n";
	text += "    \"max_warnings\" \"" + std::to_string(maxWarnings) + "\"\n";
	text += std::string("    \"kick_instead_of_spec\" \"") + (kickInsteadOfSpec ? "1" : "0") + "\"\n";
	text += "    \"aim_move_threshold_deg\" \"" + std::to_string(aimThresholdDeg) + "\"\n";
	text += std::string("    \"debug_log\" \"") + (debugLog ? "1" : "0") + "\"\n";
	text += "}\n";
	return text;
}

std::string PhrasesText(std::string_view body, std::string_view prefix = {}) {
	std::string text = "\"Phrases\"\n{\n";
	if (!prefix.empty()) {
		text += "    \"AFK_Prefix\" { \"en\" \"" + std::string(prefix) + "\" }\n";
	}
	text += "    \"AFK_SpecFinal\" { \"en\" \"" + std::string(body) + "\" }\n";
	text += "}\n";
	return text;
}

void WriteSettings(std::string_view text) {
	WriteFile(ConfigRoot() / "AntiAfk/settings.ini", text);
}

void WriteSettings(int initialWarningTime, int warningInterval, int maxWarnings, bool kickInsteadOfSpec,
				   float aimThresholdDeg = 1.0f, bool debugLog = false) {
	WriteSettings(SettingsText(initialWarningTime, warningInterval, maxWarnings, kickInsteadOfSpec,
							   aimThresholdDeg, debugLog));
}

// The raw Metamod paths pisexcompat tried before the plugin directories (relative to the cwd).
void WriteRawSettings(std::string_view text) {
	WriteFile(Cwd() / "addons/configs/AntiAfk/settings.ini", text);
}

void WriteRawSettings(int initialWarningTime, int warningInterval, int maxWarnings, bool kickInsteadOfSpec,
					  float aimThresholdDeg = 1.0f, bool debugLog = false) {
	WriteRawSettings(SettingsText(initialWarningTime, warningInterval, maxWarnings, kickInsteadOfSpec,
								  aimThresholdDeg, debugLog));
}

void WriteRawPhrases(std::string_view text) {
	WriteFile(Cwd() / "antiafk.phrases.txt", text);
}

void WritePhrases(std::string_view text) {
	WriteFile(ConfigRoot() / "AntiAfk/translations/antiafk.phrases.txt", text);
}

void UseProductionPhrases() {
	Check(CopyShipped("AntiAfk/translations/antiafk.phrases.txt",
					  ConfigRoot() / "AntiAfk/translations/antiafk.phrases.txt"),
		  "the shipped production antiafk.phrases.txt is available");
}

void WriteCoreConfig(std::string_view text) {
	WriteFile(ConfigRoot() / "core.cfg", text);
}

// The language-chain tests control s2sdk::GetServerLanguage through this.
std::string g_serverLanguage = "en";

// <GetDataDir()> of the fake host (the old pisexcompat candidate list also looked in the plugin's
// data directory for settings.ini).
std::string g_dataDir;

// ---------------------------------------------------------------------------------------------
// recording fakes for every s2sdk / plg binding the plugin uses
// ---------------------------------------------------------------------------------------------

struct FakeEvent {
	int32_t slot = -1;
};

struct FakeSlot {
	bool connected = false;
	bool inGame = false;
	bool sourceTV = false;
	bool fake = false;
	bool alive = true;
	bool throwButtons = false;
	bool throwTeam = false;
	int32_t team = 2;
	int32_t controllerHandle = -1;
	void* controllerPointer = nullptr;
	uint64_t steamId = 0;
	uint64_t buttons = 0;
	plg::vec3 forward{1.0f, 0.0f, 0.0f};
};

std::array<FakeSlot, 64> g_slots{};
uint32_t g_pointerSalt = 0;

struct FakeCounters {
	int isConnected = 0;
	int isInGame = 0;
	int isSourceTV = 0;
	int isFakeClient = 0;
	int isAlive = 0;
	int playerSlotPointer = 0;
	int playerSlotHandle = 0;
	int validHandle = 0;
	int validPointer = 0;
	int steamId = 0;
	int buttons = 0;
	int forward = 0;
	int schemaOffset = 0;
	int entData = 0;
	int teamData = 0;
	int warmupData = 0;
	int suicide = 0;
	int changeTeam = 0;
	int kick = 0;
	int createTimer = 0;
	int killsTimer = 0;
	int hook = 0;
	int unhook = 0;
	int mapStartRegister = 0;
	int mapStartUnregister = 0;
	int mapEndRegister = 0;
	int mapEndUnregister = 0;
	int eventPlayerSlot = 0;
	int printChat = 0;
	int gameTime = 0;
};

FakeCounters g_fake;

void ResetCounters() {
	g_fake = FakeCounters{};
}

struct ChatLine {
	int32_t slot = -1;
	std::string text;
};

std::vector<ChatLine> g_chat;

struct LogLine {
	std::string message;
	plg::Severity severity = plg::Severity::Unknown;
};

std::vector<LogLine> g_logs;

struct EventHookRecord {
	std::string name;
	s2sdk::EventCallback callback = nullptr;
	s2sdk::HookMode mode = s2sdk::HookMode::Pre;
	bool active = true;
};

std::vector<EventHookRecord> g_eventHooks;

s2sdk::OnMapStartCallback g_onMapStart = nullptr;
s2sdk::OnMapEndCallback g_onMapEnd = nullptr;

float g_lastUpdateTime = 0.0f;
bool g_recycleOnForward = false;
int32_t g_recycleSlot = -1;
bool g_hookFails = false;
bool g_handleValid = true;
bool g_pointerValid = true;
bool g_schemaOffsetsAvailable = true;
bool g_gameRulesAvailable = true;
float g_gameTime = 0.0f;
bool g_warmup = false;
void* g_gameRulesPointer = reinterpret_cast<void*>(static_cast<uintptr_t>(0x200000));
std::string g_lastEventKey;

constexpr int32_t kFakeTeamOffset = 100;
constexpr int32_t kFakeWarmupOffset = 200;

// Defined with the world helpers below; the forward-vector fake needs it for the reentrancy test.
void RecyclePlayer(int32_t slot);

// --- plg host ---------------------------------------------------------------------------------

void FakeLog(std::string_view message, plg::Severity severity, const plg::source_location& /*location*/) {
	g_logs.push_back(LogLine{std::string(message), severity});
}

plg::string FakeGetConfigsDir() {
	return plg::string(std::string_view(PathString(ConfigRoot())));
}

plg::string FakeGetDataDir() {
	return plg::string(std::string_view(g_dataDir));
}

plg::string FakeGetServerLanguage() {
	return plg::string(std::string_view(g_serverLanguage));
}

// --- s2sdk: clients ---------------------------------------------------------------------------

bool FakeIsClientConnected(int32_t slot) {
	++g_fake.isConnected;
	return slot >= 0 && slot < static_cast<int32_t>(g_slots.size()) && g_slots[static_cast<std::size_t>(slot)].connected;
}

bool FakeIsClientInGame(int32_t slot) {
	++g_fake.isInGame;
	return slot >= 0 && slot < static_cast<int32_t>(g_slots.size()) && g_slots[static_cast<std::size_t>(slot)].inGame;
}

bool FakeIsClientSourceTV(int32_t slot) {
	++g_fake.isSourceTV;
	return slot >= 0 && slot < static_cast<int32_t>(g_slots.size()) && g_slots[static_cast<std::size_t>(slot)].sourceTV;
}

bool FakeIsFakeClient(int32_t slot) {
	++g_fake.isFakeClient;
	return slot >= 0 && slot < static_cast<int32_t>(g_slots.size()) && g_slots[static_cast<std::size_t>(slot)].fake;
}

bool FakeIsClientAlive(int32_t slot) {
	++g_fake.isAlive;
	return slot >= 0 && slot < static_cast<int32_t>(g_slots.size()) && g_slots[static_cast<std::size_t>(slot)].alive;
}

void* FakePlayerSlotToEntPointer(int32_t slot) {
	++g_fake.playerSlotPointer;
	if (slot < 0 || slot >= static_cast<int32_t>(g_slots.size())) {
		return nullptr;
	}
	return g_slots[static_cast<std::size_t>(slot)].controllerPointer;
}

int32_t FakePlayerSlotToEntHandle(int32_t slot) {
	++g_fake.playerSlotHandle;
	if (slot < 0 || slot >= static_cast<int32_t>(g_slots.size())) {
		return -1;
	}
	return g_slots[static_cast<std::size_t>(slot)].controllerHandle;
}

bool FakeIsValidEntHandle(int32_t handle) {
	++g_fake.validHandle;
	return handle != 0 && handle != -1 && g_handleValid;
}

bool FakeIsValidEntPointer(void* pointer) {
	++g_fake.validPointer;
	return pointer != nullptr && g_pointerValid;
}

uint64_t FakeGetClientSteamID64(int32_t slot) {
	++g_fake.steamId;
	if (slot < 0 || slot >= static_cast<int32_t>(g_slots.size())) {
		return 0;
	}
	return g_slots[static_cast<std::size_t>(slot)].steamId;
}

uint64_t FakeGetClientButtons(int32_t slot, int32_t /*buttonIndex*/) {
	++g_fake.buttons;
	if (slot < 0 || slot >= static_cast<int32_t>(g_slots.size())) {
		return 0;
	}
	const FakeSlot& fake = g_slots[static_cast<std::size_t>(slot)];
	if (fake.throwButtons) {
		throw std::runtime_error("buttons unavailable");
	}
	return fake.buttons;
}

plg::vec3 FakeGetClientForwardVector(int32_t slot) {
	++g_fake.forward;
	if (slot < 0 || slot >= static_cast<int32_t>(g_slots.size())) {
		return plg::vec3{};
	}
	if (g_recycleOnForward && slot == g_recycleSlot) {
		// Recycle the slot in the middle of the tick: the identity captured at the start of the
		// tick is stale by the time EvaluateTick returns its decision.
		g_recycleOnForward = false;
		RecyclePlayer(slot);
	}
	return g_slots[static_cast<std::size_t>(slot)].forward;
}

s2sdk::CSTeam FakeGetClientTeam(int32_t slot) {
	if (slot < 0 || slot >= static_cast<int32_t>(g_slots.size())) {
		return s2sdk::CSTeam::None;
	}
	return static_cast<s2sdk::CSTeam>(g_slots[static_cast<std::size_t>(slot)].team);
}

void FakeForcePlayerSuicide(int32_t slot, bool explode, bool force) {
	(void)slot;
	(void)explode;
	(void)force;
	++g_fake.suicide;
}

void FakeChangeClientTeam(int32_t slot, s2sdk::CSTeam team) {
	(void)slot;
	(void)team;
	++g_fake.changeTeam;
}

void FakeKickClient(int32_t slot, s2sdk::NetworkDisconnectionReason reason, const plg::string& message) {
	(void)slot;
	Check(reason == s2sdk::NetworkDisconnectionReason::Kicked, "the final kick uses NETWORK_DISCONNECT_KICKED");
	CheckEqInt(static_cast<int64_t>(message.size()), 0, "the final kick carries the old empty internal message");
	++g_fake.kick;
}

// --- s2sdk: schema / rules / time --------------------------------------------------------------

// Called once per Tick() (and once by round_start's CurrentTime()); the cadence tests reset the
// counters after round_start and use this counter as the exact number of ticks that ran.
float FakeGetGameTime() {
	++g_fake.gameTime;
	return g_gameTime;
}

void* FakeGetGameRules() {
	return g_gameRulesAvailable ? g_gameRulesPointer : nullptr;
}

int32_t FakeGetSchemaOffset(const plg::string& className, const plg::string& memberName) {
	++g_fake.schemaOffset;
	if (!g_schemaOffsetsAvailable) {
		return -1;
	}
	const std::string_view classNameView(className);
	const std::string_view memberView(memberName);
	if (classNameView == "CCSGameRules" && memberView == "m_bWarmupPeriod") {
		return kFakeWarmupOffset;
	}
	if (classNameView == "CBaseEntity" && memberView == "m_iTeamNum") {
		return kFakeTeamOffset;
	}
	return -1;
}

int64_t FakeGetEntData2(void* entity, int32_t offset, int32_t /*size*/) {
	++g_fake.entData;
	if (offset == kFakeWarmupOffset) {
		++g_fake.warmupData;
		return g_warmup ? 1 : 0;
	}
	if (offset == kFakeTeamOffset) {
		for (std::size_t i = 0; i < g_slots.size(); ++i) {
			if (g_slots[i].controllerPointer == entity) {
				++g_fake.teamData;
				if (g_slots[i].throwTeam) {
					throw std::runtime_error("team unavailable");
				}
				return g_slots[i].team;
			}
		}
	}
	return 0;
}

// --- s2sdk: timers ----------------------------------------------------------------------------

// The plugin must never register an s2sdk timer: TimerSystem::RunFrame holds a non-recursive mutex
// across the callback, and the AFK actions can synchronously trigger other plugins' timer work on
// the same thread. These fakes only count calls so the tests can assert the counters stay 0.
s2sdk::TimerId FakeCreateTimer(double /*delay*/, s2sdk::TimerCallback /*callback*/, s2sdk::TimerFlag /*flags*/,
							   const plg::vector<plg::any>& /*userData*/) {
	++g_fake.createTimer;
	return 0;
}

void FakeKillsTimer(s2sdk::TimerId /*timer*/) {
	++g_fake.killsTimer;
}

// --- s2sdk: events / map listeners ------------------------------------------------------------

s2sdk::EventHookError FakeHookEvent(const plg::string& name, s2sdk::EventCallback callback, s2sdk::HookMode mode) {
	++g_fake.hook;
	if (g_hookFails) {
		return s2sdk::EventHookError::InvalidEvent;
	}
	g_eventHooks.push_back(EventHookRecord{std::string(std::string_view(name)), callback, mode, true});
	return s2sdk::EventHookError::Okay;
}

s2sdk::EventHookError FakeUnhookEvent(const plg::string& name, s2sdk::EventCallback callback, s2sdk::HookMode mode) {
	++g_fake.unhook;
	for (EventHookRecord& hook : g_eventHooks) {
		if (hook.active && hook.name == std::string(std::string_view(name)) && hook.callback == callback &&
			hook.mode == mode) {
			hook.active = false;
			return s2sdk::EventHookError::Okay;
		}
	}
	return s2sdk::EventHookError::InvalidEvent;
}

void FakeOnMapStartRegister(s2sdk::OnMapStartCallback callback) {
	++g_fake.mapStartRegister;
	g_onMapStart = callback;
}

void FakeOnMapStartUnregister(s2sdk::OnMapStartCallback callback) {
	++g_fake.mapStartUnregister;
	if (g_onMapStart == callback) {
		g_onMapStart = nullptr;
	}
}

void FakeOnMapEndRegister(s2sdk::OnMapEndCallback callback) {
	++g_fake.mapEndRegister;
	g_onMapEnd = callback;
}

void FakeOnMapEndUnregister(s2sdk::OnMapEndCallback callback) {
	++g_fake.mapEndUnregister;
	if (g_onMapEnd == callback) {
		g_onMapEnd = nullptr;
	}
}

// --- s2sdk: event fields ----------------------------------------------------------------------

int32_t FakeGetEventPlayerSlot(void* event, const plg::string& key) {
	++g_fake.eventPlayerSlot;
	g_lastEventKey = std::string(std::string_view(key));
	const FakeEvent* fake = static_cast<const FakeEvent*>(event);
	return fake != nullptr ? fake->slot : -1;
}

// --- the print channel ------------------------------------------------------------------------

void FakePrintToChat(int32_t slot, const plg::string& message) {
	++g_fake.printChat;
	g_chat.push_back(ChatLine{slot, std::string(std::string_view(message))});
}

// --- installation -----------------------------------------------------------------------------

void InstallFakes() {
	plg::Log = &FakeLog;
	plg::GetConfigsDir = &FakeGetConfigsDir;
	plg::GetDataDir = &FakeGetDataDir;

	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	__s2sdk_IsClientAlive = &FakeIsClientAlive;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_PlayerSlotToEntHandle = &FakePlayerSlotToEntHandle;
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;
	__s2sdk_IsValidEntPointer = &FakeIsValidEntPointer;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
	__s2sdk_GetClientButtons = &FakeGetClientButtons;
	__s2sdk_GetClientForwardVector = &FakeGetClientForwardVector;
	__s2sdk_GetClientTeam = &FakeGetClientTeam;
	__s2sdk_ForcePlayerSuicide = &FakeForcePlayerSuicide;
	__s2sdk_ChangeClientTeam = &FakeChangeClientTeam;
	__s2sdk_KickClient = &FakeKickClient;

	__s2sdk_GetGameTime = &FakeGetGameTime;
	__s2sdk_GetGameRules = &FakeGetGameRules;
	__s2sdk_GetSchemaOffset = &FakeGetSchemaOffset;
	__s2sdk_GetEntData2 = &FakeGetEntData2;

	__s2sdk_CreateTimer = &FakeCreateTimer;
	__s2sdk_KillsTimer = &FakeKillsTimer;

	__s2sdk_HookEvent = &FakeHookEvent;
	__s2sdk_UnhookEvent = &FakeUnhookEvent;
	__s2sdk_OnMapStart_Register = &FakeOnMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeOnMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeOnMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeOnMapEndUnregister;
	__s2sdk_GetEventPlayerSlot = &FakeGetEventPlayerSlot;
	__s2sdk_PrintToChat = &FakePrintToChat;
	__s2sdk_GetServerLanguage = &FakeGetServerLanguage;
}

// ---------------------------------------------------------------------------------------------
// world reset and helpers
// ---------------------------------------------------------------------------------------------

void ResetSlots() {
	g_pointerSalt = 0;
	for (std::size_t i = 0; i < g_slots.size(); ++i) {
		FakeSlot& slot = g_slots[i];
		slot = FakeSlot{};
		slot.connected = false;
		slot.inGame = false;
		slot.controllerHandle = 1000 + static_cast<int32_t>(i);
		slot.controllerPointer =
				reinterpret_cast<void*>(static_cast<uintptr_t>(0x100000) + static_cast<uintptr_t>(i) * 0x100);
		slot.steamId = 76561198000000000ULL + i;
		slot.team = 2;
		slot.alive = true;
		slot.forward = plg::vec3{1.0f, 0.0f, 0.0f};
	}
}

void EnablePlayer(int32_t slot, int32_t team = 2) {
	FakeSlot& fake = g_slots[static_cast<std::size_t>(slot)];
	fake.connected = true;
	fake.inGame = true;
	fake.sourceTV = false;
	fake.fake = false;
	fake.alive = true;
	fake.team = team;
}

// A new occupant of the same slot: a new controller object (pointer, handle/generation) and a new
// SteamID.
void RecyclePlayer(int32_t slot) {
	FakeSlot& fake = g_slots[static_cast<std::size_t>(slot)];
	++g_pointerSalt;
	fake.controllerPointer = reinterpret_cast<void*>(static_cast<uintptr_t>(0x300000) +
													 static_cast<uintptr_t>(slot) * 0x100 + g_pointerSalt);
	fake.controllerHandle += 0x20000; // a new serial (generation), same low index is irrelevant here
	fake.steamId += 1;
	EnablePlayer(slot, fake.team);
}

void ResetWorld() {
	// Let the plugin remove whatever the previous test left registered (A7), then clear everything.
	g_antiAfkPlugin.OnPluginEnd();

	g_afk.clear();
	g_phrases.clear();
	g_settings = antiafk::Settings{};
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = false;
	g_mapListenersRegistered = false;
	g_canCheckAfk = false;
	g_loggedChecksOff = false;
	g_tickAccumulator = 0.0;
	g_warmupOffset = -1;
	g_teamOffset = -1;
	g_roundStartHooked = false;
	g_matchEndHooked = false;
	g_gameEndHooked = false;
	g_playerDisconnectHooked = false;

	g_fake = FakeCounters{};
	g_chat.clear();
	g_logs.clear();
	g_eventHooks.clear();
	g_onMapStart = nullptr;
	g_onMapEnd = nullptr;
	g_lastUpdateTime = 0.0f;
	g_recycleOnForward = false;
	g_recycleSlot = -1;
	g_hookFails = false;
	g_handleValid = true;
	g_pointerValid = true;
	g_schemaOffsetsAvailable = true;
	g_gameRulesAvailable = true;
	g_gameTime = 0.0f;
	g_warmup = false;
	g_gameRulesPointer = reinterpret_cast<void*>(static_cast<uintptr_t>(0x200000));
	g_lastEventKey.clear();
	g_serverLanguage = "en";

	g_dataDir = PathString(DataRoot());

	std::error_code ec;
	std::filesystem::create_directories(ConfigRoot(), ec);
	// Every test writes the config/phrases it needs after ResetWorld; removing the plugin's
	// config/data subtrees keeps the candidate-order tests from seeing a previous test's decoy.
	std::filesystem::remove_all(ConfigRoot() / "AntiAfk", ec);
	std::filesystem::remove_all(DataRoot() / "AntiAfk", ec);
	RemoveFile(ConfigRoot() / "core.cfg");

	// The tests run in a private cwd sandbox. Remove the raw Metamod files (and the host-less
	// relative "AntiAfk/..." fallbacks) a previous path test may have written there, so every test
	// starts without a raw candidate.
	const std::filesystem::path cwd = std::filesystem::current_path(ec);
	if (!ec) {
		std::filesystem::remove_all(cwd / "addons", ec);
		std::filesystem::remove_all(cwd / "AntiAfk", ec);
		std::filesystem::remove(cwd / "antiafk.phrases.txt", ec);
	}

	ResetSlots();
	InstallFakes();
}

const char* kRoundStart = "round_start";
const char* kMatchEnd = "cs_win_panel_match";
const char* kGameEnd = "game_end";
const char* kPlayerDisconnect = "player_disconnect";

void StartPlugin() {
	const plg::PluginResult result = g_antiAfkPlugin.OnPluginStart();
	Check(result.GetCode() == plg::PluginCode::Ok, "OnPluginStart succeeds (never Failed)");
}

void OnPluginUpdate() {
	g_antiAfkPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
}

void FireMapStart() {
	Check(g_onMapStart != nullptr, "the map-start listener is registered");
	if (g_onMapStart != nullptr) {
		g_onMapStart();
	}
}

void FireMapEnd() {
	Check(g_onMapEnd != nullptr, "the map-end listener is registered");
	if (g_onMapEnd != nullptr) {
		g_onMapEnd();
	}
}

bool FireEventHook(const char* name, FakeEvent* event) {
	for (EventHookRecord& hook : g_eventHooks) {
		if (hook.active && hook.name == name) {
			const s2sdk::ResultType result = hook.callback(plg::string(name), event, false);
			Check(result == s2sdk::ResultType::Continue, std::string(name) + " answers Continue");
			return true;
		}
	}
	Check(false, std::string("event hook '") + name + "' is registered");
	return false;
}

// The AFK cadence is driven by OnPluginUpdate now. ResetClock starts a scenario at t = 0, and
// AdvanceTo pushes one update whose delta is at least one full interval, so exactly one Tick runs
// at the requested game time (the accumulator's fmod drops the rest). Tests that need a controlled
// partial delta use PumpUpdate directly.
void ResetClock() {
	g_gameTime = 0.0f;
	g_lastUpdateTime = 0.0f;
}

void PumpUpdate(float gameTime, double deltaSeconds) {
	g_gameTime = gameTime;
	const int64_t millis = static_cast<int64_t>(deltaSeconds * 1000.0);
	g_antiAfkPlugin.OnPluginUpdate(std::chrono::milliseconds(millis));
}

void AdvanceTo(float gameTime) {
	const double delta = std::max(1.0, static_cast<double>(gameTime) - static_cast<double>(g_lastUpdateTime));
	g_lastUpdateTime = gameTime;
	PumpUpdate(gameTime, delta);
}

// Start the plugin on a map, enable slot 3 and enable the checks with round_start, then tick once
// at t = 0 so the slot has an initial state.
void StartScenario(int initialWarningTime, int warningInterval, int maxWarnings, bool kickInsteadOfSpec,
				   int32_t team = 2) {
	ResetWorld();
	WriteSettings(initialWarningTime, warningInterval, maxWarnings, kickInsteadOfSpec);
	UseProductionPhrases();
	WriteCoreConfig("\"Core\" { \"ServerLang\" \"ru\" }");
	g_serverLanguage = "en";

	StartPlugin();
	FireMapStart();
	OnPluginUpdate(); // map-ready update: latch the gate, dt = 0, no tick

	EnablePlayer(3, team);
	ResetCounters();
	FireEventHook(kRoundStart, nullptr);

	ResetClock();
	AdvanceTo(0.0f); // one update with a full interval -> the first tick at t = 0
	ResetCounters();
}

const std::string& LastChat() {
	static const std::string empty;
	return g_chat.empty() ? empty : g_chat.back().text;
}

bool HasErrorContaining(std::string_view text) {
	for (const LogLine& line : g_logs) {
		if (line.severity == plg::Severity::Error && line.message.find(text) != std::string::npos) {
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------------------------
// 1. the local KV1 reader
// ---------------------------------------------------------------------------------------------

void TestKv1Reader() {
	Section("KV1 reader");

	const std::optional<antiafk::KvNode> root = antiafk::ParseKv1(
			"\xEF\xBB\xBF"
			"\"AntiAfk\"\n{\n"
			"  // a comment\n"
			"  \"initial_warning_time\" \"20\"\n"
			"  \"warning_interval\" 40\n"
			"  \"nested\" { \"a\" \"1\" /* block comment */ }\n"
			"}\n");
	Check(root.has_value(), "a normal settings file parses");
	if (root) {
		CheckEqInt(root->GetInt("initial_warning_time", -1), 20, "a quoted int value");
		CheckEqInt(root->GetInt("warning_interval", -1), 40, "an unquoted int value");
		CheckEqInt(root->GetInt("WARNING_INTERVAL", -1), 40, "key lookup is case-insensitive");
		CheckEqInt(root->GetInt("missing", 7), 7, "a missing key keeps the default");
		Check(root->Find("nested") != nullptr, "a nested block is found");
	}
	Check(!antiafk::ParseKv1("\"A\" { \"b\" \"c\"").has_value(), "a missing '}' is rejected");
	Check(!antiafk::ParseKv1("\"A\" { } }").has_value(), "a stray top-level '}' is rejected");
	Check(!antiafk::ParseKv1("\"A\" \"unterminated").has_value(), "an unterminated string is rejected");
	Check(antiafk::ParseKv1("").has_value(), "an empty file is an empty tree");

	const std::optional<antiafk::KvNode> numbers = antiafk::ParseKv1(
			"\"A\" { \"f\" \"40.7\" \"g\" \"9.99\" \"bad\" \"abc\" \"trailing\" \"5 // not a comment\" }");
	Check(numbers.has_value(), "the numeric file parses");
	if (numbers) {
		CheckNear(numbers->GetFloat("f", 0.0f), 40.7f, "GetFloat keeps the fraction");
		CheckEqInt(static_cast<int>(numbers->GetFloat("g", 0.0f)), 9, "the original truncates (int)GetFloat()");
		CheckNear(numbers->GetFloat("bad", -1.0f), -1.0f, "a malformed float keeps the default (pisexcompat semantics)");
		CheckEqInt(numbers->GetInt("bad", -1), 0, "a malformed int is atoi's 0");
		CheckEqInt(numbers->GetInt("trailing", -1), 5, "a trailing comment after an unquoted value is ignored");
	}

	const std::optional<antiafk::KvNode> phrases = antiafk::ParseKv1(
			"\"Phrases\" {"
			"  \"AFK_Prefix\" { \"ru\" \"R\" \"en\" \"E\" }"
			"  \"K\" { \"ru\" \"\" \"en\" \"EN\" }"
			"  \"language\" \"ru\""
			"}");
	Check(phrases.has_value(), "a phrases file parses");
	if (phrases) {
		const antiafk::PhraseMap ru = antiafk::ResolvePhrases(*phrases, "ru");
		CheckEqStr(ru.at("AFK_Prefix"), "R", "the requested language wins");
		CheckEqStr(ru.at("K"), "EN", "an empty localized value falls back to en");
		Check(!ru.contains("language"), "a plain value is not a phrase");
		const antiafk::PhraseMap de = antiafk::ResolvePhrases(*phrases, "de");
		CheckEqStr(de.at("AFK_Prefix"), "E", "a missing language falls back to en");
	}
}

// ---------------------------------------------------------------------------------------------
// 2. settings loading (production file and its defaults)
// ---------------------------------------------------------------------------------------------

void TestConfigLoading() {
	Section("config replacement");

	ResetWorld();
	UseProductionFiles();
	StartPlugin();
	CheckEqInt(g_settings.initialWarningTime, 20, "production initial_warning_time");
	CheckEqInt(g_settings.warningInterval, 40, "production warning_interval");
	CheckEqInt(g_settings.maxWarnings, 3, "production max_warnings");
	Check(!g_settings.kickInsteadOfSpec, "production kick_instead_of_spec = 0");
	CheckNear(g_settings.aimThreshDeg, 1.0f, "production aim_move_threshold_deg");
	Check(!g_settings.debugLog, "production debug_log = 0");

	ResetWorld();
	RemoveFile(ConfigRoot() / "AntiAfk/settings.ini");
	RemoveFile(ConfigRoot() / "AntiAfk/translations/antiafk.phrases.txt");
	StartPlugin();
	CheckEqInt(g_settings.initialWarningTime, 15, "missing file: built-in initial_warning_time");
	CheckEqInt(g_settings.warningInterval, 35, "missing file: built-in warning_interval");
	CheckEqInt(g_settings.maxWarnings, 3, "missing file: built-in max_warnings");
	Check(!g_settings.kickInsteadOfSpec, "missing file: built-in kick_instead_of_spec");
	CheckNear(g_settings.aimThreshDeg, 1.0f, "missing file: built-in aim_move_threshold_deg");
	Check(g_settings.debugLog, "missing file: built-in debug_log");
	Check(HasErrorContaining("failed to load"), "a missing settings file is logged as an error");

	ResetWorld();
	WriteSettings("\"AntiAfk\" { \"initial_warning_time\" \"5\" ");
	StartPlugin();
	CheckEqInt(g_settings.initialWarningTime, 15, "a malformed file keeps the built-in defaults");

	ResetWorld();
	WriteSettings(
			"\"AntiAfk\" {"
			"  \"initial_warning_time\" \"9.9\""
			"  \"warning_interval\" \"40.7\""
			"  \"max_warnings\" \"4\""
			"  \"kick_instead_of_spec\" \"1\""
			"  \"aim_move_threshold_deg\" \"2.5\""
			"  \"debug_log\" \"0\""
			"}");
	StartPlugin();
	CheckEqInt(g_settings.initialWarningTime, 9, "GetFloat truncates initial_warning_time like the original");
	CheckEqInt(g_settings.warningInterval, 40, "GetFloat truncates warning_interval like the original");
	CheckEqInt(g_settings.maxWarnings, 4, "max_warnings is an int");
	Check(g_settings.kickInsteadOfSpec, "kick_instead_of_spec = 1");
	CheckNear(g_settings.aimThreshDeg, 2.5f, "aim_move_threshold_deg keeps its fraction");
	Check(!g_settings.debugLog, "debug_log = 0");
}

// ---------------------------------------------------------------------------------------------
// 3. phrases, colour bytes and the ServerLang -> s2sdk -> en chain
// ---------------------------------------------------------------------------------------------

void PrintTestPhrase(const char* key, const char* fallback, int a = 0, int b = 0, bool twoArgs = false) {
	PrintPhrase(0, key, fallback, a, b, twoArgs);
}

void TestPhrasesAndLanguage() {
	Section("phrases / language chain");

	// core.cfg "ru" wins over the s2sdk server language "en".
	ResetWorld();
	UseProductionFiles();
	WriteCoreConfig("\"Core\" { \"ServerLang\" \"ru\" }");
	g_serverLanguage = "en";
	StartPlugin();
	FireMapStart();
	g_chat.clear();
	PrintTestPhrase("AFK_CountdownKick", "fallback %d", 5);
	CheckEqStr(LastChat(), kPrefix + " Вы не двигаетесь, и через " + kRed + "5 " + kDefault +
								   "секунд вас кикнут за AFK.",
			   "core.cfg ServerLang=ru selects the ru phrase and colorizes it");

	// Source language names are normalized.
	ResetWorld();
	UseProductionFiles();
	WriteCoreConfig("\"Core\" { \"ServerLang\" \"russian\" }");
	StartPlugin();
	FireMapStart();
	g_chat.clear();
	PrintTestPhrase("AFK_SpecFinal", "fallback");
	CheckEqStr(LastChat(), kPrefix + " Вы были переведены в наблюдатели из-за AFK.",
			   "the name 'russian' normalizes to ru");

	// No core.cfg: s2sdk's ServerLanguage is the second step.
	ResetWorld();
	UseProductionFiles();
	g_serverLanguage = "ru";
	StartPlugin();
	FireMapStart();
	g_chat.clear();
	PrintTestPhrase("AFK_CountdownKill", "fallback %d", 3);
	CheckEqStr(LastChat(), kPrefix + " Вы не двигаетесь, и через " + kRed + "3 " + kDefault +
								   "секунд вас убьют за AFK.",
			   "s2sdk::GetServerLanguage=ru is the fallback before en");

	// Neither source: en.
	ResetWorld();
	UseProductionFiles();
	g_serverLanguage = "";
	StartPlugin();
	FireMapStart();
	g_chat.clear();
	PrintTestPhrase("AFK_CountdownKick", "fallback %d", 5);
	CheckEqStr(LastChat(), kPrefix + " You are idle, and in " + kRed + "5 " + kDefault +
								   "seconds you will be kicked for AFK.",
			   "neither core.cfg nor s2sdk supplies a language: en");

	// core.cfg has priority over the s2sdk language.
	ResetWorld();
	UseProductionFiles();
	WriteCoreConfig("\"Core\" { \"ServerLang\" \"en\" }");
	g_serverLanguage = "ru";
	StartPlugin();
	FireMapStart();
	g_chat.clear();
	PrintTestPhrase("AFK_CountdownKick", "fallback %d", 5);
	CheckEqStr(LastChat(), kPrefix + " You are idle, and in " + kRed + "5 " + kDefault +
								   "seconds you will be kicked for AFK.",
			   "core.cfg ServerLang=en wins over s2sdk's ru");

	// A phrase that is not in the file uses the built-in fallback; a missing prefix prints the body
	// alone.
	ResetWorld();
	WriteSettings(20, 40, 3, false);
	WritePhrases("\"Phrases\" { \"AFK_Prefix\" { \"en\" \"{RED}[AFK]{DEFAULT}\" } }");
	StartPlugin();
	FireMapStart();
	g_chat.clear();
	PrintTestPhrase("AFK_CountdownKill", "built-in %d", 7);
	CheckEqStr(LastChat(), kPrefix + " built-in 7", "a missing phrase uses the built-in text");

	ResetWorld();
	WriteSettings(20, 40, 3, false);
	WritePhrases("\"Phrases\" { \"AFK_CountdownKill\" { \"en\" \"{RED}%d{DEFAULT} left\" } }");
	StartPlugin();
	FireMapStart();
	g_chat.clear();
	PrintTestPhrase("AFK_CountdownKill", "built-in %d", 7);
	CheckEqStr(LastChat(), kRed + "7" + kDefault + " left", "a missing prefix prints the body alone");

	ResetWorld();
	WriteSettings(20, 40, 3, false);
	WritePhrases("\"Phrases\" { \"Broken\" \"unterminated");
	StartPlugin();
	FireMapStart();
	g_chat.clear();
	PrintTestPhrase("AFK_CountdownKill", "built-in %d", 7);
	CheckEqStr(LastChat(), "built-in 7", "a malformed phrases file uses the built-in text");
	Check(HasErrorContaining("failed to load"), "the malformed phrases file is logged as an error");

	// AFK_WarningKill uses the original's two snprintf arguments.
	ResetWorld();
	UseProductionFiles();
	WriteCoreConfig("\"Core\" { \"ServerLang\" \"ru\" }");
	StartPlugin();
	FireMapStart();
	g_chat.clear();
	PrintTestPhrase("AFK_WarningKill", "fallback %d/%d", 1, 3, true);
	CheckEqStr(LastChat(), kPrefix + " Предупреждение " + kRed + "1" + kDefault + "/" + kRed + "3" +
								   kDefault + ": вас убили за AFK.",
			   "AFK_WarningKill formats both ints");

	// No PrintToChat binding: a print is a silent no-op.
	ResetWorld();
	UseProductionFiles();
	StartPlugin();
	FireMapStart();
	__s2sdk_PrintToChat = nullptr;
	g_chat.clear();
	PrintTestPhrase("AFK_CountdownKill", "built-in %d", 7);
	Check(g_chat.empty(), "a missing PrintToChat binding prints nothing");
}

// ---------------------------------------------------------------------------------------------
// 3b. the exact pisexcompat config/phrases candidate order (raw cwd first)
// ---------------------------------------------------------------------------------------------

void CheckSettingsValues(int initialWarningTime, int warningInterval, int maxWarnings, bool kickInsteadOfSpec,
						 float aimThresholdDeg, bool debugLog, const std::string& what) {
	CheckEqInt(g_settings.initialWarningTime, initialWarningTime, what + ": initial_warning_time");
	CheckEqInt(g_settings.warningInterval, warningInterval, what + ": warning_interval");
	CheckEqInt(g_settings.maxWarnings, maxWarnings, what + ": max_warnings");
	Check(g_settings.kickInsteadOfSpec == kickInsteadOfSpec, what + ": kick_instead_of_spec");
	CheckNear(g_settings.aimThreshDeg, aimThresholdDeg, what + ": aim_move_threshold_deg");
	Check(g_settings.debugLog == debugLog, what + ": debug_log");
}

void TestPathCandidates() {
	Section("config/phrases candidate order");

	// --- settings: raw cwd wins over a valid plugin decoy --------------------------------
	ResetWorld();
	UseProductionPhrases();
	WriteSettings(30, 50, 4, false, 3.0f, true); // plugin decoy
	WriteRawSettings(2, 9, 2, true, 2.0f, false);
	StartPlugin();
	CheckSettingsValues(2, 9, 2, true, 2.0f, false, "raw settings beat the plugin decoy");

	// The raw settings drive the real behaviour: first warning at 2 s, soft punishment at 9 s and
	// the kick at 18 s (max_warnings = 2, kick_instead_of_spec = 1).
	ResetWorld();
	UseProductionPhrases();
	WriteRawSettings(2, 9, 2, true, 1.0f, false);
	StartPlugin();
	FireMapStart();
	OnPluginUpdate();
	EnablePlayer(3);
	FireEventHook(kRoundStart, nullptr);
	ResetClock();
	AdvanceTo(0.0f);
	AdvanceTo(1.0f);
	CheckEqInt(static_cast<int64_t>(g_chat.size()), 0, "raw timing: no warning before initial_warning_time");
	AdvanceTo(2.0f);
	CheckEqInt(static_cast<int64_t>(g_chat.size()), 1, "raw timing: the first warning fires at 2 s");
	AdvanceTo(9.0f);
	CheckEqInt(g_fake.suicide, 1, "raw timing: the soft punishment fires at warning_interval 9 s");
	AdvanceTo(18.0f);
	CheckEqInt(g_fake.kick, 1, "raw kick_instead_of_spec: the final punishment kicks at 18 s");
	CheckEqInt(g_fake.changeTeam, 0, "raw kick_instead_of_spec: the final punishment does not move to spectators");

	// --- settings: plugin config only (raw absent) ---------------------------------------
	ResetWorld();
	UseProductionPhrases();
	WriteSettings(20, 40, 3, false);
	StartPlugin();
	CheckSettingsValues(20, 40, 3, false, 1.0f, false, "plugin settings when no raw file exists");

	// --- settings: config > translations > data ------------------------------------------
	ResetWorld();
	UseProductionPhrases();
	WriteSettings(11, 21, 2, false);
	WriteFile(ConfigRoot() / "AntiAfk/translations/settings.ini", SettingsText(12, 22, 2, true));
	WriteFile(DataRoot() / "AntiAfk/settings.ini", SettingsText(13, 23, 2, false));
	StartPlugin();
	CheckSettingsValues(11, 21, 2, false, 1.0f, false, "settings: config dir beats translations/data");

	ResetWorld();
	UseProductionPhrases();
	WriteFile(ConfigRoot() / "AntiAfk/translations/settings.ini", SettingsText(12, 22, 2, true));
	WriteFile(DataRoot() / "AntiAfk/settings.ini", SettingsText(13, 23, 2, false));
	StartPlugin();
	CheckSettingsValues(12, 22, 2, true, 1.0f, false, "settings: translations beat data");

	ResetWorld();
	UseProductionPhrases();
	WriteFile(DataRoot() / "AntiAfk/settings.ini", SettingsText(13, 23, 2, false));
	StartPlugin();
	CheckSettingsValues(13, 23, 2, false, 1.0f, false, "settings: the data dir is the last candidate");

	// --- settings: a malformed raw file is authoritative, not a reason to try the decoy ---
	ResetWorld();
	UseProductionPhrases();
	WriteSettings(20, 40, 3, false); // valid decoy
	WriteRawSettings("\"AntiAfk\" { \"initial_warning_time\" \"7\" ");
	StartPlugin();
	CheckSettingsValues(15, 35, 3, false, 1.0f, true, "malformed raw settings keep the built-in defaults");
	Check(HasErrorContaining("failed to load"), "the malformed raw settings file is logged");

	// --- phrases: raw cwd wins over the plugin decoy -------------------------------------
	ResetWorld();
	WriteSettings(20, 40, 3, false);
	WritePhrases(PhrasesText("DECOY FINAL", "DECOY"));
	WriteRawPhrases(PhrasesText("RAW FINAL", "RAW"));
	StartPlugin();
	FireMapStart();
	g_chat.clear();
	PrintTestPhrase("AFK_SpecFinal", "FALLBACK");
	CheckEqStr(LastChat(), "RAW RAW FINAL", "raw phrases beat the plugin decoy");

	// --- phrases: translations > config dir ----------------------------------------------
	ResetWorld();
	WriteSettings(20, 40, 3, false);
	WriteFile(ConfigRoot() / "AntiAfk/translations/antiafk.phrases.txt", PhrasesText("TRANS FINAL"));
	WriteFile(ConfigRoot() / "AntiAfk/antiafk.phrases.txt", PhrasesText("CONFIG FINAL"));
	StartPlugin();
	FireMapStart();
	g_chat.clear();
	PrintTestPhrase("AFK_SpecFinal", "FALLBACK");
	CheckEqStr(LastChat(), "TRANS FINAL", "phrases: translations beat the config root");

	ResetWorld();
	WriteSettings(20, 40, 3, false);
	WriteFile(ConfigRoot() / "AntiAfk/antiafk.phrases.txt", PhrasesText("CONFIG FINAL"));
	StartPlugin();
	FireMapStart();
	g_chat.clear();
	PrintTestPhrase("AFK_SpecFinal", "FALLBACK");
	CheckEqStr(LastChat(), "CONFIG FINAL", "phrases: the config root is the last candidate");

	// --- phrases: malformed raw is authoritative, not a reason to try the decoy -----------
	ResetWorld();
	WriteSettings(20, 40, 3, false);
	WritePhrases(PhrasesText("DECOY FINAL"));
	WriteRawPhrases("\"Phrases\" { \"Broken\" \"unterminated");
	StartPlugin();
	FireMapStart();
	g_chat.clear();
	PrintTestPhrase("AFK_SpecFinal", "FALLBACK");
	CheckEqStr(LastChat(), "FALLBACK", "malformed raw phrases keep the built-in text");
	Check(HasErrorContaining("failed to load"), "the malformed raw phrases file is logged");

	// --- no host: the raw paths are still first and the relative plugin decoys are not used -
	ResetWorld();
	WriteFile(Cwd() / "AntiAfk/settings.ini", SettingsText(30, 50, 4, false));
	WriteFile(Cwd() / "AntiAfk/translations/antiafk.phrases.txt", PhrasesText("RELATIVE DECOY"));
	WriteRawSettings(2, 9, 2, true, 1.0f, false);
	WriteRawPhrases(PhrasesText("RAW FINAL", "RAW"));
	plg::GetConfigsDir = nullptr;
	plg::GetDataDir = nullptr;
	StartPlugin();
	CheckSettingsValues(2, 9, 2, true, 1.0f, false,
						"no Plugify host: the raw settings are still found before the relative decoy");
	FireMapStart();
	g_chat.clear();
	PrintTestPhrase("AFK_SpecFinal", "FALLBACK");
	CheckEqStr(LastChat(), "RAW RAW FINAL",
			   "no Plugify host: the raw phrases are still found before the relative decoy");

	// --- no host + malformed raw: still authoritative, the relative decoys are not used --------
	ResetWorld();
	WriteFile(Cwd() / "AntiAfk/settings.ini", SettingsText(30, 50, 4, false));
	WriteFile(Cwd() / "AntiAfk/translations/antiafk.phrases.txt", PhrasesText("RELATIVE DECOY"));
	WriteRawSettings("\"AntiAfk\" { \"initial_warning_time\" \"7\" ");
	WriteRawPhrases("\"Phrases\" { \"Broken\" \"unterminated");
	plg::GetConfigsDir = nullptr;
	plg::GetDataDir = nullptr;
	StartPlugin();
	CheckSettingsValues(15, 35, 3, false, 1.0f, true,
						"no Plugify host: malformed raw settings keep the defaults, the relative decoy is ignored");
	FireMapStart();
	g_chat.clear();
	PrintTestPhrase("AFK_SpecFinal", "FALLBACK");
	CheckEqStr(LastChat(), "FALLBACK",
			   "no Plugify host: malformed raw phrases use the built-in text, the relative decoy is ignored");
}

// ---------------------------------------------------------------------------------------------
// 4. map gate and the OnPluginUpdate cadence (no s2sdk timer)
// ---------------------------------------------------------------------------------------------

void TestMapGateAndCadence() {
	Section("map gate / update cadence");

	ResetWorld();
	UseProductionFiles();
	StartPlugin();
	CheckEqInt(g_fake.isConnected + g_fake.isInGame + g_fake.isSourceTV + g_fake.isFakeClient + g_fake.isAlive +
					   g_fake.playerSlotPointer + g_fake.playerSlotHandle,
			   0, "OnPluginStart makes no client/controller call (PITFALLS B1)");
	Check(g_mapListenersRegistered, "the map listeners are registered in OnPluginStart");
	CheckEqInt(g_fake.mapStartRegister, 1, "OnMapStart_Register called once");
	CheckEqInt(g_fake.mapEndRegister, 1, "OnMapEnd_Register called once");
	CheckEqInt(static_cast<int64_t>(g_eventHooks.size()), 4, "four event hooks are registered");
	for (const EventHookRecord& hook : g_eventHooks) {
		Check(hook.mode == s2sdk::HookMode::Pre, hook.name + " is a Pre hook");
	}
	Check(!g_mapReady, "the gate is closed before the first map/update");
	CheckEqInt(g_fake.createTimer + g_fake.killsTimer, 0, "the plugin never registers an s2sdk timer");

	// A plugin loaded into a running map never sees OnMapStart: its first update opens the gate.
	// Even a long first delta only latches; the first check is a full interval later.
	ResetCounters();
	PumpUpdate(0.0f, 5.0);
	Check(g_mapReady, "the first update latches the gate when no OnMapEnd was seen");
	CheckEqInt(g_fake.gameTime, 0, "opening the gate does not run a check immediately");
	CheckEqInt(g_fake.isConnected, 0, "latching the gate makes no client call");
	CheckEqInt(g_fake.createTimer, 0, "still no s2sdk timer");

	FireEventHook(kRoundStart, nullptr);
	ResetCounters();
	PumpUpdate(0.0f, 0.25);
	CheckEqInt(g_fake.gameTime, 0, "0.25 s: no tick yet");
	PumpUpdate(0.0f, 0.25);
	CheckEqInt(g_fake.gameTime, 0, "0.50 s: no tick yet");
	PumpUpdate(0.0f, 0.5);
	CheckEqInt(g_fake.gameTime, 1, "1.00 s: exactly one tick, driven by OnPluginUpdate");
	CheckEqInt(g_fake.isConnected, 64, "the one tick scanned all 64 slots");

	// Map end pauses and resets the accumulator; a long delta cannot catch up.
	FireMapEnd();
	Check(!g_mapReady, "OnMapEnd closes the gate");
	Check(g_afk.empty(), "OnMapEnd clears the AFK state");
	ResetCounters();
	PumpUpdate(2.0f, 5.0);
	CheckEqInt(g_fake.gameTime, 0, "no check after OnMapEnd");
	CheckEqInt(g_fake.isConnected, 0, "no client call between OnMapEnd and the next OnMapStart");

	// OnMapStart resets the accumulator; the next check is a full interval later.
	FireMapStart();
	Check(g_mapReady, "OnMapStart reopens the gate");
	Check(g_afk.empty(), "OnMapStart starts with an empty AFK state");
	FireEventHook(kRoundStart, nullptr);
	ResetCounters();
	PumpUpdate(2.0f, 0.5);
	CheckEqInt(g_fake.gameTime, 0, "map start: 0.5 s is not enough for a check");
	PumpUpdate(2.0f, 0.5);
	CheckEqInt(g_fake.gameTime, 1, "map start: the first check runs one interval later");

	// "Pause" = the checks are off (match end): updates are ignored and the accumulator is dropped,
	// so round_start does not cause an immediate catch-up tick.
	FireEventHook(kMatchEnd, nullptr);
	ResetCounters();
	PumpUpdate(7.0f, 5.0);
	CheckEqInt(g_fake.gameTime, 0, "match end pauses the AFK checks");
	CheckEqInt(g_fake.isConnected, 0, "no client call while the checks are paused");
	FireEventHook(kRoundStart, nullptr);
	ResetCounters();
	PumpUpdate(7.0f, 0.75);
	CheckEqInt(g_fake.gameTime, 0, "resume: 0.75 s does not fire a catch-up check");
	PumpUpdate(7.0f, 0.25);
	CheckEqInt(g_fake.gameTime, 1, "resume: one check after a full interval");

	// A large update delta produces exactly one check, never a burst (no unbounded catch-up).
	ResetCounters();
	PumpUpdate(17.0f, 10.0);
	CheckEqInt(g_fake.gameTime, 1, "a 10 s delta runs exactly one check");
	CheckEqInt(g_fake.isConnected, 64, "the large delta scanned the slots once, not ten times");
	PumpUpdate(17.5f, 0.5);
	CheckEqInt(g_fake.gameTime, 1, "0.5 s after the large delta: no catch-up check");
	PumpUpdate(18.0f, 0.5);
	CheckEqInt(g_fake.gameTime, 2, "the next full interval checks normally again");

	// Loaded into the map-change window: an OnMapEnd before the first update keeps the gate shut.
	ResetWorld();
	UseProductionFiles();
	StartPlugin();
	FireMapEnd();
	ResetCounters();
	PumpUpdate(1.0f, 5.0);
	Check(!g_mapReady, "the first update does not open the gate after an OnMapEnd");
	CheckEqInt(g_fake.gameTime, 0, "no check in the map-change window");
	CheckEqInt(g_fake.isConnected, 0, "no client call in the map-change window");
	FireMapStart();
	PumpUpdate(1.0f, 5.0);
	Check(g_mapReady, "the next OnMapStart + update opens the gate");
	CheckEqInt(g_fake.gameTime, 0, "the reopened gate still waits for round_start");
	FireEventHook(kRoundStart, nullptr);
	ResetCounters();
	PumpUpdate(1.0f, 0.5);
	CheckEqInt(g_fake.gameTime, 0, "reopened gate: 0.5 s is not enough");
	PumpUpdate(1.0f, 0.5);
	CheckEqInt(g_fake.gameTime, 1, "reopened gate: the first check is a full interval later");

	// Without the map listener bindings the gate never opens (fail closed).
	ResetWorld();
	UseProductionFiles();
	__s2sdk_OnMapStart_Register = nullptr;
	__s2sdk_OnMapEnd_Register = nullptr;
	StartPlugin();
	ResetCounters();
	PumpUpdate(1.0f, 5.0);
	Check(!g_mapReady, "without the map listeners the gate stays closed");
	CheckEqInt(g_fake.gameTime, 0, "without the map listeners no check runs");
	CheckEqInt(g_fake.isConnected, 0, "without the map listeners no client call is made");

	// The whole lifecycle above never touched the s2sdk timer API.
	CheckEqInt(g_fake.createTimer + g_fake.killsTimer, 0, "no s2sdk timer was created or killed");
}

// ---------------------------------------------------------------------------------------------
// 5. the anti-AFK behaviour
// ---------------------------------------------------------------------------------------------

void TestFirstWarningCountdownAndFinal() {
	Section("idle warnings / countdown / final punishment");

	StartScenario(20, 40, 3, false);

	AdvanceTo(19.0f);
	Check(g_chat.empty(), "no message before initial_warning_time");

	AdvanceTo(20.0f);
	CheckEqInt(static_cast<int64_t>(g_chat.size()), 1, "one first warning at initial_warning_time");
	CheckEqStr(LastChat(), kPrefix + " Вы не двигаетесь, и через " + kRed + "20 " + kDefault +
								   "секунд вас убьют за AFK.",
			   "the first warning is AFK_CountdownKill with the remaining seconds");

	AdvanceTo(21.0f);
	CheckEqInt(static_cast<int64_t>(g_chat.size()), 1, "the first warning is not repeated");

	AdvanceTo(34.0f);
	CheckEqInt(static_cast<int64_t>(g_chat.size()), 1, "no countdown before interval - 5 s");

	AdvanceTo(35.2f);
	CheckEqInt(static_cast<int64_t>(g_chat.size()), 2, "the countdown starts in the last 5 s");
	CheckEqStr(LastChat(), kPrefix + " Вы не двигаетесь, и через " + kRed + "5 " + kDefault +
								   "секунд вас убьют за AFK.",
			   "the countdown shows floor(interval - idle + 0.999) = 5");

	AdvanceTo(35.8f);
	CheckEqInt(static_cast<int64_t>(g_chat.size()), 2, "the same countdown second is not repeated");

	AdvanceTo(40.0f);
	CheckEqInt(g_fake.suicide, 1, "the soft punishment suicides the idle player");
	CheckEqInt(g_fake.changeTeam, 0, "the soft punishment does not move the player");
	CheckEqInt(g_fake.kick, 0, "the soft punishment does not kick");
	CheckEqInt(static_cast<int64_t>(g_chat.size()), 3, "the soft punishment prints AFK_WarningKill");
	CheckEqStr(LastChat(), kPrefix + " Предупреждение " + kRed + "1" + kDefault + "/" + kRed + "3" +
								   kDefault + ": вас убили за AFK.",
			   "AFK_WarningKill shows warnCount/maxWarnings");
	CheckEqInt(static_cast<int64_t>(g_afk.at(3).afk.warnCount), 1, "warnCount incremented");
	CheckNear(g_afk.at(3).afk.afkStart, 40.0f, "the idle timer restarts after the soft punishment");

	AdvanceTo(60.0f);
	CheckEqInt(static_cast<int64_t>(g_chat.size()), 4, "the next interval warns again");
	CheckEqStr(LastChat(), kPrefix + " Вы не двигаетесь, и через " + kRed + "20 " + kDefault +
								   "секунд вас убьют за AFK.",
			   "the second warning is still AFK_CountdownKill (warnCount 1/3)");

	AdvanceTo(80.0f);
	CheckEqInt(g_fake.suicide, 2, "the second soft punishment suicides");
	CheckEqInt(static_cast<int64_t>(g_afk.at(3).afk.warnCount), 2, "warnCount 2");

	AdvanceTo(100.0f);
	CheckEqStr(LastChat(), kPrefix + " Вы не двигаетесь, и через " + kRed + "20 " + kDefault +
								   "секунд вас переведут в наблюдатели.",
			   "the last warning announces the move to spectators (AFK_CountdownSpec)");

	AdvanceTo(120.0f);
	CheckEqInt(g_fake.changeTeam, 1, "the final punishment moves the player to spectators");
	CheckEqInt(g_fake.kick, 0, "the final punishment does not kick when kick_instead_of_spec = 0");
	CheckEqStr(LastChat(), kPrefix + " Вы были переведены в наблюдатели из-за AFK.",
			   "AFK_SpecFinal is printed after the team change");
	CheckEqInt(static_cast<int64_t>(g_afk.count(3)), 0, "the slot state is forgotten after the final punishment");

	AdvanceTo(121.0f);
	CheckEqInt(static_cast<int64_t>(g_afk.count(3)), 1, "the next tick starts a fresh state");
}

void TestMatchEnd() {
	Section("match end / game end");

	StartScenario(20, 40, 3, false);
	AdvanceTo(40.0f);
	CheckEqInt(static_cast<int64_t>(g_afk.at(3).afk.warnCount), 1, "a warning counter exists before the match end");

	ResetCounters();
	FireEventHook(kMatchEnd, nullptr);
	Check(!g_canCheckAfk, "cs_win_panel_match disables the checks");
	CheckEqInt(static_cast<int64_t>(g_afk.at(3).afk.warnCount), 0, "cs_win_panel_match clears the warning counters");
	AdvanceTo(60.0f);
	CheckEqInt(g_fake.suicide, 0, "no punishment while the checks are off");
	CheckEqInt(static_cast<int64_t>(g_chat.size()), 1, "no new chat line while the checks are off");

	FireEventHook(kGameEnd, nullptr);
	Check(!g_canCheckAfk, "game_end also disables the checks");
	FireEventHook(kRoundStart, nullptr);
	Check(g_canCheckAfk, "round_start enables the checks again");
}

void TestKickInsteadOfSpec() {
	Section("kick_instead_of_spec");

	StartScenario(20, 40, 3, true);
	AdvanceTo(40.0f);
	CheckEqInt(g_fake.suicide, 1, "the first soft punishment is still a suicide");
	AdvanceTo(80.0f);
	CheckEqInt(g_fake.suicide, 2, "the second soft punishment is still a suicide");

	AdvanceTo(100.0f);
	CheckEqStr(LastChat(), kPrefix + " Вы не двигаетесь, и через " + kRed + "20 " + kDefault +
								   "секунд вас кикнут за AFK.",
			   "the last warning announces the kick (AFK_CountdownKick)");

	const std::size_t chatBefore = g_chat.size();
	AdvanceTo(120.0f);
	CheckEqInt(g_fake.kick, 1, "the final punishment kicks");
	CheckEqInt(g_fake.changeTeam, 0, "the final punishment does not move to spectators");
	CheckEqInt(static_cast<int64_t>(g_chat.size()), static_cast<int64_t>(chatBefore),
			   "the kick itself prints nothing");
	CheckEqInt(static_cast<int64_t>(g_afk.count(3)), 0, "the slot state is forgotten after the kick");
}

void TestResets() {
	Section("WASD / aim / warmup / death resets");

	// Movement keys reset the idle timer but not the aim reference.
	StartScenario(1000, 2000, 3, false);
	g_slots[3].buttons = 0x8ULL; // IN_FORWARD
	AdvanceTo(10.0f);
	CheckNear(g_afk.at(3).afk.afkStart, 10.0f, "a WASD key resets afkStart");
	g_slots[3].buttons = 0;
	AdvanceTo(10.5f);
	CheckNear(g_afk.at(3).afk.afkStart, 10.0f, "no key: the timer keeps running");
	CheckEqInt(g_fake.suicide, 0, "no punishment with a huge interval");

	// A >= threshold turn resets; a smaller one does not.
	StartScenario(1000, 2000, 3, false);
	const float degrees = 5.0f;
	const float radians = degrees * 3.14159265358979323846f / 180.0f;
	g_slots[3].forward = plg::vec3{std::cos(radians), std::sin(radians), 0.0f};
	AdvanceTo(10.0f);
	CheckNear(g_afk.at(3).afk.afkStart, 10.0f, "a >= threshold aim turn resets afkStart");

	StartScenario(1000, 2000, 3, false);
	const float tinyRadians = 0.5f * 3.14159265358979323846f / 180.0f;
	g_slots[3].forward = plg::vec3{std::cos(tinyRadians), std::sin(tinyRadians), 0.0f};
	AdvanceTo(10.0f);
	CheckNear(g_afk.at(3).afk.afkStart, 0.0f, "a turn below the threshold is not activity");

	// Warmup resets the idle timer and the aim reference.
	StartScenario(1000, 2000, 3, false);
	g_warmup = true;
	AdvanceTo(10.0f);
	CheckNear(g_afk.at(3).afk.afkStart, 10.0f, "warmup resets the idle timer");
	Check(!g_afk.at(3).afk.haveAimDir, "warmup clears the aim direction");
	g_warmup = false;

	// A dead player resets as well and never gets a punishment.
	StartScenario(1000, 2000, 3, false);
	g_slots[3].alive = false;
	AdvanceTo(10.0f);
	CheckNear(g_afk.at(3).afk.afkStart, 10.0f, "death resets the idle timer");
	CheckEqInt(g_fake.suicide, 0, "a dead player is never punished");
	CheckEqInt(g_fake.buttons, 0, "a dead player's buttons are not read");
	CheckEqInt(g_fake.forward, 0, "a dead player's view is not read");

	// A spectator is skipped before any state is created.
	StartScenario(1000, 2000, 3, false, 1);
	AdvanceTo(10.0f);
	CheckEqInt(static_cast<int64_t>(g_afk.count(3)), 0, "a spectator has no AFK state");
	CheckEqInt(g_fake.isAlive, 0, "a spectator's pawn is not read");
	CheckEqInt(g_fake.buttons, 0, "a spectator's buttons are not read");
}

void TestExceptionIsolation() {
	Section("exception isolation");

	ResetWorld();
	WriteSettings(20, 40, 3, false);
	UseProductionPhrases();
	StartPlugin();
	FireMapStart();
	OnPluginUpdate();
	EnablePlayer(3);
	EnablePlayer(4);
	FireEventHook(kRoundStart, nullptr);

	g_slots[3].throwButtons = true;
	g_slots[4].throwTeam = true;

	// Slot 3's button read throws inside MovingKeys (caught there); slot 4's team read throws
	// inside TeamOf (caught there). Both failures stay local and fail soft: both slots complete
	// their tick and still warn at the threshold.
	AdvanceTo(0.0f); // create both states first
	AdvanceTo(20.0f);
	CheckEqInt(g_fake.suicide, 0, "an exception in the movement/team reads is not a punishment");
	CheckEqInt(static_cast<int64_t>(g_afk.count(3)), 1, "slot 3 still has a state despite the button throw");
	CheckEqInt(static_cast<int64_t>(g_afk.count(4)), 1, "slot 4 still has a state despite the team throw");
	CheckEqInt(static_cast<int64_t>(g_chat.size()), 2, "the fail-soft reads still let both slots warn");
	Check(g_fake.isConnected > 0, "the loop continued to the next slot");
}

// ---------------------------------------------------------------------------------------------
// 6. client / controller guards (PITFALLS C2/C1)
// ---------------------------------------------------------------------------------------------

void TestClientGuards() {
	Section("client / controller guards");

	auto setupGuardScenario = [&]() {
		ResetWorld();
		WriteSettings(10, 20, 1, false);
		UseProductionPhrases();
		StartPlugin();
		FireMapStart();
		OnPluginUpdate();
		EnablePlayer(3);
		FireEventHook(kRoundStart, nullptr);
		ResetCounters();
	};

	setupGuardScenario();
	g_slots[3].connected = false;
	AdvanceTo(20.0f);
	CheckEqInt(static_cast<int64_t>(g_afk.count(3)), 0, "not connected: no AFK state");
	CheckEqInt(g_fake.isAlive, 0, "not connected: the pawn is not read");
	CheckEqInt(g_fake.suicide + g_fake.changeTeam + g_fake.kick, 0, "not connected: no punishment");

	setupGuardScenario();
	g_slots[3].inGame = false;
	AdvanceTo(20.0f);
	CheckEqInt(static_cast<int64_t>(g_afk.count(3)), 0, "not in game: no AFK state");
	CheckEqInt(g_fake.isAlive, 0, "not in game: the pawn is not read");

	setupGuardScenario();
	g_slots[3].sourceTV = true;
	AdvanceTo(20.0f);
	CheckEqInt(static_cast<int64_t>(g_afk.count(3)), 0, "SourceTV: no AFK state");
	CheckEqInt(g_fake.isAlive, 0, "SourceTV: the pawn is not read");

	setupGuardScenario();
	g_slots[3].fake = true;
	AdvanceTo(20.0f);
	CheckEqInt(static_cast<int64_t>(g_afk.count(3)), 0, "a bot: no AFK state");
	CheckEqInt(g_fake.isAlive, 0, "a bot: the pawn is not read");
	CheckEqInt(g_fake.suicide + g_fake.changeTeam + g_fake.kick, 0, "a bot: no punishment");

	// Controller pointer/handle guards.
	ResetWorld();
	WriteSettings(10, 20, 1, false);
	UseProductionPhrases();
	StartPlugin();
	FireMapStart();
	OnPluginUpdate();
	EnablePlayer(3);
	FireEventHook(kRoundStart, nullptr);

	g_slots[3].controllerPointer = nullptr;
	ResetCounters();
	AdvanceTo(20.0f);
	CheckEqInt(static_cast<int64_t>(g_afk.count(3)), 0, "a null controller pointer is refused");

	g_slots[3].controllerPointer = reinterpret_cast<void*>(static_cast<uintptr_t>(0x441000));
	g_slots[3].controllerHandle = 0;
	ResetCounters();
	AdvanceTo(20.0f);
	CheckEqInt(static_cast<int64_t>(g_afk.count(3)), 0, "controller handle 0 is refused");
	CheckEqInt(g_fake.validHandle, 0, "0 is rejected before IsValidEntHandle");
	CheckEqInt(g_fake.suicide + g_fake.changeTeam + g_fake.kick, 0, "no punishment for handle 0");

	g_slots[3].controllerHandle = -1;
	ResetCounters();
	AdvanceTo(20.0f);
	CheckEqInt(static_cast<int64_t>(g_afk.count(3)), 0, "controller handle -1 is refused");
	CheckEqInt(g_fake.validHandle, 0, "-1 is rejected before IsValidEntHandle");

	// A *valid* negative CEntityHandle is accepted (PITFALLS C1): the final punishment runs.
	g_slots[3].controllerHandle = -16777001;
	g_handleValid = true;
	FireEventHook(kRoundStart, nullptr);
	AdvanceTo(0.0f);
	AdvanceTo(20.0f);
	CheckEqInt(g_fake.changeTeam, 1, "a valid negative controller handle reaches the final punishment");

	// An engine-rejected handle is refused even when it is not 0/-1.
	ResetWorld();
	WriteSettings(10, 20, 1, false);
	UseProductionPhrases();
	StartPlugin();
	FireMapStart();
	OnPluginUpdate();
	EnablePlayer(3);
	FireEventHook(kRoundStart, nullptr);
	g_handleValid = false;
	ResetCounters();
	AdvanceTo(20.0f);
	CheckEqInt(static_cast<int64_t>(g_afk.count(3)), 0, "a handle the engine rejects is refused");
	CheckEqInt(g_fake.validHandle, 1, "the engine's IsValidEntHandle is consulted");
	CheckEqInt(g_fake.suicide + g_fake.changeTeam + g_fake.kick, 0, "no punishment for a rejected handle");

	// An invalid controller pointer from the engine is refused.
	g_handleValid = true;
	g_pointerValid = false;
	ResetCounters();
	AdvanceTo(20.0f);
	CheckEqInt(static_cast<int64_t>(g_afk.count(3)), 0, "an engine-rejected controller pointer is refused");
}

// ---------------------------------------------------------------------------------------------
// 6b. the game-rules object gate (the original's `if (!g_pGameRules) return;`)
// ---------------------------------------------------------------------------------------------

void TestGameRulesGate() {
	Section("game rules unavailable");

	ResetWorld();
	WriteSettings(20, 40, 3, false);
	UseProductionPhrases();
	StartPlugin();
	FireMapStart();
	OnPluginUpdate();
	EnablePlayer(3);
	FireEventHook(kRoundStart, nullptr);

	g_gameRulesAvailable = false;
	ResetCounters();
	AdvanceTo(20.0f);
	CheckEqInt(g_fake.isConnected, 0, "no game rules: the whole tick is skipped");
	CheckEqInt(g_fake.isAlive, 0, "no game rules: no pawn read");
	CheckEqInt(static_cast<int64_t>(g_afk.count(3)), 0, "no game rules: no AFK state");

	g_gameRulesAvailable = true;
	AdvanceTo(20.0f);
	CheckEqInt(static_cast<int64_t>(g_afk.count(3)), 1, "a state is created after the rules object returns");
	AdvanceTo(40.0f);
	CheckEqInt(static_cast<int64_t>(g_chat.size()), 1, "the tick resumes when the rules object is back");
}

// ---------------------------------------------------------------------------------------------
// 7. slot reuse / disconnect / map change
// ---------------------------------------------------------------------------------------------

void TestReuseAndDisconnect() {
	Section("slot reuse / disconnect / map change");

	struct Scenario {
		const char* name;
		enum class Kind { Disconnect, Recycle, MapChange } kind;
	};

	const Scenario scenarios[] = {{"player_disconnect", Scenario::Kind::Disconnect},
								  {"slot reuse without the event", Scenario::Kind::Recycle},
								  {"map change", Scenario::Kind::MapChange}};

	for (const Scenario& scenario : scenarios) {
		// Reach warnCount = 2 (the next interval would be the final punishment).
		StartScenario(20, 40, 3, false);
		AdvanceTo(40.0f);
		AdvanceTo(80.0f);
		CheckEqInt(static_cast<int64_t>(g_afk.at(3).afk.warnCount), 2,
				   std::string(scenario.name) + ": warnCount is 2 before the change");

		ResetCounters();
		FakeEvent event;
		event.slot = 3;
		switch (scenario.kind) {
		case Scenario::Kind::Disconnect:
			Check(FireEventHook(kPlayerDisconnect, &event), "the player_disconnect hook fires");
			CheckEqStr(g_lastEventKey, "userid", "player_disconnect reads the player-typed 'userid' field");
			CheckEqInt(static_cast<int64_t>(g_afk.count(3)), 0, "disconnect drops the slot state");
			break;
		case Scenario::Kind::Recycle:
			RecyclePlayer(3);
			break;
		case Scenario::Kind::MapChange:
			FireMapEnd();
			FireMapStart();
			CheckEqInt(static_cast<int64_t>(g_afk.count(3)), 0, "map change clears the AFK state");
			break;
		}

		// At t=120 the old state would have reached the final punishment; the new occupant must not
		// be touched.
		const int suicideBefore = g_fake.suicide;
		const int changeTeamBefore = g_fake.changeTeam;
		const int kickBefore = g_fake.kick;
		AdvanceTo(120.0f);
		CheckEqInt(g_fake.suicide, suicideBefore, std::string(scenario.name) + ": no suicide of the new occupant");
		CheckEqInt(g_fake.changeTeam, changeTeamBefore,
				   std::string(scenario.name) + ": no team change of the new occupant");
		CheckEqInt(g_fake.kick, kickBefore, std::string(scenario.name) + ": no kick of the new occupant");
		CheckEqInt(static_cast<int64_t>(g_afk.count(3)), 1, std::string(scenario.name) + ": a fresh state exists");

		// The new occupant's first punishment is the soft one, not the final one.
		AdvanceTo(160.0f);
		CheckEqInt(g_fake.suicide, suicideBefore + 1, std::string(scenario.name) + ": the new player gets a soft kill");
		CheckEqInt(g_fake.changeTeam, changeTeamBefore, std::string(scenario.name) + ": still no team change");
		CheckEqInt(g_fake.kick, kickBefore, std::string(scenario.name) + ": still no kick");
	}

	// A slot recycled in the middle of the tick: the tick captured the old identity, and the
	// re-check before commit must drop the pending final punishment instead of applying it to the
	// new occupant (the deferred-decision guard).
	StartScenario(20, 40, 3, false);
	AdvanceTo(40.0f);
	AdvanceTo(80.0f);
	CheckEqInt(static_cast<int64_t>(g_afk.at(3).afk.warnCount), 2, "the pending decision has warnCount 2");
	ResetCounters();
	g_recycleOnForward = true;
	g_recycleSlot = 3;
	AdvanceTo(120.0f); // the old identity would reach the final punishment here
	CheckEqInt(g_fake.suicide + g_fake.changeTeam + g_fake.kick, 0,
			   "a slot recycled mid-tick is not punished");
	CheckEqInt(static_cast<int64_t>(g_afk.count(3)), 0, "the stale state is dropped");
	AdvanceTo(160.0f); // the new occupant's fresh state
	AdvanceTo(200.0f);
	CheckEqInt(g_fake.suicide, 1, "the new occupant gets the soft punishment only");
	CheckEqInt(g_fake.changeTeam, 0, "the new occupant is not moved to spectators");
	CheckEqInt(g_fake.kick, 0, "the new occupant is not kicked");

	// A missed player_disconnect: the slot goes away without an event. The next tick must drop the
	// old warnCount so a reconnect starts fresh.
	StartScenario(20, 40, 3, false);
	AdvanceTo(40.0f);
	AdvanceTo(80.0f);
	g_slots[3].connected = false;
	ResetCounters();
	AdvanceTo(120.0f);
	CheckEqInt(static_cast<int64_t>(g_afk.count(3)), 0, "a disconnected slot loses its stale state");
	CheckEqInt(g_fake.suicide + g_fake.changeTeam + g_fake.kick, 0, "a disconnected slot is not punished");
	EnablePlayer(3); // reconnect with the same pointer/handle/SteamID
	AdvanceTo(160.0f);
	AdvanceTo(200.0f);
	CheckEqInt(g_fake.suicide, 1, "the reconnected player gets the soft punishment only");
	CheckEqInt(g_fake.changeTeam, 0, "the reconnected player is not moved to spectators");
	CheckEqInt(g_fake.kick, 0, "the reconnected player is not kicked");

	// A disconnect arriving while the gate is closed only touches state (PITFALLS B2).
	ResetWorld();
	UseProductionFiles();
	StartPlugin();
	FireMapStart();
	OnPluginUpdate();
	EnablePlayer(3);
	FireEventHook(kRoundStart, nullptr);
	AdvanceTo(0.0f);
	FireMapEnd();
	ResetCounters();
	FakeEvent event;
	event.slot = 3;
	Check(FireEventHook(kPlayerDisconnect, &event), "player_disconnect is delivered during a map change");
	CheckEqInt(g_fake.isConnected, 0, "the disconnect handler makes no client call");
	CheckEqInt(g_fake.eventPlayerSlot, 1, "the disconnect handler uses GetEventPlayerSlot");
}

// ---------------------------------------------------------------------------------------------
// 8. teardown / reload (PITFALLS A7)
// ---------------------------------------------------------------------------------------------

void TestTeardown() {
	Section("teardown / reload");

	ResetWorld();
	UseProductionFiles();
	StartPlugin();
	FireMapStart();
	OnPluginUpdate();
	EnablePlayer(3);
	FireEventHook(kRoundStart, nullptr);
	ResetClock();
	AdvanceTo(0.0f); // a live AFK state exists before the unload
	CheckEqInt(static_cast<int64_t>(g_afk.count(3)), 1, "an AFK state exists before the unload");

	ResetCounters();
	g_antiAfkPlugin.OnPluginEnd();

	CheckEqInt(g_fake.unhook, 4, "OnPluginEnd unhooks all four events");
	CheckEqInt(static_cast<int64_t>(g_eventHooks.size()), 4, "the hook records are still present");
	int activeHooks = 0;
	for (const EventHookRecord& hook : g_eventHooks) {
		if (hook.active) {
			++activeHooks;
		}
	}
	CheckEqInt(activeHooks, 0, "no event hook is active after the unload");
	CheckEqInt(g_fake.mapStartUnregister, 1, "OnPluginEnd unregisters OnMapStart");
	CheckEqInt(g_fake.mapEndUnregister, 1, "OnPluginEnd unregisters OnMapEnd");
	Check(g_onMapStart == nullptr && g_onMapEnd == nullptr, "the map listener pointers are cleared");
	Check(!g_mapReady, "the gate is closed after the unload");
	Check(g_afk.empty(), "the AFK state is dropped on unload");
	CheckEqInt(g_fake.createTimer + g_fake.killsTimer, 0, "there is no s2sdk timer to create or kill");

	// A stale update after the unload must not reach a client or run the AFK check.
	ResetCounters();
	PumpUpdate(10.0f, 5.0);
	CheckEqInt(g_fake.gameTime, 0, "an update after OnPluginEnd does not run the check");
	CheckEqInt(g_fake.isConnected, 0, "an update after OnPluginEnd makes no client call");

	// Reload: the hooks/listeners are registered exactly once again, and the accumulator starts at
	// zero (the first check is a full interval after round_start, not immediately).
	ResetWorld(); // OnPluginEnd is a no-op now (already unloaded)
	UseProductionFiles();
	StartPlugin();
	CheckEqInt(g_fake.mapStartRegister, 1, "a reload registers OnMapStart once");
	CheckEqInt(g_fake.mapEndRegister, 1, "a reload registers OnMapEnd once");
	int activeAfterReload = 0;
	for (const EventHookRecord& hook : g_eventHooks) {
		if (hook.active) {
			++activeAfterReload;
		}
	}
	CheckEqInt(activeAfterReload, 4, "a reload registers the four hooks once");
	CheckEqInt(g_fake.createTimer, 0, "a reload creates no s2sdk timer");

	FireMapStart();
	PumpUpdate(0.0f, 5.0); // latch only
	Check(g_mapReady, "a reload latches the gate on the first update");
	CheckEqInt(g_fake.gameTime, 0, "the reload latch does not run a check");
	FireEventHook(kRoundStart, nullptr);
	ResetCounters();
	PumpUpdate(0.0f, 0.5);
	CheckEqInt(g_fake.gameTime, 0, "reload: 0.5 s is not enough");
	PumpUpdate(0.0f, 0.5);
	CheckEqInt(g_fake.gameTime, 1, "reload: the first check is a full interval later");

	ResetCounters();
	g_antiAfkPlugin.OnPluginEnd();
	CheckEqInt(g_fake.killsTimer, 0, "the reload has no s2sdk timer to kill");

	// A double unload is harmless.
	g_antiAfkPlugin.OnPluginEnd();
	CheckEqInt(g_fake.killsTimer, 0, "a second OnPluginEnd kills nothing");

	// An unload before any map does not touch the timer API either.
	ResetWorld();
	UseProductionFiles();
	StartPlugin();
	ResetCounters();
	g_antiAfkPlugin.OnPluginEnd();
	CheckEqInt(g_fake.createTimer + g_fake.killsTimer, 0, "an unload before a map never touches the timer API");

	// Without map listener bindings the unload is still harmless.
	ResetWorld();
	UseProductionFiles();
	__s2sdk_OnMapStart_Register = nullptr;
	__s2sdk_OnMapEnd_Register = nullptr;
	StartPlugin();
	g_antiAfkPlugin.OnPluginEnd();
	CheckEqInt(g_fake.mapStartUnregister, 0, "nothing was registered, so nothing is unregistered");
	CheckEqInt(g_fake.killsTimer, 0, "still no s2sdk timer");
}
} // namespace

int main() {
	std::error_code ec;
	std::filesystem::remove_all(TempRoot(), ec);

	// Run the whole suite in a private cwd sandbox: the plugin's raw pisexcompat candidates are
	// relative to the server working directory, so an isolated cwd makes the raw/decoy tests
	// deterministic and keeps them from seeing files in the build directory or the repository.
	const std::filesystem::path originalCwd = std::filesystem::current_path(ec);
	const std::filesystem::path sandbox = TempRoot() / "cwd";
	std::filesystem::create_directories(sandbox, ec);
	std::filesystem::current_path(sandbox, ec);
	if (ec) {
		std::printf("FAIL: cannot enter the private cwd sandbox\n");
		return 1;
	}

	TestKv1Reader();
	TestConfigLoading();
	TestPhrasesAndLanguage();
	TestPathCandidates();
	TestMapGateAndCadence();
	TestFirstWarningCountdownAndFinal();
	TestMatchEnd();
	TestKickInsteadOfSpec();
	TestResets();
	TestExceptionIsolation();
	TestClientGuards();
	TestGameRulesGate();
	TestReuseAndDisconnect();
	TestTeardown();

	std::filesystem::current_path(originalCwd, ec);
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
