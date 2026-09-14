module lspmcpp.server.session;

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
import lspmcpp.engine;
import lspmcpp.engine.clangd;
import lspmcpp.server.documents;
import lspmcpp.server.payload;
import lspmcpp.server.primer;
import lspmcpp.server.router;

namespace lspmcpp::server {

namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace log = base::log;

enum class EventKind { client_message, client_closed, engine_message, engine_closed, engine_module_failed, model_loaded };

struct Event {
    EventKind kind { EventKind::client_message };
    Json message;
    int generation { 0 };
    std::shared_ptr<project::ProjectModel> model;
};

using EventChannel = platform::Channel<Event>;

enum class State { starting, loading, preparing, ready, degraded, error };

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

enum class Purpose { client, engine_initialize, engine_shutdown };

struct PendingRequest {
    Purpose purpose { Purpose::client };
    Json clientId;
    std::string method;
    std::string uri;
    Merge merge { Merge::none };
    Json params;
    Clock::time_point deadline;
    int generation { 0 };
};

struct Issue {
    std::string code;
    std::string message;
    std::string command;   // a VS Code command id, optional
};

constexpr std::chrono::milliseconds INTERACTIVE_TIMEOUT { std::chrono::seconds { 10 } };

constexpr std::array<std::string_view, 6> BUILD_FILES { "mcpp.toml", "mcpp.lock", "CMakeLists.txt", "CMakePresets.json",
                                                        "compile_commands.json", "build_database.json" };

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

// The module structure of a scan, for deciding whether an edit changes the engine database.
std::string structure_of(const project::ScanResult& scan) {
    std::string key { project::provided_name(scan) };
    key += "|" + std::string { spec::to_string(project::role_of(scan)) };
    for (const auto& name : project::required_names(scan)) key += "|" + name;
    return key;
}

class Session {
private:
    SessionOptions options_;
    std::shared_ptr<EventChannel> events_ { std::make_shared<EventChannel>() };

    // Editor side.
    bool initializeReceived_ { false };
    bool initializeAnswered_ { false };
    bool clientInitialized_ { false };
    bool shutdownRequested_ { false };
    bool exitRequested_ { false };
    Json clientInitializeId_;
    Json clientParams_;
    Json clientCapabilities_;
    std::string compilerOverride_;
    bool kitEnabled_ { true };
    std::int64_t nextServerRequest_ { 1 };

    // Workspace.
    std::string root_;
    std::string cacheDirectory_;
    std::string databaseDirectory_;
    PayloadPaths payload_;
    std::optional<spec::Kit> kit_;
    std::string macosSdk_;
    DocumentStore documents_;
    mutable std::unordered_map<std::string, std::string> canonicalByUri_;
    index::ModuleIndex index_;
    spec::MetadataReader metadataReader_ { spec::caching_metadata_reader() };

    // Project model and plan.
    std::shared_ptr<project::ProjectModel> model_;
    int modelGeneration_ { 0 };
    bool loading_ { false };
    bool reloadAfterLoad_ { false };
    normalize::EnginePlan plan_;
    std::set<std::string> excluded_;          // path keys
    std::string writtenDatabase_;
    std::string writtenStructure_;            // the written database without module hints
    bool firstPlanWritten_ { false };
    std::string contextSet_;
    std::map<std::string, std::string, std::less<>> structures_;   // path key -> module structure at planning time

    // Modules clangd could not build, by name, with the reason; cleared when sources change.
    std::map<std::string, std::string, std::less<>> failedModules_;

    // Parallel module preparation (primer.cppm): `import M;` units opened in the engine.
    Primer primer_;
    std::string primeDirectory_;
    std::string moduleHintDirectory_;
    std::map<std::string, std::string, std::less<>> primeModuleByPath_;           // path key of a prime unit being prepared -> module
    std::map<std::string, Clock::time_point, std::less<>> primeDeadlines_;        // module -> when to stop waiting for it
    // Prepared prime units still open. clangd keeps a module it built only while an open file
    // holds it; a unit opened after the last holder closed validates and copies every module it
    // reaches again. So they stay open until nothing is being prepared or waited for.
    std::map<std::string, std::string, std::less<>> heldPrimeUnits_;              // path key -> path

    // Engine (usable plan W9.5): only ever used through the interface, so a test can drive the
    // session with a fake instead of a real clangd process.
    std::unique_ptr<engine::Engine> engine_;
    int engineGeneration_ { 0 };
    bool engineHandshakeDone_ { false };
    bool engineAccepting_ { false };
    bool engineUnavailable_ { false };
    Json engineCapabilities_;
    std::map<std::int64_t, PendingRequest> pending_;
    std::int64_t nextEngineId_ { 1 };
    std::map<std::string, std::pair<int, Json>, std::less<>> engineToClient_;
    std::vector<Json> deferred_;
    std::map<std::string, Json, std::less<>> engineDiagnostics_;
    std::map<std::string, std::string, std::less<>> publishedDiagnostics_;
    std::set<std::string> awaitingDiagnostics_;
    std::map<std::string, int, std::less<>> timeoutsByUri_;
    std::deque<Clock::time_point> crashes_;
    std::vector<Issue> engineIssues_;

    // Timers.
    std::optional<Clock::time_point> reloadAt_;
    std::optional<Clock::time_point> replanAt_;
    std::optional<Clock::time_point> restartAt_;
    std::optional<Clock::time_point> loadGiveUpAt_;
    std::optional<Clock::time_point> sdkCheckAt_;   // usable plan W5.4: re-checks a missing macOS SDK

    State lastState_ { State::starting };
    std::string lastStatus_;

public:
    explicit Session(SessionOptions options)
        : options_ { std::move(options) },
          engine_ { options_.engineFactory ? options_.engineFactory() : std::make_unique<engine::Clangd>() } {}

    int run() {
        start_input_reader_();
        while (!exitRequested_) {
            const auto deadline = next_deadline_();
            std::optional<Event> event { deadline ? events_->pop_until(*deadline) : events_->pop() };
            if (event) handle_(*event);
            handle_timers_();
        }
        engine_->stop(std::chrono::seconds { 2 });
        return shutdownRequested_ ? 0 : 1;
    }

private:
    // ---- plumbing ---------------------------------------------------------

    void start_input_reader_() {
        std::thread { [events = events_] {
            lsp::FrameReader reader;
            while (true) {
                auto chunk = platform::stdio::read_input();
                if (!chunk || chunk->empty()) break;
                reader.feed(*chunk);
                while (auto message = reader.next()) {
                    if (!*message) {
                        log::warning("dropped a malformed frame from the client: {}", message->error().message);
                        continue;
                    }
                    events->push(Event { EventKind::client_message, std::move(**message) });
                }
            }
            events->push(Event { EventKind::client_closed });
        } }.detach();
    }

    void send_client_(const Json& message) {
        if (auto written = platform::stdio::write_output(lsp::encode_frame(message)); !written) {
            log::error("cannot write to the client: {}", written.error().message);
        }
    }

    void reply_(const Json& id, Json result) { send_client_(lsp::make_result(id, std::move(result))); }

    void reply_error_(const Json& id, int code, std::string_view message) { send_client_(lsp::make_error(id, code, message)); }

    void notify_client_(std::string_view method, Json params) { send_client_(lsp::make_notification(method, std::move(params))); }

