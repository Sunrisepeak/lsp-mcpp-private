module lspmcpp.server.payload;

import std;
import nlohmann.json;
import lspmcpp.os;
import lspmcpp.base.error;
import lspmcpp.base.path;
import lspmcpp.base.text;
import lspmcpp.platform.fs;
import lspmcpp.platform.env;
import lspmcpp.platform.dirs;
import lspmcpp.platform.process;
import lspmcpp.engine.clangd;

namespace lspmcpp::server {

namespace {

std::string absolute(std::string_view path) {
    if (path.empty() || base::is_absolute_path(path)) return base::normalize_path(path);
    return base::join_path(platform::fs::current_directory(), path);
}

} // namespace

namespace {

// The payload this executable sits in: <payload>/bin/lsp-mcpp next to <payload>/payload.json.
std::string enclosing_payload() {
    const auto arguments = platform::env::arguments();
    if (arguments.empty()) return {};
    std::string self { arguments.front() };
    if (!base::is_absolute_path(self)) {
        if (self.find('/') == std::string::npos && self.find('\\') == std::string::npos) {
            self = platform::env::find_executable(self).value_or("");
        } else {
            self = base::join_path(platform::fs::current_directory(), self);
        }
    }
    if (self.empty()) return {};
    const std::string candidate { base::parent_path(base::parent_path(base::normalize_path(self))) };
    return platform::fs::is_regular_file(base::join_path(candidate, "payload.json")) ? candidate : std::string {};
}

// A kit installed by xlings: <store>/xim-x-lsp-mcpp-kit/<version>[/<archive root>]/kit.json, newest version first.
std::string installed_kit() {
    const std::string home { platform::dirs::home_directory() };
    for (std::string_view store : { ".xlings/data/xpkgs", ".mcpp/registry/data/xpkgs" }) {
        auto versions = platform::fs::list_directory(base::join_path(home, base::join_path(store, "xim-x-lsp-mcpp-kit")));
        std::ranges::sort(versions, std::greater<> {});
        for (const auto& version : versions) {
            if (platform::fs::is_regular_file(base::join_path(version, "kit.json"))) return version;
            for (const auto& child : platform::fs::list_directory(version)) {
                if (platform::fs::is_regular_file(base::join_path(child, "kit.json"))) return child;
            }
        }
    }
    return {};
}

} // namespace

PayloadPaths resolve_payload(const PayloadRequest& requested) {
    PayloadPaths paths;
    paths.platform = std::string { lspmcpp::os::VSCODE_TARGET };
    const std::string suffix { lspmcpp::os::EXECUTABLE_SUFFIX };
    PayloadRequest request { requested };
    if (request.payloadDirectory.empty()) request.payloadDirectory = enclosing_payload();
    if (!request.payloadDirectory.empty()) {
        paths.directory = absolute(request.payloadDirectory);
        const std::string manifest { base::join_path(paths.directory, "payload.json") };
        if (auto text = platform::fs::read_file(manifest)) {
            nlohmann::json document = nlohmann::json::parse(*text, nullptr, false);
            if (!document.is_discarded() && document.is_object()) {
                if (auto clangd = document.find("clangd"); clangd != document.end() && clangd->is_object()) {
                    paths.clangd = base::join_path(paths.directory, clangd->value("path", std::string {}));
                    paths.clangdVersion = clangd->value("version", std::string {});
                }
                if (auto kit = document.find("kit"); kit != document.end() && kit->is_object()) {
                    paths.kit = base::join_path(paths.directory, kit->value("path", std::string { "kit" }));
                }
                paths.platform = document.value("platform", paths.platform);
            }
        }
        if (paths.clangd.empty()) paths.clangd = base::join_path(paths.directory, "clangd/bin/clangd" + suffix);
        if (paths.kit.empty()) paths.kit = base::join_path(paths.directory, "kit");
    }
    if (!request.clangd.empty()) {
        paths.clangd = absolute(request.clangd);
        paths.clangdVersion.clear();
    }
    if (!request.kit.empty()) paths.kit = absolute(request.kit);
    if (paths.clangd.empty() || !platform::fs::is_regular_file(paths.clangd)) {
        if (auto found = platform::env::find_executable("clangd")) {
            if (paths.clangd.empty()) paths.clangd = *found;
        }
    }
    if (!paths.kit.empty() && !platform::fs::is_regular_file(base::join_path(paths.kit, "kit.json"))) paths.kit.clear();
    if (paths.kit.empty() && request.kit.empty()) paths.kit = installed_kit();
    if (paths.clangdVersion.empty() && !paths.clangd.empty() && platform::fs::is_regular_file(paths.clangd)) {
        platform::SpawnOptions options;
        options.program = paths.clangd;
        options.arguments = { "--version" };
        if (auto result = platform::run(std::move(options), std::chrono::seconds { 20 }); result && !result->timedOut) {
            paths.clangdVersion = engine::parse_clangd_version(result->output + result->error);
        }
    }
    return paths;
}

std::string macos_sdk_path() {
    if constexpr (lspmcpp::os::FAMILY != lspmcpp::os::Family::macos) {
        return {};
    } else {
        if (auto sdkroot = platform::env::get("SDKROOT"); sdkroot && platform::fs::is_directory(*sdkroot)) return base::normalize_path(*sdkroot);
        if (platform::fs::is_regular_file("/usr/bin/xcrun")) {
            platform::SpawnOptions options;
            options.program = "/usr/bin/xcrun";
            options.arguments = { "--show-sdk-path" };
            if (auto result = platform::run(std::move(options), std::chrono::seconds { 20 }); result && result->exitCode == 0) {
                const std::string path { base::trim(result->output) };
                if (platform::fs::is_directory(path)) return path;
            }
        }
        for (std::string_view candidate : { "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk",
                                            "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk" }) {
            if (platform::fs::is_directory(candidate)) return std::string { candidate };
        }
        return {};
    }
}

} // namespace lspmcpp::server
