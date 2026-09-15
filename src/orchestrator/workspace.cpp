module mcppls.orchestrator.workspace;

import std;
import nlohmann.json;
import mcppls.os;
import mcppls.base.error;
import mcppls.base.glob;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.base.uri;
import mcppls.base.version;
import mcppls.platform.dirs;
import mcppls.platform.fs;
import mcppls.platform.task;
import mcppls.lsp.jsonrpc;
import mcppls.lsp.protocol;
import mcppls.spec.database;
import mcppls.spec.kit;
import mcppls.spec.metadata;
import mcppls.toolchain.probe;
import mcppls.project.scan;
import mcppls.project.detect;
import mcppls.project.model;
import mcppls.normalize.plan;
import mcppls.engine;
import mcppls.engine.payload;
import mcppls.engine.native.index;
import mcppls.orchestrator.client;
import mcppls.orchestrator.documents;
import mcppls.orchestrator.instance;
import mcppls.orchestrator.routing;

namespace mcppls::orchestrator {

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

// Changes to cxxModules/status that keep its state are sent at most this often (S3 4).
constexpr std::chrono::milliseconds STATUS_COALESCE { 250 };

// The module structure of a scan, for deciding whether an edit changes the engine database.
std::string structure_of(const project::ScanResult& scan) {
    std::string key { project::provided_name(scan) };
    key += "|" + std::string { spec::to_string(project::role_of(scan)) };
    for (const auto& name : project::required_names(scan)) key += "|" + name;
    return key;
}

// The inputs a producer named in its database's watch list (S2 5), shared with the polling worker.
struct WatchPatterns {
    std::mutex mutex;
    std::vector<std::string> entries;   // glob patterns relative to the root, or absolute paths
    int generation { 0 };               // advanced whenever `entries` is replaced
};

// Whether `path` is one of `entries`: an absolute entry names the file, a relative one is a glob under `root`.
bool matches_watch_entries(std::span<const std::string> entries, std::string_view root, std::string_view path) {
    const auto relative = base::relative_path(path, root);
    return std::ranges::any_of(entries, [&](const std::string& entry) {
        if (base::is_absolute_path(entry)) return base::same_path(base::normalize_path(entry), path);
        return relative && base::glob_match(entry, *relative);
    });
}

// Whether a watch covers `path`: the build descriptions and sources every root watches, and the model's own entries.
bool watch_covers(std::span<const std::string> entries, std::string_view root, std::string_view path) {
    return is_build_file(base::file_name(path)) || project::is_cxx_source_name(path) || matches_watch_entries(entries, root, path);
}

std::map<std::string, platform::fs::FileStamp> watched_files_snapshot_of(std::string_view root, std::span<const std::string> entries) {
    std::map<std::string, platform::fs::FileStamp> files;
    for (const auto& file : platform::fs::list_files(root, {}, WATCH_POLL_SKIP_DIRECTORIES)) {
        if (!watch_covers(entries, root, file)) continue;
        if (auto fileStamp = platform::fs::stamp(file)) files.emplace(file, *fileStamp);
    }
    // An absolute entry outside the root is watched too; the database of S2 stream mode is one.
    for (const auto& entry : entries) {
        if (!base::is_absolute_path(entry) || base::is_within(entry, root)) continue;
        if (auto fileStamp = platform::fs::stamp(entry)) files.emplace(base::normalize_path(entry), *fileStamp);
    }
    return files;
}

std::string uri_of_params(const Json& params) {
    const Json* uri { lsp::find_path(params, { "textDocument", "uri" }) };
    return uri != nullptr && uri->is_string() ? uri->get<std::string>() : std::string {};
}

} // namespace

bool is_build_file(std::string_view name) {
    return std::ranges::find(BUILD_FILES, name) != BUILD_FILES.end() || name.ends_with(".cmake");
}

std::map<std::string, platform::fs::FileStamp> watched_files_snapshot(std::string_view root) { return watched_files_snapshot_of(root, {}); }

// ---- Workspace::Impl -------------------------------------------------------------------------

struct Workspace::Impl final : engine::Host {
    std::string root;
    std::string key;
    SessionOptions options;
    std::shared_ptr<EventChannel> events;
    ClientSink& client;
    std::string compilerOverride;
    bool kitEnabled { true };

    std::string cacheDirectory;
    // overall design 6.3: the lease on <cache>/workspaces/<key>; a second instance works in a private directory.
    std::optional<WorkspaceLease> lease;
    std::optional<Clock::time_point> leaseRenewAt;
    engine::PayloadPaths payload;
    bool payloadCorrupt { false };
    std::optional<spec::Kit> kit;
    std::string macosSdk;
    DocumentStore documents_;
    mutable std::unordered_map<std::string, std::string> canonicalByUri;
    index::ModuleIndex index;
    spec::MetadataReader metadataReader { spec::caching_metadata_reader() };
    std::map<std::string, platform::fs::FileStamp> watchBaseline;
    std::shared_ptr<WatchPatterns> watchPatterns { std::make_shared<WatchPatterns>() };
    bool dynamicWatch { false };             // the client registers watchers for us (else this root polls)
    int watchRegistration { 0 };             // the id suffix of the model's current watcher registration, 0 for none
    // S2 5: a model whose producer failed to answer again is not replaced by the fallback; this says why.
    std::string staleModelReason;

    Json clientParams;
    std::string clientUri;                   // this folder's URI as the client named it (set_client_uri)
    bool clientSupportsStatus { false };
    // Sending anything before the client has even received its own `initialize` response would be a
    // protocol violation; status can otherwise be computed synchronously from inside start().
    bool initializeAnswered { false };
    std::function<void(Json)> onEngineSettled;

