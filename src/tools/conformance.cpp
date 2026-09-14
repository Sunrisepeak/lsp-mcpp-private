// lsp-mcpp-conformance: drives a language server through a fixture's scenario
// and reports each check (conformance/README.md).
//
//   lsp-mcpp-conformance run --server <lsp-mcpp> --fixture <dir> [--payload DIR] [--clangd PATH] [--kit DIR]
//                            [--msvc-env FILE] [--timeout SECONDS] [--keep] [--verbose]
//                            [--workspace-dir DIR] [--cache-dir DIR] [--measure FILE] [--expect-warm]
//                            [--navigation-budget SECONDS]
//   lsp-mcpp-conformance version
import std;
import nlohmann.json;
import mcpplibs.cmdline;
import lspmcpp.os;
import lspmcpp.base.error;
import lspmcpp.base.path;
import lspmcpp.base.text;
import lspmcpp.base.uri;
import lspmcpp.base.version;
import lspmcpp.platform.fs;
import lspmcpp.platform.dirs;
import lspmcpp.platform.env;
import lspmcpp.platform.process;
import lspmcpp.platform.task;
import lspmcpp.lsp.jsonrpc;
import lspmcpp.lsp.connection;

namespace base = lspmcpp::base;
namespace fs = lspmcpp::platform::fs;
namespace lsp = lspmcpp::lsp;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

// Lines appear as they happen, also when output is a pipe.
template <class... Args>
void say(std::format_string<Args...> format, Args&&... args) {
    std::cout << std::format(format, std::forward<Args>(args)...) << '\n' << std::flush;
}

struct Options {
    std::string server;
    std::string fixture;
    std::string payload;
    std::string clangd;
    std::string kit;
    std::string msvcEnvironment;   // "NAME=value" lines of a developer environment, for fixtures that build with MSVC
    std::chrono::seconds timeout { 180 };
    bool keep { false };
    bool verbose { false };
    std::string workspaceDirectory;   // reused across runs: the fixture is copied and prepared there once
    bool expectWarm { false };        // module-cache-reused checks require module files from an earlier run
    std::optional<double> navigationBudget;   // seconds from initialize to the first navigation that answers; more fails the run
    std::string cacheDirectory;       // the server's cache; empty: a fresh one beside the workspace
    std::string measureFile;          // where the checks' timings are written as JSON
};

std::string absolute(std::string_view path) {
    if (path.empty() || base::is_absolute_path(path)) return base::normalize_path(path);
    return base::join_path(fs::current_directory(), path);
}

void copy_tree(const std::string& from, const std::string& to) {
    (void)fs::create_directories(to);
    for (const auto& entry : fs::list_directory(from)) {
        const std::string target { base::join_path(to, base::file_name(entry)) };
        if (fs::is_directory(entry)) {
            copy_tree(entry, target);
        } else if (auto content = fs::read_file(entry)) {
            (void)fs::write_file(target, *content);
        }
    }
}

// Placeholders in prepare commands and server arguments.
struct Expansion {
    std::string workspace;
    std::string runnerDirectory;
};

// "{exe}" is the executable suffix; "{env:NAME|fallback}" is a variable or the fallback;
// "{workspace}" is the fixture's scratch copy; "{runner-dir}" is where this program lives.
std::string expand(std::string word, const Expansion& expansion = {}) {
    word = base::replace_all(word, "{exe}", lspmcpp::os::EXECUTABLE_SUFFIX);
    word = base::replace_all(word, "{workspace}", expansion.workspace);
    word = base::replace_all(word, "{runner-dir}", expansion.runnerDirectory);
    for (std::size_t at { word.find("{env:") }; at != std::string::npos; at = word.find("{env:", at)) {
        const std::size_t close { word.find('}', at) };
        if (close == std::string::npos) break;
        const std::string body { word.substr(at + 5, close - at - 5) };
        const std::size_t bar { body.find('|') };
        const std::string name { body.substr(0, bar) };
        std::string value { lspmcpp::platform::env::get(name).value_or("") };
        if (value.empty() && bar != std::string::npos) value = body.substr(bar + 1);
        word.replace(at, close - at + 1, value);
        at += value.size();
    }
    return word;
}

// The environment a prepare step runs in: this process's, with a developer environment laid over it when given.
std::optional<std::vector<std::string>> prepare_environment(const std::string& overlayFile) {
    if (overlayFile.empty()) return std::nullopt;
    auto text = fs::read_file(overlayFile);
    if (!text) return std::nullopt;
    const bool caseInsensitive { lspmcpp::os::FAMILY == lspmcpp::os::Family::windows };
    auto key = [&](std::string_view entry) {
        std::string name { entry.substr(0, entry.find('=')) };
        return caseInsensitive ? base::to_lower_ascii(name) : name;
    };
    std::vector<std::string> environment { lspmcpp::platform::env::variables() };
    for (auto line : base::split_lines(*text)) {
        line = base::trim(line);
        if (line.empty() || line.find('=') == std::string_view::npos || line.front() == '=') continue;
        std::erase_if(environment, [&](const std::string& entry) { return key(entry) == key(line); });
        environment.emplace_back(line);
    }
    return environment;
}