    // The engine is given every file under the one name the model uses for it,
    // because clangd matches an unsaved buffer to the module source it builds by
    // exact name. On Windows that is the long name in clangd's own spelling,
    // "file:///C:/Users/runneradmin/f.cppm" where an editor wrote
    // "file:///c%3A/Users/RUNNER~1/f.cppm"; elsewhere it is the name with symbolic
    // links followed, "/private/var/..." where the editor opened "/var/...".
    std::string engine_uri_(std::string_view uri) const {
        if constexpr (lspmcpp::os::FAMILY == lspmcpp::os::Family::windows) {
            const std::string path { path_of_uri_(uri) };
            if (path.size() < 2 || path[1] != ':') return std::string { uri };
            return "file:///" + path.substr(0, 2) + base::percent_encode_path(std::string_view { path }.substr(2));
        } else {
            const auto written = base::uri_to_path(uri);
            if (!written) return std::string { uri };
            const std::string path { path_of_uri_(uri) };
            return path.empty() || path == *written ? std::string { uri } : base::path_to_uri(path);
        }
    }

    Json engine_view_(const Json& message) const {
        Json copy = message;
        const auto fix = [&](Json& uri) {
            if (uri.is_string()) uri = engine_uri_(uri.get<std::string>());
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
    std::string client_uri_(std::string_view engineUri) const {
        if (documents_.find(engineUri) != nullptr) return std::string { engineUri };
        const std::string path { path_of_uri_(engineUri) };
        if (const Document* document = path.empty() ? nullptr : documents_.find_by_path(path)) return document->uri;
        return std::string { engineUri };
    }

    // An engine message with every location in an open document named the way
    // the client named that document, so a definition in a file the editor has
    // open does not arrive as a second file.
    void client_view_(Json& value) const {
        if (value.is_array()) {
            for (auto& element : value) client_view_(element);
            return;
        }
        if (!value.is_object()) return;
        for (auto item = value.begin(); item != value.end(); ++item) {
            if ((item.key() == "uri" || item.key() == "targetUri") && item.value().is_string()) {
                item.value() = client_uri_(item.value().get<std::string>());
            } else if (item.key() == "changes" && item.value().is_object()) {
                Json renamed = Json::object();
                for (auto change = item.value().begin(); change != item.value().end(); ++change) renamed[client_uri_(change.key())] = change.value();
                item.value() = std::move(renamed);
            } else {
                client_view_(item.value());
            }
        }
    }

    bool send_engine_(const Json& message) {
        if (!engine_->running()) return false;
        if (auto sent = engine_->send(engine_view_(message)); !sent) {
            log::warning("cannot write to clangd: {}", sent.error().message);
            return false;
        }
        return true;
    }

    // A file's path under the one name the model uses for it (see engine_uri_).
    std::string path_of_uri_(std::string_view uri) const {
        const std::string key { uri };
        if (const auto cached = canonicalByUri_.find(key); cached != canonicalByUri_.end()) return cached->second;
        auto path = base::uri_to_path(uri);
        std::string canonical { path ? platform::fs::canonical_path(*path) : std::string {} };
        if (canonicalByUri_.size() > 4096) canonicalByUri_.clear();
        canonicalByUri_.emplace(key, canonical);
        return canonical;
    }

    static std::string uri_of_params_(const Json& params) {
        const Json* uri { lsp::find_path(params, { "textDocument", "uri" }) };
        return uri != nullptr && uri->is_string() ? uri->get<std::string>() : std::string {};
    }

    std::optional<Clock::time_point> next_deadline_() const {
        std::optional<Clock::time_point> deadline;
        auto consider = [&](const std::optional<Clock::time_point>& at) {
            if (at && (!deadline || *at < *deadline)) deadline = at;
        };
        for (const auto& [id, request] : pending_) consider(request.deadline);
        consider(reloadAt_);
        consider(replanAt_);
        consider(restartAt_);
        consider(loadGiveUpAt_);
        consider(sdkCheckAt_);
        for (const auto& [module, at] : primeDeadlines_) consider(at);
        return deadline;
    }

    // ---- dispatch -----------------------------------------------------------

    void handle_(Event& event) {
        switch (event.kind) {
        case EventKind::client_message: handle_client_(event.message); break;
        case EventKind::client_closed:
            log::info("the client closed its input");
            exitRequested_ = true;
            break;
        case EventKind::engine_message:
            if (event.generation == engineGeneration_) handle_engine_(event.message);
            break;
        case EventKind::engine_closed:
            if (event.generation == engineGeneration_) handle_engine_closed_();
            break;
        case EventKind::engine_module_failed:
            if (event.generation == engineGeneration_) handle_module_failure_(event.message);
            break;
        case EventKind::model_loaded: handle_model_loaded_(event.generation, std::move(event.model)); break;
        }
    }

    void handle_client_(const Json& message) {
        switch (lsp::kind_of(message)) {
        case lsp::Kind::request: handle_client_request_(message); break;
        case lsp::Kind::notification: handle_client_notification_(message); break;
        case lsp::Kind::response: handle_client_response_(message); break;
        case lsp::Kind::invalid: log::warning("ignored an invalid message from the client"); break;
        }
    }

    // ---- client requests ----------------------------------------------------

    void handle_client_request_(const Json& message) {
        const Json& id { message["id"] };
        const std::string method { message.value("method", std::string {}) };
        const Json params = message.contains("params") ? message["params"] : Json::object();
        if (method == lsp::method::INITIALIZE) {
            handle_initialize_(id, params);
            return;
        }
        if (!initializeReceived_) {
            reply_error_(id, lsp::SERVER_NOT_INITIALIZED, "the server is not initialized");
            return;
        }
        if (shutdownRequested_ && method != lsp::method::SHUTDOWN) {
            reply_error_(id, lsp::INVALID_REQUEST, "the server is shutting down");
            return;
        }
        if (method == lsp::method::SHUTDOWN) {
            handle_shutdown_(id);
            return;
        }
        if (method.starts_with("cxxModules/")) {
            handle_modules_request_(id, method, params);
            return;
        }
        route_client_request_(message);
    }

    void route_client_request_(const Json& message) {
        const Json& id { message["id"] };
        const std::string method { message.value("method", std::string {}) };
        const Json params = message.contains("params") ? message["params"] : Json::object();
        const std::string uri { uri_of_params_(params) };
        const std::string path { uri.empty() ? std::string {} : path_of_uri_(uri) };
        const Document* document { uri.empty() ? nullptr : documents_.find(uri) };
        RouteDecision decision { route_request(method, params, index_, path, document ? std::string_view { document->text } : std::string_view {}) };
        if (decision.route == Route::local) {
            reply_(id, std::move(decision.localResult));
            return;
        }
        const bool excluded { !path.empty() && excluded_.contains(base::path_key(path)) };
        if (engineUnavailable_ || excluded) {
            reply_(id, local_fallback_(decision.merge, params, path));
            return;
        }
        if (!engineAccepting_) {
            deferred_.push_back(message);
            return;
        }
        const std::int64_t engineId { nextEngineId_++ };
        const auto timeout = is_interactive(method) ? std::min(options_.requestTimeout, INTERACTIVE_TIMEOUT) : options_.requestTimeout;
        pending_[engineId] = PendingRequest { Purpose::client, id, method, uri, decision.merge, params, Clock::now() + timeout, engineGeneration_ };
        Json forwarded = message;
        forwarded["id"] = engineId;
        if (!send_engine_(forwarded)) {
            pending_.erase(engineId);
            reply_(id, local_fallback_(decision.merge, params, path));
        }
    }

    Json local_fallback_(Merge merge, const Json& params, std::string_view path) const {
        if (merge == Merge::document_symbols) return path.empty() ? Json::array() : index_.document_symbols(path);
        if (merge == Merge::workspace_symbols) return index_.workspace_symbols(params.value("query", std::string {}));
        return nullptr;
    }

    void handle_initialize_(const Json& id, const Json& params) {
        if (initializeReceived_) {
            reply_error_(id, lsp::INVALID_REQUEST, "initialize was already received");
            return;
        }
        initializeReceived_ = true;
        clientInitializeId_ = id;
        clientParams_ = params;
        clientCapabilities_ = params.value("capabilities", Json::object());
        if (const Json* init = lsp::find(params, "initializationOptions"); init != nullptr && init->is_object()) {
            if (auto compiler = lsp::string_at(*init, "compiler")) compilerOverride_ = *compiler;
            if (auto kit = lsp::string_at(*init, "semanticKit")) kitEnabled_ = *kit != "off";
        }
        root_ = platform::fs::canonical_path(workspace_root_(params));
        cacheDirectory_ = base::join_path(platform::dirs::cache_directory(), base::join_path("workspaces", project::workspace_key(root_)));
        databaseDirectory_ = base::join_path(cacheDirectory_, "contexts/default/cdb");
        primeDirectory_ = base::join_path(cacheDirectory_, "contexts/default/prime");
        moduleHintDirectory_ = base::join_path(cacheDirectory_, "contexts/default/module-hints");   // never created
        primer_.set_limit(std::max<std::size_t>(2, std::thread::hardware_concurrency()));
        (void)platform::fs::create_directories(databaseDirectory_);
        // clangd starts without a database; the first plan is written before any document reaches it.
        platform::fs::remove_all(base::join_path(databaseDirectory_, "compile_commands.json"));

        payload_ = resolve_payload(PayloadRequest { options_.payloadDirectory, options_.clangd, options_.kit });
        log::info("lsp-mcpp {} ({}) root {}", base::VERSION, lspmcpp::os::FAMILY_NAME, root_);
        log::info("clangd {} at {}", payload_.clangdVersion.empty() ? "?" : payload_.clangdVersion, payload_.clangd.empty() ? "(none)" : payload_.clangd);
        if (kitEnabled_ && !payload_.kit.empty()) {
            if (auto kit = spec::load_kit(payload_.kit)) {
                kit_ = std::move(*kit);
                if (spec::requires_macos_sdk(*kit_)) {
                    macosSdk_ = macos_sdk_path();
                    // usable plan W5.4 / U7: keep looking every 30s so installing the Command Line
                    // Tools while the server runs is picked up without a restart.
                    if (macosSdk_.empty()) sdkCheckAt_ = Clock::now() + std::chrono::seconds { 30 };
                }
                log::info("semantic kit {} at {}", kit_->name, kit_->root);
            } else {
                log::warning("semantic kit unusable: {}", kit.error().message);
            }
        }

        restore_cached_model_();
        start_model_load_();
        start_engine_();
        if (engineUnavailable_) {
            // No engine: answer at once with what the index can do.
            answer_initialize_(Json::object());
            update_status_();
        }
        loadGiveUpAt_ = Clock::now() + std::chrono::seconds { 120 };
    }

    std::string workspace_root_(const Json& params) const {
        if (const Json* folders = lsp::find(params, "workspaceFolders"); folders != nullptr && folders->is_array() && !folders->empty()) {
            if (auto uri = lsp::string_at(folders->front(), "uri")) {
                if (auto path = base::uri_to_path(*uri)) return *path;
            }
        }
        if (auto uri = lsp::string_at(params, "rootUri")) {
            if (auto path = base::uri_to_path(*uri)) return *path;
        }
        if (auto path = lsp::string_at(params, "rootPath"); path && !path->empty()) return base::normalize_path(*path);
        return platform::fs::current_directory();
    }

    void answer_initialize_(const Json& engineCapabilities) {
        if (initializeAnswered_) return;
        initializeAnswered_ = true;
        Json result {
            { "capabilities", merge_capabilities(engineCapabilities) },
            { "serverInfo", Json { { "name", "lsp-mcpp" }, { "version", std::string { base::VERSION } } } },
        };
        reply_(clientInitializeId_, std::move(result));
    }

    void handle_shutdown_(const Json& id) {
        shutdownRequested_ = true;
        if (engine_->running() && engineHandshakeDone_) {
            const std::int64_t engineId { nextEngineId_++ };
            pending_[engineId] = PendingRequest { Purpose::engine_shutdown, id, "shutdown", {}, Merge::none, {},
                                                  Clock::now() + std::chrono::seconds { 3 }, engineGeneration_ };
            if (send_engine_(lsp::make_request(engineId, "shutdown", nullptr))) return;
            pending_.erase(engineId);
        }
        reply_(id, nullptr);
    }

    void handle_modules_request_(const Json& id, std::string_view method, const Json& params) {
        if (method == "cxxModules/graph") {
            reply_(id, index_.graph());
        } else if (method == "cxxModules/moduleInfo") {
            if (auto name = lsp::string_at(params, "name")) {
                reply_(id, index_.module_info(*name));
                return;
            }
            const std::string path { path_of_uri_(uri_of_params_(params)) };
            const Json* position { lsp::find(params, "position") };
            if (path.empty() || position == nullptr) {
                reply_(id, nullptr);
                return;
            }
            const base::Position at { static_cast<int>(lsp::int_at(*position, "line").value_or(0)),
                                      static_cast<int>(lsp::int_at(*position, "character").value_or(0)) };
            const auto hit = index_.module_at(path, at);
            reply_(id, hit ? index_.module_info(hit->name) : Json(nullptr));
        } else if (method == "cxxModules/contexts") {
            reply_(id, contexts_());
        } else if (method == "cxxModules/setContext") {
            const std::string context { params.value("context", std::string { "default" }) };
            if (context != "default" && (!model_ || !spec::find_set(model_->database, context))) {
                reply_error_(id, lsp::INVALID_PARAMS, std::format("unknown context {}", context));
                return;
            }
            contextSet_ = context == "default" ? std::string {} : context;
            reply_(id, nullptr);
            if (model_) replan_();
        } else {
            reply_error_(id, lsp::METHOD_NOT_FOUND, std::format("unknown request {}", method));
        }
    }

    Json profile_json_() const {
        Json profile = Json::object();
        if (model_) {
            profile["kind"] = model_->profile.kind;
            if (!model_->profile.compiler.empty()) profile["compiler"] = model_->profile.compiler;
            profile["stdlib"] = model_->profile.stdlib;
            profile["target"] = model_->profile.target;
        } else if (kit_) {
            profile = Json { { "kind", "semantic-kit" }, { "stdlib", std::format("{} {}", kit_->stdlibName, kit_->stdlibVersion) }, { "target", kit_->target } };
        } else {
            profile = Json { { "kind", "semantic-kit" }, { "stdlib", "unknown" }, { "target", std::string { lspmcpp::os::VSCODE_TARGET } } };
        }
        return profile;
    }

    Json contexts_() const {
        Json available = Json::array();
        available.push_back(Json { { "id", "default" }, { "label", "All targets" }, { "profile", profile_json_() } });
        if (model_) {
            for (const auto& set : model_->database.sets) {
                std::string label { set.name };
                if (!set.kind.empty() && set.kind != "other") label += std::format(" ({})", set.kind);
                available.push_back(Json { { "id", set.name }, { "label", label }, { "profile", profile_json_() } });
            }
        }
        return Json { { "current", contextSet_.empty() ? std::string { "default" } : contextSet_ }, { "available", available } };
    }

    // ---- client notifications -------------------------------------------------

    void handle_client_notification_(const Json& message) {
        const std::string method { message.value("method", std::string {}) };
        const Json params = message.contains("params") ? message["params"] : Json::object();
        if (method == lsp::method::EXIT) {
            if (engine_->running()) (void)send_engine_(lsp::make_notification("exit", nullptr));
            exitRequested_ = true;
            return;
        }
        if (!initializeReceived_) return;
        if (method == lsp::method::INITIALIZED) {
            // The engine's own `initialized` is sent when its initialize result arrives.
            clientInitialized_ = true;
            register_watchers_();
            return;
        }
        if (method == lsp::method::TEXT_DOCUMENT_DID_OPEN) {
            did_open_(params);
            return;
        }
        if (method == lsp::method::TEXT_DOCUMENT_DID_CHANGE) {
            did_change_(message, params);
            return;
        }
        if (method == lsp::method::TEXT_DOCUMENT_DID_CLOSE) {
            did_close_(message, params);
            return;
        }
        if (method == lsp::method::TEXT_DOCUMENT_DID_SAVE) {
            const std::string uri { uri_of_params_(params) };
            const std::string path { path_of_uri_(uri) };
            if (!path.empty() && !excluded_.contains(base::path_key(path)) && engineAccepting_) (void)send_engine_(message);
            retry_failed_modules_();
            return;
        }
        if (method == lsp::method::WORKSPACE_DID_CHANGE_WATCHED_FILES) {
            did_change_watched_files_(message, params);
            return;
        }
        if (method == lsp::method::CANCEL_REQUEST) {
            cancel_(params);
            return;
        }
        if (method == lsp::method::WORKSPACE_DID_CHANGE_CONFIGURATION || method == lsp::method::SET_TRACE || !method.starts_with("$/")) {
            if (engineAccepting_) {
                (void)send_engine_(message);
            } else if (!engineUnavailable_) {
                deferred_.push_back(message);
            }
        }
    }

    void did_open_(const Json& params) {
        const Json* item { lsp::find(params, "textDocument") };
        if (item == nullptr) return;
        const std::string uri { item->value("uri", std::string {}) };
        const std::string path { path_of_uri_(uri) };
        const Document& document = documents_.open(uri, path, item->value("languageId", std::string { "cpp" }),
                                                   item->value("version", std::int64_t { 0 }), item->value("text", std::string {}));
        if (!path.empty()) {
            index_.update(path, document.text);
            note_structure_change_(path);
        }
        publish_diagnostics_(uri);
        if (engineAccepting_ && !path_excluded_(path)) {
            open_in_engine_(document);
            prepare_imports_of_(document);
        }
    }

    void did_change_(const Json& message, const Json& params) {
        const std::string uri { uri_of_params_(params) };
        const std::int64_t version { lsp::find_path(params, { "textDocument", "version" }) != nullptr
                                         ? params["textDocument"].value("version", std::int64_t { 0 }) : 0 };
        if (!documents_.change(uri, version, params.value("contentChanges", Json::array()))) return;
        const Document* document { documents_.find(uri) };
        if (!document->path.empty()) {
            index_.update(document->path, document->text);
            note_structure_change_(document->path);
        }
        publish_diagnostics_(uri);
        if (engineAccepting_ && !path_excluded_(document->path)) (void)send_engine_(message);
    }

    // A unit of the model whose module declaration or imports changed needs a new
    // plan; a new source file of an inferred model needs a new model.
    void note_structure_change_(std::string_view path) {
        if (!model_) return;
        const auto it = structures_.find(base::path_key(path));
        if (it != structures_.end()) {
            if (const auto* scan = index_.scan_of(path); scan != nullptr && it->second != structure_of(*scan)) schedule_replan_();
            return;
        }
        if (model_->source == project::SourceKind::inferred && project::is_cxx_source_name(path) && base::is_within(path, root_)) schedule_reload_();
    }

    void did_close_(const Json& message, const Json& params) {
        const std::string uri { uri_of_params_(params) };
        const Document* document { documents_.find(uri) };
        if (document == nullptr) return;
        const std::string path { document->path };
        const bool excluded { path_excluded_(path) };
        documents_.close(uri);
        if (!path.empty()) {
            if (auto text = platform::fs::read_file(path)) index_.update(path, *text);
        }
        awaitingDiagnostics_.erase(uri);
        if (engineAccepting_ && !excluded) (void)send_engine_(message);
        release_prime_units_if_idle_();
        // Diagnostics of a closed file are cleared; the engine may send its own empty set too.
        publishedDiagnostics_.erase(uri);
        engineDiagnostics_.erase(uri);
        notify_client_("textDocument/publishDiagnostics", Json { { "uri", uri }, { "diagnostics", Json::array() } });
        update_status_();
    }

    void did_change_watched_files_(const Json& message, const Json& params) {
        bool reload { false };
        bool replan { false };
        for (const auto& change : params.value("changes", Json::array())) {
            const std::string path { path_of_uri_(change.value("uri", std::string {})) };
            if (path.empty()) continue;
            const std::string_view name { base::file_name(path) };
            const int type { change.value("type", 2) };
            if (is_build_file(name)) {
                // mcpp rewrites its own compile_commands.json while the model loads.
                if (name == "compile_commands.json" && model_ && model_->source == project::SourceKind::mcpp) continue;
                reload = true;
                continue;
            }
            if (!project::is_cxx_source_name(path) || documents_.find_by_path(path) != nullptr) continue;
            if (type == 3) {
                index_.remove(path);
            } else if (auto text = platform::fs::read_file(path)) {
                index_.update(path, *text);
            }
            if (model_ && model_->source == project::SourceKind::inferred && type != 2) reload = true;
            else replan = true;
        }
        if (reload || replan) retry_failed_modules_();
        if (reload) schedule_reload_();
        else if (replan) schedule_replan_();
        if (engineAccepting_) (void)send_engine_(message);
        for (const Document* document : documents_.all()) publish_diagnostics_(document->uri);
    }

    void cancel_(const Json& params) {
        const Json id = params.value("id", Json {});
        for (auto it = deferred_.begin(); it != deferred_.end(); ++it) {
            if (lsp::kind_of(*it) == lsp::Kind::request && (*it)["id"] == id) {
                reply_error_(id, lsp::REQUEST_CANCELLED, "cancelled");
                deferred_.erase(it);
                return;
            }
        }
        for (const auto& [engineId, request] : pending_) {
            if (request.purpose == Purpose::client && request.clientId == id) {
                (void)send_engine_(lsp::make_notification("$/cancelRequest", Json { { "id", engineId } }));
                return;
            }
        }
    }

    void handle_client_response_(const Json& message) {
        const Json& id { message["id"] };
        if (!id.is_string()) return;
        const std::string key { id.get<std::string>() };
        const auto it = engineToClient_.find(key);
        if (it == engineToClient_.end()) return;   // a response to this server's own request
        const auto [generation, engineId] = it->second;
        engineToClient_.erase(it);
        if (generation != engineGeneration_) return;
        Json forwarded = message;
        forwarded["id"] = engineId;
        (void)send_engine_(forwarded);
    }

    void register_watchers_() {
        const Json* dynamic { lsp::find_path(clientCapabilities_, { "workspace", "didChangeWatchedFiles", "dynamicRegistration" }) };
        if (dynamic == nullptr || !dynamic->is_boolean() || !dynamic->get<bool>()) return;
        Json watchers = Json::array();
        for (std::string_view glob : { "**/mcpp.toml", "**/mcpp.lock", "**/CMakeLists.txt", "**/CMakePresets.json",
                                       "**/compile_commands.json", "**/build_database.json",
                                       "**/*.{cppm,ccm,cxxm,c++m,ixx,mpp,mxx,cpp,cc,cxx}" }) {
            watchers.push_back(Json { { "globPattern", std::string { glob } } });
        }
        Json params { { "registrations", Json::array({ Json { { "id", "lsp-mcpp-watched-files" },
                                                              { "method", "workspace/didChangeWatchedFiles" },
                                                              { "registerOptions", Json { { "watchers", watchers } } } } }) } };
        send_client_(lsp::make_request(std::format("s:{}", nextServerRequest_++), "client/registerCapability", std::move(params)));
    }

    // ---- engine -------------------------------------------------------------------

    void start_engine_() {
        engineHandshakeDone_ = false;
        engineAccepting_ = false;
        if (payload_.clangd.empty() || !platform::fs::is_regular_file(payload_.clangd)) {
            engineUnavailable_ = true;
            add_engine_issue_(Issue { "engine-missing", "clangd was not found; only module-level features are available", "lspMcpp.showLogs" });
            flush_deferred_without_engine_();
            update_status_();
            return;
        }
        const int generation { ++engineGeneration_ };
        engine::EngineConfig config;
        config.executable = payload_.clangd;
        config.version = payload_.clangdVersion;
        config.databaseDirectory = databaseDirectory_;
        config.workDirectory = root_;
        config.verboseLog = options_.verboseEngineLog;
        // Extra engine arguments for troubleshooting, e.g. LSP_MCPP_ENGINE_ARGUMENTS="-j=8 --background-index-priority=background".
        if (auto extra = platform::env::get("LSP_MCPP_ENGINE_ARGUMENTS")) {
            for (auto word : base::split(*extra, ' ')) {
                if (!base::trim(word).empty()) config.extraArguments.emplace_back(base::trim(word));
            }
        }
        auto events = events_;
        auto started = engine_->start(
            config,
            [events, generation](Json message) { events->push(Event { EventKind::engine_message, std::move(message), generation }); },
            [events, generation] { events->push(Event { EventKind::engine_closed, {}, generation }); },
            [events, generation](std::string_view line) {
                log::info("clangd: {}", line);
                if (auto failure = engine::parse_module_failure(line)) {
                    events->push(Event { EventKind::engine_module_failed,
                                         Json { { "module", failure->module }, { "reason", failure->reason }, { "source", failure->failedSource } }, generation });
                }
            });
        if (!started) {
            engineUnavailable_ = true;
            add_engine_issue_(Issue { "engine-crashed", std::format("clangd could not start: {}", started.error().message), "lspMcpp.restartServer" });
            flush_deferred_without_engine_();
            update_status_();
            return;
        }
        engineUnavailable_ = false;
        Json params = clientParams_;
        params.erase("initializationOptions");
        params["processId"] = nullptr;
        // Positions are UTF-16 everywhere in this server.
        if (params.contains("capabilities") && params["capabilities"].is_object()) {
            params["capabilities"].erase("offsetEncoding");
            if (params["capabilities"].contains("general") && params["capabilities"]["general"].is_object()) {
                params["capabilities"]["general"].erase("positionEncodings");
            }
        }
        const std::int64_t engineId { nextEngineId_++ };
        pending_[engineId] = PendingRequest { Purpose::engine_initialize, nullptr, "initialize", {}, Merge::none, {},
                                              Clock::now() + std::chrono::seconds { 60 }, generation };
        (void)send_engine_(lsp::make_request(engineId, "initialize", std::move(params)));
    }

    void restart_engine_(std::string_view reason) {
        log::info("restarting clangd: {}", reason);
        // Requests to the old engine are answered with what the index knows.
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->second.purpose == Purpose::client) {
                reply_(it->second.clientId, local_fallback_(it->second.merge, it->second.params, path_of_uri_(it->second.uri)));
            }
            it = pending_.erase(it);
        }
        engineToClient_.clear();
        forget_primes_();
        ++engineGeneration_;   // late events of the old process are ignored
        engine_->stop(std::chrono::milliseconds { 500 });
        engineDiagnostics_.clear();
        timeoutsByUri_.clear();
        start_engine_();
    }

