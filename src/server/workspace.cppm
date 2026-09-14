// One workspace root's project model, module index, document subset and clangd engine (usable
// plan W9.1): everything session.cppm's Session used to hold as its own state, extracted so a
// multi-root session can have several. lspmcpp.server.session composes one or more of these,
// routes client requests to the right one by the longest root prefix of the document path, and
// otherwise stays a thin coordinator of the client-facing protocol.
export module lspmcpp.server.workspace;

import std;
import nlohmann.json;
import lspmcpp.base.error;
import lspmcpp.base.text;
import lspmcpp.spec.database;
import lspmcpp.spec.kit;
import lspmcpp.spec.metadata;
import lspmcpp.project.model;
import lspmcpp.normalize.plan;
import lspmcpp.index.modules;
import lspmcpp.engine;
import lspmcpp.platform.fs;
import lspmcpp.platform.task;
import lspmcpp.server.documents;
import lspmcpp.server.payload;
import lspmcpp.server.primer;
import lspmcpp.server.router;

export namespace lspmcpp::server {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

// Moved here from lspmcpp.server.session (usable plan W9.1) so this module, which the session
// composes one WorkspaceRoot per root from, does not depend back on it: session.cppm re-exports
// it, so a caller importing either module sees the same type.
struct SessionOptions {
    std::string payloadDirectory;
    std::string clangd;
    std::string kit;
    std::string mcpp;                      // the mcpp executable for mcpp projects; empty: found on PATH
    std::string database;                  // a workspace's own S1 document, relative to the root (usable plan W9.2)
    bool trusted { true };
    bool discoverCompilers { true };
    bool verboseEngineLog { false };
    std::chrono::milliseconds requestTimeout { std::chrono::seconds { 60 } };
    // usable plan W9.5: empty means a real clangd (lspmcpp.engine.clangd::Clangd); a test can supply
    // a fake engine instead, since a root only ever uses the lspmcpp.engine interface.
    std::function<std::unique_ptr<engine::Engine>()> engineFactory;
};

// A background thread (model loading, the engine's own I/O threads, watch polling) reports back
// through this queue; the session's one event loop is the only thing that ever changes state.
enum class EventKind { client_message, client_closed, engine_message, engine_closed, engine_module_failed, model_loaded };

struct Event {
    EventKind kind { EventKind::client_message };
    Json message;
    int generation { 0 };
    std::shared_ptr<project::ProjectModel> model;
    // Empty for client_message/client_closed, which the session itself dispatches by document
    // path; every other kind names the WorkspaceRoot it belongs to (usable plan W9.1), so a
    // multi-root session can tell whose engine or model-load a background thread answers for.
    std::string rootKey;
};

using EventChannel = platform::Channel<Event>;

enum class State { starting, loading, preparing, ready, degraded, error };
std::string_view to_string(State state);

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

inline constexpr std::chrono::milliseconds INTERACTIVE_TIMEOUT { std::chrono::seconds { 10 } };

bool is_build_file(std::string_view name);
bool is_interactive(std::string_view method);
// The files a watch (dynamic or the W9.3 polling fallback) cares about under `root`.
std::map<std::string, platform::fs::FileStamp> watched_files_snapshot(std::string_view root);

// Stateless: writes one LSP frame to the client's standard output. Shared by every WorkspaceRoot
// and the session itself, since they all speak to the one client over the one stream.
void send_client_message(const Json& message);
void reply(const Json& id, Json result);
void reply_error(const Json& id, int code, std::string_view message);
void notify_client(std::string_view method, Json params);

// The synthetic id a root gives a request its own engine sent through to the client (design 12.6):
// embeds the root's key and engine generation, so the session can find the right root and rule out
// a stale engine when the client eventually responds. An opaque pair to everyone but these two
// functions and handle_client_response.
std::string make_engine_request_key(std::string_view rootKey, int generation, const Json& engineId);
bool parse_engine_request_key(std::string_view key, std::string& rootKey, int& generation, Json& engineId);

// One workspace root's session state (design 12.5, 13.1-13.4): project model, module index, plan,
// clangd engine and the documents under it. Constructed once `initialize` names the root (or
// workspace/didChangeWorkspaceFolders adds one); `shut_down` stops its engine, e.g. when the
// folder is removed or the session exits.
class WorkspaceRoot {
public:
    WorkspaceRoot(std::string root, std::string key, SessionOptions options, PayloadPaths payload, bool payloadCorrupt,
                 bool kitEnabled, std::string compilerOverride, std::shared_ptr<EventChannel> events);
    ~WorkspaceRoot();
    WorkspaceRoot(const WorkspaceRoot&) = delete;
    WorkspaceRoot& operator=(const WorkspaceRoot&) = delete;

