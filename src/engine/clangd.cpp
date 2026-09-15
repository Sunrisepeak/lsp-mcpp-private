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

    // Plan.
    bool planApplied_ { false };
    std::string writtenDatabase_;
    std::string writtenStructure_;   // the written database without module hints
    std::set<std::string> excluded_;   // path keys
    // Modules clangd could not build, by name, with the reason; cleared when sources change.
    std::map<std::string, std::string, std::less<>> failedModules_;

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
    std::map<std::string, int, std::less<>> timeoutsByUri_;
    std::deque<Clock::time_point> crashes_;
    std::vector<Issue> issues_;
    std::optional<Clock::time_point> restartAt_;

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

    void start(Host& host) override {
        host_ = &host;
        sink_ = host.event_sink(ENGINE_ID);
        const std::string& cache { host.cache_directory() };
        databaseDirectory_ = base::join_path(cache, "contexts/default/cdb");
        primeDirectory_ = base::join_path(cache, "contexts/default/prime");
        moduleHintDirectory_ = base::join_path(cache, "contexts/default/module-hints");   // never created
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
        input.failedModules = failedModules_;
        input.primeDirectory = traits_.needsModulePreparation ? primeDirectory_ : std::string {};
        input.moduleHintDirectory = traits_.needsModuleHints ? moduleHintDirectory_ : std::string {};
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
            log::info("engine database ({}): {} entries ({} standard library units), {} left out, {} issues", host_->root_directory(), plan->entries.size(),
                      plan->stdUnits, plan->excludedFiles.size(), plan->issues.size());
        }
        std::set<std::string> newExcluded;
        for (const auto& file : plan->excludedFiles) newExcluded.insert(base::path_key(file));
        const bool restartNeeded { structureChanged && planApplied_ && handshakeDone_ };
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
        moduleSources_.clear();
        for (const auto& entry : plan->entries) {
            if (!entry.provides.empty()) moduleSources_.emplace(entry.provides, entry.file);
        }
        startupBmis_.reset();
        planApplied_ = true;
        if (restartNeeded) {
            restart_("the engine database changed");
        } else {
            accept_traffic_if_ready_();
            prepare_modules_();
        }
    }

    void document(const DocumentEvent& event) override {
        const DocumentView& document { event.document };
        switch (event.change) {
        case DocumentChange::opened:
            if (accepting_ && !excluded_path_(document.path)) {
                open_in_engine_(document);
                prepare_imports_of_(document);
            }
            break;
        case DocumentChange::changed:
            if (accepting_ && !excluded_path_(document.path) && event.message != nullptr) (void)send_(*event.message);
            break;
        case DocumentChange::closed: {
            const bool wasExcluded { excluded_path_(document.path) };
            awaitingDiagnostics_.erase(document.uri);
            diagnosed_.erase(document.uri);
            if (accepting_ && !wasExcluded && event.message != nullptr) (void)send_(*event.message);
            release_prime_units_if_idle_();
            break;
        }
        case DocumentChange::saved:
            if (!document.path.empty() && !excluded_path_(document.path) && accepting_ && event.message != nullptr) (void)send_(*event.message);
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
        if (failedModules_.empty()) return;
        failedModules_.clear();
        host_->request_replan();
    }

    bool claims(const RequestView& request) const override { return !unavailable_ && !excluded_path_(request.path); }

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
        return deadline;
    }

    void handle_timers() override {
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
                const bool filePreparing { awaitingDiagnostics_.contains(host_->client_uri(request.uri)) && primer_.busy() };
                if (keep_waiting(request, filePreparing, lastPrimeProgressAt_, now)) {
                    request.deadline = std::min(now + PREPARING_GRACE, request.limit);
                    pending_[id] = std::move(request);
                    break;
                }
                log::warning("clangd ({}) did not answer {} in time", host_->root_directory(), request.method);
                if (request.reply) request.reply(Answer {});
                (void)send_(lsp::make_notification("$/cancelRequest", Json { { "id", id } }));
                // A file whose modules are still being built is slow, not stuck: restarting would throw that work away.
                if (awaitingDiagnostics_.contains(host_->client_uri(request.uri))) break;
                add_issue_(Issue { "engine-timeout", std::format("clangd did not answer {} in time", request.method), "mcppls.restartServer" });
                if (++timeoutsByUri_[request.uri] >= 3) restart = true;
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
        if (restart) restart_("repeated timeouts");
        std::vector<std::string> overdue;
        for (const auto& [module, at] : primeDeadlines_) {
            if (at <= now) overdue.push_back(module);
        }
        for (const auto& module : overdue) {
            log::warning("stopped waiting for module {} to be prepared ({})", module, host_->root_directory());
            if (const auto* planned = primer_.find(module)) (void)finish_prime_(base::path_to_uri(planned->primeFile));
        }
        if (restartAt_ && *restartAt_ <= now) {
            restartAt_.reset();
            restart_("recovering from an exit");
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
        config.extraArguments = options_.extraArguments;
        // Extra engine arguments for troubleshooting, e.g. MCPPLS_ENGINE_ARGUMENTS="-j=8 --background-index-priority=background".
        if (auto extra = platform::env::get("MCPPLS_ENGINE_ARGUMENTS")) {
            for (auto word : base::split(*extra, ' ')) {
                if (!base::trim(word).empty()) config.extraArguments.emplace_back(base::trim(word));
            }
        }
        if (!process_) process_ = make_process_();
        auto sink = sink_;
        const std::string root { host_->root_directory() };
        auto started = process_->start(
            config,
            [sink, generation](Json message) { sink(Json { { "kind", "message" }, { "generation", generation }, { "message", std::move(message) } }); },
            [sink, generation] { sink(Json { { "kind", "closed" }, { "generation", generation } }); },
            [sink, generation, root](std::string_view line) {
                log::info("clangd ({}): {}", root, line);
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
        timeoutsByUri_.clear();
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
                                              now + timeout, generation_, now + options_.requestTimeout, std::move(reply) };
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
            if (!diagnosed_.contains(document.uri)) awaitingDiagnostics_.insert(document.uri);
        }
    }

    void accept_traffic_if_ready_() {
        if (!handshakeDone_ || !planApplied_ || accepting_) return;
        accepting_ = true;
        for (const auto& document : host_->documents()) {
            if (!excluded_path_(document.path)) open_in_engine_(document);
        }
        std::vector<std::pair<Json, Reply>> toFlush;
        toFlush.swap(deferred_);
        for (auto& [message, reply] : toFlush) {
            if (lsp::kind_of(message) == lsp::Kind::request) {
                const Json* params { lsp::find(message, "params") };
                const Json* uri { params != nullptr ? lsp::find_path(*params, { "textDocument", "uri" }) : nullptr };
                const std::string path { uri != nullptr && uri->is_string() ? host_->path_of_uri(uri->get<std::string>()) : std::string {} };
                if (excluded_path_(path)) {
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
            timeoutsByUri_.erase(request.uri);
            if (!request.reply) return;
            if (message.contains("error")) {
                request.reply(Answer { Answer::Kind::error, message["error"] });
                return;
            }
            Json result = message.value("result", Json {});
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
        while (!crashes_.empty() && now - crashes_.front() > std::chrono::minutes { 3 }) crashes_.pop_front();
        add_issue_(Issue { "engine-crashed", "clangd exited unexpectedly", "mcppls.restartServer" });
        if (crashes_.size() >= 3) {
            unavailable_ = true;
            flush_deferred_without_engine_();
        } else {
            restartAt_ = now + std::chrono::seconds { 1 << (crashes_.size() - 1) };
        }
        host_->status_changed();
    }

    void handle_module_failure_(const Json& failure) {
        bool added { false };
        const std::string reason { failure.value("reason", std::string {}) };
        auto add = [&](std::string name) {
            if (name.empty() || failedModules_.contains(name)) return;
            log::warning("clangd could not build module {} ({}): {}", name, host_->root_directory(), reason);
            failedModules_.emplace(std::move(name), reason);
            added = true;
        };
        add(failure.value("module", std::string {}));
        if (const std::string source { failure.value("source", std::string {}) }; !source.empty()) {
            if (auto text = platform::fs::read_file(source)) add(project::provided_name(project::scan_source(*text)));
        }
        if (added) host_->request_replan();
    }

    // ---- parallel module preparation ------------------------------------------------------

    void write_prime_sources_(const normalize::EnginePlan& plan) {
        if (plan.primeSources.empty()) return;
        (void)platform::fs::create_directories(primeDirectory_);
        for (const auto& [file, content] : plan.primeSources) {
            if (platform::fs::read_file(file).value_or("") != content) (void)platform::fs::write_file(file, content);
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
        const std::size_t threads { std::max<std::size_t>(1, std::thread::hardware_concurrency()) };
        const std::size_t cores { mcppls::os::FAMILY == mcppls::os::Family::macos ? threads : std::max<std::size_t>(1, threads / 2) };
        const std::size_t reserved { awaitingDiagnostics_.size() + 1 };
        primer_.set_limit(cores > reserved ? cores - reserved : 1);
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
