module mcppls.engine.clangd;

import std;
import nlohmann.json;
import mcppls.os;
import mcppls.base.error;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.base.uri;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.lsp.jsonrpc;
import mcppls.lsp.protocol;
import mcppls.project.scan;
import mcppls.normalize.plan;
import mcppls.engine;
import mcppls.engine.clangd.guard;
import mcppls.engine.clangd.primer;
import mcppls.engine.clangd.process;

namespace mcppls::engine::clangd {

namespace log = base::log;

EngineTraits traits_for_version(std::string_view version) {
    EngineTraits traits {
        .importNavigation = false,
        .pushesDiagnostics = true,
        .hangsOnUnresolvedImports = true,
        .needsModulePreparation = true,
        .needsModuleHints = true,
        .msvcStlNeedsNoAlignedAllocation = true,
        .kitStdlibVersion = std::string { version },
        .tested = false,
    };
    if (version == "23.1.0") {
        traits.tested = true;
    } else if (version.starts_with("23.1.")) {
        traits.msvcStlNeedsNoAlignedAllocation = false;
    }
    return traits;
}

bool is_interactive(std::string_view method) {
    static constexpr std::array<std::string_view, 9> INTERACTIVE { "textDocument/definition", "textDocument/declaration", "textDocument/hover",
        "textDocument/completion", "textDocument/signatureHelp", "textDocument/documentHighlight", "textDocument/typeDefinition",
        "textDocument/implementation", "completionItem/resolve" };
    return std::ranges::find(INTERACTIVE, method) != INTERACTIVE.end();
}

bool keep_waiting(const PendingRequest& request, bool filePreparing, std::optional<Clock::time_point> lastProgress, Clock::time_point now) {
    if (request.purpose != Purpose::client || !filePreparing || !lastProgress) return false;
    return now < request.limit && now - *lastProgress < INTERACTIVE_TIMEOUT;
}

namespace {

class ClangdEngine final : public Engine {
private:
    Options options_;
    EngineTraits traits_;
    Host* host_ { nullptr };
    std::function<void(Json)> sink_;
    std::unique_ptr<Process> process_;
    std::vector<MethodCapability> methods_ {
        { std::string { EVERY_METHOD }, Role::answer, 0 },
        { std::string { lsp::method::TEXT_DOCUMENT_DOCUMENT_SYMBOL }, Role::merge, 0 },
        { std::string { lsp::method::WORKSPACE_SYMBOL }, Role::merge, 0 },
    };

    std::string databaseDirectory_;
    std::string primeDirectory_;
    std::string moduleHintDirectory_;
    std::string stubDirectory_;       // stand-ins for modules nothing usable provides (robustness design C2)

    // Plan.
    bool planApplied_ { false };
    std::string writtenDatabase_;
    std::string writtenStructure_;   // the written database without module hints
    std::map<std::string, std::string, std::less<>> writtenArguments_;   // path key -> the unit's engine command, without hints
    std::set<std::string> excluded_;   // path keys
    // Modules clangd could not find (robustness design C3), with the reason and the unit the plan had
    // providing each then. An entry is forgotten when that unit, or its command, changes; not on every save.
    struct UnresolvedModule {
        std::string reason;
        std::string provider;                          // the plan's unit for the module; empty when it had none
        std::optional<platform::fs::FileStamp> stamp;  // of that unit
        std::string command;                           // its engine command
    };
    std::map<std::string, UnresolvedModule, std::less<>> unresolvedModules_;
    std::set<std::string, std::less<>> reportedFailures_;   // modules whose compile failure was logged
    std::map<std::string, std::string, std::less<>> moduleCommands_;   // importable module -> its unit's engine command
    // robustness design C5: clangd could not build the toolchain's standard library; C++ units are read with the kit.
    bool stdFromKit_ { false };

    // Parallel module preparation (primer.cppm): `import M;` units opened in clangd.
    Primer primer_;
    std::map<std::string, std::string, std::less<>> primeModuleByPath_;
    std::map<std::string, Clock::time_point, std::less<>> primeDeadlines_;
    std::map<std::string, std::string, std::less<>> heldPrimeUnits_;
    std::optional<Clock::time_point> lastPrimeProgressAt_;
    std::map<std::string, std::string, std::less<>> moduleSources_;   // importable module -> the unit providing it, from the plan
    // clangd's persistent module cache as the plan found it (cached_bmis), read the first time a
    // module becomes ready for preparation and not again until the next plan.
    std::optional<std::map<std::string, std::vector<std::string>, std::less<>>> startupBmis_;

    // Process.
    int generation_ { 0 };
    bool handshakeDone_ { false };
    bool accepting_ { false };
    bool unavailable_ { false };
    std::map<std::int64_t, PendingRequest> pending_;
    std::int64_t nextId_ { 1 };
    std::vector<std::pair<Json, Reply>> deferred_;   // client messages before clangd accepts traffic
    std::set<std::string> diagnosed_;                // client URIs clangd published diagnostics for
    std::set<std::string> awaitingDiagnostics_;      // client URIs
    std::deque<Clock::time_point> crashes_;
    std::vector<Issue> issues_;
    std::optional<Clock::time_point> restartAt_;
    std::string restartReason_;
    // robustness design C4, C6: restarts spaced out; files clangd stopped answering for set aside one by one.
    RestartGate restartGate_;
    Quarantine quarantine_;                                   // path keys
    std::optional<Clock::time_point> lastAnswerAt_;           // clangd's last answer to any client request
    // robustness design O1, O3: for a report of a problem.
    std::deque<std::pair<std::string, std::string>> restartHistory_;   // (UTC time, reason), the latest 20
    std::size_t linesLeftOut_ { 0 };                                   // clangd log lines the limiter left out
    ProcessConfig lastConfig_;
    std::map<std::string, Clock::time_point, std::less<>> touchedAt_;   // path key -> when the document was last opened or changed
    // robustness design C6: files that wait for diagnostics clangd never publishes. A unit of a module that did not
    // compile gets FAILED_MODULE_PATIENCE (clangd was seen to stop building such a unit for good); any file gets
    // GENERAL_PATIENCE while module preparation makes no progress.
    static constexpr std::chrono::seconds FAILED_MODULE_PATIENCE { 5 };
    static constexpr std::chrono::seconds GENERAL_PATIENCE { 120 };
    std::map<std::string, Clock::time_point, std::less<>> awaitingSince_;     // client URI -> when it was handed to clangd
    std::map<std::string, Clock::time_point, std::less<>> modulesFailedAt_;   // module -> when clangd said it did not compile
    std::optional<Clock::time_point> stuckCheckAt_;

public:
    explicit ClangdEngine(Options options) : options_ { std::move(options) }, traits_ { traits_for_version(options_.version) } {}

    std::string_view id() const override { return ENGINE_ID; }
    std::span<const MethodCapability> methods() const override { return methods_; }
    EngineTraits traits() const override { return traits_; }