    void handle_engine_(const Json& message) {
        switch (lsp::kind_of(message)) {
        case lsp::Kind::response: handle_engine_response_(message); break;
        case lsp::Kind::request: {
            const std::string key { std::format("e:{}:{}", engineGeneration_, lsp::dump(message["id"])) };
            engineToClient_[key] = { engineGeneration_, message["id"] };
            Json forwarded = message;
            forwarded["id"] = key;
            if (forwarded.contains("params")) client_view_(forwarded["params"]);
            send_client_(forwarded);
            break;
        }
        case lsp::Kind::notification: handle_engine_notification_(message); break;
        case lsp::Kind::invalid: break;
        }
    }

    void handle_engine_response_(const Json& message) {
        const Json& id { message["id"] };
        if (!id.is_number_integer()) return;
        const auto it = pending_.find(id.get<std::int64_t>());
        if (it == pending_.end()) return;   // answered already, after a timeout
        PendingRequest request { std::move(it->second) };
        pending_.erase(it);
        switch (request.purpose) {
        case Purpose::engine_initialize: {
            engineCapabilities_ = lsp::find_path(message, { "result", "capabilities" }) != nullptr ? message["result"]["capabilities"] : Json::object();
            answer_initialize_(engineCapabilities_);
            (void)send_engine_(lsp::make_notification("initialized", Json::object()));
            engineHandshakeDone_ = true;
            accept_traffic_if_ready_();
            update_status_();
            break;
        }
        case Purpose::engine_shutdown: reply_(request.clientId, nullptr); break;
        case Purpose::client: {
            timeoutsByUri_.erase(request.uri);
            if (message.contains("error")) {
                Json forwarded = message;
                forwarded["id"] = request.clientId;
                send_client_(forwarded);
                return;
            }
            Json result = message.value("result", Json {});
            client_view_(result);
            const std::string path { request.uri.empty() ? std::string {} : path_of_uri_(request.uri) };
            if (request.merge == Merge::document_symbols && !path.empty()) {
                result = merge_document_symbols(result, index_.document_symbols(path));
            } else if (request.merge == Merge::workspace_symbols) {
                result = merge_workspace_symbols(result, index_.workspace_symbols(request.params.value("query", std::string {})));
            }
            reply_(request.clientId, std::move(result));
            break;
        }
        }
    }

