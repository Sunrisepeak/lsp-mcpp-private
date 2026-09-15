// The engine database for one context: which units are written, with which
// arguments, which standard library units are injected, and what could not be
// resolved (design section 14.4, "remaining steps").
export module mcppls.normalize.plan;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.text;
import mcppls.spec.database;
import mcppls.spec.kit;
import mcppls.spec.metadata;
import mcppls.toolchain.probe;
import mcppls.project.scan;

export namespace mcppls::normalize {

struct EngineEntry {
    std::string directory;
    std::string file;
    std::vector<std::string> arguments;   // argv[0] first, source last
    std::string provides;                 // the module the unit provides when it is importable
    std::vector<std::string> imports;     // the modules it imports directly
    // Written before the source (usable plan W7): -fmodule-file=<name>=<path> for every module the
    // unit reaches and -fmodule-output=<path> for the one it provides, at paths nothing writes. They
    // name each module's unit to clangd, which otherwise scans the whole database to find it.
    std::vector<std::string> moduleHints;
};

struct PlanIssue {
    std::string code;                     // unresolved-module | ambiguous-module | module-build-failed | toolchain-not-found | sdk-missing
    std::string message;
    std::string file;
    std::string module;
};

// A module the engine database provides, for scheduling its build (usable plan W7).
struct PlannedModule {
    std::string name;
    std::vector<std::string> requires_;
    std::string primeFile;                // the `import M;` unit in the engine database; empty for partitions
};

struct EnginePlan {
    std::vector<EngineEntry> entries;
    std::vector<PlannedModule> modules;
    std::vector<std::pair<std::string, std::string>> primeSources;   // prime file -> its content
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
    // Modules the engine reported it could not build, with the reason: their importers are
    // left out like importers of an unresolvable module, so they are answered at once.
    std::map<std::string, std::string, std::less<>> failedModules;
    // Where `import M;` units for parallel preparation are written; empty: none are planned.
    std::string primeDirectory;
    // The directory module hints name; nothing is created there. Empty: no hints.
    std::string moduleHintDirectory;
};

EnginePlan plan_engine(const PlanInput& input);
// The compile database clangd reads. Without module hints it is the database's structure: two
// plans that differ only in hints need no engine restart, since clangd rereads the file itself.
nlohmann::json to_compile_commands(const EnginePlan& plan, bool moduleHints = true);
base::Result<void> write_engine_database(std::string_view directory, const EnginePlan& plan);

} // namespace mcppls::normalize
