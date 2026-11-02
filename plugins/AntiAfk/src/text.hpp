// Chat text helpers for AntiAfk: the cs2-menus colour tags of the production phrase files and the
// snprintf formatting the original applied to every phrase.
//
// s2sdk's PrintToChat sends a message as it is (its PrintToChatColored uses a different tag table),
// so the {TAG} words of the production antiafk.phrases.txt ("[{RED}AFK{DEFAULT}]", "{RED}%d
// {DEFAULT}") are replaced here with exactly the control bytes the Metamod cs2-menus servers sent -
// the same table, order and bytes pisexcompat used:
//
//     {DEFAULT} {WHITE} {RED} {LIGHTPURPLE} {GREEN} {LIME} {LIGHTGREEN} {DARKRED} {GRAY}
//     {LIGHTOLIVE} {OLIVE} {LIGHTBLUE} {BLUE} {PURPLE} {LIGHTRED} {GRAYBLUE} and "\n" (U+2029)
//
// The phrase text is the printf *format* of the original (`snprintf(msg, ..., phrase, remain)`),
// so FormatOne/FormatTwo keep exactly that: one int for every countdown/warning and two ints for
// AFK_WarningKill. A phrase with a different conversion would read a missing argument, which is
// what the original did as well; the production files only use %d.
//
// Header-only and engine-free: anti_afk_plugin_tests checks the tag bytes and the formatting of the
// shipped phrases file.

#pragma once

#include <array>
#include <cstdio>
#include <string>
#include <string_view>

namespace antiafk {

namespace detail {

struct ColorTag {
	std::string_view tag;
	std::string_view bytes;
};

// cs2-menus' utils.h colors_text[]/colors_hex[], same order and bytes as pisexcompat. The last
// entry is the chat line break: the phrase files spell it "\n" (backslash + n) and a chat line
// wants U+2029, the engine's line separator.
inline constexpr std::array<ColorTag, 17> kColorTags = {{{"{DEFAULT}", "\x01"},
														 {"{WHITE}", "\x01"},
														 {"{RED}", "\x02"},
														 {"{LIGHTPURPLE}", "\x03"},
														 {"{GREEN}", "\x04"},
														 {"{LIME}", "\x05"},
														 {"{LIGHTGREEN}", "\x06"},
														 {"{DARKRED}", "\x07"},
														 {"{GRAY}", "\x08"},
														 {"{LIGHTOLIVE}", "\x09"},
														 {"{OLIVE}", "\x10"},
														 {"{LIGHTBLUE}", "\x0B"},
														 {"{BLUE}", "\x0C"},
														 {"{PURPLE}", "\x0E"},
														 {"{LIGHTRED}", "\x0F"},
														 {"{GRAYBLUE}", "\x0A"},
														 {"\\n", "\xe2\x80\xa9"}}};

// Pisex formatted into char[512] and pisexcompat into char[2048]; the phrases are short, but the
// cap keeps a malformed phrase from producing an unbounded string.
inline constexpr std::size_t kFormatBufferSize = 2048;

} // namespace detail

// The phrase text with the colour tags turned into their chat control bytes.
inline std::string Colorize(std::string_view text) {
	std::string out(text);
	for (const detail::ColorTag& color : detail::kColorTags) {
		std::size_t pos = 0;
		while ((pos = out.find(color.tag, pos)) != std::string::npos) {
			out.replace(pos, color.tag.size(), color.bytes);
			pos += color.bytes.size();
		}
	}
	return out;
}

// The original's snprintf(msg, sizeof msg, phrase, a).
inline std::string FormatOne(const char* format, int a) {
	std::array<char, detail::kFormatBufferSize> buffer{};
	const int written = std::snprintf(buffer.data(), buffer.size(), format != nullptr ? format : "", a);
	if (written <= 0) {
		return {};
	}
	return std::string(buffer.data(),
					   static_cast<std::size_t>(written) < buffer.size() - 1 ? static_cast<std::size_t>(written)
																			  : buffer.size() - 1);
}

// The original's snprintf(msg, sizeof msg, phrase, a, b) for AFK_WarningKill.
inline std::string FormatTwo(const char* format, int a, int b) {
	std::array<char, detail::kFormatBufferSize> buffer{};
	const int written = std::snprintf(buffer.data(), buffer.size(), format != nullptr ? format : "", a, b);
	if (written <= 0) {
		return {};
	}
	return std::string(buffer.data(),
					   static_cast<std::size_t>(written) < buffer.size() - 1 ? static_cast<std::size_t>(written)
																			  : buffer.size() - 1);
}

} // namespace antiafk
