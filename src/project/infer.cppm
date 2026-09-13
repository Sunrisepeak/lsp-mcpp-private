// Building an S1 model where the build does not provide one: from a
// compile_commands.json plus scanning and probing, or from sources alone.
export module lspmcpp.project.infer;

import std;
import lspmcpp.spec.database;
import lspmcpp.toolchain.probe;
import lspmcpp.project.compdb;
import lspmcpp.project.scan;

export namespace lspmcpp::project {

using Scanner = std::function<ScanResult(std::string_view path)>;
using FactsMap = std::map<std::string, toolchain::ToolchainFacts, std::less<>>;
// Probes a driver with its relevant arguments; nullopt when it cannot be probed.
using Prober = std::function<std::optional<toolchain::ToolchainFacts>(std::string_view driver, std::span<const std::string> relevant)>;

struct InferredDatabase {
    spec::Database database;
    FactsMap facts;
    std::vector<std::string> problems;
};

// One set per distinct toolchain; sets see each other. Units carry scanned roles.
InferredDatabase database_from_commands(std::span<const CompileCommand> commands, std::string_view familyName,
                                        const Scanner& scanner, const Prober& prober);

struct InferOptions {
    std::optional<toolchain::ToolchainFacts> facts;   // a discovered compiler, or nullopt for kit semantics
    std::string languageStandard { "c++23" };
    std::string engineDriver { "clang++" };           // written as argv[0] when there is no compiler
};

// Every C++ source under `root` (build output and dot directories skipped).
InferredDatabase infer_database(std::string_view root, const InferOptions& options, const Scanner& scanner);

// Scans a file from disk.
Scanner file_scanner();

} // namespace lspmcpp::project