    // Engines: mcppls's own and the core engine, in the order they are asked.
    std::vector<std::unique_ptr<engine::Engine>> engines;
    engine::Engine* coreEngine { nullptr };
    engine::Engine* moduleEngine { nullptr };
    Json coreCapabilities = Json::object();
    bool settledReported { false };
    // Diagnostics published by engines other than mcppls's own, per engine and client URI.
    std::map<std::string, std::map<std::string, Json, std::less<>>, std::less<>> engineDiagnostics;
    std::map<std::string, std::string, std::less<>> publishedDiagnostics;

    // Requests in flight across engines.
    struct Job {
        Json clientId;
        std::string method;
        std::vector<engine::Engine*> answerers;
        std::size_t next { 0 };
        std::size_t awaiting { 0 };
        std::vector<std::pair<std::string, Json>> merged;
        std::optional<Json> error;
        bool cancelled { false };
        bool merging { false };
        engine::RequestView view;
        Json params;
        std::string path;
        std::string text;
        Json message;
    };
    std::map<std::uint64_t, Job> jobs;
    std::uint64_t nextJob { 1 };

    // Project model and plan.
    std::shared_ptr<project::ProjectModel> model;
    int modelGeneration { 0 };
    std::string modelDescription;            // describe_model of `model`, to recognize an unchanged reload
    bool loading { false };
    bool reloadAfterLoad { false };
    normalize::EnginePlan plan;
    bool firstPlanWritten { false };
    std::string contextSet;
    std::map<std::string, std::string, std::less<>> structures;   // path key -> module structure at planning time

    // Timers.
    std::optional<Clock::time_point> reloadAt;
    std::optional<Clock::time_point> replanAt;
    std::optional<Clock::time_point> loadGiveUpAt;
    std::optional<Clock::time_point> sdkCheckAt;

    std::string lastStatus;                  // the last cxxModules/status sent, serialized
    State lastSentState { State::starting };
    std::optional<Clock::time_point> lastStatusSentAt;
    std::optional<Clock::time_point> statusFlushAt;   // a coalesced change goes out then

    Impl(std::string root_, std::string key_, SessionOptions options_, engine::PayloadPaths payload_, bool payloadCorrupt_,
         bool kitEnabled_, std::string compilerOverride_, std::shared_ptr<EventChannel> events_, ClientSink& client_)
        : root { std::move(root_) }, key { std::move(key_) }, options { std::move(options_) }, events { std::move(events_) }, client { client_ },
          compilerOverride { std::move(compilerOverride_) }, kitEnabled { kitEnabled_ }, payload { std::move(payload_) },
          payloadCorrupt { payloadCorrupt_ } {
        const std::string workspaceDirectory { base::join_path(platform::dirs::cache_directory(), base::join_path("workspaces", project::workspace_key(root))) };
        lease = WorkspaceLease::acquire(workspaceDirectory, std::chrono::system_clock::now());
        cacheDirectory = lease->directory();
        if (!lease->shared()) leaseRenewAt = Clock::now() + LEASE_RENEWAL;
        // Under its one name, like every file the engines are given (engine_uri): the prime units
        // and the database live here, and clangd answers for them under the name it was given.
        (void)platform::fs::create_directories(cacheDirectory);
        cacheDirectory = platform::fs::canonical_path(cacheDirectory);
        // usable plan W9.3: taken now, before this root is even started, so the client cannot
        // possibly have created or changed a watched file yet (see start_watch_polling below).
        watchBaseline = watched_files_snapshot(root);
        if (options.engineFactories) {
            EngineFactories factories { options.engineFactories(options, payload, payloadCorrupt) };
            if (factories.modules) {
                engines.push_back(factories.modules(index));
                moduleEngine = engines.back().get();
            }
            if (factories.core) {
                if (auto core = factories.core()) {
                    engines.push_back(std::move(core));
                    coreEngine = engines.back().get();
                }
            }
        }
    }

    // ---- engine::Host ---------------------------------------------------------

    const std::string& root_directory() const override { return root; }
    const std::string& cache_directory() const override { return cacheDirectory; }
    const Json& client_initialize_params() const override { return clientParams; }

    std::function<void(Json)> event_sink(std::string_view engineId) override {
        auto queue = events;
        return [queue, rootKey = key, id = std::string { engineId }](Json event) {
            queue->push(Event { EventKind::engine_event, std::move(event), 0, {}, rootKey, id });
        };
    }

    void send_to_client(const Json& message) override { client.send(message); }

    std::string client_request_id(std::string_view engineId, int generation, const Json& engineRequestId) const override {
        return make_engine_request_key(key, engineId, generation, engineRequestId);
    }

    void publish_engine_diagnostics(std::string_view engineId, const std::string& uri, Json diagnostics) override {
        engineDiagnostics[std::string { engineId }][uri] = std::move(diagnostics);
        publish_diagnostics(uri, true);
    }

    void forget_engine_diagnostics(std::string_view engineId) override { engineDiagnostics.erase(std::string { engineId }); }

    void engine_settled(std::string_view engineId, const Json& serverCapabilities) override {
        if (coreEngine != nullptr && engineId != coreEngine->id()) return;
        if (coreEngine != nullptr) coreCapabilities = serverCapabilities;
        if (settledReported || !onEngineSettled) return;
        settledReported = true;
        auto callback = std::move(onEngineSettled);
        onEngineSettled = {};
        callback(merge_capabilities(coreCapabilities));
    }

    void status_changed() override { update_status(); }
    void request_replan() override { schedule_replan(); }

