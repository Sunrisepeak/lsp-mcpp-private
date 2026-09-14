// Document store, module index and routing, without processes.
import std;
import nlohmann.json;
import lspmcpp.testing;
import lspmcpp.base.error;
import lspmcpp.base.path;
import lspmcpp.base.sha256;
import lspmcpp.base.text;
import lspmcpp.base.uri;
import lspmcpp.platform.fs;
import lspmcpp.platform.dirs;
import lspmcpp.index.modules;
import lspmcpp.server.documents;
import lspmcpp.server.router;
import lspmcpp.server.payload;
import lspmcpp.server.workspace;
import lspmcpp.engine;
import lspmcpp.engine.clangd;
import lspmcpp.server.primer;

using Json = nlohmann::json;
using lspmcpp::base::Position;
namespace idx = lspmcpp::index;
namespace srv = lspmcpp::server;
namespace eng = lspmcpp::engine;

namespace {

// usable plan W9.5: a fake that satisfies the lspmcpp.engine interface without a clangd process,
// recording what the session (or anything else coded only against the interface) does to it.
class FakeEngine : public eng::Engine {
public:
    int starts { 0 };
    bool running_ { false };
    eng::EngineConfig lastConfig;
    std::vector<std::string> pushedDatabases;
    std::vector<Json> sent;
    eng::EngineCapabilities capabilitiesToReport;
    MessageHandler onMessage;
    ClosedHandler onClosed;

    lspmcpp::base::Result<void> start(const eng::EngineConfig& config, MessageHandler message, ClosedHandler closed, LogHandler) override {
        ++starts;
        running_ = true;
        lastConfig = config;
        onMessage = std::move(message);
        onClosed = std::move(closed);
        return {};
    }
    lspmcpp::base::Result<void> push_database(std::string_view compileCommandsJson) override {
        pushedDatabases.emplace_back(compileCommandsJson);
        return {};
    }
    lspmcpp::base::Result<void> send(const Json& message) override {
        sent.push_back(message);
        return {};
    }
    void stop(std::chrono::milliseconds) override { running_ = false; }
    bool running() const override { return running_; }
    const eng::EngineConfig& config() const override { return lastConfig; }
    eng::EngineCapabilities capabilities() const override { return capabilitiesToReport; }
};

idx::ModuleIndex fixture_index() {
    idx::ModuleIndex index;
    index.update("/p/src/main.cpp", "import std;\nimport hello.greet;\n\nint main() {\n    return 0;\n}\n");
    index.update("/p/src/greet/greet.cppm", "export module hello.greet;\nexport import :detail;\nimport std;\n");
    index.update("/p/src/greet/detail.cppm", "export module hello.greet:detail;\nimport std;\n");
    index.update("/p/src/greet/impl.cpp", "module hello.greet;\nimport missing;\n");
    index.set_external({ idx::ExternalModule { "std", "/kit/share/libc++/v1/std.cppm", "stdlib" },
                         idx::ExternalModule { "std.compat", "/kit/share/libc++/v1/std.compat.cppm", "stdlib" } });
    index.set_profile_label("libc++ 23.1.0 (semantic kit)");
    return index;
}

} // namespace