// Runs a prepare step in the workspace.
bool run_prepare(const Json& command, const std::string& workspace, bool verbose, const Expansion& expansion,
                 const std::optional<std::vector<std::string>>& environment) {
    if (!command.is_array() || command.empty()) return true;
    std::vector<std::string> argv;
    for (const auto& word : command) argv.push_back(expand(word.get<std::string>(), expansion));
    std::string program { argv.front() };
    if (!base::is_absolute_path(program)) {
        auto found = lspmcpp::platform::env::find_executable(program);
        if (!found) {
            say("prepare: {} is not on PATH", program);
            return false;
        }
        program = *found;
    }
    lspmcpp::platform::SpawnOptions options;
    options.program = program;
    options.arguments.assign(argv.begin() + 1, argv.end());
    options.workDirectory = workspace;
    options.environment = environment;
    auto result = lspmcpp::platform::run(std::move(options), std::chrono::minutes { 20 });
    if (!result || result->exitCode != 0 || result->timedOut) {
        say("prepare failed: {}", lsp::dump(command));
        if (result) say("{}\n{}", result->output, result->error);
        return false;
    }
    if (verbose) say("prepare: {}\n{}", lsp::dump(command), result->output);
    return true;
}

// A file's size and FNV-1a digest, read a block at a time: a build tree holds files far larger than a check needs to keep.
std::optional<std::string> digest(const std::string& path) {
    std::ifstream stream { std::filesystem::path { path }, std::ios::binary };
    if (!stream) return std::nullopt;
    std::uint64_t hash { 1469598103934665603ull };
    std::uint64_t size { 0 };
    std::vector<char> block(std::size_t { 1 } << 16);
    while (stream.read(block.data(), static_cast<std::streamsize>(block.size())) || stream.gcount() > 0) {
        const auto count = static_cast<std::size_t>(stream.gcount());
        for (std::size_t i { 0 }; i < count; ++i) {
            hash ^= static_cast<unsigned char>(block[i]);
            hash *= 1099511628211ull;
        }
        size += count;
    }
    return std::format("{}:{:016x}", size, hash);
}

// Every file under a directory with its digest, dot directories included: the server must not write any of them.
std::map<std::string, std::string> snapshot(const std::string& root) {
    std::map<std::string, std::string> files;
    std::vector<std::string> pending { root };
    while (!pending.empty()) {
        const std::string directory { pending.back() };
        pending.pop_back();
        for (const auto& entry : fs::list_directory(directory)) {
            if (fs::is_directory(entry)) {
                pending.push_back(entry);
            } else if (auto relative = base::relative_path(entry, root)) {
                if (auto content = digest(entry)) files[*relative] = std::move(*content);
            }
        }
    }
    return files;
}

// The engine's published module files for a module under a cache directory, with their stamps. clangd
// publishes <module>.pcm (a partition as <module>-<partition>.pcm) under a directory per source and
// command; the copies it hands to readers carry a timestamp in their names and are not included.
std::map<std::string, std::string> module_files(const std::string& cacheDirectory, std::string_view module) {
    std::string published { module };
    std::ranges::replace(published, ':', '-');
    published += ".pcm";
    std::map<std::string, std::string> files;
    if (cacheDirectory.empty()) return files;
    std::vector<std::string> pending { cacheDirectory };
    while (!pending.empty()) {
        const std::string directory { pending.back() };
        pending.pop_back();
        for (const auto& entry : fs::list_directory(directory)) {
            if (fs::is_directory(entry)) {
                pending.push_back(entry);
            } else if (base::file_name(entry) == published) {
                const auto stamp = fs::stamp(entry);
                files[entry] = stamp ? std::format("{}:{}", stamp->size, stamp->modified) : std::string {};
            }
        }
    }
    return files;
}

class Client {
private:
    std::unique_ptr<lsp::Connection> connection_;
    std::shared_ptr<lspmcpp::platform::Channel<Json>> inbox_ { std::make_shared<lspmcpp::platform::Channel<Json>>() };
    std::int64_t nextId_ { 1 };
    int unanswered_ { 0 }; // consecutive requests that reached their deadline
    bool verbose_ { false };

public:
    std::map<std::string, Json> diagnostics;     // uri -> latest diagnostics
    std::map<std::string, int> diagnosticsCount; // uri -> publishes received
    Json status;
    std::vector<std::string> statusHistory;
    std::optional<Clock::time_point> firstReady;         // the first status in state ready
    std::optional<Clock::time_point> firstDiagnostics;   // the first diagnostics published for any file