    EngineStatus status() const override {
        EngineStatus status;
        status.name = std::string { ENGINE_ID };
        status.version = options_.version.empty() ? std::string { "unknown" } : options_.version;
        status.role = "core";
        status.accepting = accepting_;
        status.preparing = (!awaitingDiagnostics_.empty() || primer_.busy()) && accepting_;
        status.failed = options_.payloadCorrupt || (unavailable_ && crashes_.size() >= 3);
        if (primer_.busy()) std::tie(status.prepared, status.toPrepare) = primer_.progress();
        status.issues = issues_;
        status.state = unavailable_ ? "unavailable" : status.preparing ? "preparing" : accepting_ ? "ready" : "starting";
        // overall design 5.6: a clangd outside the traits table runs with every compensation on, and says so.
        if (!unavailable_ && !traits_.tested && !options_.version.empty()) {
            status.notices.push_back(Issue { "engine-version-untested",
                std::format("clangd {} has not been run through this server's conformance suite; every workaround for clangd 23.1 stays on", options_.version), "" });
        }
        return status;
    }

    Json report() const override {
        Json restarts = Json::array();
        for (const auto& [at, reason] : restartHistory_) restarts.push_back(Json { { "at", at }, { "reason", reason } });
        Json unresolved = Json::object();
        for (const auto& [name, module] : unresolvedModules_) unresolved[name] = Json { { "reason", module.reason }, { "provider", module.provider } };
        Json compileFailures = Json::array();
        for (const auto& name : reportedFailures_) compileFailures.push_back(name);
        const auto [done, wanted] = primer_.progress();
        return Json {
            { "executable", options_.executable },
            { "arguments", clangd_arguments(lastConfig_) },
            { "generation", generation_ },
            { "handshakeDone", handshakeDone_ },
            { "accepting", accepting_ },
            { "unavailable", unavailable_ },
            { "recentExits", crashes_.size() },
            { "restarts", std::move(restarts) },
            { "restartScheduled", restartAt_.has_value() },
            { "filesSetAside", quarantine_.members() },
            { "unresolvedModules", std::move(unresolved) },
            { "modulesThatDidNotCompile", std::move(compileFailures) },
            { "stdFromSemanticKit", stdFromKit_ },
            { "preparation", Json { { "done", done }, { "wanted", wanted }, { "running", primer_.running() },
                                    { "limit", preparation_limit(std::thread::hardware_concurrency(), mcppls::os::FAMILY == mcppls::os::Family::macos,
                                                                 awaitingDiagnostics_.size()) } } },
            { "pendingRequests", pending_.size() },
            { "deferredRequests", deferred_.size() },
            { "filesAwaitingDiagnostics", awaitingDiagnostics_.size() },
            { "logLinesLeftOut", linesLeftOut_ },
            { "databaseDirectory", databaseDirectory_ },
        };
    }

    void start(Host& host) override {
        host_ = &host;
        sink_ = host.event_sink(ENGINE_ID);
        const std::string& cache { host.cache_directory() };
        databaseDirectory_ = base::join_path(cache, "contexts/default/cdb");
        primeDirectory_ = base::join_path(cache, "contexts/default/prime");
        moduleHintDirectory_ = base::join_path(cache, "contexts/default/module-hints");   // never created
        stubDirectory_ = base::join_path(cache, "contexts/default/stubs");
        (void)platform::fs::create_directories(databaseDirectory_);
        // clangd starts without a database; the first plan is written before any document reaches it.
        platform::fs::remove_all(base::join_path(databaseDirectory_, "compile_commands.json"));
        log::info("clangd {} at {}", options_.version.empty() ? "?" : options_.version, options_.executable.empty() ? "(none)" : options_.executable);
        start_process_();
    }

    void shut_down() override {
        if (process_ && process_->running() && handshakeDone_) (void)send_(lsp::make_notification("exit", nullptr));
        if (process_) process_->stop(std::chrono::seconds { 2 });
    }

    void configure_plan(normalize::PlanInput& input) const override {
        input.engineDriverDirectory = options_.executable.empty() ? std::string {} : base::parent_path(options_.executable);
        for (const auto& [name, unresolved] : unresolvedModules_) {
            // The kit brings its own standard library: what clangd could not find of the toolchain's does not apply.
            if (stdFromKit_ && input.kit != nullptr && (name == "std" || name == "std.compat")) continue;
            input.unresolvedModules.emplace(name, unresolved.reason);
        }
        input.preferKit = stdFromKit_;
        input.primeDirectory = traits_.needsModulePreparation ? primeDirectory_ : std::string {};
        input.moduleHintDirectory = traits_.needsModuleHints ? moduleHintDirectory_ : std::string {};
        input.stubDirectory = traits_.hangsOnUnresolvedImports ? stubDirectory_ : std::string {};
        input.excludeUnresolvedImports = traits_.hangsOnUnresolvedImports;
        input.noAlignedAllocationWithMsvcStl = traits_.msvcStlNeedsNoAlignedAllocation;
    }

