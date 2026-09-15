// The options every entry shares (serve, mcp, query, verify, review), and the composition root that
// gives every workspace root its engines (overall design 4.1).
export module mcppls.cli.options;

import std;
import mcpplibs.cmdline;
import mcppls.engine.payload;
import mcppls.orchestrator.workspace;

export namespace mcppls::cli {

// The engines of a root: mcppls's own always; clangd unless the engine is none.
orchestrator::EngineFactories engine_factories(const orchestrator::SessionOptions& options, const engine::PayloadPaths& payload, bool payloadCorrupt);

// Options of the global flags, with the composition root's factories.
orchestrator::SessionOptions session_options(const mcpplibs::cmdline::ParsedArgs& args);

void apply_log_level(const mcpplibs::cmdline::ParsedArgs& args);

// A path argument made absolute against the current directory.
std::string absolute(std::string_view path);

// A seconds argument, or the fallback.
std::chrono::seconds seconds_option(const mcpplibs::cmdline::ParsedArgs& args, std::string_view name, std::chrono::seconds fallback);

} // namespace mcppls::cli
