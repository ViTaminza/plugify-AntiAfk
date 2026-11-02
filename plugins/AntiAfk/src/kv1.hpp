// The read-only Valve KeyValues (KV1) reader AntiAfk needs for its two production files.
//
// The files are the original Metamod installation's files, unchanged (PLUGIN_STANDARD §7: the
// production KeyValues files are read as they are, no conversion):
//
//     // settings.ini
//     "AntiAfk"
//     {
//         "initial_warning_time" "20"
//         "warning_interval"     "40"
//     }
//
//     // antiafk.phrases.txt
//     "Phrases"
//     {
//         "AFK_Prefix"  { "en" "[AFK]"  "ru" "[AFK]" }
//     }
//
// Accepted grammar is Valve's: quoted ("key" "value") and unquoted tokens, a key may carry a value
// and a block, nested blocks, // and /* */ comments, \" \\ \n \r \t escapes inside quotes, an
// optional UTF-8 BOM, and #base / #include / #define lines are skipped instead of followed. A
// malformed file - an unterminated string, a missing '}' or a stray top-level '}' - is rejected as
// a whole (std::nullopt) so the caller keeps its defaults instead of using half a settings table
// or half a translation table.
//
// Like Valve's loader, a file that is a single "Root" { ... } block is returned as that block, so
// `LoadKv1File(...)->GetString("initial_warning_time", ...)` reads a key inside "AntiAfk" { ... }.
//
// Header-only and engine-free on purpose: the plugin and anti_afk_plugin_tests parse the shipped
// files with exactly this code (no s2sdk, no Plugify host needed).

#pragma once

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace antiafk {

struct KvNode {
	std::string name;
	std::string value;
	bool hasValue = false;
	std::vector<KvNode> children;

	// First child with that name (case-insensitive), or nullptr.
	const KvNode* Find(std::string_view key) const;
	// The child's value; `def` when the child is missing or has no value.
	std::string_view GetString(std::string_view key, std::string_view def = {}) const;
	// The child's value as an int, with Valve's atoi fallback ("abc" -> 0); `def` when the child is
	// missing or its value is empty.
	int GetInt(std::string_view key, int def = 0) const;
	// The child's value as a float, with Valve's atof/truncation semantics (the original stored
	// GetFloat() in an int for the two time settings); `def` when the child is missing/empty.
	float GetFloat(std::string_view key, float def = 0.0f) const;
};

// key -> resolved text, the same shape the original kept in g_Phrases.
using PhraseMap = std::map<std::string, std::string>;

namespace detail {

inline char LowerCase(char c) {
	return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

inline bool EqualNoCase(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (std::size_t i = 0; i < a.size(); ++i) {
		if (LowerCase(a[i]) != LowerCase(b[i])) {
			return false;
		}
	}
	return true;
}

inline std::string_view Trim(std::string_view text) {
	std::size_t begin = 0;
	std::size_t end = text.size();
	while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
		++begin;
	}
	while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
		--end;
	}
	return text.substr(begin, end - begin);
}

//   file  := node* EOF
//   node  := token [ token ] [ '{' node* '}' ]
//   token := '"' escaped '"' | raw
class Parser {
public:
	explicit Parser(std::string_view text) : text_(text) {}

	bool Parse(KvNode& root) {
		ParseNodes(root, false);
		return !failed_;
	}

private:
	void SkipSpace() {
		while (pos_ < text_.size()) {
			const char c = text_[pos_];
			if (std::isspace(static_cast<unsigned char>(c))) {
				++pos_;
				continue;
			}
			if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '/') {
				pos_ += 2;
				while (pos_ < text_.size() && text_[pos_] != '\n') {
					++pos_;
				}
				continue;
			}
			if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '*') {
				pos_ += 2;
				while (pos_ + 1 < text_.size() && !(text_[pos_] == '*' && text_[pos_ + 1] == '/')) {
					++pos_;
				}
				pos_ = (pos_ + 1 < text_.size()) ? pos_ + 2 : text_.size();
				continue;
			}
			break;
		}
	}

	// One token; false at the end of the input or at a '}'.
	bool ReadToken(std::string& out) {
		SkipSpace();
		if (pos_ >= text_.size() || text_[pos_] == '}') {
			return false;
		}
		out.clear();
		if (text_[pos_] == '"') {
			++pos_;
			while (pos_ < text_.size()) {
				const char c = text_[pos_++];
				if (c == '\\' && pos_ < text_.size()) {
					const char esc = text_[pos_++];
					switch (esc) {
					case 'n':
						out.push_back('\n');
						break;
					case 'r':
						out.push_back('\r');
						break;
					case 't':
						out.push_back('\t');
						break;
					case '"':
						out.push_back('"');
						break;
					case '\\':
						out.push_back('\\');
						break;
					default:
						out.push_back('\\');
						out.push_back(esc);
						break;
					}
					continue;
				}
				if (c == '"') {
					return true;
				}
				out.push_back(c);
			}
			failed_ = true; // unterminated string
			return true;
		}
		while (pos_ < text_.size()) {
			const char c = text_[pos_];
			if (std::isspace(static_cast<unsigned char>(c)) || c == '{' || c == '}' || c == '"') {
				break;
			}
			if (c == '/' && pos_ + 1 < text_.size() && (text_[pos_ + 1] == '/' || text_[pos_ + 1] == '*')) {
				break;
			}
			out.push_back(c);
			++pos_;
		}
		return true;
	}

	bool PeekIs(char c) {
		SkipSpace();
		return pos_ < text_.size() && text_[pos_] == c;
	}

	void ParseNodes(KvNode& parent, bool insideBlock) {
		while (true) {
			SkipSpace();
			if (pos_ >= text_.size()) {
				if (insideBlock) {
					failed_ = true; // missing '}'
				}
				return;
			}
			if (text_[pos_] == '}') {
				if (!insideBlock) {
					failed_ = true; // stray '}'
				}
				++pos_;
				return;
			}
			std::string key;
			if (!ReadToken(key)) {
				return;
			}
			if (!key.empty() && key[0] == '#') {
				// #base / #include / #define: no inclusion, skip the line.
				while (pos_ < text_.size() && text_[pos_] != '\n') {
					++pos_;
				}
				continue;
			}
			parent.children.push_back(KvNode{});
			// Filled through an index: the recursion below may reallocate parent.children.
			const std::size_t index = parent.children.size() - 1;
			parent.children[index].name = std::move(key);
			if (!PeekIs('{')) {
				std::string value;
				if (ReadToken(value)) {
					parent.children[index].value = std::move(value);
					parent.children[index].hasValue = true;
				}
			}
			if (PeekIs('{')) {
				++pos_;
				KvNode child = std::move(parent.children[index]);
				ParseNodes(child, true);
				parent.children[index] = std::move(child);
			}
		}
	}

	std::string_view text_;
	std::size_t pos_ = 0;
	bool failed_ = false;
};

} // namespace detail

