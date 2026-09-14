// S2: the discovery command protocol (specs/s2-discovery.md). In stream mode a
// producer is started with one JSON request on stdin and answers JSON lines on
// stdout; in single-document mode it prints one envelope with the database inline.
export module lspmcpp.spec.discovery;

import std;
import nlohmann.json;
import lspmcpp.base.error;

export namespace lspmcpp::spec {

struct DiscoveryRequest {
    std::string workspace;
    std::vector<std::string> files;
    std::string configuration;
};

struct DiscoveryResult {
    std::string database;
    std::vector<std::string> watch;
    std::vector<std::string> progress;
};

// S2 0.2 single-document mode: a producer that follows a machine-output envelope
// (mcpp's wire protocol v1) prints one JSON document whose `data` carries the
// database inline, and advertises that it can with a protocol description.
struct EnvelopeDiagnostic {
    std::string code;
    std::string severity;   // error | warning | note
    std::string message;
};

struct DatabaseDocument {
    nlohmann::json database;                  // the S1 document
    std::vector<std::string> watch;           // absolute paths or LSP glob patterns relative to the workspace
    std::string inputsFingerprint;
    std::vector<std::string> effects;         // what running the producer did
    std::vector<EnvelopeDiagnostic> diagnostics;
};

struct ProducerProtocol {
    std::map<std::string, int, std::less<>> kinds;                                  // kind -> version
    std::map<std::string, std::vector<std::string>, std::less<>> commandEffects;    // "emit build-database" -> effects
};

inline constexpr std::string_view BUILD_DATABASE_KIND_SUFFIX { ".build-database" };

// `<producer> --protocol-version`.
base::Result<ProducerProtocol> parse_producer_protocol(std::string_view output);
// Whether a consumer may run a command with these declared effects to describe a project (S2 3.4).
// Reading the project, the network, the producer's own caches and build scripts are what describing
// a build takes; writing into the project is what single-document mode exists to avoid.
bool effects_acceptable(std::span<const std::string> effects);
// An envelope of a `*.build-database` kind with `data.database`.
base::Result<DatabaseDocument> parse_database_envelope(std::string_view output);
// Runs a producer command without input and interprets its output as a database envelope.
base::Result<DatabaseDocument> run_database_command(std::span<const std::string> command, std::string_view workDirectory,
                                                    std::chrono::milliseconds timeout);

nlohmann::json make_discovery_request(const DiscoveryRequest& request);
// Interprets a producer's complete standard output.
base::Result<DiscoveryResult> parse_discovery_output(std::string_view output);
// Runs `command` (argv; absolute program first) in `workDirectory`.
base::Result<DiscoveryResult> run_discovery(std::span<const std::string> command, const DiscoveryRequest& request,
                                            std::string_view workDirectory, std::chrono::milliseconds timeout);

} // namespace lspmcpp::spec
