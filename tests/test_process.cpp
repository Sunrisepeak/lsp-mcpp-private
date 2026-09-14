// The test program starts copies of itself as the child, so the same test runs
// on every system without depending on a shell or a system utility. The one
// exception is on Windows, where the command interpreter is itself the subject:
// build tools there are often batch files, and those run through it.
import std;
import lspmcpp.os;
import lspmcpp.testing;
import lspmcpp.base.path;
import lspmcpp.base.text;
import lspmcpp.platform.process;
import lspmcpp.platform.env;
import lspmcpp.platform.fs;
import lspmcpp.platform.dirs;
import lspmcpp.platform.stdio;

namespace platform = lspmcpp::platform;
namespace base = lspmcpp::base;

namespace {

std::string self_path() {
    const auto arguments = platform::env::arguments();
    std::string path { arguments.empty() ? std::string {} : arguments.front() };
    if (!base::is_absolute_path(path)) path = base::join_path(platform::fs::current_directory(), path);
    return base::normalize_path(path);
}

int child_main(std::span<const std::string> arguments) {
    const std::string_view mode { arguments[1] };
    if (mode == "--echo") {
        while (true) {
            auto chunk = platform::stdio::read_input();
            if (!chunk || chunk->empty()) break;
            (void)platform::stdio::write_output(*chunk);
        }
        return 0;
    }
    if (mode == "--exit") return std::stoi(arguments[2]);
    if (mode == "--sleep") {
        std::this_thread::sleep_for(std::chrono::seconds { 30 });
        return 0;
    }
    if (mode == "--env") {
        (void)platform::stdio::write_output(platform::env::get(arguments[2]).value_or("<unset>"));
        return 0;
    }
    if (mode == "--read-relative") {
        std::ifstream stream { arguments[2] };
        std::string line;
        std::getline(stream, line);
        (void)platform::stdio::write_output(stream || !line.empty() ? line : std::string { "<missing>" });
        return 0;
    }
    if (mode == "--arguments") {
        std::string joined;
        for (std::size_t i { 2 }; i < arguments.size(); ++i) joined += std::format("[{}]", arguments[i]);
        (void)platform::stdio::write_output(joined);
        return 0;
    }
    if (mode == "--stderr") {
        (void)platform::stdio::write_error("to-stderr");
        return 3;
    }
    return 100;
}

} // namespace