    void apply(const normalize::EnginePlan* plan) override {
        if (plan == nullptr) {
            // The project model did not load in time: serve without a database.
            if (!planApplied_) {
                planApplied_ = true;
                accept_traffic_if_ready_();
            }
            return;
        }
        write_prime_sources_(*plan);
        const std::string database { normalize::to_compile_commands(*plan).dump(1) };
        const std::string structure { normalize::to_compile_commands(*plan, false).dump(1) };
        const bool changed { database != writtenDatabase_ };
        // Hints alone change as imports do; clangd rereads the database within five seconds.
        const bool structureChanged { structure != writtenStructure_ };
        writtenStructure_ = structure;
        if (changed) {
            // clangd rereads --compile-commands-dir/compile_commands.json itself (v1 design 15.1).
            if (auto written = platform::fs::write_file_atomic(base::join_path(databaseDirectory_, "compile_commands.json"), database); !written) {
                log::error("cannot write the engine database ({}): {}", host_->root_directory(), written.error().message);
            }
            writtenDatabase_ = database;
            log::info("engine database ({}): {} entries ({} standard library units, {} stand-ins), {} left out, {} issues", host_->root_directory(),
                      plan->entries.size(), plan->stdUnits, plan->stubModules.size(), plan->excludedFiles.size(), plan->issues.size());
            host_->record_event("engine-database", Json { { "entries", plan->entries.size() }, { "standIns", plan->stubModules.size() },
                                                          { "leftOut", plan->excludedFiles.size() } });
        }
        std::set<std::string> newExcluded;
        for (const auto& file : plan->excludedFiles) newExcluded.insert(base::path_key(file));
        std::map<std::string, std::string, std::less<>> newModuleSources;
        std::map<std::string, std::string, std::less<>> newModuleCommands;
        for (const auto& entry : plan->entries) {
            if (entry.provides.empty()) continue;
            newModuleSources.emplace(entry.provides, entry.file);
            newModuleCommands.emplace(entry.provides, lsp::dump(Json(entry.arguments)));
        }
        // robustness design C4: clangd keeps where it found a module after the database drops that unit, and
        // building the dropped unit can deadlock it (experiment S12). Only a provider leaving, or moving, needs a
        // fresh clangd; new units, units coming back and every other change are read from the database as it is.
        std::set<std::string, std::less<>> imported;
        for (const auto& entry : plan->entries) imported.insert(entry.imports.begin(), entry.imports.end());
        bool providerLeft { false };
        for (const auto& [name, source] : moduleSources_) {
            const auto now = newModuleSources.find(name);
            const bool moved { now == newModuleSources.end() || !base::same_path(now->second, source) };
            // A module nothing imports any more cannot be built by mistake.
            if (moved && imported.contains(name)) providerLeft = true;
        }
        // A unit of the project compiled with other arguments (another context, changed build flags): clangd
        // does not rebuild a document it has open for a changed database, so a fresh clangd applies them.
        std::map<std::string, std::string, std::less<>> newArguments;
        for (const auto& entry : plan->entries) newArguments.emplace(base::path_key(entry.file), lsp::dump(Json(entry.arguments)));
        bool argumentsChanged { false };
        for (const auto& [file, arguments] : writtenArguments_) {
            if ((!stubDirectory_.empty() && base::is_within(file, base::path_key(stubDirectory_)))
                || (!primeDirectory_.empty() && base::is_within(file, base::path_key(primeDirectory_)))) continue;
            if (const auto now = newArguments.find(file); now != newArguments.end() && now->second != arguments) argumentsChanged = true;
        }
        writtenArguments_ = std::move(newArguments);
        (void)structureChanged;
        const bool restartNeeded { (providerLeft || argumentsChanged) && planApplied_ && handshakeDone_ };
        if (!restartNeeded && accepting_) {
            for (const auto& document : host_->documents()) {
                if (document.path.empty()) continue;
                const std::string pathKey { base::path_key(document.path) };
                const bool wasExcluded { excluded_.contains(pathKey) };
                const bool isExcluded { newExcluded.contains(pathKey) };
                if (wasExcluded && !isExcluded) open_in_engine_(document);
                if (!wasExcluded && isExcluded) {
                    (void)send_(lsp::make_notification("textDocument/didClose", Json { { "textDocument", Json { { "uri", document.uri } } } }));
                }
            }
        }
        excluded_ = std::move(newExcluded);
        {
            std::vector<PrimeModule> modules;
            for (const auto& module : plan->modules) modules.push_back(PrimeModule { module.name, module.requires_, module.primeFile });
            // An edit that leaves the module graph as it was leaves its preparation running, units and all.
            if (restartNeeded || !primer_.same_modules(modules)) {
                close_prime_units_();
                primer_.set_modules(std::move(modules));
            }
        }
        moduleSources_ = std::move(newModuleSources);
        moduleCommands_ = std::move(newModuleCommands);
        startupBmis_.reset();
        planApplied_ = true;
        const bool forgot { forget_changed_unresolved_() };
        if (restartNeeded) {
            request_restart_(providerLeft ? "a module's unit left the engine database" : "units are compiled with other arguments");
        } else {
            accept_traffic_if_ready_();
            prepare_modules_();
        }
        if (forgot) host_->request_replan();
    }

    void document(const DocumentEvent& event) override {
        const DocumentView& document { event.document };
        switch (event.change) {
        case DocumentChange::opened:
            touch_(document.path);
            if (accepting_ && !excluded_path_(document.path) && !quarantined_(document.path)) {
                open_in_engine_(document);
                prepare_imports_of_(document);
            }
            break;
        case DocumentChange::changed:
            touch_(document.path);
            // A file set aside goes back to clangd when it changes, with its whole text.
            if (!document.path.empty() && quarantine_.release(base::path_key(document.path))) {
                update_quarantine_issue_();
                if (accepting_ && !excluded_path_(document.path)) open_in_engine_(document);
                break;
            }
            if (accepting_ && !excluded_path_(document.path) && event.message != nullptr) (void)send_(*event.message);
            break;
        case DocumentChange::closed: {
            const bool wasExcluded { excluded_path_(document.path) || quarantined_(document.path) };
            awaitingDiagnostics_.erase(document.uri);
            awaitingSince_.erase(document.uri);
            diagnosed_.erase(document.uri);
            if (accepting_ && !wasExcluded && event.message != nullptr) (void)send_(*event.message);
            release_prime_units_if_idle_();
            break;
        }
        case DocumentChange::saved:
            if (!document.path.empty() && !excluded_path_(document.path) && !quarantined_(document.path) && accepting_ && event.message != nullptr) {
                (void)send_(*event.message);
            }
            sources_changed();
            break;
        }
    }

    void notify(const Json& message) override {
        if (accepting_) {
            (void)send_(message);
        } else if (!unavailable_ && message.value("method", std::string {}) != lsp::method::WORKSPACE_DID_CHANGE_WATCHED_FILES) {
            deferred_.emplace_back(message, Reply {});
        }
    }

    void sources_changed() override {
        modulesFailedAt_.clear();   // a module that still does not compile is reported again
        if (forget_changed_unresolved_()) host_->request_replan();
    }

    bool claims(const RequestView& request) const override { return !unavailable_ && !excluded_path_(request.path) && !quarantined_(request.path); }

    void request(const RequestView&, const Json& message, Reply reply) override {
        if (unavailable_) {
            reply(Answer {});
            return;
        }
        if (!accepting_) {
            deferred_.emplace_back(message, std::move(reply));
            return;
        }
        request_now_(message, std::move(reply));
    }

    void cancel(const Json& clientRequestId) override {
        for (auto it = deferred_.begin(); it != deferred_.end(); ++it) {
            if (lsp::kind_of(it->first) == lsp::Kind::request && it->first["id"] == clientRequestId) {
                Reply reply { std::move(it->second) };
                deferred_.erase(it);
                if (reply) reply(Answer { Answer::Kind::cancelled, nullptr });
                return;
            }
        }
        for (const auto& [engineId, request] : pending_) {
            if (request.purpose == Purpose::client && request.clientId == clientRequestId) {
                (void)send_(lsp::make_notification("$/cancelRequest", Json { { "id", engineId } }));
                return;
            }
        }
    }

    void client_response(int generation, const Json& engineRequestId, const Json& response) override {
        if (generation != generation_) return;
        Json forwarded = response;
        forwarded["id"] = engineRequestId;
        (void)send_(forwarded);
    }

    void handle_event(const Json& event) override {
        if (event.value("generation", -1) != generation_) return;
        const std::string kind { event.value("kind", std::string {}) };
        if (kind == "message") {
            handle_message_(event["message"]);
        } else if (kind == "closed") {
            handle_closed_();
        } else if (kind == "module-failed") {
            handle_module_failure_(event["failure"]);
        } else if (kind == "log-left-out") {
            linesLeftOut_ += event.value("count", std::size_t { 0 });
            host_->record_event("engine-log-left-out", Json { { "count", event.value("count", std::size_t { 0 }) } });
        }
    }

