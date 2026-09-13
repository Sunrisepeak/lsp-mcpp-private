module lspmcpp.platform.dirs;

import std;
import lspmcpp.os;
import lspmcpp.base.path;
import lspmcpp.platform.env;

namespace lspmcpp::platform::dirs {

namespace {

std::optional<std::string> non_empty(std::string_view name) {
    auto value = env::get(name);
    if (!value || value->empty()) return std::nullopt;
    return base::normalize_path(*value);
}

} // namespace

std::string home_directory() {
    if constexpr (lspmcpp::os::FAMILY == lspmcpp::os::Family::windows) {
        if (auto profile = non_empty("USERPROFILE")) return *profile;
        auto drive = env::get("HOMEDRIVE");
        auto path = env::get("HOMEPATH");
        if (drive && path) return base::normalize_path(*drive + *path);
        return "C:/";
    } else {
        if (auto home = non_empty("HOME")) return *home;
        return "/";
    }
}

std::string cache_directory() {
    if (auto overridden = non_empty("LSP_MCPP_CACHE_DIR")) return *overridden;
    if constexpr (lspmcpp::os::FAMILY == lspmcpp::os::Family::windows) {
        if (auto local = non_empty("LOCALAPPDATA")) return base::join_path(*local, "lsp-mcpp");
        return base::join_path(home_directory(), "AppData/Local/lsp-mcpp");
    } else if constexpr (lspmcpp::os::FAMILY == lspmcpp::os::Family::macos) {
        return base::join_path(home_directory(), "Library/Caches/lsp-mcpp");
    } else {
        if (auto xdg = non_empty("XDG_CACHE_HOME"); xdg && base::is_absolute_path(*xdg)) return base::join_path(*xdg, "lsp-mcpp");
        return base::join_path(home_directory(), ".cache/lsp-mcpp");
    }
}

std::string temp_directory() {
    if constexpr (lspmcpp::os::FAMILY == lspmcpp::os::Family::windows) {
        for (std::string_view name : { "TEMP", "TMP" }) {
            if (auto value = non_empty(name)) return *value;
        }
        return base::join_path(home_directory(), "AppData/Local/Temp");
    } else {
        if (auto value = non_empty("TMPDIR")) return *value;
        return "/tmp";
    }
}

} // namespace lspmcpp::platform::dirs