inline const KvNode* KvNode::Find(std::string_view key) const {
	if (key.empty()) {
		return nullptr;
	}
	for (const KvNode& child : children) {
		if (detail::EqualNoCase(child.name, key)) {
			return &child;
		}
	}
	return nullptr;
}

inline std::string_view KvNode::GetString(std::string_view key, std::string_view def) const {
	const KvNode* found = Find(key);
	if (found == nullptr || !found->hasValue) {
		return def;
	}
	return found->value;
}

inline int KvNode::GetInt(std::string_view key, int def) const {
	const std::string_view text = detail::Trim(GetString(key));
	if (text.empty()) {
		return def;
	}
	int number = 0;
	const auto result = std::from_chars(text.data(), text.data() + text.size(), number);
	if (result.ec != std::errc{}) {
		return std::atoi(std::string(text).c_str()); // Valve's atoi semantics
	}
	return number;
}

inline float KvNode::GetFloat(std::string_view key, float def) const {
	const std::string_view text = detail::Trim(GetString(key));
	if (text.empty()) {
		return def;
	}
	// The ported loader's semantics (pisexcompat's KeyValues::GetFloat): strtof with a fallback to
	// the default when it consumes nothing at all ("abc" -> def, "40.7 // x" -> 40.7), and the
	// original's `(int)GetFloat(...)` truncates the result.
	const std::string copy(text);
	char* end = nullptr;
	const float value = std::strtof(copy.c_str(), &end);
	if (end == copy.c_str()) {
		return def;
	}
	return value;
}

inline std::optional<KvNode> ParseKv1(std::string_view text) {
	// A file saved by a Windows editor starts with a UTF-8 BOM: it is not part of the first key.
	if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
		static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
		text.remove_prefix(3);
	}
	KvNode root;
	detail::Parser parser(text);
	if (!parser.Parse(root)) {
		return std::nullopt;
	}
	// A single top-level block is the file's root ("AntiAfk" { ... } / "Phrases" { ... }), as in
	// Valve's loader.
	if (root.children.size() == 1 && !root.children.front().children.empty()) {
		return std::move(root.children.front());
	}
	return root;
}

inline std::optional<KvNode> LoadKv1File(const std::string& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return std::nullopt;
	}
	// An empty file parses as an empty root: the caller keeps its defaults instead of treating the
	// file as malformed.
	std::ostringstream content;
	content << file.rdbuf();
	return ParseKv1(content.str());
}

// A phrases file ("Key" { "ru" "..." "en" "..." }): every block child resolved for `language`,
// then for "en", then for the first language that has a non-empty text. The plain values next to
// the blocks (the file's own "language" key, if any) are not phrases and are skipped.
inline PhraseMap ResolvePhrases(const KvNode& root, std::string_view language) {
	constexpr std::string_view kFallback = "en";
	PhraseMap out;
	for (const KvNode& key : root.children) {
		if (key.children.empty()) {
			continue; // a plain value ("language" "ru"), not a phrase
		}
		std::string text;
		if (const KvNode* localized = key.Find(language); localized != nullptr && localized->hasValue) {
			text = localized->value;
		}
		if (text.empty() && !detail::EqualNoCase(language, kFallback)) {
			if (const KvNode* english = key.Find(kFallback); english != nullptr && english->hasValue) {
				text = english->value;
			}
		}
		if (text.empty()) {
			// Last resort: any language the file has, so a partially translated file still shows
			// something instead of an empty chat line (pisexcompat did the same).
			for (const KvNode& candidate : key.children) {
				if (candidate.hasValue) {
					text = candidate.value;
					break;
				}
			}
		}
		out[key.name] = std::move(text);
	}
	return out;
}

} // namespace antiafk