    std::optional<Clock::time_point> next_deadline() const override {
        std::optional<Clock::time_point> deadline;
        auto consider = [&](const std::optional<Clock::time_point>& at) {
            if (at && (!deadline || *at < *deadline)) deadline = at;
        };
        for (const auto& [id, request] : pending_) consider(request.deadline);
        for (const auto& [module, at] : primeDeadlines_) consider(at);
        consider(restartAt_);
        consider(stuckCheckAt_);
        return deadline;
    }

    void handle_timers() override {
        const auto now = Clock::now();
        std::vector<std::int64_t> expired;
        for (const auto& [id, request] : pending_) {
            if (request.deadline <= now) expired.push_back(id);
        }
        bool restart { false };
        bool stalled { false };
        for (const auto id : expired) {
            PendingRequest request { std::move(pending_[id]) };
            pending_.erase(id);
            switch (request.purpose) {
            case Purpose::client: {
                const bool filePreparing { awaitingDiagnostics_.contains(host_->client_uri(request.uri)) && primer_.busy() };
                if (keep_waiting(request, filePreparing, lastPrimeProgressAt_, now)) {
                    request.deadline = std::min(now + PREPARING_GRACE, request.limit);
                    pending_[id] = std::move(request);
                    break;
                }
                log::warning("clangd ({}) did not answer {} in time", host_->root_directory(), request.method);
                host_->record_event("request-timeout", Json { { "method", request.method }, { "file", host_->path_of_uri(request.uri) },
                                                              { "seconds", std::chrono::duration_cast<std::chrono::seconds>(now - request.sent).count() } });
                if (request.reply) request.reply(Answer {});
                (void)send_(lsp::make_notification("$/cancelRequest", Json { { "id", id } }));
                // A file whose modules are still being built is slow, not stuck: setting it aside would throw that work away.
                if (awaitingDiagnostics_.contains(host_->client_uri(request.uri))) break;
                const std::string path { host_->path_of_uri(request.uri) };
                if (path.empty()) break;
                switch (quarantine_.timed_out(base::path_key(path), request.sent, now, lastAnswerAt_)) {
                case Quarantine::Verdict::wait: break;
                case Quarantine::Verdict::quarantined: set_aside_(path, "it stopped answering its requests"); break;
                case Quarantine::Verdict::stalled: stalled = true; break;
                }
                break;
            }
            case Purpose::engine_initialize:
                log::error("clangd ({}) did not answer initialize", host_->root_directory());
                add_issue_(Issue { "engine-timeout", "clangd did not answer initialize", "mcppls.restartServer" });
                host_->engine_settled(ENGINE_ID, Json::object());
                restart = true;
                break;
            }
        }
        if (stalled) {
            // clangd answered nobody: the engine is stuck. The file asked about first is the likeliest cause.
            host_->record_event("engine-stalled", Json { { "firstFile", quarantine_.first_stalled().value_or(std::string {}) } });
            if (auto first = quarantine_.first_stalled()) {
                for (const auto& document : host_->documents()) {
                    if (!document.path.empty() && base::path_key(document.path) == *first) set_aside_(document.path, "clangd stopped answering after it");
                }
            }
            add_issue_(Issue { "engine-timeout", "clangd stopped answering; it was restarted", "mcppls.restartServer" });
            request_restart_("clangd stopped answering");
        } else if (restart) {
            request_restart_("clangd did not answer initialize");
        }
        for (const auto& key : quarantine_.due(now)) {
            for (const auto& document : host_->documents()) {
                if (document.path.empty() || base::path_key(document.path) != key) continue;
                log::info("handing {} back to clangd ({})", document.path, host_->root_directory());
                host_->record_event("file-handed-back", Json { { "file", document.path } });
                if (accepting_ && !excluded_path_(document.path)) open_in_engine_(document);
            }
            update_quarantine_issue_();
        }
        std::vector<std::string> overdue;
        for (const auto& [module, at] : primeDeadlines_) {
            if (at <= now) overdue.push_back(module);
        }
        for (const auto& module : overdue) {
            log::warning("stopped waiting for module {} to be prepared ({})", module, host_->root_directory());
            if (const auto* planned = primer_.find(module)) (void)finish_prime_(base::path_to_uri(planned->primeFile));
        }
        if (stuckCheckAt_ && *stuckCheckAt_ <= now) check_stuck_files_(now);
        if (restartAt_ && *restartAt_ <= now) {
            restartAt_.reset();
            restart_(restartReason_.empty() ? std::string_view { "recovering from an exit" } : std::string_view { restartReason_ });
        }
        if (!expired.empty()) host_->status_changed();
    }

private:
    std::unique_ptr<Process> make_process_() const {
        return options_.processFactory ? options_.processFactory() : std::make_unique<ClangdProcess>();
    }

    bool excluded_path_(std::string_view path) const { return !path.empty() && excluded_.contains(base::path_key(path)); }

    void add_issue_(Issue issue) {
        for (const auto& existing : issues_) {
            if (existing.code == issue.code) return;
        }
        issues_.push_back(std::move(issue));
    }

