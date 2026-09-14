module lspmcpp.server.workspace;

import std;
import nlohmann.json;
import lspmcpp.os;
import lspmcpp.base.error;
import lspmcpp.base.log;
import lspmcpp.base.path;
import lspmcpp.base.text;
import lspmcpp.base.uri;
import lspmcpp.base.version;
import lspmcpp.platform.dirs;
import lspmcpp.platform.env;
import lspmcpp.platform.fs;
import lspmcpp.platform.stdio;
import lspmcpp.platform.task;
import lspmcpp.lsp.jsonrpc;
import lspmcpp.lsp.protocol;
import lspmcpp.spec.database;
import lspmcpp.spec.kit;
import lspmcpp.spec.metadata;
import lspmcpp.toolchain.probe;
import lspmcpp.project.scan;
import lspmcpp.project.detect;
import lspmcpp.project.model;
import lspmcpp.normalize.plan;
import lspmcpp.index.modules;
import lspmcpp.engine.clangd;
import lspmcpp.server.documents;
import lspmcpp.server.payload;
import lspmcpp.server.primer;
import lspmcpp.server.router;

namespace lspmcpp::server {

namespace log = base::log;

std::string_view to_string(State state) {
    switch (state) {
    case State::starting: return "starting";
    case State::loading: return "loading";
    case State::preparing: return "preparing";
    case State::ready: return "ready";
    case State::degraded: return "degraded";
    case State::error: return "error";
    }
    return "error";
}

namespace {

constexpr std::array<std::string_view, 6> BUILD_FILES { "mcpp.toml", "mcpp.lock", "CMakeLists.txt", "CMakePresets.json",
                                                        "compile_commands.json", "build_database.json" };

constexpr std::array<std::string_view, 7> WATCH_POLL_SKIP_DIRECTORIES { "target", "build", "node_modules", "out",
                                                                        "_build", "cmake-build-debug", "cmake-build-release" };

// The module structure of a scan, for deciding whether an edit changes the engine database.
std::string structure_of(const project::ScanResult& scan) {
    std::string key { project::provided_name(scan) };
    key += "|" + std::string { spec::to_string(project::role_of(scan)) };
    for (const auto& name : project::required_names(scan)) key += "|" + name;
    return key;
}

} // namespace

bool is_build_file(std::string_view name) {
    return std::ranges::find(BUILD_FILES, name) != BUILD_FILES.end() || name.ends_with(".cmake");
}

// Requests a person waits for are answered without the engine after this long; the rest wait for the configured timeout.
bool is_interactive(std::string_view method) {
    static constexpr std::array<std::string_view, 9> INTERACTIVE { "textDocument/definition", "textDocument/declaration", "textDocument/hover",
        "textDocument/completion", "textDocument/signatureHelp", "textDocument/documentHighlight", "textDocument/typeDefinition",
        "textDocument/implementation", "completionItem/resolve" };
    return std::ranges::find(INTERACTIVE, method) != INTERACTIVE.end();
}

std::map<std::string, platform::fs::FileStamp> watched_files_snapshot(std::string_view root) {
    std::map<std::string, platform::fs::FileStamp> files;
    for (const auto& file : platform::fs::list_files(root, {}, WATCH_POLL_SKIP_DIRECTORIES)) {
        if (!is_build_file(base::file_name(file)) && !project::is_cxx_source_name(file)) continue;
        if (auto fileStamp = platform::fs::stamp(file)) files.emplace(file, *fileStamp);
    }
    return files;
}

void send_client_message(const Json& message) {
    if (auto written = platform::stdio::write_output(lsp::encode_frame(message)); !written) {
        log::error("cannot write to the client: {}", written.error().message);
    }
}

void reply(const Json& id, Json result) { send_client_message(lsp::make_result(id, std::move(result))); }

void reply_error(const Json& id, int code, std::string_view message) { send_client_message(lsp::make_error(id, code, message)); }

void notify_client(std::string_view method, Json params) { send_client_message(lsp::make_notification(method, std::move(params))); }

std::string make_engine_request_key(std::string_view rootKey, int generation, const Json& engineId) {
    return std::format("e:{}:{}:{}:{}", rootKey.size(), rootKey, generation, lsp::dump(engineId));
}

bool parse_engine_request_key(std::string_view key, std::string& rootKey, int& generation, Json& engineId) {
    if (!key.starts_with("e:")) return false;
    std::string_view rest { key.substr(2) };
    // The root key is length-prefixed, not delimiter-split like the rest: a workspace root's own
    // path is exactly the kind of thing that might contain ':' and break a naive split on it —
    // every Windows path does, right after its drive letter.
    const std::size_t lengthEnd { rest.find(':') };
    if (lengthEnd == std::string_view::npos) return false;
    std::size_t rootKeyLength { 0 };
    try {
        rootKeyLength = static_cast<std::size_t>(std::stoull(std::string { rest.substr(0, lengthEnd) }));
    } catch (...) {
        return false;
    }
    rest = rest.substr(lengthEnd + 1);
    if (rest.size() < rootKeyLength + 1 || rest[rootKeyLength] != ':') return false;
    const std::string_view keyText { rest.substr(0, rootKeyLength) };
    rest = rest.substr(rootKeyLength + 1);
    const std::size_t generationEnd { rest.find(':') };
    if (generationEnd == std::string_view::npos) return false;
    int parsedGeneration { 0 };
    try {
        parsedGeneration = std::stoi(std::string { rest.substr(0, generationEnd) });
    } catch (...) {
        return false;
    }
    // `Json parsedId { ... }` would wrap a scalar (e.g. the plain integer ids clangd itself uses)
    // in a one-element array; `=` parses it as the value it is.
    Json parsedId = Json::parse(rest.substr(generationEnd + 1), nullptr, false);
    if (parsedId.is_discarded()) return false;
    rootKey = std::string { keyText };
    generation = parsedGeneration;
    engineId = std::move(parsedId);
    return true;
}

// ---- WorkspaceRoot::Impl ---------------------------------------------------------------------
//
// Everything one workspace root owns (usable plan W9.1): the project model, module index, plan,
// clangd engine and the documents under it. This used to be all of lspmcpp::server::(anonymous)
// Session; a session now composes one of these per root and stays a thin router of client
// requests, by the longest root prefix of the document path, to the right one.

struct WorkspaceRoot::Impl {
    std::string root;
    std::string key;
    SessionOptions options;
    std::shared_ptr<EventChannel> events;
    std::string compilerOverride;
    bool kitEnabled { true };

    std::string cacheDirectory;
    std::string databaseDirectory;
    PayloadPaths payload;
    bool payloadCorrupt { false };
    std::optional<spec::Kit> kit;
    std::string macosSdk;
    DocumentStore documents;
    mutable std::unordered_map<std::string, std::string> canonicalByUri;
    index::ModuleIndex index;
    spec::MetadataReader metadataReader { spec::caching_metadata_reader() };
    std::map<std::string, platform::fs::FileStamp> watchBaseline;

    Json clientParams;
    bool clientSupportsStatus { false };
    // Sending anything before the client has even received its own `initialize` response would be
    // a protocol violation; update_status can otherwise fire synchronously from deep inside start()
    // (an unavailable engine answers at once). Set once, by allow_status_notifications.
    bool initializeAnswered { false };
    std::function<void(Json)> onEngineSettled;

    // Project model and plan.
    std::shared_ptr<project::ProjectModel> model;
    int modelGeneration { 0 };
    bool loading { false };
    bool reloadAfterLoad { false };
    normalize::EnginePlan plan;
    std::set<std::string> excluded;          // path keys
    std::string writtenDatabase;
    std::string writtenStructure;            // the written database without module hints
    bool firstPlanWritten { false };
    std::string contextSet;
    std::map<std::string, std::string, std::less<>> structures;   // path key -> module structure at planning time

