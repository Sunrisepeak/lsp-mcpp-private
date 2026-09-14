import std;
import lspmcpp.testing;
import lspmcpp.os;
import lspmcpp.base.path;
import lspmcpp.platform.env;
import lspmcpp.platform.dirs;
import lspmcpp.platform.fs;

namespace env = lspmcpp::platform::env;
namespace base = lspmcpp::base;

int main() {
    using namespace lspmcpp::testing;

    "the environment is visible"_test = [] {
        expect(!env::variables().empty());
        expect(env::get("PATH").has_value());
        expect(!env::search_path().empty());
        expect(!env::get("LSPMCPP_SURELY_UNSET_VARIABLE").has_value());
    };

    "a value is reported as it was set"_test = [] {
        if constexpr (lspmcpp::os::FAMILY == lspmcpp::os::Family::windows) {
            const auto interpreter = env::get("ComSpec");
            expect(fatal(interpreter.has_value()));
            expect(interpreter->contains('\\') && !interpreter->contains('/')) << *interpreter;
        } else {
            expect(env::get("PATH").value_or("").contains('/'));
        }
    };

    "a system executable is found on PATH"_test = [] {
        if constexpr (lspmcpp::os::FAMILY == lspmcpp::os::Family::windows) {
            const auto found = env::find_executable("cmd");
            expect(fatal(found.has_value()));
            expect(base::file_name(*found) == "cmd.exe" || base::file_name(*found) == "CMD.EXE") << *found;
        } else {
            const auto found = env::find_executable("sh");
            expect(fatal(found.has_value()));
            expect(base::is_absolute_path(*found)) << *found;
        }
        expect(!env::find_executable("lsp-mcpp-no-such-program").has_value());
    };

    "an executable is found on another environment's PATH"_test = [] {
        const std::string first { base::join_path(lspmcpp::platform::dirs::temp_directory(),
            std::format("lsp-mcpp-test-env-first-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        const std::string second { first + "-second" };
        const std::string name { std::string { "lsp-mcpp-fake-tool" } + std::string { lspmcpp::os::EXECUTABLE_SUFFIX } };
        (void)lspmcpp::platform::fs::create_directories(first);
        (void)lspmcpp::platform::fs::create_directories(second);
        (void)lspmcpp::platform::fs::write_file(base::join_path(second, name), "");
        const std::string pathList { first + std::string { lspmcpp::os::PATH_LIST_SEPARATOR } + second };
        const auto found = env::find_executable("lsp-mcpp-fake-tool", pathList);
        expect(found.has_value() && base::same_path(*found, base::join_path(second, name))) << found.value_or("<none>");
        expect(!env::find_executable("lsp-mcpp-fake-tool").has_value()) << "not on this process's PATH";
        expect(!env::find_executable("lsp-mcpp-fake-tool", first).has_value());
        lspmcpp::platform::fs::remove_all(first);
        lspmcpp::platform::fs::remove_all(second);
    };

    "arguments include this program"_test = [] {
        const auto arguments = env::arguments();
        expect(fatal(!arguments.empty()));
        expect(arguments.front().find("test_env") != std::string::npos) << arguments.front();
    };

    "well-known directories are absolute"_test = [] {
        expect(base::is_absolute_path(lspmcpp::platform::dirs::home_directory())) << lspmcpp::platform::dirs::home_directory();
        expect(base::is_absolute_path(lspmcpp::platform::dirs::temp_directory())) << lspmcpp::platform::dirs::temp_directory();
        const std::string cache { lspmcpp::platform::dirs::cache_directory() };
        expect(base::is_absolute_path(cache)) << cache;
        expect(cache.ends_with("lsp-mcpp") || env::get("LSP_MCPP_CACHE_DIR").has_value()) << cache;
    };

    return report();
}