    base::Result<void> start(const Options& options, const std::vector<std::string>& serverArguments, const std::string& workspace,
                             const std::string& cacheDirectory) {
        verbose_ = options.verbose;
        lspmcpp::platform::SpawnOptions spawn;
        spawn.program = options.server;
        spawn.arguments = { "serve" };
        if (!options.payload.empty()) spawn.arguments.insert(spawn.arguments.end(), { "--payload", options.payload });
        if (!options.clangd.empty()) spawn.arguments.insert(spawn.arguments.end(), { "--clangd", options.clangd });
        if (!options.kit.empty()) spawn.arguments.insert(spawn.arguments.end(), { "--kit", options.kit });
        spawn.arguments.insert(spawn.arguments.end(), serverArguments.begin(), serverArguments.end());
        spawn.workDirectory = workspace;
        auto environment = lspmcpp::platform::env::variables();
        environment.push_back("LSP_MCPP_CACHE_DIR=" + cacheDirectory);
        spawn.environment = std::move(environment);
        const bool verbose { verbose_ };
        auto inbox = inbox_;
        auto connection = lsp::Connection::start(
            std::move(spawn), [inbox, verbose](Json message) {
                if (verbose) std::cerr << "  <<< " << lsp::dump(message).substr(0, 120) << std::endl;
                inbox->push(std::move(message));
            }, [inbox] { inbox->close(); },
            [verbose](std::string_view line) {
                if (verbose) say("  server: {}", line);
            });
        if (!connection) return std::unexpected { connection.error() };
        connection_ = std::move(*connection);
        return {};
    }

    void notify(std::string_view method, Json params) { (void)connection_->send(lsp::make_notification(method, std::move(params))); }

    std::optional<Json> request(std::string_view method, Json params, std::chrono::seconds timeout) {
        const std::int64_t id { nextId_++ };
        (void)connection_->send(lsp::make_request(id, method, std::move(params)));
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            auto message = inbox_->pop_until(deadline);
            if (!message) break;
            if (lsp::kind_of(*message) == lsp::Kind::response && (*message)["id"] == Json(id)) {
                unanswered_ = 0;
                if (message->contains("error")) {
                    if (verbose_) say("  error response to {}: {}", method, lsp::dump((*message)["error"]));
                    return Json(nullptr);
                }
                return message->value("result", Json {});
            }
            dispatch(*message);
        }
        if (!inbox_->closed()) ++unanswered_;
        return std::nullopt;
    }

    // Why the remaining checks cannot run, or empty while the server is usable:
    // a server that exited, or one that let two requests in a row reach their
    // deadline, would only make every later check wait out its own.
    std::string unusable() {
        if (inbox_->closed() && inbox_->size() == 0) {
            const auto code = connection_->exit_code();
            return code ? std::format("the server exited with status {}", *code) : std::string { "the server closed its output" };
        }
        if (unanswered_ >= 2) return std::format("the server answered none of the last {} requests", unanswered_);
        return {};
    }

    // Processes incoming messages until `done` holds or the deadline passes.
    bool wait_for(const std::function<bool()>& done, std::chrono::seconds timeout) {
        const auto deadline = Clock::now() + timeout;
        while (!done()) {
            if (Clock::now() >= deadline) return false;
            auto message = inbox_->pop_until(std::min(deadline, Clock::now() + std::chrono::milliseconds { 200 }));
            if (message) dispatch(*message);
            else if (inbox_->closed() && inbox_->size() == 0) return done();
        }
        return true;
    }

    void drain(std::chrono::milliseconds quiet) {
        while (auto message = inbox_->pop_until(Clock::now() + quiet)) dispatch(*message);
    }

    void dispatch(const Json& message) {
        switch (lsp::kind_of(message)) {
        case lsp::Kind::request: {
            const std::string method { message.value("method", std::string {}) };
            Json result = nullptr;
            if (method == "workspace/configuration") {
                result = Json::array();
                for (std::size_t i { 0 }; i < message["params"].value("items", Json::array()).size(); ++i) result.push_back(nullptr);
            }
            (void)connection_->send(lsp::make_result(message["id"], std::move(result)));
            break;
        }
        case lsp::Kind::notification: {
            const std::string method { message.value("method", std::string {}) };
            if (method == "textDocument/publishDiagnostics") {
                const std::string uri { message["params"].value("uri", std::string {}) };
                diagnostics[uri] = message["params"].value("diagnostics", Json::array());
                ++diagnosticsCount[uri];
                if (!firstDiagnostics) firstDiagnostics = Clock::now();
            } else if (method == "cxxModules/status") {
                status = message["params"];
                statusHistory.push_back(status.value("state", std::string {}));
                if (!firstReady && statusHistory.back() == "ready") firstReady = Clock::now();
                if (verbose_) say("  status: {}", lsp::dump(status));
            } else if (verbose_ && method == "window/logMessage") {
                say("  log: {}", message["params"].value("message", std::string {}));
            }
            break;
        }
        default: break;
        }
    }

    void stop() {
        if (!connection_) return;
        (void)request("shutdown", nullptr, std::chrono::seconds { 10 });
        notify("exit", nullptr);
        connection_->stop(std::chrono::seconds { 5 });
    }
};

std::string state_of(const Json& status) { return status.is_object() ? status.value("state", std::string {}) : std::string {}; }

