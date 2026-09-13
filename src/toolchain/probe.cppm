// Asking a compiler driver what it is: family, version, target, standard
// library and its module manifest, and the installation facts normalization needs.
export module lspmcpp.toolchain.probe;

import std;
import nlohmann.json;
import lspmcpp.base.error;
import lspmcpp.platform.process;
import lspmcpp.spec.database;

export namespace lspmcpp::toolchain {

struct ToolchainFacts {
    spec::Toolchain toolchain;
    std::string gccInstallDirectory;   // GCC for non-MinGW targets: the directory holding libgcc.a
    std::string mingwRoot;             // GCC for MinGW targets: the toolchain root above bin/
    std::string resourceDirectory;     // Clang: -print-resource-dir
    bool appleClang { false };
    std::vector<std::string> problems;
};

using Runner = std::function<base::Result<platform::RunResult>(std::span<const std::string> argv)>;

// Runs drivers as child processes with a bound on each query.
Runner process_runner(std::chrono::milliseconds timeout);

spec::Family classify_driver(std::string_view driverPath);
// The arguments of a compile command that change what a query answers:
// target, sysroot, standard library selection and configuration files.
std::vector<std::string> probe_relevant_arguments(std::span<const std::string> arguments);
base::Result<ToolchainFacts> probe_toolchain(std::string_view driverPath, std::span<const std::string> relevantArguments,
                                             const Runner& runner);
std::string toolchain_id(const spec::Toolchain& toolchain);

nlohmann::json facts_to_json(const ToolchainFacts& facts);
std::optional<ToolchainFacts> facts_from_json(const nlohmann::json& value);

// Probe results persisted in a JSON file, keyed by driver path, file stamp and relevant arguments.
class ProbeCache {
private:
    std::string path_;
    nlohmann::json entries_ = nlohmann::json::object();
    std::mutex mutex_;

public:
    explicit ProbeCache(std::string path);
    std::optional<ToolchainFacts> find(std::string_view driverPath, std::span<const std::string> relevantArguments);
    void store(std::string_view driverPath, std::span<const std::string> relevantArguments, const ToolchainFacts& facts);
    void save();

private:
    std::string key_(std::string_view driverPath, std::span<const std::string> relevantArguments) const;
};

// Probes through the cache.
base::Result<ToolchainFacts> probe_cached(std::string_view driverPath, std::span<const std::string> relevantArguments,
                                          const Runner& runner, ProbeCache* cache);

} // namespace lspmcpp::toolchain