    void handle_engine_notification_(const Json& message) {
        const std::string method { message.value("method", std::string {}) };
        if (method == lsp::method::TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS) {
            const Json& params { message["params"] };
            if (finish_prime_(params.value("uri", std::string {}))) return;
            const std::string uri { client_uri_(params.value("uri", std::string {})) };
            awaitingDiagnostics_.erase(uri);
            release_prime_units_if_idle_();
            if (documents_.find(uri) == nullptr) {
                Json forwarded = message;
                client_view_(forwarded["params"]);
                send_client_(forwarded);
            } else {
                engineDiagnostics_[uri] = params.value("diagnostics", Json::array());
                publish_diagnostics_(uri, true);
            }
            update_status_();
            return;
        }
        if (method == lsp::method::WINDOW_SHOW_MESSAGE) {
            // Nothing pops up from the engine; it goes to the log (design 16.2).
            Json forwarded = message;
            forwarded["method"] = "window/logMessage";
            send_client_(forwarded);
            return;
        }
        send_client_(message);
    }

    void handle_engine_closed_() {
        engineHandshakeDone_ = false;
        engineAccepting_ = false;
        forget_primes_();
        if (shutdownRequested_ || exitRequested_) return;
        log::warning("clangd exited unexpectedly");
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->second.purpose == Purpose::client) {
                reply_(it->second.clientId, local_fallback_(it->second.merge, it->second.params, path_of_uri_(it->second.uri)));
            }
            it = pending_.erase(it);
        }
        const auto now = Clock::now();
        crashes_.push_back(now);
        while (!crashes_.empty() && now - crashes_.front() > std::chrono::minutes { 3 }) crashes_.pop_front();
        add_engine_issue_(Issue { "engine-crashed", "clangd exited unexpectedly", "lspMcpp.restartServer" });
        if (crashes_.size() >= 3) {
            engineUnavailable_ = true;
            flush_deferred_without_engine_();
        } else {
            restartAt_ = now + std::chrono::seconds { 1 << (crashes_.size() - 1) };
        }
        update_status_();
    }

    void accept_traffic_if_ready_() {
        if (!engineHandshakeDone_ || !firstPlanWritten_ || engineAccepting_) return;
        engineAccepting_ = true;
        for (const Document* document : documents_.all()) {
            if (!path_excluded_(document->path)) open_in_engine_(*document);
        }
        std::vector<Json> deferred;
        deferred.swap(deferred_);
        for (const auto& message : deferred) {
            if (lsp::kind_of(message) == lsp::Kind::request) route_client_request_(message);
            else (void)send_engine_(message);
        }
        prepare_modules_();
        update_status_();
    }

    void flush_deferred_without_engine_() {
        std::vector<Json> deferred;
        deferred.swap(deferred_);
        for (const auto& message : deferred) {
            if (lsp::kind_of(message) != lsp::Kind::request) continue;
            const Json params = message.contains("params") ? message["params"] : Json::object();
            const std::string method { message.value("method", std::string {}) };
            const std::string path { path_of_uri_(uri_of_params_(params)) };
            const RouteDecision decision { route_request(method, params, index_, path, {}) };
            reply_(message["id"], decision.route == Route::local ? decision.localResult : local_fallback_(decision.merge, params, path));
        }
    }

    void open_in_engine_(const Document& document) {
        Json params { { "textDocument", Json { { "uri", document.uri }, { "languageId", document.languageId },
                                               { "version", document.version }, { "text", document.text } } } };
        if (send_engine_(lsp::make_notification("textDocument/didOpen", std::move(params)))) {
            if (!engineDiagnostics_.contains(document.uri)) awaitingDiagnostics_.insert(document.uri);
        }
    }

    bool path_excluded_(std::string_view path) const { return !path.empty() && excluded_.contains(base::path_key(path)); }

    void add_engine_issue_(Issue issue) {
        for (const auto& existing : engineIssues_) {
            if (existing.code == issue.code) return;
        }
        engineIssues_.push_back(std::move(issue));
    }

    // ---- model and plan -----------------------------------------------------------

    // The last model that loaded, so module-level features answer before this one does (design 13.1).
    std::string model_cache_path_() const { return base::join_path(cacheDirectory_, "model.json"); }

    void restore_cached_model_() {
        auto text = platform::fs::read_file(model_cache_path_());
        if (!text) return;
        const Json cached = Json::parse(*text, nullptr, false);
        if (cached.is_discarded() || !cached.is_object() || !cached.contains("database")) return;
        auto database = spec::from_json(cached["database"]);
        if (!database) return;
        std::size_t files { 0 };
        for (const auto& set : database->sets) {
            for (const auto& unit : set.units) {
                const std::string path { spec::absolute_source(unit) };
                if (index_.contains(path)) continue;
                if (auto source = platform::fs::read_file(path)) {
                    index_.update(path, *source);
                    ++files;
                }
            }
        }
        log::info("restored the previous model's module index ({} files)", files);
    }

    void save_model_cache_() const {
        if (!model_) return;
        Json envelope {
            { "source", std::string { project::to_string(model_->source) } },
            { "level", model_->level },
            { "database", Json::parse(spec::to_json(model_->database).dump()) },
        };
        (void)platform::fs::write_file_atomic(model_cache_path_(), envelope.dump());
    }

    void start_model_load_() {
        if (loading_) {
            reloadAfterLoad_ = true;
            return;
        }
        loading_ = true;
        const int generation { ++modelGeneration_ };
        project::LoadOptions load;
        load.trusted = options_.trusted;
        load.cacheDirectory = cacheDirectory_;
        load.compilerOverride = compilerOverride_;
        load.mcppExecutable = options_.mcpp;
        load.configuredDatabase = options_.database;
        load.discoverCompilers = options_.discoverCompilers;
        std::shared_ptr<const spec::Kit> kit = kit_ ? std::make_shared<const spec::Kit>(*kit_) : nullptr;
        const std::string root { root_ };
        const std::string probeCachePath { base::join_path(platform::dirs::cache_directory(), "toolchains/probe.json") };
        auto events = events_;
        std::thread { [events, generation, load, kit, root, probeCachePath]() mutable {
            toolchain::ProbeCache cache { probeCachePath };
            load.kit = kit.get();
            load.runner = toolchain::process_runner(std::chrono::seconds { 20 });
            load.probeCache = &cache;
            auto model = std::make_shared<project::ProjectModel>(project::load_project(root, load));
            events->push(Event { EventKind::model_loaded, {}, generation, std::move(model) });
        } }.detach();
        update_status_();
    }

    void handle_model_loaded_(int generation, std::shared_ptr<project::ProjectModel> model) {
        if (generation != modelGeneration_) return;
        loading_ = false;
        failedModules_.clear();
        loadGiveUpAt_.reset();
        model_ = std::move(model);
        log::info("project model: source {}, level {}, {} sets, profile {} {} {}", project::to_string(model_->source), model_->level,
                  model_->database.sets.size(), model_->profile.kind, model_->profile.compiler, model_->profile.stdlib);
        for (const auto& issue : model_->issues) log::info("model issue [{}] {}", issue.code, issue.message);

        save_model_cache_();
        index_.clear();
        for (const auto& set : model_->database.sets) {
            for (const auto& unit : set.units) {
                const std::string path { spec::absolute_source(unit) };
                if (index_.contains(path)) continue;
                if (const Document* document = documents_.find_by_path(path)) {
                    index_.update(path, document->text);
                } else if (auto text = platform::fs::read_file(path)) {
                    index_.update(path, *text);
                }
            }
        }
        for (const Document* document : documents_.all()) {
            if (!document->path.empty()) index_.update(document->path, document->text);
        }
        std::vector<std::pair<std::string, std::string>> manifests;
        for (auto& manifest : project::module_manifests(*model_, kit_ ? &*kit_ : nullptr)) manifests.emplace_back(manifest.path, manifest.origin);
        auto external = index::external_modules(manifests, metadataReader_);
        index_.set_external(std::move(external));
        const auto& profile = model_->profile;
        index_.set_profile_label(profile.kind == "semantic-kit" ? std::format("{} (semantic kit, {})", profile.stdlib, profile.target)
                                                                : std::format("{} · {} ({})", profile.compiler, profile.stdlib, profile.target));
        replan_();
        if (reloadAfterLoad_) {
            reloadAfterLoad_ = false;
            start_model_load_();
        }
    }

    void replan_() {
        replanAt_.reset();
        if (!model_) return;
        normalize::PlanInput input;
        input.database = &model_->database;
        input.contextSet = contextSet_;
        input.facts = &model_->facts;
        input.kit = kit_ ? &*kit_ : nullptr;
        input.engineDriverDirectory = payload_.clangd.empty() ? std::string {} : base::parent_path(payload_.clangd);
        input.macosSdk = macosSdk_;
        input.scanner = [this](std::string_view path) {
            if (const auto* scan = index_.scan_of(path)) return *scan;
            auto text = platform::fs::read_file(path);
            return text ? project::scan_source(*text) : project::ScanResult {};
        };
        input.metadataReader = metadataReader_;
        input.failedModules = failedModules_;
        input.primeDirectory = primeDirectory_;
        input.moduleHintDirectory = moduleHintDirectory_;
        normalize::EnginePlan plan { normalize::plan_engine(input) };
        write_prime_sources_(plan);
        const std::string database { normalize::to_compile_commands(plan).dump(1) };
        const std::string structure { normalize::to_compile_commands(plan, false).dump(1) };
        const bool changed { database != writtenDatabase_ };
        // Hints alone change as imports do; clangd rereads the database within five seconds.
        const bool structureChanged { structure != writtenStructure_ };
        writtenStructure_ = structure;
        if (changed) {
            if (auto pushed = engine_->push_database(database); !pushed) {
                log::error("cannot write the engine database: {}", pushed.error().message);
            }
            writtenDatabase_ = database;
            log::info("engine database: {} entries ({} standard library units), {} left out, {} issues", plan.entries.size(),
                      plan.stdUnits, plan.excludedFiles.size(), plan.issues.size());
        }
        std::set<std::string> excluded;
        for (const auto& file : plan.excludedFiles) excluded.insert(base::path_key(file));
        const bool restartNeeded { structureChanged && firstPlanWritten_ && engineHandshakeDone_ };
        if (!restartNeeded && engineAccepting_) {
            for (const Document* document : documents_.all()) {
                if (document->path.empty()) continue;
                const std::string key { base::path_key(document->path) };
                const bool wasExcluded { excluded_.contains(key) };
                const bool isExcluded { excluded.contains(key) };
                if (wasExcluded && !isExcluded) open_in_engine_(*document);
                if (!wasExcluded && isExcluded) {
                    (void)send_engine_(lsp::make_notification("textDocument/didClose", Json { { "textDocument", Json { { "uri", document->uri } } } }));
                }
            }
        }
        excluded_ = std::move(excluded);
        plan_ = std::move(plan);
        close_prime_units_();
        {
            std::vector<PrimeModule> modules;
            for (const auto& module : plan_.modules) modules.push_back(PrimeModule { module.name, module.requires_, module.primeFile });
            primer_.set_modules(std::move(modules));
        }
        structures_.clear();
        for (const auto& path : index_.files()) {
            if (const auto* scan = index_.scan_of(path)) structures_[base::path_key(path)] = structure_of(*scan);
        }
        firstPlanWritten_ = true;
        if (restartNeeded) {
            restart_engine_("the engine database changed");
        } else {
            accept_traffic_if_ready_();
            prepare_modules_();
        }
        for (const Document* document : documents_.all()) publish_diagnostics_(document->uri);
        update_status_();
    }

    // ---- parallel module preparation ----------------------------------------------------

    void write_prime_sources_(const normalize::EnginePlan& plan) {
        if (plan.primeSources.empty()) return;
        (void)platform::fs::create_directories(primeDirectory_);
        for (const auto& [file, content] : plan.primeSources) {
            if (platform::fs::read_file(file).value_or("") != content) (void)platform::fs::write_file(file, content);
        }
    }

    // The standard library and the imports of every open document, then as many ready modules as the limit allows.
    void prepare_modules_() {
        if (!engineAccepting_) return;
        const std::vector<std::string> standard { "std", "std.compat" };
        primer_.want(standard);
        for (const Document* document : documents_.all()) prepare_imports_of_(*document, false);
        pump_primer_();
    }

    void prepare_imports_of_(const Document& document, bool pump = true) {
        if (document.path.empty() || path_excluded_(document.path)) return;
        const auto* scan = index_.scan_of(document.path);
        if (scan == nullptr) return;
        const auto names = project::required_names(*scan);
        if (primer_.want(names) > 0 && pump) pump_primer_();
    }

    void pump_primer_() {
        if (!engineAccepting_) return;
        for (const PrimeModule* module : primer_.start_ready()) {
            const std::string uri { base::path_to_uri(module->primeFile) };
            Json params { { "textDocument", Json { { "uri", uri }, { "languageId", "cpp" }, { "version", 1 },
                                                   { "text", std::format("import {};\n", module->name) } } } };
            if (!send_engine_(lsp::make_notification("textDocument/didOpen", std::move(params)))) {
                primer_.finish(module->name);
                continue;
            }
            primeModuleByPath_[base::path_key(module->primeFile)] = module->name;
            primeDeadlines_[module->name] = Clock::now() + std::chrono::minutes { 3 };
        }
        update_status_();
    }

    // Diagnostics for a prime unit: when it was being prepared, its module is built (or failed,
    // which the log reports). True for any prime unit, whose diagnostics are nobody's.
    bool finish_prime_(std::string_view engineUri) {
        if (primeDirectory_.empty()) return false;
        auto path = base::uri_to_path(engineUri);
        if (!path || !base::is_within(*path, primeDirectory_)) return false;
        const std::string key { base::path_key(*path) };
        const auto it = primeModuleByPath_.find(key);
        if (it == primeModuleByPath_.end()) return true;
        const std::string module { it->second };
        primeModuleByPath_.erase(it);
        primeDeadlines_.erase(module);
        heldPrimeUnits_.emplace(key, *path);
        primer_.finish(module);
        pump_primer_();
        release_prime_units_if_idle_();
        return true;
    }

    void release_prime_units_if_idle_() {
        if (heldPrimeUnits_.empty() || primer_.busy() || !awaitingDiagnostics_.empty()) return;
        log::info("module preparation idle: closing {} prime units", heldPrimeUnits_.size());
        close_prime_units_();
    }

    // Every prime unit leaves the engine, prepared or not.
    void close_prime_units_() {
        std::set<std::string> uris;
        for (const auto& [key, module] : primeModuleByPath_) {
            if (const auto* planned = primer_.find(module)) uris.insert(base::path_to_uri(planned->primeFile));
        }
        for (const auto& [key, path] : heldPrimeUnits_) uris.insert(base::path_to_uri(path));
        if (engineAccepting_) {
            for (const auto& uri : uris) (void)send_engine_(lsp::make_notification("textDocument/didClose", Json { { "textDocument", Json { { "uri", uri } } } }));
        }
        primeModuleByPath_.clear();
        primeDeadlines_.clear();
        heldPrimeUnits_.clear();
    }

    // The engine's state is gone: nothing is open and nothing is known to be built.
    void forget_primes_() {
        primeModuleByPath_.clear();
        primeDeadlines_.clear();
        heldPrimeUnits_.clear();
        primer_.reset();
    }

    // clangd could not build a module: importers of it, and of the module whose source failed
    // to compile, leave the engine database and are answered at once, with an issue saying why.
    void handle_module_failure_(const Json& failure) {
        bool added { false };
        const std::string reason { failure.value("reason", std::string {}) };
        auto add = [&](std::string name) {
            if (name.empty() || failedModules_.contains(name)) return;
            log::warning("clangd could not build module {}: {}", name, reason);
            failedModules_.emplace(std::move(name), reason);
            added = true;
        };
        add(failure.value("module", std::string {}));
        if (const std::string source { failure.value("source", std::string {}) }; !source.empty()) {
            if (auto text = platform::fs::read_file(source)) add(project::provided_name(project::scan_source(*text)));
        }
        if (added) schedule_replan_();
    }

    // A change to sources or build files may have fixed a module that did not build.
    void retry_failed_modules_() {
        if (failedModules_.empty()) return;
        failedModules_.clear();
        schedule_replan_();
    }

    void schedule_replan_() { replanAt_ = Clock::now() + std::chrono::milliseconds { 800 }; }
    void schedule_reload_() { reloadAt_ = Clock::now() + std::chrono::milliseconds { 1500 }; }

    // ---- diagnostics and status -------------------------------------------------------

    void publish_diagnostics_(std::string_view uri, bool force = false) {
        const Document* document { documents_.find(uri) };
        if (document == nullptr) return;
        const Json moduleDiagnostics = document->path.empty() ? Json::array() : index_.diagnostics(document->path);
        const auto engine = engineDiagnostics_.find(uri);
        std::string label;
        if (model_) label = model_->profile.kind == "semantic-kit" ? model_->profile.stdlib + " kit" : model_->profile.compiler;
        Json merged = merge_diagnostics(engine == engineDiagnostics_.end() ? Json::array() : engine->second, moduleDiagnostics, label);
        std::string serialized { lsp::dump(merged) };
        auto& previous = publishedDiagnostics_[std::string { uri }];
        if (!force && previous == serialized) return;
        previous = std::move(serialized);
        notify_client_("textDocument/publishDiagnostics", Json { { "uri", std::string { uri } }, { "diagnostics", std::move(merged) } });
    }

    State compute_state_() const {
        if (engineUnavailable_ && crashes_.size() >= 3) return State::error;
        if (!model_) return loading_ ? State::loading : State::starting;
        if (loading_) return State::loading;
        if ((!awaitingDiagnostics_.empty() || primer_.busy()) && engineAccepting_) return State::preparing;
        // usable plan W5.4: degraded regardless of whether any open file happens to need std yet.
        if (kit_ && spec::requires_macos_sdk(*kit_) && macosSdk_.empty()) return State::degraded;
        if (!engineIssues_.empty() || !model_->issues.empty() || !plan_.issues.empty()) return State::degraded;
        return State::ready;
    }

    void update_status_() {
        const State state { compute_state_() };
        if (!initializeAnswered_) return;
        if (!client_supports(clientCapabilities_, "status")) {
            lastState_ = state;
            return;
        }
        Json issues = Json::array();
        auto add = [&](std::string_view code, std::string_view message, std::string_view command, std::string_view title = "Fix") {
            if (issues.size() >= 20) return;
            Json issue { { "code", std::string { code } }, { "message", std::string { message } } };
            if (!command.empty()) issue["command"] = Json { { "title", std::string { title } }, { "command", std::string { command } } };
            issues.push_back(std::move(issue));
        };
        for (const auto& issue : engineIssues_) add(issue.code, issue.message, issue.command);
        if (model_) {
            for (const auto& issue : model_->issues) add(issue.code, issue.message, "lspMcpp.showLogs");
        }
        for (const auto& issue : plan_.issues) {
            // sdk-missing is reported once below, workspace-wide, with the fix command (W5.4).
            if (issue.code == "sdk-missing") continue;
            add(issue.code, std::format("{} ({})", issue.message, base::file_name(issue.file)), "");
        }
        if (!options_.trusted) add("untrusted-workspace", "the workspace is not trusted: build tools and compilers are not run", "");
        if (kit_ && spec::requires_macos_sdk(*kit_) && macosSdk_.empty()) {
            add("sdk-missing", "the macOS SDK was not found; install the Command Line Tools", "lspMcpp.installCommandLineTools", "Install Command Line Tools");
        }
        Json notices = Json::array();
        if (model_) {
            for (const auto& notice : model_->notices) notices.push_back(Json { { "code", notice.code }, { "message", notice.message } });
        }
        Json project { { "root", base::path_to_uri(root_) }, { "source", model_ ? std::string { project::to_string(model_->source) } : std::string { "inferred" } } };
        if (model_) project["level"] = model_->level;
        Json params {
            { "state", std::string { to_string(state) } },
            { "project", project },
            { "profile", profile_json_() },
            { "engine", Json { { "name", "clangd" }, { "version", payload_.clangdVersion.empty() ? std::string { "unknown" } : payload_.clangdVersion } } },
            { "issues", issues },
        };
        if (!notices.empty()) params["notices"] = std::move(notices);
        if (primer_.busy()) {
            const auto [done, total] = primer_.progress();
            params["progress"] = Json { { "done", done }, { "total", total } };
        }
        std::string serialized { lsp::dump(params) };
        if (serialized == lastStatus_) return;
        lastStatus_ = std::move(serialized);
        lastState_ = state;
        notify_client_("cxxModules/status", std::move(params));
    }

    // ---- timers -----------------------------------------------------------------------

    void handle_timers_() {
        const auto now = Clock::now();
        std::vector<std::int64_t> expired;
        for (const auto& [id, request] : pending_) {
            if (request.deadline <= now) expired.push_back(id);
        }
        bool restart { false };
        for (const auto id : expired) {
            PendingRequest request { std::move(pending_[id]) };
            pending_.erase(id);
            switch (request.purpose) {
            case Purpose::client: {
                log::warning("clangd did not answer {} in time", request.method);
                reply_(request.clientId, local_fallback_(request.merge, request.params, path_of_uri_(request.uri)));
                (void)send_engine_(lsp::make_notification("$/cancelRequest", Json { { "id", id } }));
                // A file whose modules are still being built is slow, not stuck: restarting would throw that work away.
                if (awaitingDiagnostics_.contains(client_uri_(request.uri))) break;
                add_engine_issue_(Issue { "engine-timeout", std::format("clangd did not answer {} in time", request.method), "lspMcpp.restartServer" });
                if (++timeoutsByUri_[request.uri] >= 3) restart = true;
                break;
            }
            case Purpose::engine_initialize:
                log::error("clangd did not answer initialize");
                add_engine_issue_(Issue { "engine-timeout", "clangd did not answer initialize", "lspMcpp.restartServer" });
                answer_initialize_(Json::object());
                restart = true;
                break;
            case Purpose::engine_shutdown: reply_(request.clientId, nullptr); break;
            }
        }
        if (restart && !shutdownRequested_) restart_engine_("repeated timeouts");
        std::vector<std::string> overdue;
        for (const auto& [module, at] : primeDeadlines_) {
            if (at <= now) overdue.push_back(module);
        }
        for (const auto& module : overdue) {
            log::warning("stopped waiting for module {} to be prepared", module);
            if (const auto* planned = primer_.find(module)) (void)finish_prime_(base::path_to_uri(planned->primeFile));
        }
        if (reloadAt_ && *reloadAt_ <= now) {
            reloadAt_.reset();
            start_model_load_();
        }
        if (replanAt_ && *replanAt_ <= now) replan_();
        if (restartAt_ && *restartAt_ <= now) {
            restartAt_.reset();
            restart_engine_("recovering from an exit");
        }
        if (sdkCheckAt_ && *sdkCheckAt_ <= now) {
            sdkCheckAt_.reset();
            if (kit_ && spec::requires_macos_sdk(*kit_) && macosSdk_.empty()) {
                if (std::string found { macos_sdk_path() }; !found.empty()) {
                    log::info("the macOS SDK appeared at {}; re-probing and refreshing the model", found);
                    macosSdk_ = found;
                    start_model_load_();   // re-probes toolchains and replans; no restart of the server itself
                } else {
                    sdkCheckAt_ = now + std::chrono::seconds { 30 };
                }
            }
        }
        if (loadGiveUpAt_ && *loadGiveUpAt_ <= now) {
            loadGiveUpAt_.reset();
            if (!firstPlanWritten_) {
                log::warning("the project model is still loading; serving without it");
                firstPlanWritten_ = true;
                accept_traffic_if_ready_();
            }
        }
        if (!expired.empty()) update_status_();
    }
};

} // namespace

int run_session(const SessionOptions& options) {
    Session session { options };
    return session.run();
}

} // namespace lspmcpp::server
