#pragma once

// Where the app keeps its things: settings and layout in the config folder
// ($XDG_CONFIG_HOME/dantagger or ~/.config/dantagger), saved tag streams in
// the data folder (<repo>/data when the binary runs from the repository's
// build/ folder, user 2026-10-22; else ~/Dantagger-data; File > Data folder
// changes it).  Settings files are plain "key = value" lines.

#include <string>
#include <utility>
#include <vector>

namespace app {

std::string config_dir();
std::string default_data_dir();

using KeyValues = std::vector<std::pair<std::string, std::string>>;
KeyValues read_kv(const std::string& path);
bool write_kv(const std::string& path, const KeyValues& kv);   // atomically (temporary file + rename)
// The value of `key` (the last one), or `fallback`.
std::string kv_get(const KeyValues& kv, const std::string& key, const std::string& fallback = {});
double kv_num(const KeyValues& kv, const std::string& key, double fallback);

}  // namespace app
