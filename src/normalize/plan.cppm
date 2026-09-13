// The engine database for one context: which units are written, with which
// arguments, which standard library units are injected, and what could not be
// resolved (design section 14.4, "remaining steps").
export module lspmcpp.normalize.plan;

import std;
import nlohmann.json;
import lspmcpp.base.error;
import lspmcpp.base.text;
import lspmcpp.spec.database;
import lspmcpp.spec.kit;
import lspmcpp.spec.metadata;
import lspmcpp.toolchain.probe;
import lspmcpp.project.scan;

export namespace lspmcpp::normalize {

struct EngineEntry {
    std::string directory;
    std::string file;
    std::vector<std::string> arguments;   // argv[0] first, source last
};

struct PlanIssue {
    std::string code;                     // unresolved-module | ambiguous-module | toolchain-not-found | sdk-missing
    std::string message;
    std::string file;
    std::string module;
};

struct EnginePlan {
    std::vector<EngineEntry> entries;
    std::vector<PlanIssue> issues;
    std::vector<std::string> excludedFiles;   // importable units left out because an import cannot resolve
    std::string contextSet;                   // empty: every set
    std::size_t stdUnits { 0 };
};

struct PlanInput {
    const spec::Database* database { nullptr };
    std::string contextSet;                                          // a set name, or empty for every set
    const std::map<std::string, toolchain::ToolchainFacts, std::less<>>* facts { nullptr };   // by toolchain id
    const spec::Kit* kit { nullptr };                                // used by sets without a usable toolchain
    std::string engineDriverDirectory;                               // where synthetic driver names are placed
    std::string macosSdk;                                            // for kits that require it
    std::function<project::ScanResult(std::string_view path)> scanner;
    spec::MetadataReader metadataReader;
};

EnginePlan plan_engine(const PlanInput& input);
nlohmann::json to_compile_commands(const EnginePlan& plan);
base::Result<void> write_engine_database(std::string_view directory, const EnginePlan& plan);

} // namespace lspmcpp::normalize
