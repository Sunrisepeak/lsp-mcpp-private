module mcppls.cli.options;

import std;
import mcpplibs.cmdline;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.platform.fs;
import mcppls.engine;
import mcppls.engine.payload;
import mcppls.engine.native;
import mcppls.engine.native.index;
import mcppls.engine.clangd;
import mcppls.orchestrator.workspace;

namespace mcppls::cli {

using namespace mcpplibs;

orchestrator::EngineFactories engine_factories(const orchestrator::SessionOptions& options, const engine::PayloadPaths& payload, bool payloadCorrupt) {
    orchestrator::EngineFactories factories;
    factories.modules = [](const index::ModuleIndex& index) { return engine::native::make_engine(index); };
    if (options.engine == "none") return factories;
    if (options.engine != "clangd") base::log::warning("unknown engine {}; using clangd", options.engine);
    factories.core = [options, payload, payloadCorrupt]() -> std::unique_ptr<engine::Engine> {
        engine::clangd::Options clangd;
        clangd.executable = payload.clangd;
        clangd.version = payload.clangdVersion;
        clangd.payloadCorrupt = payloadCorrupt;
        clangd.verboseLog = options.verboseEngineLog;
        clangd.requestTimeout = options.requestTimeout;
        return engine::clangd::make_engine(std::move(clangd));
    };
    return factories;
}

orchestrator::SessionOptions session_options(const cmdline::ParsedArgs& args) {
    orchestrator::SessionOptions options;
    options.payloadDirectory = args.value("payload").value_or("");
    options.clangd = args.value("clangd").value_or("");
    options.kit = args.value("kit").value_or("");
    options.mcpp = args.value("mcpp").value_or("");
    options.database = args.value("database").value_or("");
    options.trusted = !args.is_flag_set("untrusted");
    options.discoverCompilers = !args.is_flag_set("no-discover");
    options.verboseEngineLog = args.value("log-level").value_or("") == "debug";
    if (auto chosen = args.value("engine")) {
        options.engine = *chosen;
        options.engineFromCommandLine = true;
    }
    options.engineFactories = engine_factories;
    options.requestTimeout = seconds_option(args, "request-timeout", options.requestTimeout.count() > 0
                                                                          ? std::chrono::duration_cast<std::chrono::seconds>(options.requestTimeout)
                                                                          : std::chrono::seconds { 60 });
    return options;
}

void apply_log_level(const cmdline::ParsedArgs& args) {
    if (auto level = args.value("log-level")) {
        if (auto parsed = base::log::parse_level(*level)) base::log::set_level(*parsed);
    }
}

std::string absolute(std::string_view path) {
    if (path.empty() || base::is_absolute_path(path)) return base::normalize_path(path);
    return base::join_path(platform::fs::current_directory(), path);
}

std::chrono::seconds seconds_option(const cmdline::ParsedArgs& args, std::string_view name, std::chrono::seconds fallback) {
    auto text = args.value(name);
    if (!text) return fallback;
    try {
        return std::chrono::seconds { std::stoi(*text) };
    } catch (...) {
        return fallback;
    }
}

} // namespace mcppls::cli