    // Modules clangd could not build, by name, with the reason; cleared when sources change.
    std::map<std::string, std::string, std::less<>> failedModules;

    // Parallel module preparation (primer.cppm): `import M;` units opened in the engine.
    Primer primer;
    std::string primeDirectory;
    std::string moduleHintDirectory;
    std::map<std::string, std::string, std::less<>> primeModuleByPath;
    std::map<std::string, std::chrono::steady_clock::time_point, std::less<>> primeDeadlines;
    std::map<std::string, std::string, std::less<>> heldPrimeUnits;

    // Engine (usable plan W9.5): only ever used through the interface.
    std::unique_ptr<engine::Engine> engine;
    int engineGeneration { 0 };
    bool engineHandshakeDone { false };
    bool engineAccepting { false };
    bool engineUnavailable { false };
    Json engineCapabilities;
    std::map<std::int64_t, PendingRequest> pending;
    std::int64_t nextEngineId { 1 };
    std::map<std::string, std::pair<int, Json>, std::less<>> engineToClient;   // synthetic key -> (generation, engine id)
    std::vector<Json> deferred;
    std::map<std::string, Json, std::less<>> engineDiagnostics;
    std::map<std::string, std::string, std::less<>> publishedDiagnostics;
    std::set<std::string> awaitingDiagnostics;
    std::map<std::string, int, std::less<>> timeoutsByUri;
    std::deque<std::chrono::steady_clock::time_point> crashes;
    std::vector<Issue> engineIssues;

    // Timers.
    std::optional<Clock::time_point> reloadAt;
    std::optional<Clock::time_point> replanAt;
    std::optional<Clock::time_point> restartAt;
    std::optional<Clock::time_point> loadGiveUpAt;
    std::optional<Clock::time_point> sdkCheckAt;

    State lastState { State::starting };
    std::string lastStatus;

    Impl(std::string root_, std::string key_, SessionOptions options_, PayloadPaths payload_, bool payloadCorrupt_,
        bool kitEnabled_, std::string compilerOverride_, std::shared_ptr<EventChannel> events_)
        : root { std::move(root_) }, key { std::move(key_) }, options { std::move(options_) }, events { std::move(events_) },
          compilerOverride { std::move(compilerOverride_) }, kitEnabled { kitEnabled_ }, payload { std::move(payload_) },
          payloadCorrupt { payloadCorrupt_ },
          engine { options.engineFactory ? options.engineFactory() : std::make_unique<engine::Clangd>() } {
        cacheDirectory = base::join_path(platform::dirs::cache_directory(), base::join_path("workspaces", project::workspace_key(root)));
        // Under its one name, like every file the engine is given (engine_uri): the prime units
        // and the database live here, and clangd answers for them under the name it was given.
        (void)platform::fs::create_directories(cacheDirectory);
        cacheDirectory = platform::fs::canonical_path(cacheDirectory);
        databaseDirectory = base::join_path(cacheDirectory, "contexts/default/cdb");
        primeDirectory = base::join_path(cacheDirectory, "contexts/default/prime");
        moduleHintDirectory = base::join_path(cacheDirectory, "contexts/default/module-hints");   // never created
        primer.set_limit(std::max<std::size_t>(2, std::thread::hardware_concurrency()));
        (void)platform::fs::create_directories(databaseDirectory);
        // clangd starts without a database; the first plan is written before any document reaches it.
        platform::fs::remove_all(base::join_path(databaseDirectory, "compile_commands.json"));
        // usable plan W9.3: taken now, before this root is even started, so the client cannot
        // possibly have created or changed a watched file yet (see start_watch_polling below).
        watchBaseline = watched_files_snapshot(root);
    }

    // ---- plumbing ---------------------------------------------------------

    // The engine is given every file under the one name the model uses for it (see design 14.3's
    // "one file, one name"): clangd matches an unsaved buffer to the module source it builds by
    // exact name.
    std::string engine_uri(std::string_view uri) const {
        if constexpr (lspmcpp::os::FAMILY == lspmcpp::os::Family::windows) {
            const std::string path { path_of_uri(uri) };
            if (path.size() < 2 || path[1] != ':') return std::string { uri };
            return "file:///" + path.substr(0, 2) + base::percent_encode_path(std::string_view { path }.substr(2));
        } else {
            const auto written = base::uri_to_path(uri);
            if (!written) return std::string { uri };
            const std::string path { path_of_uri(uri) };
            return path.empty() || path == *written ? std::string { uri } : base::path_to_uri(path);
        }
    }

    Json engine_view(const Json& message) const {
        Json copy = message;
        const auto fix = [&](Json& uri) {
            if (uri.is_string()) uri = engine_uri(uri.get<std::string>());
        };
        if (copy.contains("params") && copy["params"].is_object()) {
            Json& params = copy["params"];
            if (params.contains("textDocument") && params["textDocument"].is_object() && params["textDocument"].contains("uri")) {
                fix(params["textDocument"]["uri"]);
            }
            if (params.contains("changes") && params["changes"].is_array()) {
                for (auto& change : params["changes"]) {
                    if (change.is_object() && change.contains("uri")) fix(change["uri"]);
                }
            }
        }
        return copy;
    }

    // The client's URI for a document the engine names in its own form.
    std::string client_uri(std::string_view engineUri) const {
        if (documents.find(engineUri) != nullptr) return std::string { engineUri };
        const std::string path { path_of_uri(engineUri) };
        if (const Document* document = path.empty() ? nullptr : documents.find_by_path(path)) return document->uri;
        return std::string { engineUri };
    }

    void client_view(Json& value) const {
        if (value.is_array()) {
            for (auto& element : value) client_view(element);
            return;
        }
        if (!value.is_object()) return;
        for (auto item = value.begin(); item != value.end(); ++item) {
            if ((item.key() == "uri" || item.key() == "targetUri") && item.value().is_string()) {
                item.value() = client_uri(item.value().get<std::string>());
            } else if (item.key() == "changes" && item.value().is_object()) {
                Json renamed = Json::object();
                for (auto change = item.value().begin(); change != item.value().end(); ++change) renamed[client_uri(change.key())] = change.value();
                item.value() = std::move(renamed);
            } else {
                client_view(item.value());
            }
        }
    }

    bool send_engine(const Json& message) {
        if (!engine->running()) return false;
        if (auto sent = engine->send(engine_view(message)); !sent) {
            log::warning("cannot write to clangd ({}): {}", root, sent.error().message);
            return false;
        }
        return true;
    }

    // A file's path under the one name the model uses for it (see engine_uri).
    std::string path_of_uri(std::string_view uri) const {
        const std::string key_ { uri };
        if (const auto cached = canonicalByUri.find(key_); cached != canonicalByUri.end()) return cached->second;
        auto path = base::uri_to_path(uri);
        std::string canonical { path ? platform::fs::canonical_path(*path) : std::string {} };
        if (canonicalByUri.size() > 4096) canonicalByUri.clear();
        canonicalByUri.emplace(key_, canonical);
        return canonical;
    }

    static std::string uri_of_params(const Json& params) {
        const Json* uri { lsp::find_path(params, { "textDocument", "uri" }) };
        return uri != nullptr && uri->is_string() ? uri->get<std::string>() : std::string {};
    }

    std::optional<Clock::time_point> next_deadline() const {
        std::optional<Clock::time_point> deadline;
        auto consider = [&](const std::optional<Clock::time_point>& at) {
            if (at && (!deadline || *at < *deadline)) deadline = at;
        };
        for (const auto& [id, request] : pending) consider(request.deadline);
        consider(reloadAt);
        consider(replanAt);
        consider(restartAt);
        consider(loadGiveUpAt);
        consider(sdkCheckAt);
        for (const auto& [module, at] : primeDeadlines) consider(at);
        return deadline;
    }

