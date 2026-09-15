// One workspace root (overall design 6.1): its project model, source index, plan, documents and the
// engines serving it. The workspace routes each client request to engines by their declared
// capabilities, merges what they answer and publishes status (S3) and merged diagnostics. A session
// (the LSP entry) composes one per root and speaks for the client.
export module mcppls.orchestrator.workspace;

import std;
import nlohmann.json;
import mcppls.base.text;
import mcppls.platform.fs;
import mcppls.platform.task;
import mcppls.project.model;
import mcppls.engine;
import mcppls.engine.payload;
import mcppls.engine.native.index;
import mcppls.orchestrator.client;

export namespace mcppls::orchestrator {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

// The engines of a root, made by the composition root (cli): mcppls's own engine over the root's
// module index, and the core engine for C++ semantics (none when it returns nullptr).
struct EngineFactories {
    std::function<std::unique_ptr<engine::Engine>(const index::ModuleIndex&)> modules;
    std::function<std::unique_ptr<engine::Engine>()> core;
};

struct SessionOptions {
    std::string payloadDirectory;
    std::string clangd;
    std::string kit;
    std::string mcpp;                      // the mcpp executable for mcpp projects; empty: found on PATH
    std::string database;                  // a workspace's own S1 document, relative to the root (usable plan W9.2)
    std::string engine { "clangd" };       // the core engine: clangd | none (overall design 5.6)
    bool engineFromCommandLine { false };  // --engine was given, so the client's initializationOptions do not override it
    bool trusted { true };
    bool discoverCompilers { true };
    bool verboseEngineLog { false };
    std::chrono::milliseconds requestTimeout { std::chrono::seconds { 60 } };
    // Given by the composition root; a test can substitute engines that start no process.
    std::function<EngineFactories(const SessionOptions&, const engine::PayloadPaths&, bool payloadCorrupt)> engineFactories;
};

// A background thread (model loading, an engine's I/O threads, watch polling) reports back through
// this queue; the session's one event loop is the only thing that ever changes state.
enum class EventKind { client_message, client_closed, engine_event, model_loaded };

struct Event {
    EventKind kind { EventKind::client_message };
    Json message;
    int generation { 0 };
    std::shared_ptr<project::ProjectModel> model;
    // Empty for client_message and client_closed, which the session dispatches by document path;
    // every other kind names the root it belongs to, and engine events the engine.
    std::string rootKey;
    std::string engineId;
};

using EventChannel = platform::Channel<Event>;

enum class State { starting, loading, preparing, ready, degraded, error };
std::string_view to_string(State state);

bool is_build_file(std::string_view name);
// The files a watch (dynamic or the W9.3 polling fallback) cares about under `root`.
std::map<std::string, platform::fs::FileStamp> watched_files_snapshot(std::string_view root);

class Workspace {
public:
    Workspace(std::string root, std::string key, SessionOptions options, engine::PayloadPaths payload, bool payloadCorrupt, bool kitEnabled,
              std::string compilerOverride, std::shared_ptr<EventChannel> events, ClientSink& client);
    ~Workspace();
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    const std::string& root() const { return root_; }
    const std::string& key() const { return key_; }
    // The folder's URI as the client named it, which cxxModules/status reports as project.root (S3 4).
    void set_client_uri(std::string uri);
    bool owns_path(std::string_view path) const;

    // ---- lifecycle ----------------------------------------------------------------
    // `onEngineSettled` fires once, with the merged server capabilities of this root's engines, when
    // the core engine finished its handshake or was found unavailable (right away without one).
    void start(Json clientParams, bool clientSupportsStatus, bool usePolling, std::function<void(Json)> onEngineSettled = {});
    void shut_down();
    // Nothing is sent to the client before its own initialize was answered.
    void allow_status_notifications();

    // ---- client-driven, already known to belong to this root ----------------------------
    void did_open(const Json& params);
    void did_change(const Json& message, const Json& params);
    void did_close(const Json& message, const Json& params);
    void did_save(const Json& message, const Json& params);
    // Only the entries of a workspace/didChangeWatchedFiles notification this root owns.
    void handle_watched_files(const Json& changes);
    void cancel(const Json& id);
    void route_client_request(const Json& message);
    // The client's response to a request one of this root's engines sent through to it.
    void handle_client_response(const EngineRequestKey& key, const Json& response);
    void forward_other_notification(const Json& message);

    // ---- cxxModules/* (S3) ----------------------------------------------------------
    Json graph() const;
    Json module_info(std::string_view name) const;
    Json module_info_at(std::string_view path, base::Position at) const;
    Json contexts() const;
    // Replies to `id` itself, then replans if it changed anything.
    void set_context(const Json& id, std::string_view context);

    // ---- background events ------------------------------------------------------------
    void handle_engine_event(std::string_view engineId, const Json& event);
    void handle_model_loaded(int generation, std::shared_ptr<project::ProjectModel> model);

    // ---- timers -----------------------------------------------------------------------
    std::optional<Clock::time_point> next_deadline() const;
    void handle_timers();

private:
    std::string root_;
    std::string key_;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mcppls::orchestrator