    Json engine_view_(const Json& message) const {
        Json copy = message;
        const auto fix = [&](Json& uri) {
            if (uri.is_string()) uri = host_->engine_uri(uri.get<std::string>());
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

    bool send_(const Json& message) {
        if (!process_ || !process_->running()) return false;
        if (auto sent = process_->send(engine_view_(message)); !sent) {
            log::warning("cannot write to clangd ({}): {}", host_->root_directory(), sent.error().message);
            return false;
        }
        return true;
    }

    void flush_deferred_without_engine_() {
        std::vector<std::pair<Json, Reply>> toFlush;
        toFlush.swap(deferred_);
        for (auto& [message, reply] : toFlush) {
            if (reply) reply(Answer {});
        }
    }

    void start_process_() {
        handshakeDone_ = false;
        accepting_ = false;
        if (options_.payloadCorrupt) {
            unavailable_ = true;
            add_issue_(Issue { "payload-corrupt", "the extension's payload is corrupt or was modified; reinstall the extension", "mcppls.showLogs" });
            flush_deferred_without_engine_();
            host_->engine_settled(ENGINE_ID, Json::object());
            host_->status_changed();
            return;
        }
        if (options_.executable.empty() || !platform::fs::is_regular_file(options_.executable)) {
            unavailable_ = true;
            add_issue_(Issue { "engine-missing", "clangd was not found; only module-level features are available", "mcppls.showLogs" });
            flush_deferred_without_engine_();
            host_->engine_settled(ENGINE_ID, Json::object());
            host_->status_changed();
            return;
        }
        const int generation { ++generation_ };
        ProcessConfig config;
        config.executable = options_.executable;
        config.version = options_.version;
        config.databaseDirectory = databaseDirectory_;
        config.workDirectory = host_->root_directory();
        config.verboseLog = options_.verboseLog;
        config.workers = engine_workers(std::thread::hardware_concurrency(), mcppls::os::FAMILY == mcppls::os::Family::macos);
        config.extraArguments = options_.extraArguments;
        // Extra engine arguments for troubleshooting, e.g. MCPPLS_ENGINE_ARGUMENTS="-j=8 --background-index-priority=background".
        if (auto extra = platform::env::get("MCPPLS_ENGINE_ARGUMENTS")) {
            for (auto word : base::split(*extra, ' ')) {
                if (!base::trim(word).empty()) config.extraArguments.emplace_back(base::trim(word));
            }
        }
        if (!process_) process_ = make_process_();
        lastConfig_ = config;
        host_->record_event("engine-start", Json { { "engine", std::string { ENGINE_ID } }, { "generation", generation }, { "arguments", clangd_arguments(config) } });
        auto sink = sink_;
        const std::string root { host_->root_directory() };
        // robustness design C7: clangd's errors are forwarded without flooding the log; failures are read from every line.
        // A verbose log is asked for to see everything, so it is not limited.
        struct LimitedLog {
            explicit LimitedLog(std::size_t burst) : limiter { burst, std::chrono::seconds { 10 } } {}
            std::mutex mutex;
            LineLimiter limiter;
        };
        auto limited = std::make_shared<LimitedLog>(options_.verboseLog ? std::numeric_limits<std::size_t>::max() : std::size_t { 40 });
        auto started = process_->start(
            config,
            [sink, generation](Json message) { sink(Json { { "kind", "message" }, { "generation", generation }, { "message", std::move(message) } }); },
            [sink, generation] { sink(Json { { "kind", "closed" }, { "generation", generation } }); },
            [sink, generation, root, limited](std::string_view line) {
                LineLimiter::Decision decision;
                {
                    const std::lock_guard lock { limited->mutex };
                    decision = limited->limiter.admit(GuardClock::now());
                }
                if (decision.suppressedBefore > 0) {
                    log::info("clangd ({}): {} more lines left out of this log", root, decision.suppressedBefore);
                    sink(Json { { "kind", "log-left-out" }, { "generation", generation }, { "count", decision.suppressedBefore } });
                }
                if (decision.forward) log::info("clangd ({}): {}", root, line);
                if (auto failure = parse_module_failure(line)) {
                    sink(Json { { "kind", "module-failed" }, { "generation", generation },
                                { "failure", Json { { "module", failure->module }, { "reason", failure->reason }, { "source", failure->failedSource } } } });
                }
            });
        if (!started) {
            unavailable_ = true;
            add_issue_(Issue { "engine-crashed", std::format("clangd could not start: {}", started.error().message), "mcppls.restartServer" });
            flush_deferred_without_engine_();
            host_->engine_settled(ENGINE_ID, Json::object());
            host_->status_changed();
            return;
        }
        unavailable_ = false;
        Json params = host_->client_initialize_params();
        params.erase("initializationOptions");
        params["processId"] = nullptr;
        // Positions are UTF-16 everywhere in this server.
        if (params.contains("capabilities") && params["capabilities"].is_object()) {
            params["capabilities"].erase("offsetEncoding");
            if (params["capabilities"].contains("general") && params["capabilities"]["general"].is_object()) {
                params["capabilities"]["general"].erase("positionEncodings");
            }
        }
        const std::int64_t engineId { nextId_++ };
        pending_[engineId] = PendingRequest { Purpose::engine_initialize, nullptr, "initialize", {}, Clock::now() + std::chrono::seconds { 60 }, generation, {}, {} };
        (void)send_(lsp::make_request(engineId, "initialize", std::move(params)));
    }

    void restart_(std::string_view reason) {
        log::info("restarting clangd ({}): {}", host_->root_directory(), reason);
        restartGate_.record(Clock::now());
        restartHistory_.emplace_back(std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now())), std::string { reason });
        if (restartHistory_.size() > 20) restartHistory_.pop_front();
        host_->record_event("engine-restart", Json { { "reason", std::string { reason } } });
        restartAt_.reset();
        // Requests to the old process are answered by the other engines.
        auto old = std::move(pending_);
        pending_.clear();
        for (auto& [id, request] : old) {
            if (request.purpose == Purpose::client && request.reply) request.reply(Answer {});
        }
        forget_primes_();
        ++generation_;   // late events of the old process are ignored
        if (process_) process_->stop(std::chrono::milliseconds { 500 });
        diagnosed_.clear();
        host_->forget_engine_diagnostics(ENGINE_ID);
        start_process_();
    }

    void request_now_(const Json& message, Reply reply) {
        const Json& id { message["id"] };
        const std::string method { message.value("method", std::string {}) };
        const Json* params { lsp::find(message, "params") };
        const Json* uri { params != nullptr ? lsp::find_path(*params, { "textDocument", "uri" }) : nullptr };
        const std::int64_t engineId { nextId_++ };
        const auto timeout = is_interactive(method) ? std::min(options_.requestTimeout, INTERACTIVE_TIMEOUT) : options_.requestTimeout;
        const auto now = Clock::now();
        pending_[engineId] = PendingRequest { Purpose::client, id, method, uri != nullptr && uri->is_string() ? uri->get<std::string>() : std::string {},
                                              now + timeout, generation_, now + options_.requestTimeout, std::move(reply), now };
        Json forwarded = message;
        forwarded["id"] = engineId;
        if (!send_(forwarded)) {
            auto it = pending_.find(engineId);
            Reply failed { std::move(it->second.reply) };
            pending_.erase(it);
            if (failed) failed(Answer {});
        }
    }

    void open_in_engine_(const DocumentView& document) {
        Json params { { "textDocument", Json { { "uri", document.uri }, { "languageId", document.languageId },
                                               { "version", document.version }, { "text", std::string { document.text } } } } };
        if (send_(lsp::make_notification("textDocument/didOpen", std::move(params)))) {
            if (!diagnosed_.contains(document.uri)) {
                awaitingDiagnostics_.insert(document.uri);
                const auto now = Clock::now();
                awaitingSince_[document.uri] = now;
                schedule_stuck_check_(now + GENERAL_PATIENCE);
            }
        }
    }

    void schedule_stuck_check_(Clock::time_point at) {
        if (!stuckCheckAt_ || at < *stuckCheckAt_) stuckCheckAt_ = at;
    }

    // Files clangd will not publish diagnostics for go to mcppls's engine until they change.
    void check_stuck_files_(Clock::time_point now) {
        stuckCheckAt_.reset();
        std::vector<std::pair<std::string, std::string>> stuck;
        const bool preparing { lastPrimeProgressAt_ && now - *lastPrimeProgressAt_ < std::chrono::seconds { 60 } };
        for (const auto& document : host_->documents()) {
            const auto since = awaitingSince_.find(document.uri);
            if (document.path.empty() || since == awaitingSince_.end() || !awaitingDiagnostics_.contains(document.uri)) continue;
            // A unit of a module other than its interface: an implementation unit or a partition.
            const auto scan = project::scan_source(document.text);
            if (scan.declaration && (!scan.declaration->isExported || !scan.declaration->partition.empty())) {
                if (const auto failed = modulesFailedAt_.find(scan.declaration->module); failed != modulesFailedAt_.end()) {
                    const auto due = std::max(failed->second, since->second) + FAILED_MODULE_PATIENCE;
                    if (now >= due) {
                        stuck.emplace_back(document.path, std::format("module {} did not compile and clangd published nothing for this unit of it", failed->first));
                        continue;
                    }
                    schedule_stuck_check_(due);
                }
            }
            const auto due = since->second + GENERAL_PATIENCE;
            if (now >= due && !preparing) {
                stuck.emplace_back(document.path, "clangd published no diagnostics for it in two minutes, and no module was being prepared");
                continue;
            }
            schedule_stuck_check_(now >= due ? now + std::chrono::seconds { 30 } : due);
        }
        for (const auto& [path, why] : stuck) set_aside_(path, why);
    }