    std::vector<engine::DocumentView> documents() const override {
        std::vector<engine::DocumentView> views;
        for (const Document* document : documents_.all()) views.push_back(view_of(*document));
        return views;
    }

    bool has_document(std::string_view clientUri) const override { return documents_.find(clientUri) != nullptr; }

    // The engines are given every file under the one name the model uses for it (v1 design 14.3's
    // "one file, one name"): clangd matches an unsaved buffer to the module source it builds by name.
    std::string engine_uri(std::string_view uri) const override {
        if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) {
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

    // The client's URI for a document an engine names in its own form.
    std::string client_uri(std::string_view engineUri) const override {
        if (documents_.find(engineUri) != nullptr) return std::string { engineUri };
        const std::string path { path_of_uri(engineUri) };
        if (const Document* document = path.empty() ? nullptr : documents_.find_by_path(path)) return document->uri;
        return std::string { engineUri };
    }

    void client_view(Json& value) const override {
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

    // A file's path under the one name the model uses for it (see engine_uri).
    std::string path_of_uri(std::string_view uri) const override {
        const std::string key_ { uri };
        if (const auto cached = canonicalByUri.find(key_); cached != canonicalByUri.end()) return cached->second;
        auto path = base::uri_to_path(uri);
        std::string canonical { path ? platform::fs::canonical_path(*path) : std::string {} };
        if (canonicalByUri.size() > 4096) canonicalByUri.clear();
        canonicalByUri.emplace(key_, canonical);
        return canonical;
    }

    std::vector<std::string> imports_of(std::string_view path) const override {
        const auto* scan = index.scan_of(path);
        return scan == nullptr ? std::vector<std::string> {} : project::required_names(*scan);
    }

    // ---- plumbing ---------------------------------------------------------------

    static engine::DocumentView view_of(const Document& document) {
        return engine::DocumentView { document.uri, document.path, document.languageId, document.version, document.text };
    }

    std::optional<Clock::time_point> next_deadline() const {
        std::optional<Clock::time_point> deadline;
        auto consider = [&](const std::optional<Clock::time_point>& at) {
            if (at && (!deadline || *at < *deadline)) deadline = at;
        };
        consider(reloadAt);
        consider(replanAt);
        consider(loadGiveUpAt);
        consider(sdkCheckAt);
        consider(statusFlushAt);
        consider(leaseRenewAt);
        for (const auto& engine : engines) consider(engine->next_deadline());
        return deadline;
    }

    // usable plan W9.3: every 2s, compares size and modification time of the same build description
    // and source files a dynamic watch would cover, and turns a difference into the exact
    // notification workspace/didChangeWatchedFiles would have carried, so handle_watched_files treats
    // a polled change exactly like an editor's own. The thread touches only its own snapshot and the
    // shared event queue; the workspace changes state only on the event loop.
    void start_watch_polling() {
        auto queue = events;
        const std::string rootPath { root };
        std::thread { [queue, rootPath, known = watchBaseline, patterns = watchPatterns]() mutable {
            std::vector<std::string> knownEntries;
            int knownGeneration { 0 };
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds { 2 });
                std::vector<std::string> entries;
                int generation { 0 };
                {
                    const std::lock_guard lock { patterns->mutex };
                    entries = patterns->entries;
                    generation = patterns->generation;
                }
                std::map<std::string, platform::fs::FileStamp> current { watched_files_snapshot_of(rootPath, entries) };
                // A model that names new inputs brings files into the watch that were there all along:
                // they start from what they are now rather than being reported as created.
                const bool entriesChanged { generation != knownGeneration };
                Json changes = Json::array();
                for (const auto& [file, fileStamp] : current) {
                    const auto previous = known.find(file);
                    if (previous == known.end()) {
                        if (entriesChanged && !watch_covers(knownEntries, rootPath, file)) continue;
                        changes.push_back(Json { { "uri", base::path_to_uri(file) }, { "type", 1 } });
                    } else if (previous->second != fileStamp) {
                        changes.push_back(Json { { "uri", base::path_to_uri(file) }, { "type", 2 } });
                    }
                }
                for (const auto& [file, fileStamp] : known) {
                    if (current.contains(file)) continue;
                    if (entriesChanged && !watch_covers(entries, rootPath, file)) continue;
                    changes.push_back(Json { { "uri", base::path_to_uri(file) }, { "type", 3 } });
                }
                known = std::move(current);
                knownEntries = std::move(entries);
                knownGeneration = generation;
                if (changes.empty()) continue;
                // `Json message { make_notification(...) }` would wrap the result in a one-element array.
                Json message = lsp::make_notification("workspace/didChangeWatchedFiles", Json { { "changes", std::move(changes) } });
                queue->push(Event { EventKind::client_message, std::move(message) });
            }
        } }.detach();
    }

    // ---- requests ---------------------------------------------------------------------

    void route_client_request(const Json& message) {
        const std::uint64_t jobId { nextJob++ };
        Job& job = jobs[jobId];
        job.clientId = message["id"];
        job.method = message.value("method", std::string {});
        job.message = message;
        job.params = message.contains("params") ? message["params"] : Json::object();
        const std::string uri { uri_of_params(job.params) };
        job.path = uri.empty() ? std::string {} : path_of_uri(uri);
        const Document* document { uri.empty() ? nullptr : documents_.find(uri) };
        if (document != nullptr) job.text = document->text;
        job.view = engine::RequestView { job.method, &job.params, job.path, job.text };

        std::vector<engine::Engine*> candidates;
        for (const auto& engine : engines) candidates.push_back(engine.get());
        Selection selection { select_engines(candidates, job.view) };
        if (!selection.mergers.empty()) {
            job.merging = true;
            job.awaiting = selection.mergers.size();
            for (engine::Engine* merger : selection.mergers) {
                const std::string engineId { merger->id() };
                merger->request(job.view, job.message, [this, jobId, engineId](engine::Answer answer) { merge_answer(jobId, engineId, std::move(answer)); });
                if (!jobs.contains(jobId)) return;
            }
            return;
        }
        if (selection.answerers.empty()) {
            finish_job(jobId, Json(nullptr));
            return;
        }
        job.answerers = std::move(selection.answerers);
        ask_next(jobId);
    }