int main() {
    using namespace lspmcpp::testing;

    "incremental changes use UTF-16 positions"_test = [] {
        srv::DocumentStore store;
        store.open("file:///p/a.cpp", "/p/a.cpp", "cpp", 1, "int a\xF0\x9F\x98\x80 = 1;\nint b = 2;\n");
        const Json changes = Json::parse(R"([
            {"range": {"start": {"line": 0, "character": 7}, "end": {"line": 0, "character": 8}}, "text": "3"},
            {"range": {"start": {"line": 1, "character": 4}, "end": {"line": 1, "character": 5}}, "text": "bee"},
            {"range": {"start": {"line": 2, "character": 0}, "end": {"line": 2, "character": 0}}, "text": "// end\n"}
        ])");
        // "int a😀 = 1;": the emoji takes characters 5 and 6, so character 7 is the space before '='.
        expect(store.change("file:///p/a.cpp", 2, changes));
        const auto* document = store.find("file:///p/a.cpp");
        expect(fatal(document != nullptr));
        expect(document->text == "int a\xF0\x9F\x98\x80" "3= 1;\nint bee = 2;\n// end\n") << document->text;
        expect(document->version == 2);
        expect(store.change("file:///p/a.cpp", 3, Json::parse(R"([{"text": "whole"}])")));
        expect(store.find("file:///p/a.cpp")->text == "whole");
        expect(store.find_by_path("/p/a.cpp") != nullptr);
        store.close("file:///p/a.cpp");
        expect(store.find("file:///p/a.cpp") == nullptr);
        expect(!store.change("file:///p/a.cpp", 4, Json::array()));
    };

    "module names navigate"_test = [] {
        const auto index = fixture_index();
        const Json toInterface = index.definition("/p/src/main.cpp", Position { 1, 9 });
        expect(fatal(toInterface.is_array() && toInterface.size() == 1u));
        expect(toInterface[0]["uri"] == lspmcpp::base::path_to_uri("/p/src/greet/greet.cppm"));
        expect(toInterface[0]["range"]["start"]["character"] == 14);
        const Json toPartition = index.definition("/p/src/greet/greet.cppm", Position { 1, 15 });
        expect(fatal(toPartition.size() == 1u));
        expect(toPartition[0]["uri"] == lspmcpp::base::path_to_uri("/p/src/greet/detail.cppm"));
        const Json toStd = index.definition("/p/src/main.cpp", Position { 0, 8 });
        expect(fatal(toStd.size() == 1u));
        expect(toStd[0]["uri"] == lspmcpp::base::path_to_uri("/kit/share/libc++/v1/std.cppm"));
        const Json implementation = index.definition("/p/src/greet/impl.cpp", Position { 0, 9 });
        expect(fatal(implementation.size() == 1u));
        expect(implementation[0]["uri"] == lspmcpp::base::path_to_uri("/p/src/greet/greet.cppm"));
        expect(index.definition("/p/src/main.cpp", Position { 3, 5 }).is_null());
    };

    "hover, completion and symbols"_test = [] {
        const auto index = fixture_index();
        const std::string hover { index.hover("/p/src/main.cpp", Position { 1, 12 })["contents"]["value"].get<std::string>() };
        expect(hover.find("module hello.greet") != std::string::npos && hover.find("greet.cppm") != std::string::npos) << hover;
        expect(hover.find("libc++ 23.1.0") != std::string::npos);

        const std::string text { "import std;\nexport import hel" };
        const Json completion = index.completion("/p/src/new.cppm", text, Position { 1, 17 });
        expect(fatal(completion.is_object()));
        expect(completion["items"].size() == 1u && completion["items"][0]["label"] == "hello.greet") << completion.dump();
        expect(completion["items"][0]["textEdit"]["range"]["start"]["character"] == 14);
        const Json partitions = index.completion("/p/src/greet/greet.cppm", "export module hello.greet;\nimport :", Position { 1, 8 });
        expect(partitions["items"].size() == 1u && partitions["items"][0]["label"] == ":detail") << partitions.dump();
        const Json all = index.completion("/p/src/x.cpp", "import ", Position { 0, 7 });
        expect(all["items"].size() == 3u) << all.dump();   // hello.greet, std, std.compat
        expect(index.completion("/p/src/x.cpp", "int important = 1;", Position { 0, 10 }).is_null());
        expect(index.completion("/p/src/x.cpp", "hello::", Position { 0, 7 }).is_null());

        const Json symbols = index.document_symbols("/p/src/greet/detail.cppm");
        expect(symbols.size() == 1u && symbols[0]["name"] == "hello.greet:detail" && symbols[0]["kind"] == 2);
        expect(index.workspace_symbols("GREET").size() == 2u);
        expect(index.workspace_symbols("detail").size() == 1u);
    };

    "module diagnostics"_test = [] {
        auto index = fixture_index();
        const Json impl = index.diagnostics("/p/src/greet/impl.cpp");
        expect(impl.size() == 1u && impl[0]["code"] == "unresolved-module" && impl[0]["source"] == "lsp-mcpp") << impl.dump();
        expect(index.diagnostics("/p/src/main.cpp").empty());
        index.update("/p/src/loose.cpp", "import :part;\n");
        expect(index.diagnostics("/p/src/loose.cpp")[0]["code"] == "partition-outside-module");
        index.update("/p/src/copy.cppm", "export module hello.greet;\n");
        expect(index.diagnostics("/p/src/main.cpp")[0]["code"] == "ambiguous-module");
        index.remove("/p/src/copy.cppm");
        expect(index.diagnostics("/p/src/main.cpp").empty());
        index.update("/p/src/greet/noiface.cpp", "module nobody;\n");
        expect(index.diagnostics("/p/src/greet/noiface.cpp")[0]["code"] == "unresolved-module");
    };

    "graph and module info"_test = [] {
        const auto index = fixture_index();
        const Json graph = index.graph();
        bool sawStd { false };
        for (const auto& module : graph["modules"]) {
            if (module["name"] == "std") sawStd = module["external"].get<bool>();
        }
        expect(sawStd);
        expect(graph["imports"].size() >= 5u);
        const Json info = index.module_info("hello.greet");
        expect(info["resolvedFrom"] == "set" && info["providers"].size() == 1u && !info["ambiguous"].get<bool>());
        expect(index.module_info("std")["resolvedFrom"] == "stdlib");
        expect(!index.module_info("nothing").contains("resolvedFrom"));
    };

    "routing sends module positions to the index"_test = [] {
        const auto index = fixture_index();
        const std::string text { "import std;\nimport hello.greet;\n" };
        const Json onModule = Json::parse(R"({"textDocument": {"uri": "file:///p/src/main.cpp"}, "position": {"line": 1, "character": 10}})");
        const Json elsewhere = Json::parse(R"({"textDocument": {"uri": "file:///p/src/main.cpp"}, "position": {"line": 3, "character": 1}})");
        expect(srv::route_request("textDocument/definition", onModule, index, "/p/src/main.cpp", text).route == srv::Route::local);
        expect(srv::route_request("textDocument/hover", onModule, index, "/p/src/main.cpp", text).route == srv::Route::local);
        expect(srv::route_request("textDocument/definition", elsewhere, index, "/p/src/main.cpp", text).route == srv::Route::engine);
        expect(srv::route_request("textDocument/references", onModule, index, "/p/src/main.cpp", text).route == srv::Route::engine);
        expect(srv::route_request("textDocument/documentSymbol", onModule, index, "/p/src/main.cpp", text).merge == srv::Merge::document_symbols);
        expect(srv::route_request("workspace/symbol", Json::object(), index, "", "").merge == srv::Merge::workspace_symbols);
    };

    "clangd's module build failures are recognized"_test = [] {
        const auto failure = lspmcpp::engine::parse_module_failure(
            R"(E[03:15:19.435] Failed to build module greet; due to Failed to compile C:\Program Files\VS\modules\std.ixx. Use '--log=verbose' to view detailed failure reasons.)");
        expect(fatal(failure.has_value()));
        expect(failure->module == "greet") << failure->module;
        expect(failure->reason == R"(Failed to compile C:\Program Files\VS\modules\std.ixx)") << failure->reason;
        expect(failure->failedSource == R"(C:\Program Files\VS\modules\std.ixx)") << failure->failedSource;
        const auto other = lspmcpp::engine::parse_module_failure("E[04:05:38.910] Failed to build module std; due to Don't get the module unit for module std");
        expect(fatal(other.has_value()));
        expect(other->module == "std" && other->failedSource.empty());
        expect(!lspmcpp::engine::parse_module_failure("I[04:34:47.305] Built module std to /cache/std.pcm").has_value());
    };

    "merging"_test = [] {
        const Json engineSymbols = Json::parse(R"([{"name": "hello", "kind": 3, "range": {}, "selectionRange": {}}])");
        const Json moduleSymbols = Json::parse(R"([{"name": "hello.greet", "kind": 2, "range": {}, "selectionRange": {}}])");
        const Json merged = srv::merge_document_symbols(engineSymbols, moduleSymbols);
        expect(merged.size() == 2u && merged[0]["name"] == "hello.greet");
        const Json flat = Json::parse(R"([{"name": "hello", "kind": 3, "location": {}}])");
        expect(srv::merge_document_symbols(flat, moduleSymbols).size() == 1u);
        expect(srv::merge_workspace_symbols(nullptr, moduleSymbols).size() == 1u);

        const Json range = Json::parse(R"({"start": {"line": 1, "character": 7}, "end": {"line": 1, "character": 18}})");
        const Json moduleDiagnostics = Json::array({ Json { { "range", range }, { "message", "module 'x' not found" }, { "source", "lsp-mcpp" } } });
        const Json engineDiagnostics = Json::array({ Json { { "range", range }, { "message", "module 'x' not found" }, { "source", "clang" } },
                                                     Json { { "range", Json::parse(R"({"start": {"line": 4, "character": 0}, "end": {"line": 4, "character": 1}})") }, { "message", "other" }, { "source", "clang" } } });
        const Json diagnostics = srv::merge_diagnostics(engineDiagnostics, moduleDiagnostics, "gcc 16.1.0");
        expect(diagnostics.size() == 2u) << diagnostics.dump();
        expect(diagnostics[1]["source"] == "clang \xC2\xB7 gcc 16.1.0") << diagnostics.dump();

        const Json capabilities = srv::merge_capabilities(Json::parse(R"({"hoverProvider": true, "textDocumentSync": {"change": 2}})"));
        expect(capabilities["experimental"]["cxxModules"]["version"] == 1);
        expect(capabilities["definitionProvider"] == true && capabilities["textDocumentSync"]["change"] == 2);
        const Json client = Json::parse(R"({"experimental": {"cxxModules": {"version": 1, "status": true}}})");
        expect(srv::client_supports(client, "status") && !srv::client_supports(client, "graph"));
    };

    "modules are prepared as soon as their imports are, within the limit"_test = [] {
        using State = srv::Primer::State;
        srv::Primer primer;
        primer.set_limit(2);
        primer.set_modules({
            { "std", {}, "/prime/std.cpp" },
            { "base", { "std" }, "/prime/base.cpp" },
            { "util", { "std" }, "/prime/util.cpp" },
            { "app:part", { "base" }, "" },
            { "app", { "app:part", "util" }, "/prime/app.cpp" },
            { "tool", { "std" }, "/prime/tool.cpp" },
        });
        const std::vector<std::string> wanted { "app" };
        expect(primer.want(wanted) == 5u) << "everything app reaches, and nothing else";
        expect(primer.state("tool") == State::unwanted);
        expect(primer.busy());
        auto names = [](const std::vector<const srv::PrimeModule*>& modules) {
            std::vector<std::string> result;
            for (const auto* module : modules) result.push_back(module->name);
            return result;
        };
        expect(names(primer.start_ready()) == std::vector<std::string> { "std" }) << "only std has no imports";
        expect(primer.start_ready().empty()) << "a started module is not started twice";
        primer.finish("std");
        expect(names(primer.start_ready()) == std::vector<std::string> { "base", "util" });
        expect(primer.running() == 2u);
        primer.finish("base");
        // app:part has no unit of its own: it completes with base, and app still waits for util.
        expect(primer.start_ready().empty());
        expect(primer.state("app:part") == State::done);
        primer.finish("util");
        expect(names(primer.start_ready()) == std::vector<std::string> { "app" });
        expect(primer.progress() == std::pair<std::size_t, std::size_t> { 4u, 5u });
        primer.finish("app");
        expect(!primer.busy());
        primer.finish("app");
        expect(primer.running() == 0u) << "a module finished twice is counted once";

        const std::vector<srv::PrimeModule> smaller { { "std", {}, "/prime/std.cpp" }, { "base", { "std" }, "/prime/base.cpp" } };
        expect(!primer.same_modules(smaller));
        const std::vector<srv::PrimeModule> otherImports {
            { "std", {}, "/prime/std.cpp" },       { "base", { "std" }, "/prime/base.cpp" },          { "util", { "std", "base" }, "/prime/util.cpp" },
            { "app:part", { "base" }, "" },         { "app", { "app:part", "util" }, "/prime/app.cpp" }, { "tool", { "std" }, "/prime/tool.cpp" },
        };
        expect(!primer.same_modules(otherImports)) << "util imports base now";
        const std::vector<srv::PrimeModule> reordered {
            { "tool", { "std" }, "/prime/tool.cpp" }, { "app", { "app:part", "util" }, "/prime/app.cpp" }, { "app:part", { "base" }, "" },
            { "util", { "std" }, "/prime/util.cpp" }, { "base", { "std" }, "/prime/base.cpp" },          { "std", {}, "/prime/std.cpp" },
        };
        expect(primer.same_modules(reordered)) << "the order modules are listed in does not matter";

        // A new graph keeps what is done; reset forgets it, as after an engine restart.
        primer.set_modules(smaller);
        expect(primer.state("std") == State::done && primer.state("base") == State::done);
        expect(primer.find("app") == nullptr && primer.find("base") != nullptr);
        primer.reset();
        expect(primer.state("std") == State::unwanted && !primer.busy());
    };

    "a request about a file still being prepared waits while preparation progresses"_test = [] {
        using namespace std::chrono_literals;
        const auto now = srv::Clock::now();
        srv::PendingRequest request;
        request.purpose = srv::Purpose::client;
        request.deadline = now;
        request.limit = now + 50s;
        expect(srv::keep_waiting(request, true, now - 2s, now)) << "a module finished two seconds ago";
        expect(!srv::keep_waiting(request, false, now - 2s, now)) << "the file's modules are ready: answer";
        expect(!srv::keep_waiting(request, true, std::nullopt, now)) << "nothing has finished yet: no sign of progress";
        expect(!srv::keep_waiting(request, true, now - 11s, now)) << "no module finished for longer than a request's own wait";
        request.limit = now;
        expect(!srv::keep_waiting(request, true, now - 2s, now)) << "the request's limit is reached";
        request.limit = now + 50s;
        request.purpose = srv::Purpose::engine_initialize;
        expect(!srv::keep_waiting(request, true, now - 2s, now)) << "only a client's request waits";
    };

    "the module more work waits on starts first"_test = [] {
        srv::Primer primer;
        primer.set_limit(1);
        primer.set_modules({
            { "std", {}, "/prime/std.cpp" },
            { "a-leaf", { "std" }, "/prime/a-leaf.cpp" },     // first by name, nothing above it
            { "z-root", { "std" }, "/prime/z-root.cpp" },     // last by name, two modules above it
            { "middle", { "z-root" }, "/prime/middle.cpp" },
            { "top", { "middle" }, "/prime/top.cpp" },
        });
        const std::vector<std::string> wanted { "top", "a-leaf" };
        expect(primer.want(wanted) == 5u);
        auto first = primer.start_ready();
        expect(fatal(first.size() == 1u));
        expect(first.front()->name == "std");
        primer.finish("std");
        const auto next = primer.start_ready();
        expect(fatal(next.size() == 1u));
        expect(next.front()->name == "z-root") << next.front()->name;
    };

    "the clangd capability table is keyed by version"_test = [] {
        const auto pinned = lspmcpp::engine::capabilities_for_clangd_version("23.1.0");
        expect(pinned.experimentalModulesSupport && pinned.useDirtyHeaders && pinned.persistentModuleCache && pinned.msvcStlNeedsNoAlignedAllocation);
        const auto other = lspmcpp::engine::capabilities_for_clangd_version("22.1.8");
        expect(!other.experimentalModulesSupport && !other.useDirtyHeaders && !other.persistentModuleCache && !other.msvcStlNeedsNoAlignedAllocation)
            << "an unrecognized version assumes none of the optional behaviour, not the pinned one's";
    };

    "a fake engine satisfies the lspmcpp.engine interface with no clangd process"_test = [] {
        // usable plan W9.5: anything coded only against eng::Engine (the session, and this test)
        // works the same with this fake as with lspmcpp.engine.clangd::Clangd.
        FakeEngine fake;
        expect(!fake.running());
        eng::EngineConfig config;
        config.executable = "/payload/clangd/bin/clangd";
        config.version = "23.1.0";
        config.databaseDirectory = "/cache/contexts/default/cdb";
        bool closed { false };
        std::vector<Json> received;
        const auto started = fake.start(
            config, [&](Json message) { received.push_back(std::move(message)); }, [&] { closed = true; }, [](std::string_view) {});
        expect(started.has_value() && fake.running() && fake.starts == 1);
        expect(fake.config().version == "23.1.0" && fake.config().databaseDirectory == "/cache/contexts/default/cdb");
        expect(fake.capabilities().experimentalModulesSupport == false) << "a fresh fake reports no capability until the test sets one";
        fake.capabilitiesToReport = eng::capabilities_for_clangd_version("23.1.0");
        expect(fake.capabilities().useDirtyHeaders);

        expect(fake.push_database("[]").has_value());
        expect(fake.pushedDatabases == std::vector<std::string> { "[]" });
        expect(fake.send(Json { { "method", "initialized" } }).has_value());
        expect(fake.sent.size() == 1u && fake.sent.front()["method"] == "initialized");

        // The handlers given to start() work like a real connection's: the "engine" can call back in.
        fake.onMessage(Json { { "method", "textDocument/publishDiagnostics" } });
        expect(received.size() == 1u && received.front()["method"] == "textDocument/publishDiagnostics");
        fake.onClosed();
        expect(closed);

        fake.stop(std::chrono::milliseconds { 0 });
        expect(!fake.running());
    };

    "payload integrity checks size and sha256, and caches the hash"_test = [] {
        // usable plan W9.4.
        namespace fs = lspmcpp::platform::fs;
        const std::string root { lspmcpp::base::join_path(lspmcpp::platform::dirs::temp_directory(),
            std::format("lsp-mcpp-test-payload-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        (void)fs::create_directories(root);
        const std::string clangdPath { lspmcpp::base::join_path(root, "clangd") };
        const std::string kitJsonPath { lspmcpp::base::join_path(root, "kit.json") };
        const std::string content { "pretend-clangd-bytes" };
        const std::string kitContent { "{\"name\":\"k\"}" };
        (void)fs::write_file(clangdPath, content);
        (void)fs::write_file(kitJsonPath, kitContent);
        const std::string cacheFile { lspmcpp::base::join_path(root, "cache.json") };

        srv::PayloadPaths payload;
        payload.directory = root;
        payload.files.emplace("clangd", srv::PayloadFileIntegrity { content.size(), lspmcpp::base::sha256_hex(content) });
        payload.files.emplace("kit.json", srv::PayloadFileIntegrity { kitContent.size(), lspmcpp::base::sha256_hex(kitContent) });

        expect(srv::verify_payload_integrity(payload, cacheFile).empty()) << "both files match their manifest entry";
        expect(fs::is_regular_file(cacheFile)) << "a hash was computed and cached";

        // A no-op payload.files: nothing to check, regardless of what is on disk.
        srv::PayloadPaths empty;
        empty.directory = root;
        expect(srv::verify_payload_integrity(empty, cacheFile).empty());

        // Truncated: the size check alone catches it, no need to hash.
        srv::PayloadPaths truncated { payload };
        truncated.files.at("clangd").size = content.size() + 1;
        {
            const auto issues = srv::verify_payload_integrity(truncated, cacheFile);
            expect(fatal(issues.size() == 1u));
            expect(issues.front().path == "clangd" && issues.front().reason.find("expected") != std::string::npos) << issues.front().reason;
        }

        // Same size, wrong sha256 (a manifest that does not describe this file).
        srv::PayloadPaths wrongHash { payload };
        wrongHash.files.at("clangd").sha256 = std::string(64, '0');
        {
            const auto issues = srv::verify_payload_integrity(wrongHash, cacheFile);
            expect(fatal(issues.size() == 1u));
            expect(issues.front().path == "clangd");
        }

        // Missing entirely.
        srv::PayloadPaths missing { payload };
        missing.files.emplace("nonexistent", srv::PayloadFileIntegrity { 1, "x" });
        {
            const auto issues = srv::verify_payload_integrity(missing, cacheFile);
            expect(fatal(issues.size() == 1u));
            expect(issues.front().path == "nonexistent" && issues.front().reason == "is missing");
        }

        // The cache is trusted while size and modification time have not changed: tampering the
        // cached hash for an unmodified file (same stamp) changes the verdict, proving the second
        // call reused it instead of re-hashing the untouched file.
        expect(srv::verify_payload_integrity(payload, cacheFile).empty());
        auto tampered = fs::read_file(cacheFile);
        expect(fatal(tampered.has_value()));
        Json cacheJson = Json::parse(*tampered);
        cacheJson[clangdPath]["sha256"] = std::string(64, 'f');
        (void)fs::write_file(cacheFile, cacheJson.dump());
        {
            const auto issues = srv::verify_payload_integrity(payload, cacheFile);
            expect(fatal(issues.size() == 1u)) << "the tampered cache entry was trusted, not recomputed";
            expect(issues.front().path == "clangd");
        }

        fs::remove_all(root);
    };

    "engine request keys round-trip and tell roots apart"_test = [] {
        // usable plan W9.1: the session parses this back out of a client response's id to find
        // which root's engine to forward it to, and to discard a stale generation.
        // `Json id { 42 }` would wrap the plain integer in a one-element array; `=` keeps it a
        // scalar, matching the plain integer ids clangd itself sends.
        const Json id = 42;
        const std::string key { srv::make_engine_request_key("/work/root-a", 3, id) };
        std::string rootKey;
        int generation { 0 };
        Json parsedId;
        expect(srv::parse_engine_request_key(key, rootKey, generation, parsedId));
        expect(rootKey == "/work/root-a" && generation == 3 && parsedId == id);

        // A different root or generation makes a different key, so the session's lookup cannot
        // confuse one root's in-flight request with another's, or an old engine with the current one.
        expect(srv::make_engine_request_key("/work/root-b", 3, id) != key);
        expect(srv::make_engine_request_key("/work/root-a", 4, id) != key);

        // Not a value this function ever produced: parsed as not-a-key rather than misread.
        expect(!srv::parse_engine_request_key("not-a-key", rootKey, generation, parsedId));
        expect(!srv::parse_engine_request_key("e:onlyonecolon", rootKey, generation, parsedId));

        // A string id, and a root key that itself contains ':' (every Windows path does, right
        // after its drive letter): the root key is length-prefixed rather than split on ':', so it
        // round-trips exactly regardless of what it contains.
        // `Json("s:5")` (parentheses): `Json { "s:5" }` would, like the integer above, wrap the
        // string in a one-element array instead of holding it as the one string value.
        const Json stringId("s:5");
        const std::string key2 { srv::make_engine_request_key("/work/a:b", 1, stringId) };
        expect(srv::parse_engine_request_key(key2, rootKey, generation, parsedId));
        expect(rootKey == "/work/a:b" && generation == 1 && parsedId == stringId);
    };

    "build files, interactive methods and state names"_test = [] {
        expect(srv::is_build_file("mcpp.toml") && srv::is_build_file("CMakeLists.txt") && srv::is_build_file("x.cmake"));
        expect(!srv::is_build_file("main.cpp") && !srv::is_build_file("greet.cppm"));
        expect(srv::is_interactive("textDocument/hover") && srv::is_interactive("textDocument/definition"));
        expect(!srv::is_interactive("textDocument/didOpen") && !srv::is_interactive("workspace/symbol"));
        expect(srv::to_string(srv::State::ready) == "ready" && srv::to_string(srv::State::error) == "error");
        expect(srv::to_string(srv::State::degraded) == "degraded" && srv::to_string(srv::State::preparing) == "preparing");
    };

    return report();
}