std::vector<std::string> location_uris(const Json& result) {
    std::vector<std::string> uris;
    auto add = [&](const Json& location) {
        if (!location.is_object()) return;
        if (location.contains("targetUri")) uris.push_back(location.value("targetUri", std::string {}));
        else if (location.contains("uri")) uris.push_back(location.value("uri", std::string {}));
    };
    if (result.is_array()) {
        for (const auto& location : result) add(location);
    } else {
        add(result);
    }
    return uris;
}

std::string hover_text(const Json& result) {
    if (!result.is_object()) return {};
    const Json contents = result.value("contents", Json {});
    if (contents.is_string()) return contents.get<std::string>();
    if (contents.is_object()) return contents.value("value", std::string {});
    std::string text;
    if (contents.is_array()) {
        for (const auto& item : contents) text += item.is_string() ? item.get<std::string>() : item.value("value", std::string {});
    }
    return text;
}

std::vector<std::string> completion_labels(const Json& result) {
    std::vector<std::string> labels;
    const Json items = result.is_object() ? result.value("items", Json::array()) : result;
    if (!items.is_array()) return labels;
    for (const auto& item : items) labels.push_back(std::string { base::trim(item.value("label", std::string {})) });
    return labels;
}

bool ends_with_path(std::string_view uri, std::string_view suffix) {
    auto path = base::uri_to_path(uri);
    if (!path) return false;
    return base::path_key(*path).ends_with(base::path_key(base::normalize_path(suffix)));
}

Json position(const Json& at) { return Json { { "line", at.at(0) }, { "character", at.at(1) } }; }

class Scenario {
private:
    Client& client_;
    std::string workspace_;
    std::map<std::string, std::pair<std::string, int>> open_;   // relative path -> (text, version)
    std::chrono::seconds timeout_;
    std::map<std::string, std::string> prepared_;               // the workspace as the prepare steps left it
    std::string cacheDirectory_;                                // the server's cache
    bool expectWarm_ { false };
    std::map<std::string, std::map<std::string, std::string>> moduleFilesBefore_;   // module -> its published files before the server started

public:
    Scenario(Client& client, std::string workspace, std::chrono::seconds timeout, std::map<std::string, std::string> prepared,
             std::string cacheDirectory, bool expectWarm, std::map<std::string, std::map<std::string, std::string>> moduleFilesBefore)
        : client_ { client }, workspace_ { std::move(workspace) }, timeout_ { timeout }, prepared_ { std::move(prepared) },
          cacheDirectory_ { std::move(cacheDirectory) }, expectWarm_ { expectWarm }, moduleFilesBefore_ { std::move(moduleFilesBefore) } {}

    std::string uri(std::string_view relative) const { return base::path_to_uri(base::join_path(workspace_, relative)); }

    std::string text_of(std::string_view relative) {
        if (auto it = open_.find(std::string { relative }); it != open_.end()) return it->second.first;
        return fs::read_file(base::join_path(workspace_, relative)).value_or("");
    }

    void open(std::string_view relative, std::optional<std::string> text = {}) {
        const std::string content { text ? *text : text_of(relative) };
        if (open_.contains(std::string { relative })) {
            change(relative, content);
            return;
        }
        open_[std::string { relative }] = { content, 1 };
        client_.notify("textDocument/didOpen", Json { { "textDocument", Json { { "uri", uri(relative) }, { "languageId", "cpp" }, { "version", 1 }, { "text", content } } } });
    }

    void change(std::string_view relative, const std::string& text) {
        auto& [current, version] = open_[std::string { relative }];
        current = text;
        ++version;
        client_.notify("textDocument/didChange", Json { { "textDocument", Json { { "uri", uri(relative) }, { "version", version } } },
                                                        { "contentChanges", Json::array({ Json { { "text", text } } }) } });
    }

    // Repeats a request until `accept` holds, because the engine may still be preparing modules.
    std::pair<bool, Json> retry(std::string_view method, const std::function<Json()>& params, const std::function<bool(const Json&)>& accept) {
        const auto deadline = Clock::now() + timeout_;
        Json last;
        while (Clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(deadline - Clock::now());
            auto result = client_.request(method, params(), std::max(std::chrono::seconds { 1 }, remaining));
            if (result) {
                last = *result;
                if (accept(*result)) return { true, last };
            }
            client_.drain(std::chrono::milliseconds { 500 });
        }
        return { false, last };
    }