    void ask_next(std::uint64_t jobId) {
        auto it = jobs.find(jobId);
        if (it == jobs.end()) return;
        Job& job = it->second;
        if (job.next >= job.answerers.size()) {
            finish_job(jobId, Json(nullptr));
            return;
        }
        engine::Engine* answerer { job.answerers[job.next++] };
        answerer->request(job.view, job.message, [this, jobId](engine::Answer answer) {
            auto current = jobs.find(jobId);
            if (current == jobs.end()) return;
            switch (answer.kind) {
            case engine::Answer::Kind::result:
                if (!answer.value.is_null()) {
                    finish_job(jobId, std::move(answer.value));
                    return;
                }
                ask_next(jobId);
                return;
            case engine::Answer::Kind::unavailable: ask_next(jobId); return;
            case engine::Answer::Kind::error: finish_job_with_error(jobId, std::move(answer.value)); return;
            case engine::Answer::Kind::cancelled: finish_job_cancelled(jobId); return;
            }
        });
    }

    void merge_answer(std::uint64_t jobId, const std::string& engineId, engine::Answer answer) {
        auto it = jobs.find(jobId);
        if (it == jobs.end()) return;
        Job& job = it->second;
        switch (answer.kind) {
        case engine::Answer::Kind::result: job.merged.emplace_back(engineId, std::move(answer.value)); break;
        case engine::Answer::Kind::unavailable: break;
        case engine::Answer::Kind::error:
            if (!job.error) job.error = std::move(answer.value);
            break;
        case engine::Answer::Kind::cancelled: job.cancelled = true; break;
        }
        if (--job.awaiting > 0) return;
        if (job.cancelled) {
            finish_job_cancelled(jobId);
        } else if (job.error) {
            finish_job_with_error(jobId, std::move(*job.error));
        } else {
            finish_job(jobId, merge_results(job.method, job.merged));
        }
    }

    void finish_job(std::uint64_t jobId, Json result) {
        auto it = jobs.find(jobId);
        if (it == jobs.end()) return;
        const Json id = it->second.clientId;
        jobs.erase(it);
        client.reply(id, std::move(result));
    }

    void finish_job_with_error(std::uint64_t jobId, Json error) {
        auto it = jobs.find(jobId);
        if (it == jobs.end()) return;
        Json response { { "jsonrpc", "2.0" }, { "id", it->second.clientId }, { "error", std::move(error) } };
        jobs.erase(it);
        client.send(response);
    }

    void finish_job_cancelled(std::uint64_t jobId) {
        auto it = jobs.find(jobId);
        if (it == jobs.end()) return;
        const Json id = it->second.clientId;
        jobs.erase(it);
        client.reply_error(id, lsp::REQUEST_CANCELLED, "cancelled");
    }

    // A unit of the model whose module declaration or imports changed needs a new plan; a new
    // source file of an inferred model needs a new model.
    void note_structure_change(std::string_view path) {
        if (!model) return;
        const auto it = structures.find(base::path_key(path));
        if (it != structures.end()) {
            if (const auto* scan = index.scan_of(path); scan != nullptr && it->second != structure_of(*scan)) schedule_replan();
            return;
        }
        if (model->source == project::SourceKind::inferred && project::is_cxx_source_name(path) && base::is_within(path, root)) schedule_reload();
    }