    // usable plan W9.3: every 2s, compares size and modification time of the same build
    // description and source files a dynamic watch would cover, and turns a difference into the
    // exact notification workspace/didChangeWatchedFiles would have carried, so
    // handle_watched_files treats a polled change exactly like an editor's own. Starts from
    // watchBaseline (taken in the constructor, before this root could receive any client-driven
    // change), so a file created the instant the client can act is never mistaken for one already
    // there. Runs for the life of the process on its own thread, touching only its own local
    // snapshot and the shared event queue (WorkspaceRoot state changes only on the main thread, via
    // that queue, as elsewhere in this file); a root removed by workspace/didChangeWorkspaceFolders
    // simply stops being found when the queue is drained, and the thread exits with the process.
    void start_watch_polling() {
        auto queue = events;
        const std::string rootPath { root };
        std::thread { [queue, rootPath, known = watchBaseline]() mutable {
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds { 2 });
                std::map<std::string, platform::fs::FileStamp> current { watched_files_snapshot(rootPath) };
                Json changes = Json::array();
                for (const auto& [file, fileStamp] : current) {
                    const auto previous = known.find(file);
                    if (previous == known.end()) changes.push_back(Json { { "uri", base::path_to_uri(file) }, { "type", 1 } });
                    else if (previous->second != fileStamp) changes.push_back(Json { { "uri", base::path_to_uri(file) }, { "type", 2 } });
                }
                for (const auto& [file, fileStamp] : known) {
                    if (!current.contains(file)) changes.push_back(Json { { "uri", base::path_to_uri(file) }, { "type", 3 } });
                }
                known = std::move(current);
                if (changes.empty()) continue;
                // `Json message { make_notification(...) }` would wrap the result in a one-element
                // array (nlohmann's initializer-list constructor); `=` copies it as intended.
                // No rootKey: this is a plain client_message, split by path like any other
                // workspace/didChangeWatchedFiles notification (this thread's own entries all fall
                // under `rootPath` anyway, so they land back on this same root).
                Json message = lsp::make_notification("workspace/didChangeWatchedFiles", Json { { "changes", std::move(changes) } });
                queue->push(Event { EventKind::client_message, std::move(message) });
            }
        } }.detach();
    }

    // ---- client-driven ----------------------------------------------------------

    Json local_fallback(Merge merge, const Json& params, std::string_view path) const {
        if (merge == Merge::document_symbols) return path.empty() ? Json::array() : index.document_symbols(path);
        if (merge == Merge::workspace_symbols) return index.workspace_symbols(params.value("query", std::string {}));
        return nullptr;
    }

    bool path_excluded(std::string_view path) const { return !path.empty() && excluded.contains(base::path_key(path)); }

    void route_client_request(const Json& message) {
        const Json& id { message["id"] };
        const std::string method { message.value("method", std::string {}) };
        const Json params = message.contains("params") ? message["params"] : Json::object();
        const std::string uri { uri_of_params(params) };
        const std::string path { uri.empty() ? std::string {} : path_of_uri(uri) };
        const Document* document { uri.empty() ? nullptr : documents.find(uri) };
        RouteDecision decision { route_request(method, params, index, path, document ? std::string_view { document->text } : std::string_view {}) };
        if (decision.route == Route::local) {
            reply(id, std::move(decision.localResult));
            return;
        }
        const bool isExcluded { !path.empty() && path_excluded(path) };
        if (engineUnavailable || isExcluded) {
            reply(id, local_fallback(decision.merge, params, path));
            return;
        }
        if (!engineAccepting) {
            deferred.push_back(message);
            return;
        }
        const std::int64_t engineId { nextEngineId++ };
        const auto timeout = is_interactive(method) ? std::min(options.requestTimeout, INTERACTIVE_TIMEOUT) : options.requestTimeout;
        pending[engineId] = PendingRequest { Purpose::client, id, method, uri, decision.merge, params, Clock::now() + timeout, engineGeneration };
        Json forwarded = message;
        forwarded["id"] = engineId;
        if (!send_engine(forwarded)) {
            pending.erase(engineId);
            reply(id, local_fallback(decision.merge, params, path));
        }
    }

    void answer_initialize_settled(const Json& engineCapabilities) {
        if (onEngineSettled) {
            auto callback = std::move(onEngineSettled);
            onEngineSettled = {};
            callback(engineCapabilities);
        }
    }

    void add_engine_issue(Issue issue) {
        for (const auto& existing : engineIssues) {
            if (existing.code == issue.code) return;
        }
        engineIssues.push_back(std::move(issue));
    }

    void flush_deferred_without_engine() {
        std::vector<Json> toFlush;
        toFlush.swap(deferred);
        for (const auto& message : toFlush) {
            if (lsp::kind_of(message) != lsp::Kind::request) continue;
            const Json params = message.contains("params") ? message["params"] : Json::object();
            const std::string method { message.value("method", std::string {}) };
            const std::string path { path_of_uri(uri_of_params(params)) };
            const RouteDecision decision { route_request(method, params, index, path, {}) };
            reply(message["id"], decision.route == Route::local ? decision.localResult : local_fallback(decision.merge, params, path));
        }
    }

    void open_in_engine(const Document& document) {
        Json params { { "textDocument", Json { { "uri", document.uri }, { "languageId", document.languageId },
                                               { "version", document.version }, { "text", document.text } } } };
        if (send_engine(lsp::make_notification("textDocument/didOpen", std::move(params)))) {
            if (!engineDiagnostics.contains(document.uri)) awaitingDiagnostics.insert(document.uri);
        }
    }

    // A unit of the model whose module declaration or imports changed needs a new
    // plan; a new source file of an inferred model needs a new model.
    void note_structure_change(std::string_view path) {
        if (!model) return;
        const auto it = structures.find(base::path_key(path));
        if (it != structures.end()) {
            if (const auto* scan = index.scan_of(path); scan != nullptr && it->second != structure_of(*scan)) schedule_replan();
            return;
        }
        if (model->source == project::SourceKind::inferred && project::is_cxx_source_name(path) && base::is_within(path, root)) schedule_reload();
    }

    // ---- engine -------------------------------------------------------------------

    void start_engine() {
        engineHandshakeDone = false;
        engineAccepting = false;
        if (payloadCorrupt) {
            engineUnavailable = true;
            add_engine_issue(Issue { "payload-corrupt", "the extension's payload is corrupt or was modified; reinstall the extension", "lspMcpp.showLogs" });
            flush_deferred_without_engine();
            answer_initialize_settled(Json::object());
            update_status();
            return;
        }
        if (payload.clangd.empty() || !platform::fs::is_regular_file(payload.clangd)) {
            engineUnavailable = true;
            add_engine_issue(Issue { "engine-missing", "clangd was not found; only module-level features are available", "lspMcpp.showLogs" });
            flush_deferred_without_engine();
            answer_initialize_settled(Json::object());
            update_status();
            return;
        }
        const int generation { ++engineGeneration };
        engine::EngineConfig config;
        config.executable = payload.clangd;
        config.version = payload.clangdVersion;
        config.databaseDirectory = databaseDirectory;
        config.workDirectory = root;
        config.verboseLog = options.verboseEngineLog;
        // Extra engine arguments for troubleshooting, e.g. LSP_MCPP_ENGINE_ARGUMENTS="-j=8 --background-index-priority=background".
        if (auto extra = platform::env::get("LSP_MCPP_ENGINE_ARGUMENTS")) {
            for (auto word : base::split(*extra, ' ')) {
                if (!base::trim(word).empty()) config.extraArguments.emplace_back(base::trim(word));
            }
        }
        auto queue = events;
        const std::string rootKey { key };
        auto started = engine->start(
            config,
            [queue, generation, rootKey](Json message) { queue->push(Event { EventKind::engine_message, std::move(message), generation, {}, rootKey }); },
            [queue, generation, rootKey] { queue->push(Event { EventKind::engine_closed, {}, generation, {}, rootKey }); },
            [queue, generation, rootKey](std::string_view line) {
                log::info("clangd ({}): {}", rootKey, line);
                if (auto failure = engine::parse_module_failure(line)) {
                    queue->push(Event { EventKind::engine_module_failed,
                                        Json { { "module", failure->module }, { "reason", failure->reason }, { "source", failure->failedSource } },
                                        generation, {}, rootKey });
                }
            });
        if (!started) {
            engineUnavailable = true;
            add_engine_issue(Issue { "engine-crashed", std::format("clangd could not start: {}", started.error().message), "lspMcpp.restartServer" });
            flush_deferred_without_engine();
            answer_initialize_settled(Json::object());
            update_status();
            return;
        }
        engineUnavailable = false;
        Json params = clientParams;
        params.erase("initializationOptions");
        params["processId"] = nullptr;
        // Positions are UTF-16 everywhere in this server.
        if (params.contains("capabilities") && params["capabilities"].is_object()) {
            params["capabilities"].erase("offsetEncoding");
            if (params["capabilities"].contains("general") && params["capabilities"]["general"].is_object()) {
                params["capabilities"]["general"].erase("positionEncodings");
            }
        }
        const std::int64_t engineId { nextEngineId++ };
        pending[engineId] = PendingRequest { Purpose::engine_initialize, nullptr, "initialize", {}, Merge::none, {},
                                             Clock::now() + std::chrono::seconds { 60 }, generation };
        (void)send_engine(lsp::make_request(engineId, "initialize", std::move(params)));
    }

    void restart_engine(std::string_view reason) {
        log::info("restarting clangd ({}): {}", root, reason);
        // Requests to the old engine are answered with what the index knows.
        for (auto it = pending.begin(); it != pending.end();) {
            if (it->second.purpose == Purpose::client) {
                reply(it->second.clientId, local_fallback(it->second.merge, it->second.params, path_of_uri(it->second.uri)));
            }
            it = pending.erase(it);
        }
        engineToClient.clear();
        forget_primes();
        ++engineGeneration;   // late events of the old process are ignored
        engine->stop(std::chrono::milliseconds { 500 });
        engineDiagnostics.clear();
        timeoutsByUri.clear();
        start_engine();
    }

    void handle_engine_message(const Json& message) {
        switch (lsp::kind_of(message)) {
        case lsp::Kind::response: handle_engine_response(message); break;
        case lsp::Kind::request: {
            const std::string requestKey { make_engine_request_key(key, engineGeneration, message["id"]) };
            engineToClient[requestKey] = { engineGeneration, message["id"] };
            Json forwarded = message;
            forwarded["id"] = requestKey;
            if (forwarded.contains("params")) client_view(forwarded["params"]);
            send_client_message(forwarded);
            break;
        }
        case lsp::Kind::notification: handle_engine_notification(message); break;
        case lsp::Kind::invalid: break;
        }
    }

    void handle_engine_response(const Json& message) {
        const Json& id { message["id"] };
        if (!id.is_number_integer()) return;
        const auto it = pending.find(id.get<std::int64_t>());
        if (it == pending.end()) return;   // answered already, after a timeout
        PendingRequest request { std::move(it->second) };
        pending.erase(it);
        switch (request.purpose) {
        case Purpose::engine_initialize: {
            engineCapabilities = lsp::find_path(message, { "result", "capabilities" }) != nullptr ? message["result"]["capabilities"] : Json::object();
            answer_initialize_settled(engineCapabilities);
            (void)send_engine(lsp::make_notification("initialized", Json::object()));
            engineHandshakeDone = true;
            accept_traffic_if_ready();
            update_status();
            break;
        }
        case Purpose::engine_shutdown: reply(request.clientId, nullptr); break;
        case Purpose::client: {
            timeoutsByUri.erase(request.uri);
            if (message.contains("error")) {
                Json forwarded = message;
                forwarded["id"] = request.clientId;
                send_client_message(forwarded);
                return;
            }
            Json result = message.value("result", Json {});
            client_view(result);
            const std::string path { request.uri.empty() ? std::string {} : path_of_uri(request.uri) };
            if (request.merge == Merge::document_symbols && !path.empty()) {
                result = merge_document_symbols(result, index.document_symbols(path));
            } else if (request.merge == Merge::workspace_symbols) {
                result = merge_workspace_symbols(result, index.workspace_symbols(request.params.value("query", std::string {})));
            }
            reply(request.clientId, std::move(result));
            break;
        }
        }
    }

    void handle_engine_notification(const Json& message) {
        const std::string method { message.value("method", std::string {}) };
        if (method == lsp::method::TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS) {
            const Json& params { message["params"] };
            if (finish_prime(params.value("uri", std::string {}))) return;
            const std::string uri { client_uri(params.value("uri", std::string {})) };
            awaitingDiagnostics.erase(uri);
            release_prime_units_if_idle();
            if (documents.find(uri) == nullptr) {
                Json forwarded = message;
                client_view(forwarded["params"]);
                send_client_message(forwarded);
            } else {
                engineDiagnostics[uri] = params.value("diagnostics", Json::array());
                publish_diagnostics(uri, true);
            }
            update_status();
            return;
        }
        if (method == lsp::method::WINDOW_SHOW_MESSAGE) {
            // Nothing pops up from the engine; it goes to the log (design 16.2).
            Json forwarded = message;
            forwarded["method"] = "window/logMessage";
            send_client_message(forwarded);
            return;
        }
        send_client_message(message);
    }

    void handle_engine_closed(bool shuttingDown) {
        engineHandshakeDone = false;
        engineAccepting = false;
        forget_primes();
        if (shuttingDown) return;
        log::warning("clangd exited unexpectedly ({})", root);
        for (auto it = pending.begin(); it != pending.end();) {
            if (it->second.purpose == Purpose::client) {
                reply(it->second.clientId, local_fallback(it->second.merge, it->second.params, path_of_uri(it->second.uri)));
            }
            it = pending.erase(it);
        }
        const auto now = Clock::now();
        crashes.push_back(now);
        while (!crashes.empty() && now - crashes.front() > std::chrono::minutes { 3 }) crashes.pop_front();
        add_engine_issue(Issue { "engine-crashed", "clangd exited unexpectedly", "lspMcpp.restartServer" });
        if (crashes.size() >= 3) {
            engineUnavailable = true;
            flush_deferred_without_engine();
        } else {
            restartAt = now + std::chrono::seconds { 1 << (crashes.size() - 1) };
        }
        update_status();
    }

    void accept_traffic_if_ready() {
        if (!engineHandshakeDone || !firstPlanWritten || engineAccepting) return;
        engineAccepting = true;
        for (const Document* document : documents.all()) {
            if (!path_excluded(document->path)) open_in_engine(*document);
        }
        std::vector<Json> toFlush;
        toFlush.swap(deferred);
        for (const auto& message : toFlush) {
            if (lsp::kind_of(message) == lsp::Kind::request) route_client_request(message);
            else (void)send_engine(message);
        }
        prepare_modules();
        update_status();
    }

    // ---- model and plan -----------------------------------------------------------

    std::string model_cache_path() const { return base::join_path(cacheDirectory, "model.json"); }

    void restore_cached_model() {
        auto text = platform::fs::read_file(model_cache_path());
        if (!text) return;
        const Json cached = Json::parse(*text, nullptr, false);
        if (cached.is_discarded() || !cached.is_object() || !cached.contains("database")) return;
        auto database = spec::from_json(cached["database"]);
        if (!database) return;
        std::size_t files { 0 };
        for (const auto& set : database->sets) {
            for (const auto& unit : set.units) {
                const std::string path { spec::absolute_source(unit) };
                if (index.contains(path)) continue;
                if (auto source = platform::fs::read_file(path)) {
                    index.update(path, *source);
                    ++files;
                }
            }
        }
        log::info("restored the previous model's module index for {} ({} files)", root, files);
    }

    void save_model_cache() const {
        if (!model) return;
        Json envelope {
            { "source", std::string { project::to_string(model->source) } },
            { "level", model->level },
            { "database", Json::parse(spec::to_json(model->database).dump()) },
        };
        (void)platform::fs::write_file_atomic(model_cache_path(), envelope.dump());
    }

    void start_model_load() {
        if (loading) {
            reloadAfterLoad = true;
            return;
        }
        loading = true;
        const int generation { ++modelGeneration };
        project::LoadOptions load;
        load.trusted = options.trusted;
        load.cacheDirectory = cacheDirectory;
        load.compilerOverride = compilerOverride;
        load.mcppExecutable = options.mcpp;
        load.configuredDatabase = options.database;
        load.discoverCompilers = options.discoverCompilers;
        std::shared_ptr<const spec::Kit> kitCopy = kit ? std::make_shared<const spec::Kit>(*kit) : nullptr;
        const std::string rootCopy { root };
        const std::string probeCachePath { base::join_path(platform::dirs::cache_directory(), "toolchains/probe.json") };
        auto queue = events;
        const std::string rootKey { key };
        std::thread { [queue, generation, load, kitCopy, rootCopy, probeCachePath, rootKey]() mutable {
            toolchain::ProbeCache cache { probeCachePath };
            load.kit = kitCopy.get();
            load.runner = toolchain::process_runner(std::chrono::seconds { 20 });
            load.probeCache = &cache;
            auto loadedModel = std::make_shared<project::ProjectModel>(project::load_project(rootCopy, load));
            queue->push(Event { EventKind::model_loaded, {}, generation, std::move(loadedModel), rootKey });
        } }.detach();
        update_status();
    }

    void handle_model_loaded(std::shared_ptr<project::ProjectModel> loadedModel) {
        loading = false;
        failedModules.clear();
        loadGiveUpAt.reset();
        model = std::move(loadedModel);
        log::info("project model ({}): source {}, level {}, {} sets, profile {} {} {}", root, project::to_string(model->source), model->level,
                  model->database.sets.size(), model->profile.kind, model->profile.compiler, model->profile.stdlib);
        for (const auto& issue : model->issues) log::info("model issue [{}] {}", issue.code, issue.message);

        save_model_cache();
        index.clear();
        for (const auto& set : model->database.sets) {
            for (const auto& unit : set.units) {
                const std::string path { spec::absolute_source(unit) };
                if (index.contains(path)) continue;
                if (const Document* document = documents.find_by_path(path)) {
                    index.update(path, document->text);
                } else if (auto text = platform::fs::read_file(path)) {
                    index.update(path, *text);
                }
            }
        }
        for (const Document* document : documents.all()) {
            if (!document->path.empty()) index.update(document->path, document->text);
        }
        std::vector<std::pair<std::string, std::string>> manifests;
        for (auto& manifest : project::module_manifests(*model, kit ? &*kit : nullptr)) manifests.emplace_back(manifest.path, manifest.origin);
        auto external = index::external_modules(manifests, metadataReader);
        index.set_external(std::move(external));
        const auto& profile = model->profile;
        index.set_profile_label(profile.kind == "semantic-kit" ? std::format("{} (semantic kit, {})", profile.stdlib, profile.target)
                                                               : std::format("{} · {} ({})", profile.compiler, profile.stdlib, profile.target));
        replan();
        if (reloadAfterLoad) {
            reloadAfterLoad = false;
            start_model_load();
        }
    }

    void replan() {
        replanAt.reset();
        if (!model) return;
        normalize::PlanInput input;
        input.database = &model->database;
        input.contextSet = contextSet;
        input.facts = &model->facts;
        input.kit = kit ? &*kit : nullptr;
        input.engineDriverDirectory = payload.clangd.empty() ? std::string {} : base::parent_path(payload.clangd);
        input.macosSdk = macosSdk;
        input.scanner = [this](std::string_view path) {
            if (const auto* scan = index.scan_of(path)) return *scan;
            auto text = platform::fs::read_file(path);
            return text ? project::scan_source(*text) : project::ScanResult {};
        };
        input.metadataReader = metadataReader;
        input.failedModules = failedModules;
        input.primeDirectory = primeDirectory;
        input.moduleHintDirectory = moduleHintDirectory;
        normalize::EnginePlan newPlan { normalize::plan_engine(input) };
        write_prime_sources(newPlan);
        const std::string database { normalize::to_compile_commands(newPlan).dump(1) };
        const std::string structure { normalize::to_compile_commands(newPlan, false).dump(1) };
        const bool changed { database != writtenDatabase };
        // Hints alone change as imports do; clangd rereads the database within five seconds.
        const bool structureChanged { structure != writtenStructure };
        writtenStructure = structure;
        if (changed) {
            if (auto pushed = engine->push_database(database); !pushed) {
                log::error("cannot write the engine database ({}): {}", root, pushed.error().message);
            }
            writtenDatabase = database;
            log::info("engine database ({}): {} entries ({} standard library units), {} left out, {} issues", root, newPlan.entries.size(),
                      newPlan.stdUnits, newPlan.excludedFiles.size(), newPlan.issues.size());
        }
        std::set<std::string> newExcluded;
        for (const auto& file : newPlan.excludedFiles) newExcluded.insert(base::path_key(file));
        const bool restartNeeded { structureChanged && firstPlanWritten && engineHandshakeDone };
        if (!restartNeeded && engineAccepting) {
            for (const Document* document : documents.all()) {
                if (document->path.empty()) continue;
                const std::string pathKey { base::path_key(document->path) };
                const bool wasExcluded { excluded.contains(pathKey) };
                const bool isExcluded { newExcluded.contains(pathKey) };
                if (wasExcluded && !isExcluded) open_in_engine(*document);
                if (!wasExcluded && isExcluded) {
                    (void)send_engine(lsp::make_notification("textDocument/didClose", Json { { "textDocument", Json { { "uri", document->uri } } } }));
                }
            }
        }
        excluded = std::move(newExcluded);
        plan = std::move(newPlan);
        close_prime_units();
        {
            std::vector<PrimeModule> modules;
            for (const auto& module : plan.modules) modules.push_back(PrimeModule { module.name, module.requires_, module.primeFile });
            primer.set_modules(std::move(modules));
        }
        structures.clear();
        for (const auto& path : index.files()) {
            if (const auto* scan = index.scan_of(path)) structures[base::path_key(path)] = structure_of(*scan);
        }
        firstPlanWritten = true;
        if (restartNeeded) {
            restart_engine("the engine database changed");
        } else {
            accept_traffic_if_ready();
            prepare_modules();
        }
        for (const Document* document : documents.all()) publish_diagnostics(document->uri);
        update_status();
    }

    // ---- parallel module preparation ----------------------------------------------------

    void write_prime_sources(const normalize::EnginePlan& thePlan) {
        if (thePlan.primeSources.empty()) return;
        (void)platform::fs::create_directories(primeDirectory);
        for (const auto& [file, content] : thePlan.primeSources) {
            if (platform::fs::read_file(file).value_or("") != content) (void)platform::fs::write_file(file, content);
        }
    }

    // std, which nearly every file imports, and the imports of every open document, then as many
    // ready modules as the limit allows. std.compat waits for a file that imports it: building it
    // takes cores a cold start needs.
    void prepare_modules() {
        if (!engineAccepting) return;
        const std::vector<std::string> standard { "std" };
        primer.want(standard);
        for (const Document* document : documents.all()) prepare_imports_of(*document, false);
        pump_primer();
    }

    void prepare_imports_of(const Document& document, bool pump = true) {
        if (document.path.empty() || path_excluded(document.path)) return;
        const auto* scan = index.scan_of(document.path);
        if (scan == nullptr) return;
        const auto names = project::required_names(*scan);
        if (primer.want(names) > 0 && pump) pump_primer();
    }

    void pump_primer() {
        if (!engineAccepting) return;
        for (const PrimeModule* module : primer.start_ready()) {
            const std::string uri { base::path_to_uri(module->primeFile) };
            Json params { { "textDocument", Json { { "uri", uri }, { "languageId", "cpp" }, { "version", 1 },
                                                   { "text", std::format("import {};\n", module->name) } } } };
            if (!send_engine(lsp::make_notification("textDocument/didOpen", std::move(params)))) {
                primer.finish(module->name);
                continue;
            }
            primeModuleByPath[base::path_key(module->primeFile)] = module->name;
            primeDeadlines[module->name] = Clock::now() + std::chrono::minutes { 3 };
        }
        update_status();
    }

    bool finish_prime(std::string_view engineUri) {
        if (primeDirectory.empty()) return false;
        const std::string canonical { path_of_uri(engineUri) };
        const std::optional<std::string> path { canonical.empty() ? std::nullopt : std::optional<std::string> { canonical } };
        if (!path || !base::is_within(*path, primeDirectory)) return false;
        const std::string pathKey { base::path_key(*path) };
        const auto it = primeModuleByPath.find(pathKey);
        if (it == primeModuleByPath.end()) return true;
        const std::string module { it->second };
        primeModuleByPath.erase(it);
        primeDeadlines.erase(module);
        heldPrimeUnits.emplace(pathKey, *path);
        primer.finish(module);
        pump_primer();
        release_prime_units_if_idle();
        return true;
    }

    void release_prime_units_if_idle() {
        if (heldPrimeUnits.empty() || primer.busy() || !awaitingDiagnostics.empty()) return;
        log::info("module preparation idle ({}): closing {} prime units", root, heldPrimeUnits.size());
        close_prime_units();
    }

    void close_prime_units() {
        std::set<std::string> uris;
        for (const auto& [pathKey, module] : primeModuleByPath) {
            if (const auto* planned = primer.find(module)) uris.insert(base::path_to_uri(planned->primeFile));
        }
        for (const auto& [pathKey, path] : heldPrimeUnits) uris.insert(base::path_to_uri(path));
        if (engineAccepting) {
            for (const auto& uri : uris) (void)send_engine(lsp::make_notification("textDocument/didClose", Json { { "textDocument", Json { { "uri", uri } } } }));
        }
        primeModuleByPath.clear();
        primeDeadlines.clear();
        heldPrimeUnits.clear();
    }

    void forget_primes() {
        primeModuleByPath.clear();
        primeDeadlines.clear();
        heldPrimeUnits.clear();
        primer.reset();
    }

    void handle_module_failure(const Json& failure) {
        bool added { false };
        const std::string reason { failure.value("reason", std::string {}) };
        auto add = [&](std::string name) {
            if (name.empty() || failedModules.contains(name)) return;
            log::warning("clangd could not build module {} ({}): {}", name, root, reason);
            failedModules.emplace(std::move(name), reason);
            added = true;
        };
        add(failure.value("module", std::string {}));
        if (const std::string source { failure.value("source", std::string {}) }; !source.empty()) {
            if (auto text = platform::fs::read_file(source)) add(project::provided_name(project::scan_source(*text)));
        }
        if (added) schedule_replan();
    }

    void retry_failed_modules() {
        if (failedModules.empty()) return;
        failedModules.clear();
        schedule_replan();
    }

    void schedule_replan() { replanAt = Clock::now() + std::chrono::milliseconds { 800 }; }
    void schedule_reload() { reloadAt = Clock::now() + std::chrono::milliseconds { 1500 }; }

    // ---- diagnostics and status -------------------------------------------------------

    void publish_diagnostics(std::string_view uri, bool force = false) {
        const Document* document { documents.find(uri) };
        if (document == nullptr) return;
        const Json moduleDiagnostics = document->path.empty() ? Json::array() : index.diagnostics(document->path);
        const auto engineDiagnosticsEntry = engineDiagnostics.find(uri);
        std::string label;
        if (model) label = model->profile.kind == "semantic-kit" ? model->profile.stdlib + " kit" : model->profile.compiler;
        Json merged = merge_diagnostics(engineDiagnosticsEntry == engineDiagnostics.end() ? Json::array() : engineDiagnosticsEntry->second,
                                        moduleDiagnostics, label);
        std::string serialized { lsp::dump(merged) };
        auto& previous = publishedDiagnostics[std::string { uri }];
        if (!force && previous == serialized) return;
        previous = std::move(serialized);
        notify_client("textDocument/publishDiagnostics", Json { { "uri", std::string { uri } }, { "diagnostics", std::move(merged) } });
    }

    Json profile_json() const {
        Json profile = Json::object();
        if (model) {
            profile["kind"] = model->profile.kind;
            if (!model->profile.compiler.empty()) profile["compiler"] = model->profile.compiler;
            profile["stdlib"] = model->profile.stdlib;
            profile["target"] = model->profile.target;
        } else if (kit) {
            profile = Json { { "kind", "semantic-kit" }, { "stdlib", std::format("{} {}", kit->stdlibName, kit->stdlibVersion) }, { "target", kit->target } };
        } else {
            profile = Json { { "kind", "semantic-kit" }, { "stdlib", "unknown" }, { "target", std::string { lspmcpp::os::VSCODE_TARGET } } };
        }
        return profile;
    }

    State compute_state() const {
        // usable plan W9.4: a corrupt payload is not merely degraded (design 13.6): only
        // syntax-level features are trustworthy, the same as three engine crashes in a row.
        if (payloadCorrupt) return State::error;
        if (engineUnavailable && crashes.size() >= 3) return State::error;
        if (!model) return loading ? State::loading : State::starting;
        if (loading) return State::loading;
        if ((!awaitingDiagnostics.empty() || primer.busy()) && engineAccepting) return State::preparing;
        // usable plan W5.4: degraded regardless of whether any open file happens to need std yet.
        if (kit && spec::requires_macos_sdk(*kit) && macosSdk.empty()) return State::degraded;
        if (!engineIssues.empty() || !model->issues.empty() || !plan.issues.empty()) return State::degraded;
        return State::ready;
    }

    void update_status() {
        if (!initializeAnswered) return;   // see the field's own comment
        const State state { compute_state() };
        lastState = state;
        if (!clientSupportsStatus) return;
        Json issues = Json::array();
        auto add = [&](std::string_view code, std::string_view message, std::string_view command, std::string_view title = "Fix") {
            if (issues.size() >= 20) return;
            Json issue { { "code", std::string { code } }, { "message", std::string { message } } };
            if (!command.empty()) issue["command"] = Json { { "title", std::string { title } }, { "command", std::string { command } } };
            issues.push_back(std::move(issue));
        };
        for (const auto& issue : engineIssues) add(issue.code, issue.message, issue.command);
        if (model) {
            for (const auto& issue : model->issues) add(issue.code, issue.message, "lspMcpp.showLogs");
        }
        for (const auto& issue : plan.issues) {
            // sdk-missing is reported once below, workspace-wide, with the fix command (W5.4).
            if (issue.code == "sdk-missing") continue;
            add(issue.code, std::format("{} ({})", issue.message, base::file_name(issue.file)), "");
        }
        if (!options.trusted) add("untrusted-workspace", "the workspace is not trusted: build tools and compilers are not run", "");
        if (kit && spec::requires_macos_sdk(*kit) && macosSdk.empty()) {
            add("sdk-missing", "the macOS SDK was not found; install the Command Line Tools", "lspMcpp.installCommandLineTools", "Install Command Line Tools");
        }
        Json notices = Json::array();
        if (model) {
            for (const auto& notice : model->notices) notices.push_back(Json { { "code", notice.code }, { "message", notice.message } });
        }
        // usable plan W9.1: `project.root` is this WorkspaceRoot's own path, so a multi-root
        // session's several notifications (one per root, S3's backward-compatible addition) are
        // told apart by it, exactly as a single-root session's one notification always named it.
        Json project { { "root", base::path_to_uri(root) }, { "source", model ? std::string { project::to_string(model->source) } : std::string { "inferred" } } };
        if (model) project["level"] = model->level;
        Json params {
            { "state", std::string { to_string(state) } },
            { "project", project },
            { "profile", profile_json() },
            { "engine", Json { { "name", "clangd" }, { "version", payload.clangdVersion.empty() ? std::string { "unknown" } : payload.clangdVersion } } },
            { "issues", issues },
        };
        if (!notices.empty()) params["notices"] = std::move(notices);
        if (primer.busy()) {
            const auto [done, total] = primer.progress();
            params["progress"] = Json { { "done", done }, { "total", total } };
        }
        std::string serialized { lsp::dump(params) };
        if (serialized == lastStatus) return;
        lastStatus = std::move(serialized);
        notify_client("cxxModules/status", std::move(params));
    }

    // ---- timers -----------------------------------------------------------------------

    void handle_timers() {
        const auto now = Clock::now();
        std::vector<std::int64_t> expired;
        for (const auto& [id, request] : pending) {
            if (request.deadline <= now) expired.push_back(id);
        }
        bool restart { false };
        for (const auto id : expired) {
            PendingRequest request { std::move(pending[id]) };
            pending.erase(id);
            switch (request.purpose) {
            case Purpose::client: {
                log::warning("clangd ({}) did not answer {} in time", root, request.method);
                reply(request.clientId, local_fallback(request.merge, request.params, path_of_uri(request.uri)));
                (void)send_engine(lsp::make_notification("$/cancelRequest", Json { { "id", id } }));
                // A file whose modules are still being built is slow, not stuck: restarting would throw that work away.
                if (awaitingDiagnostics.contains(client_uri(request.uri))) break;
                add_engine_issue(Issue { "engine-timeout", std::format("clangd did not answer {} in time", request.method), "lspMcpp.restartServer" });
                if (++timeoutsByUri[request.uri] >= 3) restart = true;
                break;
            }
            case Purpose::engine_initialize:
                log::error("clangd ({}) did not answer initialize", root);
                add_engine_issue(Issue { "engine-timeout", "clangd did not answer initialize", "lspMcpp.restartServer" });
                answer_initialize_settled(Json::object());
                restart = true;
                break;
            case Purpose::engine_shutdown: reply(request.clientId, nullptr); break;
            }
        }
        if (restart) restart_engine("repeated timeouts");
        std::vector<std::string> overdue;
        for (const auto& [module, at] : primeDeadlines) {
            if (at <= now) overdue.push_back(module);
        }
        for (const auto& module : overdue) {
            log::warning("stopped waiting for module {} to be prepared ({})", module, root);
            if (const auto* planned = primer.find(module)) (void)finish_prime(base::path_to_uri(planned->primeFile));
        }
        if (reloadAt && *reloadAt <= now) {
            reloadAt.reset();
            start_model_load();
        }
        if (replanAt && *replanAt <= now) replan();
        if (restartAt && *restartAt <= now) {
            restartAt.reset();
            restart_engine("recovering from an exit");
        }
        if (sdkCheckAt && *sdkCheckAt <= now) {
            sdkCheckAt.reset();
            if (kit && spec::requires_macos_sdk(*kit) && macosSdk.empty()) {
                if (std::string found { macos_sdk_path() }; !found.empty()) {
                    log::info("the macOS SDK appeared at {}; re-probing and refreshing the model ({})", found, root);
                    macosSdk = found;
                    start_model_load();   // re-probes toolchains and replans; no restart of the server itself
                } else {
                    sdkCheckAt = now + std::chrono::seconds { 30 };
                }
            }
        }
        if (loadGiveUpAt && *loadGiveUpAt <= now) {
            loadGiveUpAt.reset();
            if (!firstPlanWritten) {
                log::warning("the project model ({}) is still loading; serving without it", root);
                firstPlanWritten = true;
                accept_traffic_if_ready();
            }
        }
        if (!expired.empty()) update_status();
    }
};