    std::pair<bool, std::string> run(const Json& check) {
        const std::string kind { check.value("kind", std::string {}) };
        const std::string file { check.value("file", std::string { "src/main.cpp" }) };
        // A check may bring its own unsaved buffer.
        if (auto text = check.find("text"); text != check.end()) open(file, text->get<std::string>());
        if (kind == "status") {
            const bool ok { client_.wait_for([&] {
                const std::string state { state_of(client_.status) };
                return state == "ready" || state == "degraded";
            }, timeout_) };
            std::string detail { lsp::dump(client_.status) };
            bool matches { ok };
            if (auto source = check.find("source"); source != check.end()) matches = matches && client_.status["project"].value("source", std::string {}) == source->get<std::string>();
            if (auto profile = check.find("profile-kind"); profile != check.end()) matches = matches && client_.status["profile"].value("kind", std::string {}) == profile->get<std::string>();
            if (auto state = check.find("state"); state != check.end()) matches = matches && state_of(client_.status) == state->get<std::string>();
            if (auto level = check.find("level"); level != check.end()) matches = matches && client_.status["project"].value("level", 0) == level->get<int>();
            if (auto compiler = check.find("profile-compiler"); compiler != check.end()) {
                matches = matches && client_.status["profile"].value("compiler", std::string {}).starts_with(compiler->get<std::string>());
            }
            return { matches, detail };
        }
        if (kind == "module-cache-reused") {
            // SC4: a warm start builds no module the previous run left in the cache. Every published
            // file of the module is still there unchanged, and none was added under another command.
            const std::string module { check.value("module", std::string { "std" }) };
            const auto before = moduleFilesBefore_.find(module);
            if (before == moduleFilesBefore_.end() || before->second.empty()) {
                return { !expectWarm_, std::format("no module file of {} before the server started: a cold start", module) };
            }
            const auto now = module_files(cacheDirectory_, module);
            std::vector<std::string> differences;
            for (const auto& [path, stamp] : before->second) {
                const auto it = now.find(path);
                if (it == now.end()) differences.push_back("removed " + path);
                else if (it->second != stamp) differences.push_back("rebuilt " + path);
            }
            for (const auto& [path, stamp] : now) {
                if (!before->second.contains(path)) differences.push_back("added " + path);
            }
            return { differences.empty(), differences.empty() ? std::format("{} file(s) of {} reused", now.size(), module) : lsp::dump(differences) };
        }
        if (kind == "workspace-unchanged") {
            // Give the server time to do what it does after opening the workspace.
            client_.drain(std::chrono::milliseconds { 2000 });
            const auto now = snapshot(workspace_);
            std::vector<std::string> differences;
            for (const auto& [path, content] : now) {
                const auto before = prepared_.find(path);
                if (before == prepared_.end()) differences.push_back("added " + path);
                else if (before->second != content) differences.push_back("changed " + path);
            }
            for (const auto& [path, content] : prepared_) {
                if (!now.contains(path)) differences.push_back("removed " + path);
            }
            if (differences.size() > 8) differences.resize(8);
            return { differences.empty(), lsp::dump(differences) };
        }
        if (kind == "open") {
            open(file);
            return { true, file };
        }
        if (kind == "diagnostics-empty") {
            open(file);
            const std::string documentUri { uri(file) };
            const bool published { client_.wait_for([&] {
                return client_.diagnosticsCount[documentUri] > 0 && state_of(client_.status) != "preparing" && state_of(client_.status) != "loading";
            }, timeout_) };
            client_.drain(std::chrono::milliseconds { 1500 });
            Json errors = Json::array();
            for (const auto& diagnostic : client_.diagnostics[documentUri]) {
                if (diagnostic.value("severity", 1) == 1) errors.push_back(diagnostic.value("message", std::string {}));
            }
            return { published && errors.empty(), published ? lsp::dump(errors) : std::string { "no diagnostics were published" } };
        }
        if (kind == "diagnostic-code") {
            open(file);
            const std::string documentUri { uri(file) };
            const std::string code { check.value("expect", std::string {}) };
            const bool found { client_.wait_for([&] {
                for (const auto& diagnostic : client_.diagnostics[documentUri]) {
                    if (diagnostic.value("code", Json {}) == Json(code)) return true;
                }
                return false;
            }, timeout_) };
            return { found, lsp::dump(client_.diagnostics[documentUri]) };
        }
        if (kind == "definition" || kind == "definition-any" || kind == "declaration") {
            open(file);
            const std::string expected { check.value("expect", std::string {}) };
            auto [ok, result] = retry(kind == "declaration" ? "textDocument/declaration" : "textDocument/definition",
                [&] { return Json { { "textDocument", Json { { "uri", uri(file) } } }, { "position", position(check.at("at")) } }; },
                [&](const Json& value) {
                    const auto uris = location_uris(value);
                    if (kind == "definition-any") return !uris.empty();
                    return std::ranges::any_of(uris, [&](const std::string& found) { return ends_with_path(found, expected); });
                });
            return { ok, lsp::dump(location_uris(result)) };
        }
        if (kind == "hover-contains") {
            open(file);
            const std::string expected { check.value("expect", std::string {}) };
            auto [ok, result] = retry("textDocument/hover",
                [&] { return Json { { "textDocument", Json { { "uri", uri(file) } } }, { "position", position(check.at("at")) } }; },
                [&](const Json& value) { return hover_text(value).find(expected) != std::string::npos; });
            std::string text { hover_text(result) };
            return { ok, text.substr(0, std::min<std::size_t>(text.size(), 160)) };
        }
        if (kind == "completion-contains") {
            open(file);
            if (auto insert = check.find("insert"); insert != check.end()) {
                // [line, text]: the text is inserted as a new line before `line`.
                auto lines = base::split_lines(text_of(file));
                std::vector<std::string> copy { lines.begin(), lines.end() };
                const std::size_t at { std::min<std::size_t>(insert->at(0).get<std::size_t>(), copy.size()) };
                copy.insert(copy.begin() + static_cast<std::ptrdiff_t>(at), insert->at(1).get<std::string>());
                change(file, base::join(copy, "\n") + "\n");
            }
            if (auto edit = check.find("edit"); edit != check.end()) {
                // {"file": "...", "replace": "...", "with": "..."}: an unsaved edit in another open buffer.
                const std::string other { edit->value("file", std::string {}) };
                std::string content { text_of(other) };
                content = base::replace_all(content, edit->value("replace", std::string {}), edit->value("with", std::string {}));
                open(other, content);
                client_.drain(std::chrono::milliseconds { 1000 });
                // Touch the importing buffer so it is rebuilt against the edited module.
                change(file, text_of(file) + " ");
            }
            const std::string expected { check.value("expect", std::string {}) };
            auto [ok, result] = retry("textDocument/completion",
                [&] { return Json { { "textDocument", Json { { "uri", uri(file) } } }, { "position", position(check.at("at")) } }; },
                [&](const Json& value) {
                    const auto labels = completion_labels(value);
                    return std::ranges::any_of(labels, [&](const std::string& label) { return label.starts_with(expected); });
                });
            auto labels = completion_labels(result);
            if (labels.size() > 12) labels.resize(12);
            return { ok, lsp::dump(labels) };
        }
        if (kind == "references-span") {
            open(file);
            std::vector<std::string> expected;
            for (const auto& item : check.value("expect", Json::array())) expected.push_back(item.get<std::string>());
            auto [ok, result] = retry("textDocument/references",
                [&] { return Json { { "textDocument", Json { { "uri", uri(file) } } }, { "position", position(check.at("at")) },
                                    { "context", Json { { "includeDeclaration", true } } } }; },
                [&](const Json& value) {
                    const auto uris = location_uris(value);
                    return std::ranges::all_of(expected, [&](const std::string& path) {
                        return std::ranges::any_of(uris, [&](const std::string& found) { return ends_with_path(found, path); });
                    });
                });
            std::set<std::string> files;
            for (const auto& found : location_uris(result)) files.insert(std::string { base::file_name(found) });
            return { ok, lsp::dump(files) };
        }
        if (kind == "document-symbol-contains") {
            open(file);
            const std::string expected { check.value("expect", std::string {}) };
            auto [ok, result] = retry("textDocument/documentSymbol", [&] { return Json { { "textDocument", Json { { "uri", uri(file) } } } }; },
                [&](const Json& value) {
                    if (!value.is_array()) return false;
                    return std::ranges::any_of(value, [&](const Json& symbol) { return symbol.value("name", std::string {}) == expected; });
                });
            return { ok, lsp::dump(result).substr(0, 160) };
        }
        if (kind == "module-graph-contains") {
            const std::string expected { check.value("expect", std::string {}) };
            auto result = client_.request("cxxModules/graph", Json::object(), timeout_);
            bool ok { false };
            if (result && result->is_object()) {
                for (const auto& module : result->value("modules", Json::array())) ok = ok || module.value("name", std::string {}) == expected;
            }
            return { ok, result ? lsp::dump(*result).substr(0, 160) : std::string { "no response" } };
        }
        return { false, std::format("unknown check kind {}", kind) };
    }
};