    void accept_traffic_if_ready_() {
        if (!handshakeDone_ || !planApplied_ || accepting_) return;
        accepting_ = true;
        for (const auto& document : host_->documents()) {
            if (!excluded_path_(document.path) && !quarantined_(document.path)) open_in_engine_(document);
        }
        std::vector<std::pair<Json, Reply>> toFlush;
        toFlush.swap(deferred_);
        for (auto& [message, reply] : toFlush) {
            if (lsp::kind_of(message) == lsp::Kind::request) {
                const Json* params { lsp::find(message, "params") };
                const Json* uri { params != nullptr ? lsp::find_path(*params, { "textDocument", "uri" }) : nullptr };
                const std::string path { uri != nullptr && uri->is_string() ? host_->path_of_uri(uri->get<std::string>()) : std::string {} };
                if (excluded_path_(path) || quarantined_(path)) {
                    if (reply) reply(Answer {});
                } else {
                    request_now_(message, std::move(reply));
                }
            } else {
                (void)send_(message);
            }
        }
        prepare_modules_();
        host_->status_changed();
    }

    // ---- messages from clangd ----------------------------------------------------------

    void handle_message_(const Json& message) {
        switch (lsp::kind_of(message)) {
        case lsp::Kind::response: handle_response_(message); break;
        case lsp::Kind::request: {
            Json forwarded = message;
            forwarded["id"] = host_->client_request_id(ENGINE_ID, generation_, message["id"]);
            if (forwarded.contains("params")) host_->client_view(forwarded["params"]);
            host_->send_to_client(forwarded);
            break;
        }
        case lsp::Kind::notification: handle_notification_(message); break;
        case lsp::Kind::invalid: break;
        }
    }

    void handle_response_(const Json& message) {
        const Json& id { message["id"] };
        if (!id.is_number_integer()) return;
        const auto it = pending_.find(id.get<std::int64_t>());
        if (it == pending_.end()) return;   // answered already, after a timeout
        PendingRequest request { std::move(it->second) };
        pending_.erase(it);
        switch (request.purpose) {
        case Purpose::engine_initialize: {
            const Json capabilities = lsp::find_path(message, { "result", "capabilities" }) != nullptr ? message["result"]["capabilities"] : Json::object();
            host_->engine_settled(ENGINE_ID, capabilities);
            (void)send_(lsp::make_notification("initialized", Json::object()));
            handshakeDone_ = true;
            accept_traffic_if_ready_();
            host_->status_changed();
            break;
        }
        case Purpose::client: {
            lastAnswerAt_ = Clock::now();
            if (const std::string path { host_->path_of_uri(request.uri) }; !path.empty()) quarantine_.answered(base::path_key(path));
            if (!request.reply) return;
            if (message.contains("error")) {
                request.reply(Answer { Answer::Kind::error, message["error"] });
                return;
            }
            Json result = message.value("result", Json {});
            drop_generated_locations_(result);
            host_->client_view(result);
            request.reply(Answer { Answer::Kind::result, std::move(result) });
            break;
        }
        }
    }

    void handle_notification_(const Json& message) {
        const std::string method { message.value("method", std::string {}) };
        if (method == lsp::method::TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS) {
            const Json& params { message["params"] };
            if (finish_prime_(params.value("uri", std::string {}))) return;
            const std::string uri { host_->client_uri(params.value("uri", std::string {})) };
            awaitingDiagnostics_.erase(uri);
            awaitingSince_.erase(uri);
            release_prime_units_if_idle_();
            if (!host_->has_document(uri)) {
                Json forwarded = message;
                host_->client_view(forwarded["params"]);
                host_->send_to_client(forwarded);
            } else {
                diagnosed_.insert(uri);
                const auto version = lsp::int_at(params, "version");
                host_->publish_engine_diagnostics(ENGINE_ID, uri, params.value("diagnostics", Json::array()), version);
            }
            host_->status_changed();
            return;
        }
        if (method == lsp::method::WINDOW_SHOW_MESSAGE) {
            // Nothing pops up from the engine; it goes to the log (v1 design 16.2).
            Json forwarded = message;
            forwarded["method"] = "window/logMessage";
            host_->send_to_client(forwarded);
            return;
        }
        host_->send_to_client(message);
    }

    void handle_closed_() {
        handshakeDone_ = false;
        accepting_ = false;
        forget_primes_();
        log::warning("clangd exited unexpectedly ({})", host_->root_directory());
        auto old = std::move(pending_);
        pending_.clear();
        for (auto& [id, request] : old) {
            if (request.purpose == Purpose::client && request.reply) request.reply(Answer {});
        }
        const auto now = Clock::now();
        crashes_.push_back(now);
        while (!crashes_.empty() && now - crashes_.front() > std::chrono::minutes { 5 }) crashes_.pop_front();
        add_issue_(Issue { "engine-crashed", "clangd exited unexpectedly", "mcppls.restartServer" });
        // robustness design C6: what clangd was asked about, or given, just before it exited is set aside, so
        // one file that crashes it does not take clangd away from the others.
        std::set<std::string> suspects;
        for (const auto& [id, request] : old) {
            if (request.purpose == Purpose::client) {
                if (const std::string path { host_->path_of_uri(request.uri) }; !path.empty()) suspects.insert(path);
            }
        }
        for (const auto& document : host_->documents()) {
            const auto touched = touchedAt_.find(base::path_key(document.path));
            if (!document.path.empty() && touched != touchedAt_.end() && now - touched->second < std::chrono::seconds { 10 }) suspects.insert(document.path);
        }
        host_->record_event("engine-exit", Json { { "recentExits", crashes_.size() }, { "suspects", Json(std::vector<std::string> { suspects.begin(), suspects.end() }) } });
        for (const auto& path : suspects) set_aside_(path, "clangd exited while working on it");
        if (crashes_.size() >= 5) {
            unavailable_ = true;
            flush_deferred_without_engine_();
        } else {
            restartReason_ = "recovering from an exit";
            restartAt_ = std::max(now + std::chrono::seconds { 1 << std::min<std::size_t>(crashes_.size() - 1, 6) }, restartGate_.earliest(now));
        }
        host_->status_changed();
    }