// ---- WorkspaceRoot ------------------------------------------------------------------------

WorkspaceRoot::WorkspaceRoot(std::string root, std::string key, SessionOptions options, PayloadPaths payload, bool payloadCorrupt,
                            bool kitEnabled, std::string compilerOverride, std::shared_ptr<EventChannel> events)
    : root_ { root }, key_ { key },
      impl_ { std::make_unique<Impl>(std::move(root), std::move(key), std::move(options), std::move(payload), payloadCorrupt, kitEnabled,
                                    std::move(compilerOverride), std::move(events)) } {}

WorkspaceRoot::~WorkspaceRoot() = default;

bool WorkspaceRoot::owns_path(std::string_view path) const { return !path.empty() && base::is_within(path, root_); }

void WorkspaceRoot::start(Json clientParams, bool clientSupportsStatus, bool usePolling, std::function<void(Json)> onEngineSettled) {
    impl_->clientParams = std::move(clientParams);
    impl_->clientSupportsStatus = clientSupportsStatus;
    impl_->onEngineSettled = std::move(onEngineSettled);
    if (usePolling) impl_->start_watch_polling();
    log::info("lsp-mcpp {} ({}) root {}", base::VERSION, lspmcpp::os::FAMILY_NAME, root_);
    log::info("clangd {} at {}", impl_->payload.clangdVersion.empty() ? "?" : impl_->payload.clangdVersion,
              impl_->payload.clangd.empty() ? "(none)" : impl_->payload.clangd);
    if (!impl_->payloadCorrupt && impl_->kitEnabled && !impl_->payload.kit.empty()) {
        if (auto kit = spec::load_kit(impl_->payload.kit)) {
            impl_->kit = std::move(*kit);
            if (spec::requires_macos_sdk(*impl_->kit)) {
                impl_->macosSdk = macos_sdk_path();
                // usable plan W5.4 / U7: keep looking every 30s so installing the Command Line
                // Tools while the server runs is picked up without a restart.
                if (impl_->macosSdk.empty()) impl_->sdkCheckAt = Clock::now() + std::chrono::seconds { 30 };
            }
            log::info("semantic kit {} at {} ({})", impl_->kit->name, impl_->kit->root, root_);
        } else {
            log::warning("semantic kit unusable ({}): {}", root_, kit.error().message);
        }
    }
    impl_->restore_cached_model();
    impl_->start_model_load();
    impl_->start_engine();
    impl_->loadGiveUpAt = Clock::now() + std::chrono::seconds { 120 };
}

