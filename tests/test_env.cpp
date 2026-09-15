import std;
import mcppls.testing;
import mcppls.os;
import mcppls.base.path;
import mcppls.platform.env;
import mcppls.platform.dirs;
import mcppls.platform.fs;

namespace env = mcppls::platform::env;
namespace base = mcppls::base;

int main() {
    using namespace mcppls::testing;

    "the environment is visible"_test = [] {
        expect(!env::variables().empty());
        expect(env::get("PATH").has_value());
        expect(!env::search_path().empty());
        expect(!env::get("LSPMCPP_SURELY_UNSET_VARIABLE").has_value());
    };

    "a value is reported as it was set"_test = [] {
        if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) {
            const auto interpreter = env::get("ComSpec");
            expect(fatal(interpreter.has_value()));
            expect(interpreter->contains('\\') && !interpreter->contains('/')) << *interpreter;
        } else {
            expect(env::get("PATH").value_or("").contains('/'));
        }
    };

    "a system executable is found on PATH"_test = [] {
        if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) {
            const auto found = env::find_executable("cmd");
            expect(fatal(found.has_value()));
            expect(base::file_name(*found) == "cmd.exe" || base::file_name(*found) == "CMD.EXE") << *found;
        } else {
            const auto found = env::find_executable("sh");
            expect(fatal(found.has_value()));
            expect(base::is_absolute_path(*found)) << *found;
        }
        expect(!env::find_executable("mcppls-no-such-program").has_value());
    };

    "an executable is found on another environment's PATH"_test = [] {
        const std::string first { base::join_path(mcppls::platform::dirs::temp_directory(),
            std::format("mcppls-test-env-first-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        const std::string second { first + "-second" };
        const std::string name { std::string { "mcppls-fake-tool" } + std::string { mcppls::os::EXECUTABLE_SUFFIX } };
        (void)mcppls::platform::fs::create_directories(first);
        (void)mcppls::platform::fs::create_directories(second);
        (void)mcppls::platform::fs::write_file(base::join_path(second, name), "");
        const std::string pathList { first + std::string { mcppls::os::PATH_LIST_SEPARATOR } + second };
        const auto found = env::find_executable("mcppls-fake-tool", pathList);
        expect(found.has_value() && base::same_path(*found, base::join_path(second, name))) << found.value_or("<none>");
        expect(!env::find_executable("mcppls-fake-tool").has_value()) << "not on this process's PATH";
        expect(!env::find_executable("mcppls-fake-tool", first).has_value());
        mcppls::platform::fs::remove_all(first);
        mcppls::platform::fs::remove_all(second);
    };

    "arguments include this program"_test = [] {
        const auto arguments = env::arguments();
        expect(fatal(!arguments.empty()));
        expect(arguments.front().find("test_env") != std::string::npos) << arguments.front();
    };

    "well-known directories are absolute"_test = [] {
        expect(base::is_absolute_path(mcppls::platform::dirs::home_directory())) << mcppls::platform::dirs::home_directory();
        expect(base::is_absolute_path(mcppls::platform::dirs::temp_directory())) << mcppls::platform::dirs::temp_directory();
        const std::string cache { mcppls::platform::dirs::cache_directory() };
        expect(base::is_absolute_path(cache)) << cache;
        expect(cache.ends_with("mcppls") || env::get("MCPPLS_CACHE_DIR").has_value()) << cache;
    };

    return report();
}
