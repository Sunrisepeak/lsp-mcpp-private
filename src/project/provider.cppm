// What every project data source needs from its caller: whether executing
// programs is allowed, and how to run, scan and probe.
export module mcppls.project.provider;

import std;
import mcppls.toolchain.probe;
import mcppls.project.infer;

export namespace mcppls::project {

struct ProviderContext {
    bool trusted { false };
    toolchain::Runner runner;               // bounded by the provider's own timeouts
    Scanner scanner;
    Prober prober;
    std::chrono::milliseconds configureTimeout { std::chrono::minutes { 5 } };
    std::string mcppExecutable;             // empty: found on PATH or in the usual install locations
};

// The first executable found on PATH or at one of the fallback absolute paths.
std::optional<std::string> find_tool(std::string_view name, std::span<const std::string> fallbacks);

} // namespace mcppls::project