    void handle_module_failure_(const Json& failure) {
        const ModuleFailure parsed { failure.value("module", std::string {}), failure.value("reason", std::string {}), failure.value("source", std::string {}) };
        if (parsed.module.empty()) return;
        const FailureKind kind { failure_kind(parsed) };
        // The standard library, which nearly every unit imports: a failure there is how this server built it,
        // not the project, and it would take nearly every module with it. The semantic kit brings its own
        // (robustness design C5).
        const auto standard = moduleSources_.find("std");
        const bool stdFailed { parsed.module == "std" || parsed.module == "std.compat"
                               || (kind == FailureKind::compile && standard != moduleSources_.end() && base::same_path(parsed.failedSource, standard->second)) };
        if (stdFailed && kind != FailureKind::other && !stdFromKit_) {
            stdFromKit_ = true;
            log::warning("clangd could not build the standard library module ({}): {}; reading the project with the semantic kit",
                         host_->root_directory(), parsed.reason);
            host_->record_event("std-fallback-kit", Json { { "module", parsed.module }, { "reason", parsed.reason } });
            add_issue_(Issue { "std-fallback-kit",
                std::format("clangd could not build the toolchain's standard library module ({}); files are read with the semantic kit", parsed.reason),
                "mcppls.showLogs" });
            host_->request_replan();
            host_->status_changed();
        }
        if (kind == FailureKind::compile) {
            const auto now = Clock::now();
            modulesFailedAt_[parsed.module] = now;
            schedule_stuck_check_(now + FAILED_MODULE_PATIENCE);
        }
        if (kind != FailureKind::unresolved) {
            // Found and not compiled: its importers get errors, they do not hang (experiment S3). Nothing to replan.
            if (reportedFailures_.insert(parsed.module).second) {
                log::info("clangd could not build module {} ({}): {}", parsed.module, host_->root_directory(), parsed.reason);
                host_->record_event("module-failed", Json { { "module", parsed.module }, { "kind", kind == FailureKind::compile ? "compile" : "other" },
                                                            { "reason", parsed.reason } });
            }
            return;
        }
        if (unresolvedModules_.contains(parsed.module)) return;
        log::warning("clangd could not find module {} ({}): {}", parsed.module, host_->root_directory(), parsed.reason);
        host_->record_event("module-failed", Json { { "module", parsed.module }, { "kind", "unresolved" }, { "reason", parsed.reason } });
        UnresolvedModule unresolved { parsed.reason, {}, {}, {} };
        if (const auto provider = moduleSources_.find(parsed.module); provider != moduleSources_.end()) {
            unresolved.provider = provider->second;
            unresolved.stamp = platform::fs::stamp(provider->second);
            unresolved.command = moduleCommands_.contains(parsed.module) ? moduleCommands_.find(parsed.module)->second : std::string {};
        }
        unresolvedModules_.emplace(parsed.module, std::move(unresolved));
        host_->request_replan();
    }

    // Unresolved modules whose unit or command is no longer what it was when clangd reported them are
    // forgotten, so the next plan tries them again. Returns whether any was.
    bool forget_changed_unresolved_() {
        bool forgot { false };
        for (auto it = unresolvedModules_.begin(); it != unresolvedModules_.end();) {
            const auto provider = moduleSources_.find(it->first);
            const std::string current { provider == moduleSources_.end() ? std::string {} : provider->second };
            const auto command = moduleCommands_.find(it->first);
            const bool changed { !base::same_path(current, it->second.provider) || (!current.empty() && platform::fs::stamp(current) != it->second.stamp)
                                 || (!current.empty() && (command == moduleCommands_.end() ? std::string {} : command->second) != it->second.command) };
            if (changed) {
                log::info("trying module {} again ({})", it->first, host_->root_directory());
                host_->record_event("module-retry", Json { { "module", it->first } });
                it = unresolvedModules_.erase(it);
                forgot = true;
            } else {
                ++it;
            }
        }
        return forgot;
    }

    // ---- files set aside, and restarts -------------------------------------------------------

    void touch_(std::string_view path) {
        if (!path.empty()) touchedAt_[base::path_key(path)] = Clock::now();
    }

    bool quarantined_(std::string_view path) const { return !path.empty() && quarantine_.contains(base::path_key(path)); }

    void set_aside_(const std::string& path, std::string_view why) {
        const std::string key { base::path_key(path) };
        if (!quarantine_.contains(key)) quarantine_.put(key, Clock::now());
        log::warning("setting {} aside from clangd for a while ({}): {}; mcppls's engine answers for it", path, host_->root_directory(), why);
        host_->record_event("file-set-aside", Json { { "file", path }, { "why", std::string { why } } });
        for (const auto& document : host_->documents()) {
            if (document.path.empty() || base::path_key(document.path) != key) continue;
            awaitingDiagnostics_.erase(document.uri);
            awaitingSince_.erase(document.uri);
            if (accepting_) (void)send_(lsp::make_notification("textDocument/didClose", Json { { "textDocument", Json { { "uri", document.uri } } } }));
        }
        update_quarantine_issue_();
    }

    void update_quarantine_issue_() {
        std::erase_if(issues_, [](const Issue& issue) { return issue.code == "file-quarantined"; });
        if (const std::size_t count { quarantine_.size() }; count > 0) {
            issues_.push_back(Issue { "file-quarantined",
                std::format("clangd stopped answering for {} file{}; mcppls's engine answers for {} until {} changes", count, count == 1 ? "" : "s",
                            count == 1 ? "it" : "them", count == 1 ? "it" : "they"), "mcppls.restartServer" });
        }
        host_->status_changed();
    }

    // A restart now, or as soon as the gate allows (robustness design C4).
    void request_restart_(std::string_view reason) {
        const auto now = Clock::now();
        const auto at = restartGate_.earliest(now);
        if (at <= now) {
            restart_(reason);
            return;
        }
        if (!restartAt_ || at < *restartAt_) {
            restartAt_ = at;
            restartReason_ = std::string { reason };
            host_->record_event("engine-restart-deferred", Json { { "reason", std::string { reason } },
                                                                  { "seconds", std::chrono::duration_cast<std::chrono::seconds>(at - now).count() } });
            log::info("restarting clangd ({}) in {} s: {}", host_->root_directory(),
                      std::chrono::duration_cast<std::chrono::seconds>(at - now).count(), reason);
        }
    }

    // ---- parallel module preparation ------------------------------------------------------

    void write_prime_sources_(const normalize::EnginePlan& plan) {
        if (!plan.primeSources.empty()) (void)platform::fs::create_directories(primeDirectory_);
        if (!plan.stubSources.empty()) (void)platform::fs::create_directories(stubDirectory_);
        for (const auto& sources : { &plan.primeSources, &plan.stubSources }) {
            for (const auto& [file, content] : *sources) {
                if (platform::fs::read_file(file).value_or("") != content) (void)platform::fs::write_file(file, content);
            }
        }
    }

