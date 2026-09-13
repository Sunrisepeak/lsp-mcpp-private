// What every project data source needs from its caller: whether executing
// programs is allowed, and how to run, scan and probe.
export module lspmcpp.project.provider;

import std;
import lspmcpp.toolchain.probe;
import lspmcpp.project.infer;

export namespace lspmcpp::project {

struct ProviderContext {
    bool trusted { false };
    toolchain::Runner runner;               // bounded by the provider's own timeouts
    Scanner scanner;
    Prober prober;
    std::chrono::milliseconds configureTimeout { std::chrono::minutes { 5 } };
};

// The first executable found on PATH or at one of the fallback absolute paths.
std::optional<std::string> find_tool(std::string_view name, std::span<const std::string> fallbacks);

} // namespace lspmcpp::project