void WorkspaceRoot::allow_status_notifications() {
    if (impl_->initializeAnswered) return;
    impl_->initializeAnswered = true;
    impl_->update_status();
}

void WorkspaceRoot::shut_down() {
    if (impl_->engine->running() && impl_->engineHandshakeDone) (void)impl_->send_engine(lsp::make_notification("exit", nullptr));
    impl_->engine->stop(std::chrono::seconds { 2 });
}

void WorkspaceRoot::did_open(const Json& params) {
    const Json* item { lsp::find(params, "textDocument") };
    if (item == nullptr) return;
    const std::string uri { item->value("uri", std::string {}) };
    const std::string path { impl_->path_of_uri(uri) };
    const Document& document = impl_->documents.open(uri, path, item->value("languageId", std::string { "cpp" }),
                                                     item->value("version", std::int64_t { 0 }), item->value("text", std::string {}));
    if (!path.empty()) {
        impl_->index.update(path, document.text);
        impl_->note_structure_change(path);
    }
    impl_->publish_diagnostics(uri);
    if (impl_->engineAccepting && !impl_->path_excluded(path)) {
        impl_->open_in_engine(document);
        impl_->prepare_imports_of(document);
    }
}

void WorkspaceRoot::did_change(const Json& message, const Json& params) {
    const std::string uri { Impl::uri_of_params(params) };
    const std::int64_t version { lsp::find_path(params, { "textDocument", "version" }) != nullptr
                                     ? params["textDocument"].value("version", std::int64_t { 0 }) : 0 };
    if (!impl_->documents.change(uri, version, params.value("contentChanges", Json::array()))) return;
    const Document* document { impl_->documents.find(uri) };
    if (!document->path.empty()) {
        impl_->index.update(document->path, document->text);
        impl_->note_structure_change(document->path);
    }
    impl_->publish_diagnostics(uri);
    if (impl_->engineAccepting && !impl_->path_excluded(document->path)) (void)impl_->send_engine(message);
}