    const std::string& root() const { return root_; }
    const std::string& key() const { return key_; }
    // Whether `path` names a file this root claims: it or a descendant of it. Session uses the
    // root with the longest such match (usable plan W9.1); ties do not occur since roots are
    // themselves not nested inside one another (workspace/didChangeWorkspaceFolders keeps that true).
    bool owns_path(std::string_view path) const;

    // ---- lifecycle ----------------------------------------------------------------
    // `clientParams` is the session's own `initialize` params, reused for this root's engine
    // handshake (design 13.1); `clientSupportsStatus` decides whether update_status_ notifies at
    // all. `usePolling` starts this root's own watch-polling worker (usable plan W9.3), decided
    // once by the session from the client's capabilities and applied to every root, including ones
    // workspace/didChangeWorkspaceFolders adds later: each root's polling thread only ever touches
    // that root's own state, so roots can come and go without the threads coordinating.
    // `onEngineSettled`, when given, fires exactly once, with this root's engine capabilities (or
    // an empty object if it never got any), the first time this root's engine finishes its own
    // handshake or is found unavailable: the session uses it on the first root only, to answer the
    // client's own `initialize` the same way a single-root session always has.
    void start(Json clientParams, bool clientSupportsStatus, bool usePolling, std::function<void(Json)> onEngineSettled = {});
    void shut_down();   // stops the engine; used when the root is removed or the session exits
    // The session's own `initialize` has now been answered (possibly by this root's own engine
    // settling, possibly by another root's, possibly long before this root existed at all): only
    // from this point on may update_status_ actually notify, matching a single-root session's own
    // long-standing rule that nothing is sent to the client before its initialize response is.
    void allow_status_notifications();

    // ---- client-driven, already known to belong to this root -----------------------------
    void did_open(const Json& params);
    void did_change(const Json& message, const Json& params);
    void did_close(const Json& message, const Json& params);
    void did_save(const Json& message, const Json& params);
    // `changes` holds only the entries of a workspace/didChangeWatchedFiles notification this
    // root owns; the session splits a client's own notification, or a polled one, by path first.
    void handle_watched_files(const Json& changes);
    void cancel(const Json& id);
    // The general (non-cxxModules) request path: local module answer, engine forward, or deferred.
    void route_client_request(const Json& message);
    // A response to a request this root's engine itself sent through to the client, addressed by
    // the session (its id embeds this root's key: see engine_request_key in workspace.cpp).
    void handle_client_response(const Json& clientResponse, int generation, const Json& engineRequestId);
    // Any other client notification this root's engine should also see (configuration changes,
    // $/setTrace): forwarded once per root, exactly as the client sent it.
    void forward_other_notification(const Json& message);

    // ---- cxxModules/* (S3), for this root ------------------------------------------------
    Json graph() const;
    Json module_info(std::string_view name) const;
    Json module_info_at(std::string_view path, base::Position at) const;
    Json contexts() const;
    // Replies to `id` itself (null, or InvalidParams when `context` names none of this root's
    // sets), then replans if it changed anything: S3 5.4's request has side effects the reply
    // cannot wait for, so route_client_request's uniform "return a Json result" shape does not fit.
    void set_context(const Json& id, std::string_view context);

    // ---- background events (usable plan W9.5/W9.1) -----------------------------------------
    void handle_engine_message(int generation, const Json& message);
    void handle_engine_closed(int generation);
    void handle_module_failure(int generation, const Json& failure);
    void handle_model_loaded(int generation, std::shared_ptr<project::ProjectModel> model);

    // ---- timers -----------------------------------------------------------------------
    std::optional<Clock::time_point> next_deadline() const;
    void handle_timers();

private:
    // Identity fields Session's routing reads often enough to skip the impl_ indirection for;
    // everything else this root owns is behind it, in workspace.cpp's Impl (exported classes need
    // a complete definition wherever used, so this keeps that module-private).
    std::string root_;
    std::string key_;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lspmcpp::server