int main() {
    const auto arguments = platform::env::arguments();
    // A child leaves without running the test framework's report at exit.
    if (arguments.size() >= 2 && arguments[1].starts_with("--")) std::_Exit(child_main(arguments));

    using namespace lspmcpp::testing;
    const std::string self { self_path() };

    "self path is an existing absolute file"_test = [&] {
        expect(base::is_absolute_path(self)) << self;
        expect(platform::fs::is_regular_file(self)) << self;
    };

    "echo round trip through pipes"_test = [&] {
        auto process = platform::Process::spawn({ .program = self, .arguments = { "--echo" } });
        expect(fatal(process.has_value())) << (process ? "" : process.error().message);
        std::string collected;
        std::jthread reader { [&] {
            while (true) {
                auto chunk = process->read_output();
                if (!chunk || chunk->empty()) break;
                collected += *chunk;
            }
        } };
        expect(process->write("hello\n").has_value());
        expect(process->write("from the parent\n").has_value());
        process->close_input();
        reader.join();
        auto status = process->wait();
        expect(status.has_value() && *status == 0);
        expect(collected == "hello\nfrom the parent\n") << collected;
    };

    // Each argument arrives exactly as given, whatever the system does to pass a
    // vector through one command line.
    "arguments arrive unaltered"_test = [&] {
        const std::vector<std::string> given { "plain", "two words", "", "C:\\dir\\file.txt", "trailing\\",
                                               "quote\"inside", "back\\\"quote", "\\\\server\\share", "tab\there", "--flag=a b" };
        std::vector<std::string> arguments { "--arguments" };
        arguments.insert(arguments.end(), given.begin(), given.end());
        auto result = platform::run({ .program = self, .arguments = arguments }, std::chrono::seconds { 60 });
        expect(fatal(result.has_value()));
        std::string expected;
        for (const auto& argument : given) expected += std::format("[{}]", argument);
        expect(result->output == expected) << result->output;
    };

    "exit status is propagated"_test = [&] {
        auto result = platform::run({ .program = self, .arguments = { "--exit", "7" } }, std::chrono::seconds { 60 });
        expect(fatal(result.has_value()));
        expect(result->exitCode == 7) << result->exitCode;
        expect(!result->timedOut);
    };

    "standard error is captured separately"_test = [&] {
        auto result = platform::run({ .program = self, .arguments = { "--stderr" } }, std::chrono::seconds { 60 });
        expect(fatal(result.has_value()));
        expect(result->exitCode == 3);
        expect(result->error == "to-stderr") << result->error;
        expect(result->output.empty());
    };

    "a bound on waiting ends a sleeping child"_test = [&] {
        const auto started = std::chrono::steady_clock::now();
        auto result = platform::run({ .program = self, .arguments = { "--sleep" } }, std::chrono::milliseconds { 500 });
        const auto elapsed = std::chrono::steady_clock::now() - started;
        expect(fatal(result.has_value()));
        expect(result->timedOut);
        expect(elapsed < std::chrono::seconds { 20 });
    };

    "environment reaches the child"_test = [&] {
        auto environment = platform::env::variables();
        environment.push_back("LSPMCPP_TEST_VARIABLE=from-parent");
        auto result = platform::run({ .program = self, .arguments = { "--env", "LSPMCPP_TEST_VARIABLE" },
                                      .environment = environment }, std::chrono::seconds { 60 });
        expect(fatal(result.has_value()));
        expect(result->output == "from-parent") << result->output;
    };

    // A value is reported as it was set, backslashes included: a program that
    // starts the command interpreter from %ComSpec% received a forward-slashed
    // name, which the interpreter reads as switches.
    "an environment value arrives unaltered"_test = [&] {
        auto environment = platform::env::variables();
        environment.push_back("LSPMCPP_TEST_PATHS=C:\\dir\\sub;\\\\host\\share\\x;a\"b");
        auto result = platform::run({ .program = self, .arguments = { "--env", "LSPMCPP_TEST_PATHS" },
                                      .environment = environment }, std::chrono::seconds { 60 });
        expect(fatal(result.has_value()));
        expect(result->output == "C:\\dir\\sub;\\\\host\\share\\x;a\"b") << result->output;
    };

    "work directory is where relative names resolve"_test = [&] {
        const std::string directory { base::join_path(platform::dirs::temp_directory(),
            std::format("lsp-mcpp-test-process-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        expect(fatal(platform::fs::create_directories(directory).has_value())) << directory;
        expect(platform::fs::write_file(base::join_path(directory, "marker.txt"), "marker-content\n").has_value());
        auto result = platform::run({ .program = self, .arguments = { "--read-relative", "marker.txt" },
                                      .workDirectory = directory }, std::chrono::seconds { 60 });
        expect(fatal(result.has_value()));
        expect(result->output == "marker-content") << result->output;
        platform::fs::remove_all(directory);
    };

    // The command interpreter refuses a current directory given in the `\\?\` form
    // ("UNC paths are not supported") and runs in the Windows directory instead,
    // so a batch file started in a project ran somewhere else.
    "the command interpreter runs in the work directory"_test = [&] {
        if constexpr (lspmcpp::os::FAMILY != lspmcpp::os::Family::windows) {
            return;
        } else {
            const std::string directory { base::join_path(platform::dirs::temp_directory(),
                std::format("lsp-mcpp-test-cmd-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
            expect(fatal(platform::fs::create_directories(directory).has_value())) << directory;
            expect(platform::fs::write_file(base::join_path(directory, "marker.txt"), "marker-content").has_value());
            const std::string interpreter { base::normalize_path(platform::env::get("ComSpec").value_or("C:/Windows/System32/cmd.exe")) };
            auto result = platform::run({ .program = interpreter, .arguments = { "/d", "/c", "type", "marker.txt" },
                                          .workDirectory = directory }, std::chrono::seconds { 60 });
            expect(fatal(result.has_value()));
            expect(base::trim(result->output) == "marker-content") << result->output << result->error;
            expect(!result->error.contains("UNC")) << result->error;
            platform::fs::remove_all(directory);
        }
    };

    "a relative program path is refused"_test = [] {
        auto process = platform::Process::spawn({ .program = "relative/program" });
        expect(!process.has_value());
    };

    "every absolute path of this program matches a preopen"_test = [&] {
        expect(platform::match_preopen(self).has_value()) << self;
    };

    return report();
}