    // Locations in files this server generates (stand-ins, prime units) are not places in the project: a
    // definition of a module nothing provides is no definition at all.
    void drop_generated_locations_(Json& result) const {
        const auto generated = [&](const Json& location) {
            if (!location.is_object()) return false;
            const std::string uri { location.value("uri", location.value("targetUri", std::string {})) };
            if (uri.empty()) return false;
            const std::string path { host_->path_of_uri(uri) };
            return !path.empty() && ((!stubDirectory_.empty() && base::is_within(path, stubDirectory_)) || (!primeDirectory_.empty() && base::is_within(path, primeDirectory_)));
        };
        if (result.is_array()) {
            result.erase(std::remove_if(result.begin(), result.end(), generated), result.end());
        } else if (generated(result)) {
            result = nullptr;
        }
    }

    // std, which nearly every file imports, and the imports of every open document, then as many
    // ready modules as the limit allows. std.compat waits for a file that imports it: building it
    // takes cores a cold start needs.
    void prepare_modules_() {
        if (!accepting_) return;
        const std::vector<std::string> standard { "std" };
        primer_.want(standard);
        for (const auto& document : host_->documents()) prepare_imports_of_(document, false);
        pump_primer_();
    }

    void prepare_imports_of_(const DocumentView& document, bool pump = true) {
        if (document.path.empty() || excluded_path_(document.path)) return;
        const auto names = host_->imports_of(document.path);
        if (primer_.want(names) > 0 && pump) pump_primer_();
    }

    // clangd's persistent module cache as it is now: for each source file name, the BMIs built from
    // it (<database>/.cache/clangd/modules/<source file name>-<hash>/<command hash>/<module>.pcm).
    std::map<std::string, std::vector<std::string>, std::less<>> cached_bmis_() const {
        std::map<std::string, std::vector<std::string>, std::less<>> bmis;
        for (const auto& sourceDirectory : platform::fs::list_directory(base::join_path(databaseDirectory_, ".cache/clangd/modules"))) {
            const std::string_view name { base::file_name(sourceDirectory) };
            const std::size_t dash { name.rfind('-') };
            if (dash == std::string_view::npos || dash == 0) continue;
            auto& files = bmis[std::string { name.substr(0, dash) }];
            for (const auto& commandDirectory : platform::fs::list_directory(sourceDirectory)) {
                for (auto& file : platform::fs::list_directory(commandDirectory)) {
                    if (file.ends_with(".pcm")) files.push_back(std::move(file));
                }
            }
        }
        return bmis;
    }

    // usable plan W7: whether clangd already keeps `module`'s BMI, finished (no lock file beside it)
    // and at least as new as the module's source. An importer's own build then reuses it, and a
    // prime unit would only take clangd workers from the files a person opened.
    bool module_already_built_(const PrimeModule& module) {
        const auto source = moduleSources_.find(module.name);
        if (source == moduleSources_.end()) return false;
        if (!startupBmis_) startupBmis_ = cached_bmis_();
        const auto files = startupBmis_->find(base::file_name(source->second));
        if (files == startupBmis_->end()) return false;
        const auto sourceStamp = platform::fs::stamp(source->second);
        if (!sourceStamp) return false;
        std::string wanted { module.name };
        std::ranges::replace(wanted, ':', '-');
        wanted += ".pcm";
        return std::ranges::any_of(files->second, [&](const std::string& file) {
            if (base::file_name(file) != wanted || platform::fs::exists(file + ".lock")) return false;
            const auto bmiStamp = platform::fs::stamp(file);
            return bmiStamp && bmiStamp->modified >= sourceStamp->modified;
        });
    }

    void pump_primer_() {
        if (!accepting_) return;
        // Prime units take the same clangd workers (-j, one per core) as everything a person does:
        // opening a file, typing, asking for completion. Preparation leaves a core to each file still
        // waiting for its modules and one more for requests, so it never holds every worker (hardware
        // threads count as two per core except on macOS, where they are cores).
        // robustness design C7: a quarter of the cores, half of that while a file waits, so preparing a large
        // workspace does not take the machine from the person using it.
        primer_.set_limit(preparation_limit(std::thread::hardware_concurrency(), mcppls::os::FAMILY == mcppls::os::Family::macos, awaitingDiagnostics_.size()));
        for (const PrimeModule* module : primer_.start_ready([this](const PrimeModule& candidate) { return module_already_built_(candidate); })) {
            const std::string uri { base::path_to_uri(module->primeFile) };
            Json params { { "textDocument", Json { { "uri", uri }, { "languageId", "cpp" }, { "version", 1 },
                                                   { "text", std::format("import {};\n", module->name) } } } };
            if (!send_(lsp::make_notification("textDocument/didOpen", std::move(params)))) {
                primer_.finish(module->name);
                continue;
            }
            primeModuleByPath_[base::path_key(module->primeFile)] = module->name;
            primeDeadlines_[module->name] = Clock::now() + std::chrono::minutes { 3 };
        }
        host_->status_changed();
    }

    bool finish_prime_(std::string_view engineUri) {
        if (primeDirectory_.empty()) return false;
        const std::string canonical { host_->path_of_uri(engineUri) };
        if (canonical.empty() || !base::is_within(canonical, primeDirectory_)) return false;
        const std::string pathKey { base::path_key(canonical) };
        const auto it = primeModuleByPath_.find(pathKey);
        if (it == primeModuleByPath_.end()) return true;
        const std::string module { it->second };
        primeModuleByPath_.erase(it);
        primeDeadlines_.erase(module);
        heldPrimeUnits_.emplace(pathKey, canonical);
        primer_.finish(module);
        lastPrimeProgressAt_ = Clock::now();
        pump_primer_();
        release_prime_units_if_idle_();
        return true;
    }

    void release_prime_units_if_idle_() {
        if (heldPrimeUnits_.empty() || primer_.busy() || !awaitingDiagnostics_.empty()) return;
        log::info("module preparation idle ({}): closing {} prime units", host_->root_directory(), heldPrimeUnits_.size());
        close_prime_units_();
    }

    void close_prime_units_() {
        std::set<std::string> uris;
        for (const auto& [pathKey, module] : primeModuleByPath_) {
            if (const auto* planned = primer_.find(module)) uris.insert(base::path_to_uri(planned->primeFile));
        }
        for (const auto& [pathKey, path] : heldPrimeUnits_) uris.insert(base::path_to_uri(path));
        if (accepting_) {
            for (const auto& uri : uris) (void)send_(lsp::make_notification("textDocument/didClose", Json { { "textDocument", Json { { "uri", uri } } } }));
        }
        primeModuleByPath_.clear();
        primeDeadlines_.clear();
        heldPrimeUnits_.clear();
    }

    void forget_primes_() {
        primeModuleByPath_.clear();
        primeDeadlines_.clear();
        heldPrimeUnits_.clear();
        primer_.reset();
    }
};

} // namespace

std::unique_ptr<Engine> make_engine(Options options) { return std::make_unique<ClangdEngine>(std::move(options)); }

} // namespace mcppls::engine::clangd