void WorkspaceRoot::did_close(const Json& message, const Json& params) {
    const std::string uri { Impl::uri_of_params(params) };
    const Document* document { impl_->documents.find(uri) };
    if (document == nullptr) return;
    const std::string path { document->path };
    const bool wasExcluded { impl_->path_excluded(path) };
    impl_->documents.close(uri);
    if (!path.empty()) {
        if (auto text = platform::fs::read_file(path)) impl_->index.update(path, *text);
    }
    impl_->awaitingDiagnostics.erase(uri);
    if (impl_->engineAccepting && !wasExcluded) (void)impl_->send_engine(message);
    impl_->release_prime_units_if_idle();
    // Diagnostics of a closed file are cleared; the engine may send its own empty set too.
    impl_->publishedDiagnostics.erase(uri);
    impl_->engineDiagnostics.erase(uri);
    notify_client("textDocument/publishDiagnostics", Json { { "uri", uri }, { "diagnostics", Json::array() } });
    impl_->update_status();
}

void WorkspaceRoot::did_save(const Json& message, const Json& params) {
    const std::string uri { Impl::uri_of_params(params) };
    const std::string path { impl_->path_of_uri(uri) };
    if (!path.empty() && !impl_->path_excluded(path) && impl_->engineAccepting) (void)impl_->send_engine(message);
    impl_->retry_failed_modules();
}

