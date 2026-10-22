#include "config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace app {

namespace fs = std::filesystem;

std::string config_dir() {
    std::string dir;
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) dir = std::string(x) + "/dantagger";
    else dir = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/.config/dantagger";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

std::string default_data_dir() {
    if (const char* d = std::getenv("DANTAGGER_DATA"); d && *d) return d;
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        // <repo>/build/app/dantagger -> <repo>/data
        const fs::path repo = exe.parent_path().parent_path().parent_path();
        if (fs::exists(repo / "build.py", ec) && fs::exists(repo / "core", ec)) return (repo / "data").string();
    }
    return std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/Dantagger-data";
}

KeyValues read_kv(const std::string& path) {
    KeyValues kv;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find(" = ");
        if (eq == std::string::npos) continue;
        kv.emplace_back(line.substr(0, eq), line.substr(eq + 3));
    }
    return kv;
}

bool write_kv(const std::string& path, const KeyValues& kv) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) return false;
        out << "# Dantagger app settings (key = value)\n";
        for (const auto& [k, v] : kv) out << k << " = " << v << "\n";
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

std::string kv_get(const KeyValues& kv, const std::string& key, const std::string& fallback) {
    std::string v = fallback;
    for (const auto& [k, val] : kv)
        if (k == key) v = val;
    return v;
}

double kv_num(const KeyValues& kv, const std::string& key, double fallback) {
    const std::string v = kv_get(kv, key);
    if (v.empty()) return fallback;
    try {
        return std::stod(v);
    } catch (...) {
        return fallback;
    }
}

}  // namespace app
