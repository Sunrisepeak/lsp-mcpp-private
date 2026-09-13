module lspmcpp.toolchain.discover;

import std;
import lspmcpp.os;
import lspmcpp.base.path;
import lspmcpp.base.text;
import lspmcpp.platform.env;
import lspmcpp.platform.fs;
import lspmcpp.platform.dirs;
import lspmcpp.spec.database;
import lspmcpp.toolchain.probe;

namespace lspmcpp::toolchain {

namespace {

void add(std::vector<CompilerCandidate>& out, std::string driver, std::string_view origin) {
    driver = base::normalize_path(driver);
    if (!platform::fs::is_regular_file(driver)) return;
    if (std::ranges::any_of(out, [&](const CompilerCandidate& candidate) { return base::same_path(candidate.driver, driver); })) return;
    out.push_back(CompilerCandidate { driver, classify_driver(driver), std::string { origin } });
}

// <store>/xim-x-gcc/<version>/bin/g++ and <store>/xim-x-llvm/<version>/bin/clang++, newest first.
void add_store(std::vector<CompilerCandidate>& out, std::string_view store, std::string_view origin) {
    const std::string suffix { lspmcpp::os::EXECUTABLE_SUFFIX };
    for (const auto& [package, driver] : { std::pair { "xim-x-llvm", "clang++" }, std::pair { "xim-x-gcc", "g++" } }) {
        auto versions = platform::fs::list_directory(base::join_path(store, package));
        std::ranges::sort(versions, std::greater<> {});
        for (const auto& version : versions) add(out, base::join_path(version, std::format("bin/{}{}", driver, suffix)), origin);
    }
}

} // namespace

std::vector<CompilerCandidate> discover_compilers(const Runner& runner) {
    std::vector<CompilerCandidate> out;
    for (std::string_view name : { "c++", "g++", "clang++", "cl", "clang-cl" }) {
        if (auto found = platform::env::find_executable(name)) add(out, *found, "PATH");
    }
    const std::string home { platform::dirs::home_directory() };
    add_store(out, base::join_path(home, ".mcpp/registry/data/xpkgs"), "mcpp");
    add_store(out, base::join_path(home, ".xlings/data/xpkgs"), "xlings");
    if constexpr (lspmcpp::os::FAMILY == lspmcpp::os::Family::macos) {
        add(out, "/opt/homebrew/opt/llvm/bin/clang++", "homebrew");
    }
    if constexpr (lspmcpp::os::FAMILY == lspmcpp::os::Family::windows) {
        const std::string vswhere { "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe" };
        if (platform::fs::is_regular_file(vswhere)) {
            const std::vector<std::string> argv { vswhere, "-latest", "-products", "*", "-requires",
                                                  "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath" };
            if (auto result = runner(argv); result && result->exitCode == 0) {
                const std::string installation { base::normalize_path(base::trim(result->output)) };
                auto versions = platform::fs::list_directory(base::join_path(installation, "VC/Tools/MSVC"));
                std::ranges::sort(versions, std::greater<> {});
                for (const auto& version : versions) add(out, base::join_path(version, "bin/Hostx64/x64/cl.exe"), "visual-studio");
                add(out, base::join_path(installation, "VC/Tools/Llvm/x64/bin/clang-cl.exe"), "visual-studio");
            }
        }
    }
    return out;
}

} // namespace lspmcpp::toolchain