void WorkspaceRoot::handle_watched_files(const Json& changes) {
    bool reload { false };
    bool replan { false };
    for (const auto& change : changes) {
        const std::string path { impl_->path_of_uri(change.value("uri", std::string {})) };
        if (path.empty()) continue;
        const std::string_view name { base::file_name(path) };
        const int type { change.value("type", 2) };
        if (is_build_file(name)) {
            // mcpp rewrites its own compile_commands.json while the model loads.
            if (name == "compile_commands.json" && impl_->model && impl_->model->source == project::SourceKind::mcpp) continue;
            reload = true;
            continue;
        }
        if (!project::is_cxx_source_name(path) || impl_->documents.find_by_path(path) != nullptr) continue;
        if (type == 3) {
            impl_->index.remove(path);
        } else if (auto text = platform::fs::read_file(path)) {
            impl_->index.update(path, *text);
        }
        if (impl_->model && impl_->model->source == project::SourceKind::inferred && type != 2) reload = true;
        else replan = true;
    }
    if (reload || replan) impl_->retry_failed_modules();
    if (reload) impl_->schedule_reload();
    else if (replan) impl_->schedule_replan();
    if (impl_->engineAccepting) {
        Json message = lsp::make_notification("workspace/didChangeWatchedFiles", Json { { "changes", changes } });
        (void)impl_->send_engine(message);
    }
    for (const Document* document : impl_->documents.all()) impl_->publish_diagnostics(document->uri);
}

