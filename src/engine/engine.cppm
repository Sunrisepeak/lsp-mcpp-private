// The interface a session drives to get semantic help from a backend (design
// section 15.1 and usable plan W9.5): start it, push it the engine database,
// forward LSP messages to and from it, and query what it can do. clangd
// (mcppls.engine.clangd) is the only implementation; the interface exists so
// a fake can stand in for tests without a real clangd process, and so a future
// backend (design 15.1 mentions clice) needs no change to the session.
export module mcppls.engine;

import std;
import nlohmann.json;
import mcppls.base.error;

export namespace mcppls::engine {

struct EngineConfig {
    std::string executable;
    std::string version;             // e.g. "23.1.0"; looks up capabilities(), otherwise informational
    std::string databaseDirectory;   // where push_database writes, and what a file-based engine watches
    std::string workDirectory;
    std::vector<std::string> extraArguments;
    bool verboseLog { false };
};

// What a version of an engine can do, so the rest of the server can decide without knowing which
// engine or version is running underneath the interface.
struct EngineCapabilities {
    bool experimentalModulesSupport { false };
    bool useDirtyHeaders { false };           // rebuilds a module from the editor's unsaved buffer
    bool persistentModuleCache { false };     // a warm start reuses BMIs written by an earlier run (design SC4)
    // MSVC STL's headers redeclare std::align_val_t (usable plan W1.4, E8/E9): some engine versions
    // need aligned allocation turned off for every unit of a context that uses the MSVC STL, others
    // do not.
    bool msvcStlNeedsNoAlignedAllocation { false };
};

class Engine {
public:
    using MessageHandler = std::function<void(nlohmann::json)>;
    using ClosedHandler = std::function<void()>;
    using LogHandler = std::function<void(std::string_view)>;

    virtual ~Engine() = default;
    // Starts the engine with no database; the session pushes one before it opens any file.
    // Starting again while running stops the previous run first.
    virtual base::Result<void> start(const EngineConfig& config, MessageHandler onMessage, ClosedHandler onClosed, LogHandler onLog) = 0;
    // Replaces the engine's compile database: a compile_commands.json-shaped JSON array.
    virtual base::Result<void> push_database(std::string_view compileCommandsJson) = 0;
    // Forwards one LSP message (request, response or notification) to the engine.
    virtual base::Result<void> send(const nlohmann::json& message) = 0;
    virtual void stop(std::chrono::milliseconds grace) = 0;
    virtual bool running() const = 0;
    virtual const EngineConfig& config() const = 0;
    virtual EngineCapabilities capabilities() const = 0;
};

} // namespace mcppls::engine