int run(const Options& options) {
    const std::string scenarioPath { base::join_path(options.fixture, "scenario.json") };
    auto scenarioText = fs::read_file(scenarioPath);
    if (!scenarioText) {
        say("conformance: {} not found", scenarioPath);
        return 2;
    }
    Json scenario = Json::parse(*scenarioText, nullptr, false);
    if (scenario.is_discarded()) {
        say("conformance: {} is not valid JSON", scenarioPath);
        return 2;
    }
    const std::string name { scenario.value("name", std::string { base::file_name(options.fixture) }) };
    const bool reused { !options.workspaceDirectory.empty() };
    const std::string scratch { reused ? options.workspaceDirectory
                                       : base::join_path(lspmcpp::platform::dirs::temp_directory(),
                                             std::format("lsp-mcpp-conformance-{}-{}", name, Clock::now().time_since_epoch().count())) };
    const std::string workspace { base::join_path(scratch, name) };
    // A reused workspace is prepared once; the marker sits beside it, outside what the server sees.
    const std::string preparedMarker { base::join_path(scratch, name + ".prepared") };
    const bool alreadyPrepared { reused && fs::exists(preparedMarker) };
    if (!alreadyPrepared) {
        fs::remove_all(workspace);
        copy_tree(options.fixture, workspace);
        fs::remove_all(base::join_path(workspace, "scenario.json"));
    }
    say("fixture {} in {}{}", name, workspace, alreadyPrepared ? " (prepared before)" : "");

    Expansion expansion { workspace, base::parent_path(absolute(lspmcpp::platform::env::arguments().front())) };
    std::optional<std::vector<std::string>> prepareEnvironment;
    if (scenario.value("prepare-environment", std::string {}) == "msvc") {
        if (options.msvcEnvironment.empty()) {
            say("conformance: {} builds with MSVC; pass --msvc-env with a developer environment", name);
            return 2;
        }
        prepareEnvironment = prepare_environment(options.msvcEnvironment);
    }
    if (!alreadyPrepared) {
        for (const auto& command : scenario.value("prepare", Json::array())) {
            if (!run_prepare(command, workspace, options.verbose, expansion, prepareEnvironment)) return 1;
        }
        for (const auto& removed : scenario.value("remove", Json::array())) fs::remove_all(base::join_path(workspace, removed.get<std::string>()));
        if (reused) (void)fs::write_file(preparedMarker, "");
    }

    std::vector<std::string> serverArguments;
    for (const auto& argument : scenario.value("server-arguments", Json::array())) serverArguments.push_back(expand(argument.get<std::string>(), expansion));
    auto prepared = snapshot(workspace);
    Client client;
    const std::string cacheDirectory { options.cacheDirectory.empty() ? base::join_path(scratch, "cache") : options.cacheDirectory };
    std::map<std::string, std::map<std::string, std::string>> moduleFilesBefore;
    for (const auto& check : scenario.value("checks", Json::array())) {
        if (check.value("kind", std::string {}) != "module-cache-reused") continue;
        const std::string module { check.value("module", std::string { "std" }) };
        moduleFilesBefore[module] = module_files(cacheDirectory, module);
    }
    if (auto started = client.start(options, serverArguments, workspace, cacheDirectory); !started) {
        say("conformance: cannot start the server: {}", started.error().message);
        return 2;
    }
    const auto begin = Clock::now();
    Json capabilities {
        { "textDocument", Json { { "hover", Json { { "contentFormat", Json::array({ "markdown", "plaintext" }) } } },
                                 { "documentSymbol", Json { { "hierarchicalDocumentSymbolSupport", true } } },
                                 { "completion", Json { { "completionItem", Json { { "snippetSupport", false } } } } },
                                 { "publishDiagnostics", Json { { "relatedInformation", true } } } } },
        { "workspace", Json { { "didChangeWatchedFiles", Json { { "dynamicRegistration", true } } }, { "configuration", true } } },
        { "experimental", Json { { "cxxModules", Json { { "version", 1 }, { "status", true }, { "graph", true }, { "contexts", true } } } } },
    };
    auto initialized = client.request("initialize", Json { { "processId", nullptr }, { "rootUri", base::path_to_uri(workspace) },
        { "workspaceFolders", Json::array({ Json { { "uri", base::path_to_uri(workspace) }, { "name", name } } }) },
        { "capabilities", capabilities } }, std::chrono::seconds { 120 });
    if (!initialized || !initialized->is_object()) {
        say("FAIL initialize: no result");
        return 1;
    }
    const bool advertised { initialized->contains("capabilities") && (*initialized)["capabilities"].contains("experimental")
                            && (*initialized)["capabilities"]["experimental"].contains("cxxModules") };
    const double initializeSeconds { std::chrono::duration<double>(Clock::now() - begin).count() };
    say("{} initialize ({:.1f}s) experimental.cxxModules={}", advertised ? "PASS" : "FAIL", initializeSeconds, advertised);
    client.notify("initialized", Json::object());

    Scenario runner { client, workspace, options.timeout, std::move(prepared), cacheDirectory, options.expectWarm, std::move(moduleFilesBefore) };
    int failures { advertised ? 0 : 1 };
    Json measured = Json::array();
    for (const auto& check : scenario.value("checks", Json::array())) {
        const std::string id { check.value("id", std::string { "-" }) };
        const bool optional { check.value("optional", false) };
        if (const auto reason = client.unusable(); !reason.empty()) {
            if (!optional) ++failures;
            say("{} {} {} (not run) {}", optional ? "SKIP" : "FAIL", id, check.value("kind", std::string {}), reason);
            continue;
        }
        const auto started = Clock::now();
        auto [ok, detail] = runner.run(check);
        if (!ok && !optional) ++failures;
        const double seconds { std::chrono::duration<double>(Clock::now() - started).count() };
        say("{} {} {} ({:.1f}s) {}", ok ? "PASS" : (optional ? "SKIP" : "FAIL"), id, check.value("kind", std::string {}), seconds, detail);
        measured.push_back(Json { { "id", id }, { "kind", check.value("kind", std::string {}) }, { "ok", ok }, { "seconds", seconds },
                                  { "since-start", std::chrono::duration<double>(Clock::now() - begin).count() } });
    }
    client.stop();
    Json firstNavigation = nullptr;
    for (const auto& check : measured) {
        const std::string kind { check.value("kind", std::string {}) };
        if ((kind == "definition" || kind == "declaration" || kind == "definition-any") && check.value("ok", false)) {
            firstNavigation = check["since-start"];
            break;
        }
    }
    if (options.navigationBudget) {
        const bool within { firstNavigation.is_number() && firstNavigation.get<double>() <= *options.navigationBudget };
        if (!within) ++failures;
        say("{} navigation-budget first navigation {} within {:.1f}s", within ? "PASS" : "FAIL",
            firstNavigation.is_number() ? std::format("{:.2f}s", firstNavigation.get<double>()) : std::string { "never answered" }, *options.navigationBudget);
    }
    const double total { std::chrono::duration<double>(Clock::now() - begin).count() };
    say("{}: {} failure(s), {:.1f}s", name, failures, total);
    if (!options.measureFile.empty()) {
        // The timeline of usable plan W7: initialize, the first ready state, the first diagnostics, the first navigation.
        auto since = [&](const std::optional<Clock::time_point>& at) -> Json {
            return at ? Json(std::chrono::duration<double>(*at - begin).count()) : Json(nullptr);
        };
        Json summary { { "fixture", name }, { "failures", failures }, { "seconds", total }, { "reused-workspace", alreadyPrepared } };
        summary["initialize"] = initializeSeconds;
        summary["ready"] = since(client.firstReady);
        summary["first-diagnostics"] = since(client.firstDiagnostics);
        summary["first-navigation"] = firstNavigation;
        summary["checks"] = measured;
        if (auto written = fs::write_file(options.measureFile, summary.dump(2) + "\n"); !written) say("conformance: cannot write {}", options.measureFile);
    }
    if (!options.keep && !reused) fs::remove_all(scratch);
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[]) {
    using namespace mcpplibs;
    int status { 0 };
    cmdline::App app { "lsp-mcpp-conformance" };
    (void)app.version(std::string { base::VERSION });
    (void)app.description("Run a conformance fixture against a language server");

    cmdline::App runCommand { "run" };
    (void)runCommand.description("Run one fixture");
    (void)runCommand.option("server").takes_value().help("The lsp-mcpp executable");
    (void)runCommand.option("fixture").takes_value().help("Fixture directory with scenario.json");
    (void)runCommand.option("payload").takes_value().help("Payload directory");
    (void)runCommand.option("clangd").takes_value().help("clangd executable");
    (void)runCommand.option("kit").takes_value().help("Semantic kit directory");
    (void)runCommand.option("timeout").takes_value().help("Seconds each check may take (default 180)");
    (void)runCommand.option("msvc-env").takes_value().help("File of NAME=value lines: the developer environment for fixtures that build with MSVC");
    (void)runCommand.option("keep").help("Keep the scratch workspace");
    (void)runCommand.option("verbose").help("Print server logs and status notifications");
    (void)runCommand.option("workspace-dir").takes_value().help("Directory reused across runs: the fixture is copied and prepared there once");
    (void)runCommand.option("cache-dir").takes_value().help("The server's cache directory, e.g. shared by a cold and a warm run");
    (void)runCommand.option("measure").takes_value().help("File the checks' timings are written to, as JSON");
    (void)runCommand.option("expect-warm").help("module-cache-reused checks fail unless an earlier run left module files in --cache-dir");
    (void)runCommand.option("navigation-budget").takes_value().help("Seconds the first navigation may take from initialize; more fails the run");
    (void)runCommand.action([&](const cmdline::ParsedArgs& args) {
        Options options;
        options.server = absolute(args.value("server").value_or(""));
        options.fixture = absolute(args.value("fixture").value_or(""));
        options.payload = args.value("payload") ? absolute(*args.value("payload")) : std::string {};
        options.clangd = args.value("clangd") ? absolute(*args.value("clangd")) : std::string {};
        options.kit = args.value("kit") ? absolute(*args.value("kit")) : std::string {};
        options.msvcEnvironment = args.value("msvc-env") ? absolute(*args.value("msvc-env")) : std::string {};
        if (auto timeout = args.value("timeout")) options.timeout = std::chrono::seconds { std::stoi(*timeout) };
        options.keep = args.is_flag_set("keep");
        options.verbose = args.is_flag_set("verbose");
        options.workspaceDirectory = args.value("workspace-dir") ? absolute(*args.value("workspace-dir")) : std::string {};
        options.cacheDirectory = args.value("cache-dir") ? absolute(*args.value("cache-dir")) : std::string {};
        options.measureFile = args.value("measure") ? absolute(*args.value("measure")) : std::string {};
        options.expectWarm = args.is_flag_set("expect-warm");
        if (auto budget = args.value("navigation-budget")) {
            try {
                options.navigationBudget = std::stod(*budget);
            } catch (...) {
                say("run: --navigation-budget takes seconds");
                status = 2;
                return;
            }
        }
        if (options.server.empty() || options.fixture.empty()) {
            say("run: --server and --fixture are required");
            status = 2;
            return;
        }
        status = run(options);
    });
    (void)app.subcommand(std::move(runCommand));

    cmdline::App versionCommand { "version" };
    (void)versionCommand.description("Print the version");
    (void)versionCommand.action([](const cmdline::ParsedArgs&) { say("lsp-mcpp-conformance {}", base::VERSION); });
    (void)app.subcommand(std::move(versionCommand));

    const int parsed { app.run(argc, argv) };
    return parsed != 0 ? parsed : status;
}