void WorkspaceRoot::cancel(const Json& id) {
    for (auto it = impl_->deferred.begin(); it != impl_->deferred.end(); ++it) {
        if (lsp::kind_of(*it) == lsp::Kind::request && (*it)["id"] == id) {
            reply_error(id, lsp::REQUEST_CANCELLED, "cancelled");
            impl_->deferred.erase(it);
            return;
        }
    }
    for (const auto& [engineId, request] : impl_->pending) {
        if (request.purpose == Purpose::client && request.clientId == id) {
            (void)impl_->send_engine(lsp::make_notification("$/cancelRequest", Json { { "id", engineId } }));
            return;
        }
    }
}

void WorkspaceRoot::route_client_request(const Json& message) { impl_->route_client_request(message); }

void WorkspaceRoot::handle_client_response(const Json& clientResponse, int generation, const Json& engineRequestId) {
    if (generation != impl_->engineGeneration) return;
    Json forwarded = clientResponse;
    forwarded["id"] = engineRequestId;
    (void)impl_->send_engine(forwarded);
}

void WorkspaceRoot::forward_other_notification(const Json& message) {
    if (impl_->engineAccepting) {
        (void)impl_->send_engine(message);
    } else if (!impl_->engineUnavailable) {
        impl_->deferred.push_back(message);
    }
}

Json WorkspaceRoot::graph() const { return impl_->index.graph(); }

Json WorkspaceRoot::module_info(std::string_view name) const { return impl_->index.module_info(name); }

Json WorkspaceRoot::module_info_at(std::string_view path, base::Position at) const {
    const auto hit = impl_->index.module_at(path, at);
    return hit ? impl_->index.module_info(hit->name) : Json(nullptr);
}

Json WorkspaceRoot::contexts() const {
    Json available = Json::array();
    available.push_back(Json { { "id", "default" }, { "label", "All targets" }, { "profile", impl_->profile_json() } });
    if (impl_->model) {
        for (const auto& set : impl_->model->database.sets) {
            std::string label { set.name };
            if (!set.kind.empty() && set.kind != "other") label += std::format(" ({})", set.kind);
            available.push_back(Json { { "id", set.name }, { "label", label }, { "profile", impl_->profile_json() } });
        }
    }
    return Json { { "current", impl_->contextSet.empty() ? std::string { "default" } : impl_->contextSet }, { "available", available } };
}

void WorkspaceRoot::set_context(const Json& id, std::string_view context) {
    if (context != "default" && (!impl_->model || !spec::find_set(impl_->model->database, context))) {
        reply_error(id, lsp::INVALID_PARAMS, std::format("unknown context {}", context));
        return;
    }
    impl_->contextSet = context == "default" ? std::string {} : std::string { context };
    reply(id, nullptr);
    if (impl_->model) impl_->replan();
}

void WorkspaceRoot::handle_engine_message(int generation, const Json& message) {
    if (generation != impl_->engineGeneration) return;
    impl_->handle_engine_message(message);
}

void WorkspaceRoot::handle_engine_closed(int generation) {
    if (generation != impl_->engineGeneration) return;
    impl_->handle_engine_closed(false);
}

void WorkspaceRoot::handle_module_failure(int generation, const Json& failure) {
    if (generation != impl_->engineGeneration) return;
    impl_->handle_module_failure(failure);
}

void WorkspaceRoot::handle_model_loaded(int generation, std::shared_ptr<project::ProjectModel> model) {
    if (generation != impl_->modelGeneration) return;
    impl_->handle_model_loaded(std::move(model));
}

std::optional<Clock::time_point> WorkspaceRoot::next_deadline() const { return impl_->next_deadline(); }

void WorkspaceRoot::handle_timers() { impl_->handle_timers(); }

} // namespace lspmcpp::server