    void document_event(engine::DocumentChange change, const Document& document, const Json* message) {
        const engine::DocumentEvent event { change, view_of(document), message };
        for (const auto& engine : engines) engine->document(event);
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

    // Everything about a model that the index and the plan are built from, as one comparable string.
    static std::string describe_model(const project::ProjectModel& candidate) {
        Json facts = Json::object();
        for (const auto& [driver, driverFacts] : candidate.facts) facts[driver] = Json::parse(toolchain::facts_to_json(driverFacts).dump());
        const Json description {
            { "source", std::string { project::to_string(candidate.source) } },
            { "level", candidate.level },
            { "usesKit", candidate.usesKit },
            { "profile", Json::array({ candidate.profile.kind, candidate.profile.compiler, candidate.profile.stdlib, candidate.profile.target }) },
            { "facts", std::move(facts) },
            { "database", Json::parse(spec::to_json(candidate.database).dump()) },
        };
        return description.dump();
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

    // S2 5: the inputs the model's producer named are watched like the build files are. The client
    // watches them when it registers watchers dynamically, as patterns under this root; otherwise the
    // polling worker reads the same entries. A new model replaces the previous registration.
    void register_model_watch() {
        if (!dynamicWatch || !model || !initializeAnswered) return;
        if (watchRegistration != 0) {
            Json unregister { { "unregisterations", Json::array({ Json { { "id", std::format("mcppls-model-watch:{}:{}", key, watchRegistration) },
                                                                        { "method", "workspace/didChangeWatchedFiles" } } }) } };
            client.send(lsp::make_request(std::format("w:{}:u{}", key, watchRegistration), "client/unregisterCapability", std::move(unregister)));
            watchRegistration = 0;
        }
        // An inferred model watches the sources the session's own watchers already cover.
        if (model->watch.empty() || model->source == project::SourceKind::inferred) return;
        const Json* relative { lsp::find_path(clientParams, { "capabilities", "workspace", "didChangeWatchedFiles", "relativePatternSupport" }) };
        const bool relativePatterns { relative != nullptr && relative->is_boolean() && relative->get<bool>() };
        Json watchers = Json::array();
        for (const auto& entry : model->watch) {
            const bool absolute { base::is_absolute_path(entry) };
            if (relativePatterns) {
                const std::string base { absolute ? base::parent_path(entry) : root };
                const std::string pattern { absolute ? std::string { base::file_name(entry) } : entry };
                watchers.push_back(Json { { "globPattern", Json { { "baseUri", base::path_to_uri(base) }, { "pattern", pattern } } } });
            } else {
                watchers.push_back(Json { { "globPattern", absolute ? base::normalize_path(entry) : base::join_path(root, entry) } });
            }
        }
        static int nextRegistration { 0 };
        watchRegistration = ++nextRegistration;
        Json registrations { { "registrations", Json::array({ Json { { "id", std::format("mcppls-model-watch:{}:{}", key, watchRegistration) },
                                                                    { "method", "workspace/didChangeWatchedFiles" },
                                                                    { "registerOptions", Json { { "watchers", std::move(watchers) } } } } }) } };
        client.send(lsp::make_request(std::format("w:{}:r{}", key, watchRegistration), "client/registerCapability", std::move(registrations)));
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
        loadGiveUpAt.reset();
        // S2 5: a producer that answered before and fails now (or answers with nothing) leaves the last
        // model in place, and the status says it may be stale, rather than the project falling back to
        // scanned sources. A project that is no longer that kind of project takes the new model.
        const bool fellBack { loadedModel->detected != project::SourceKind::inferred && loadedModel->source == project::SourceKind::inferred };
        if (model && model->source != project::SourceKind::inferred && model->source == loadedModel->detected && fellBack) {
            const std::string why { loadedModel->issues.empty() ? std::string {} : std::format(" ({})", loadedModel->issues.front().message) };
            staleModelReason = std::format("{} could not describe the project again{}; the last model that loaded is kept and may be stale",
                                           project::to_string(model->source), why);
            for (const auto& issue : loadedModel->issues) log::warning("model reload failed ({}): [{}] {}", root, issue.code, issue.message);
            if (reloadAfterLoad) {
                reloadAfterLoad = false;
                start_model_load();
            }
            update_status();
            return;
        }
        staleModelReason.clear();
        const bool watchChanged { !model || model->watch != loadedModel->watch };
        std::string description { describe_model(*loadedModel) };
        const bool unchanged { model && description == modelDescription };
        model = std::move(loadedModel);
        if (watchChanged) {
            {
                const std::lock_guard lock { watchPatterns->mutex };
                watchPatterns->entries = model->watch;
                ++watchPatterns->generation;
            }
            register_model_watch();
        }
        if (unchanged) {
            // The usual answer to a saved source the producer watches: the same project. The index
            // already has the file and the plan already has the graph, so the work of a new model is skipped.
            log::info("project model ({}) loaded again: unchanged", root);
            for (const auto& issue : model->issues) log::info("model issue [{}] {}", issue.code, issue.message);
            if (reloadAfterLoad) {
                reloadAfterLoad = false;
                start_model_load();
            }
            update_status();
            return;
        }
        modelDescription = std::move(description);
        log::info("project model ({}): source {}, level {}, {} sets, profile {} {} {}", root, project::to_string(model->source), model->level,
                  model->database.sets.size(), model->profile.kind, model->profile.compiler, model->profile.stdlib);
        for (const auto& issue : model->issues) log::info("model issue [{}] {}", issue.code, issue.message);

        save_model_cache();
        index.clear();
        for (const auto& set : model->database.sets) {
            for (const auto& unit : set.units) {
                const std::string path { spec::absolute_source(unit) };
                if (index.contains(path)) continue;
                if (const Document* document = documents_.find_by_path(path)) {
                    index.update(path, document->text);
                } else if (auto text = platform::fs::read_file(path)) {
                    index.update(path, *text);
                }
            }
        }
        for (const Document* document : documents_.all()) {
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
        input.macosSdk = macosSdk;
        input.scanner = [this](std::string_view path) {
            if (const auto* scan = index.scan_of(path)) return *scan;
            auto text = platform::fs::read_file(path);
            return text ? project::scan_source(*text) : project::ScanResult {};
        };
        input.metadataReader = metadataReader;
        if (coreEngine != nullptr) coreEngine->configure_plan(input);
        normalize::EnginePlan newPlan { normalize::plan_engine(input) };
        plan = std::move(newPlan);
        structures.clear();
        for (const auto& path : index.files()) {
            if (const auto* scan = index.scan_of(path)) structures[base::path_key(path)] = structure_of(*scan);
        }
        firstPlanWritten = true;
        for (const auto& engine : engines) engine->apply(&plan);
        for (const Document* document : documents_.all()) publish_diagnostics(document->uri);
        update_status();
    }

    void schedule_replan() { replanAt = Clock::now() + std::chrono::milliseconds { 800 }; }
    void schedule_reload() { reloadAt = Clock::now() + std::chrono::milliseconds { 1500 }; }

    // ---- diagnostics and status -------------------------------------------------------

    void publish_diagnostics(std::string_view uri, bool force = false) {
        const Document* document { documents_.find(uri) };
        if (document == nullptr) return;
        const Json moduleDiagnostics = document->path.empty() ? Json::array() : index.diagnostics(document->path);
        Json fromEngines = Json::array();
        for (const auto& [engineId, byUri] : engineDiagnostics) {
            const auto entry = byUri.find(uri);
            if (entry == byUri.end() || !entry->second.is_array()) continue;
            for (const auto& diagnostic : entry->second) fromEngines.push_back(diagnostic);
        }
        std::string label;
        if (model) label = model->profile.kind == "semantic-kit" ? model->profile.stdlib + " kit" : model->profile.compiler;
        Json merged = merge_diagnostics(fromEngines, moduleDiagnostics, label);
        std::string serialized { lsp::dump(merged) };
        auto& previous = publishedDiagnostics[std::string { uri }];
        if (!force && previous == serialized) return;
        previous = std::move(serialized);
        client.notify("textDocument/publishDiagnostics", Json { { "uri", std::string { uri } }, { "diagnostics", std::move(merged) } });
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
            profile = Json { { "kind", "semantic-kit" }, { "stdlib", "unknown" }, { "target", std::string { mcppls::os::VSCODE_TARGET } } };
        }
        return profile;
    }

    State compute_state() const {
        const std::optional<engine::EngineStatus> core { coreEngine != nullptr ? std::optional { coreEngine->status() } : std::nullopt };
        // usable plan W9.4: a corrupt payload is not merely degraded: only syntax-level features are
        // trustworthy, the same as three engine crashes in a row.
        if (core && core->failed) return State::error;
        if (!model) return loading ? State::loading : State::starting;
        if (loading) return State::loading;
        if (core && core->preparing) return State::preparing;
        // usable plan W5.4: degraded regardless of whether any open file happens to need std yet.
        if (kit && spec::requires_macos_sdk(*kit) && macosSdk.empty()) return State::degraded;
        bool engineIssues { false };
        for (const auto& engine : engines) engineIssues = engineIssues || !engine->status().issues.empty();
        // S2 5: a kept model the producer could not confirm may be stale: said, not hidden in a ready state.
        if (engineIssues || !model->issues.empty() || !plan.issues.empty() || !staleModelReason.empty()) return State::degraded;
        return State::ready;
    }

    void update_status() {
        if (!initializeAnswered) return;   // see the field's own comment
        const State state { compute_state() };
        if (!clientSupportsStatus) return;
        Json issues = Json::array();
        auto add = [&](std::string_view code, std::string_view message, std::string_view command, std::string_view title = "Fix") {
            if (issues.size() >= 20) return;
            Json issue { { "code", std::string { code } }, { "message", std::string { message } } };
            if (!command.empty()) issue["command"] = Json { { "title", std::string { title } }, { "command", std::string { command } } };
            issues.push_back(std::move(issue));
        };
        for (const auto& engine : engines) {
            for (const auto& issue : engine->status().issues) add(issue.code, issue.message, issue.command);
        }
        if (!staleModelReason.empty()) add("model-stale", staleModelReason, "mcppls.showLogs", "Show Logs");
        if (model) {
            for (const auto& issue : model->issues) add(issue.code, issue.message, "mcppls.showLogs");
        }
        for (const auto& issue : plan.issues) {
            // sdk-missing is reported once below, workspace-wide, with the fix command (W5.4).
            if (issue.code == "sdk-missing") continue;
            add(issue.code, std::format("{} ({})", issue.message, base::file_name(issue.file)), "");
        }
        if (!options.trusted) add("untrusted-workspace", "the workspace is not trusted: build tools and compilers are not run", "");
        if (kit && spec::requires_macos_sdk(*kit) && macosSdk.empty()) {
            add("sdk-missing", "the macOS SDK was not found; install the Command Line Tools", "mcppls.installCommandLineTools", "Install Command Line Tools");
        }
        Json notices = Json::array();
        if (model) {
            for (const auto& notice : model->notices) notices.push_back(Json { { "code", notice.code }, { "message", notice.message } });
        }
        if (!payload.kitNotice.empty()) notices.push_back(Json { { "code", "kit-version-mismatch" }, { "message", payload.kitNotice } });
        if (lease && lease->shared()) {
            notices.push_back(Json { { "code", "shared-workspace" },
                                     { "message", "another mcppls instance serves this workspace; this one keeps a private cache and starts cold" } });
        }
        // S3 4 (overall design 5.2): every engine serving the root, with its role and state.
        Json engineList = Json::array();
        for (const auto& engine : engines) {
            const engine::EngineStatus engineStatus { engine->status() };
            engineList.push_back(Json { { "name", engineStatus.name }, { "version", engineStatus.version }, { "role", engineStatus.role },
                                        { "state", engineStatus.state } });
            for (const auto& notice : engineStatus.notices) notices.push_back(Json { { "code", notice.code }, { "message", notice.message } });
        }
        // usable plan W9.1: `project.root` is this root's own path, so a multi-root session's several
        // notifications are told apart by it.
        Json project { { "root", clientUri.empty() ? base::path_to_uri(root) : clientUri },
                       { "source", model ? std::string { project::to_string(model->source) } : std::string { "inferred" } } };
        if (model) project["level"] = model->level;
        const std::optional<engine::EngineStatus> core { coreEngine != nullptr ? std::optional { coreEngine->status() } : std::nullopt };
        Json params {
            { "state", std::string { to_string(state) } },
            { "project", project },
            { "profile", profile_json() },
            { "engine", core ? Json { { "name", core->name }, { "version", core->version } } : Json { { "name", "none" }, { "version", "" } } },
            { "engines", std::move(engineList) },
            { "issues", issues },
        };
        if (!notices.empty()) params["notices"] = std::move(notices);
        if (core && core->toPrepare > 0) params["progress"] = Json { { "done", core->prepared }, { "total", core->toPrepare } };
        std::string serialized { lsp::dump(params) };
        if (serialized == lastStatus) {
            statusFlushAt.reset();
            return;
        }
        // S3 4: a new state goes out at once; other changes within STATUS_COALESCE of the last
        // notification (module counts while many modules build) go out together when it has passed.
        const auto now = Clock::now();
        if (lastStatusSentAt && state == lastSentState && now - *lastStatusSentAt < STATUS_COALESCE) {
            if (!statusFlushAt) statusFlushAt = *lastStatusSentAt + STATUS_COALESCE;
            return;
        }
        statusFlushAt.reset();
        lastStatus = std::move(serialized);
        lastSentState = state;
        lastStatusSentAt = now;
        client.notify("cxxModules/status", std::move(params));
    }

    // ---- timers -----------------------------------------------------------------------

    void handle_timers() {
        const auto now = Clock::now();
        for (const auto& engine : engines) engine->handle_timers();
        if (leaseRenewAt && *leaseRenewAt <= now) {
            lease->renew(std::chrono::system_clock::now());
            leaseRenewAt = now + LEASE_RENEWAL;
        }
        if (reloadAt && *reloadAt <= now) {
            reloadAt.reset();
            start_model_load();
        }
        if (replanAt && *replanAt <= now) replan();
        if (statusFlushAt && *statusFlushAt <= now) {
            statusFlushAt.reset();
            update_status();
        }
        if (sdkCheckAt && *sdkCheckAt <= now) {
            sdkCheckAt.reset();
            if (kit && spec::requires_macos_sdk(*kit) && macosSdk.empty()) {
                if (std::string found { engine::macos_sdk_path() }; !found.empty()) {
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
                for (const auto& engine : engines) engine->apply(nullptr);
            }
        }
    }
};

// ---- Workspace --------------------------------------------------------------------------

Workspace::Workspace(std::string root, std::string key, SessionOptions options, engine::PayloadPaths payload, bool payloadCorrupt, bool kitEnabled,
                     std::string compilerOverride, std::shared_ptr<EventChannel> events, ClientSink& client)
    : root_ { root }, key_ { key },
      impl_ { std::make_unique<Impl>(std::move(root), std::move(key), std::move(options), std::move(payload), payloadCorrupt, kitEnabled,
                                    std::move(compilerOverride), std::move(events), client) } {}

Workspace::~Workspace() = default;

void Workspace::set_client_uri(std::string uri) { impl_->clientUri = std::move(uri); }

bool Workspace::owns_path(std::string_view path) const { return !path.empty() && base::is_within(path, root_); }

void Workspace::start(Json clientParams, bool clientSupportsStatus, bool usePolling, std::function<void(Json)> onEngineSettled) {
    impl_->clientParams = std::move(clientParams);
    impl_->clientSupportsStatus = clientSupportsStatus;
    impl_->onEngineSettled = std::move(onEngineSettled);
    impl_->dynamicWatch = !usePolling;
    if (usePolling) impl_->start_watch_polling();
    log::info("mcppls {} ({}) root {}", base::VERSION, mcppls::os::FAMILY_NAME, root_);
    if (!impl_->payloadCorrupt && impl_->kitEnabled && !impl_->payload.kit.empty()) {
        if (auto kit = spec::load_kit(impl_->payload.kit)) {
            impl_->kit = std::move(*kit);
            if (spec::requires_macos_sdk(*impl_->kit)) {
                impl_->macosSdk = engine::macos_sdk_path();
                // usable plan W5.4 / U7: keep looking every 30s so installing the Command Line Tools
                // while the server runs is picked up without a restart.
                if (impl_->macosSdk.empty()) impl_->sdkCheckAt = Clock::now() + std::chrono::seconds { 30 };
            }
            log::info("semantic kit {} at {} ({})", impl_->kit->name, impl_->kit->root, root_);
        } else {
            log::warning("semantic kit unusable ({}): {}", root_, kit.error().message);
        }
    }
    impl_->restore_cached_model();
    impl_->start_model_load();
    for (const auto& engine : impl_->engines) engine->start(*impl_);
    // Without a core engine nothing else settles initialize.
    if (impl_->coreEngine == nullptr) impl_->engine_settled("", Json::object());
    impl_->loadGiveUpAt = Clock::now() + std::chrono::seconds { 120 };
}

void Workspace::allow_status_notifications() {
    if (impl_->initializeAnswered) return;
    impl_->initializeAnswered = true;
    impl_->register_model_watch();
    impl_->update_status();
}

void Workspace::shut_down() {
    for (const auto& engine : impl_->engines) engine->shut_down();
    if (impl_->lease) impl_->lease->release();
}

void Workspace::did_open(const Json& params) {
    const Json* item { lsp::find(params, "textDocument") };
    if (item == nullptr) return;
    const std::string uri { item->value("uri", std::string {}) };
    const std::string path { impl_->path_of_uri(uri) };
    const Document& document = impl_->documents_.open(uri, path, item->value("languageId", std::string { "cpp" }),
                                                      item->value("version", std::int64_t { 0 }), item->value("text", std::string {}));
    if (!path.empty()) {
        impl_->index.update(path, document.text);
        impl_->note_structure_change(path);
    }
    impl_->publish_diagnostics(uri);
    impl_->document_event(engine::DocumentChange::opened, document, nullptr);
}

void Workspace::did_change(const Json& message, const Json& params) {
    const std::string uri { uri_of_params(params) };
    const std::int64_t version { lsp::find_path(params, { "textDocument", "version" }) != nullptr
                                     ? params["textDocument"].value("version", std::int64_t { 0 }) : 0 };
    if (!impl_->documents_.change(uri, version, params.value("contentChanges", Json::array()))) return;
    const Document* document { impl_->documents_.find(uri) };
    if (!document->path.empty()) {
        impl_->index.update(document->path, document->text);
        impl_->note_structure_change(document->path);
    }
    impl_->publish_diagnostics(uri);
    impl_->document_event(engine::DocumentChange::changed, *document, &message);
}

void Workspace::did_close(const Json& message, const Json& params) {
    const std::string uri { uri_of_params(params) };
    const Document* found { impl_->documents_.find(uri) };
    if (found == nullptr) return;
    const Document document { *found };
    impl_->documents_.close(uri);
    if (!document.path.empty()) {
        if (auto text = platform::fs::read_file(document.path)) impl_->index.update(document.path, *text);
    }
    impl_->document_event(engine::DocumentChange::closed, document, &message);
    // Diagnostics of a closed file are cleared; an engine may send its own empty set too.
    impl_->publishedDiagnostics.erase(uri);
    for (auto& [engineId, byUri] : impl_->engineDiagnostics) byUri.erase(uri);
    impl_->client.notify("textDocument/publishDiagnostics", Json { { "uri", uri }, { "diagnostics", Json::array() } });
    impl_->update_status();
}

void Workspace::did_save(const Json& message, const Json& params) {
    const std::string uri { uri_of_params(params) };
    const std::string path { impl_->path_of_uri(uri) };
    Document saved;
    if (const Document* document = impl_->documents_.find(uri)) {
        saved = *document;
    } else {
        saved.uri = uri;
        saved.path = path;
    }
    impl_->document_event(engine::DocumentChange::saved, saved, &message);
}

void Workspace::handle_watched_files(const Json& changes) {
    bool reload { false };
    bool replan { false };
    for (const auto& change : changes) {
        const std::string path { impl_->path_of_uri(change.value("uri", std::string {})) };
        if (path.empty()) continue;
        const std::string_view name { base::file_name(path) };
        const int type { change.value("type", 2) };
        // S2 5: an input the producer named changes what it would answer, so the model is loaded again.
        // A source among them still updates the index at once below; the reload only confirms the model.
        if (impl_->model && impl_->model->source != project::SourceKind::inferred && name != "compile_commands.json"
            && matches_watch_entries(impl_->model->watch, impl_->root, path)) {
            reload = true;
        }
        if (is_build_file(name)) {
            // mcpp rewrites its own compile_commands.json while the model loads.
            if (name == "compile_commands.json" && impl_->model && impl_->model->source == project::SourceKind::mcpp) continue;
            reload = true;
            continue;
        }
        if (!project::is_cxx_source_name(path) || impl_->documents_.find_by_path(path) != nullptr) continue;
        if (type == 3) {
            impl_->index.remove(path);
        } else if (auto text = platform::fs::read_file(path)) {
            impl_->index.update(path, *text);
        }
        if (impl_->model && impl_->model->source == project::SourceKind::inferred && type != 2) reload = true;
        else replan = true;
    }
    if (reload || replan) {
        for (const auto& engine : impl_->engines) engine->sources_changed();
    }
    if (reload) impl_->schedule_reload();
    else if (replan) impl_->schedule_replan();
    const Json message = lsp::make_notification("workspace/didChangeWatchedFiles", Json { { "changes", changes } });
    for (const auto& engine : impl_->engines) engine->notify(message);
    for (const Document* document : impl_->documents_.all()) impl_->publish_diagnostics(document->uri);
}

void Workspace::cancel(const Json& id) {
    for (const auto& engine : impl_->engines) engine->cancel(id);
}

void Workspace::route_client_request(const Json& message) { impl_->route_client_request(message); }

void Workspace::handle_client_response(const EngineRequestKey& key, const Json& response) {
    for (const auto& engine : impl_->engines) {
        if (engine->id() == key.engineId) engine->client_response(key.generation, key.engineRequestId, response);
    }
}

void Workspace::forward_other_notification(const Json& message) {
    for (const auto& engine : impl_->engines) engine->notify(message);
}

Json Workspace::graph() const { return impl_->index.graph(); }

Json Workspace::module_info(std::string_view name) const { return impl_->index.module_info(name); }

Json Workspace::module_info_at(std::string_view path, base::Position at) const {
    const auto hit = impl_->index.module_at(path, at);
    return hit ? impl_->index.module_info(hit->name) : Json(nullptr);
}

Json Workspace::contexts() const {
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

void Workspace::set_context(const Json& id, std::string_view context) {
    if (context != "default" && (!impl_->model || !spec::find_set(impl_->model->database, context))) {
        impl_->client.reply_error(id, lsp::INVALID_PARAMS, std::format("unknown context {}", context));
        return;
    }
    impl_->contextSet = context == "default" ? std::string {} : std::string { context };
    impl_->client.reply(id, nullptr);
    if (impl_->model) impl_->replan();
}

void Workspace::handle_engine_event(std::string_view engineId, const Json& event) {
    for (const auto& engine : impl_->engines) {
        if (engine->id() == engineId) engine->handle_event(event);
    }
}

void Workspace::handle_model_loaded(int generation, std::shared_ptr<project::ProjectModel> model) {
    if (generation != impl_->modelGeneration) return;
    impl_->handle_model_loaded(std::move(model));
}

std::optional<Clock::time_point> Workspace::next_deadline() const { return impl_->next_deadline(); }

void Workspace::handle_timers() { impl_->handle_timers(); }

} // namespace mcppls::orchestrator
