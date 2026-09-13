// S2: the discovery command protocol (specs/s2-discovery.md). A producer is
// started with one JSON request on stdin and answers JSON lines on stdout.
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

nlohmann::json make_discovery_request(const DiscoveryRequest& request);
// Interprets a producer's complete standard output.
base::Result<DiscoveryResult> parse_discovery_output(std::string_view output);
// Runs `command` (argv; absolute program first) in `workDirectory`.
base::Result<DiscoveryResult> run_discovery(std::span<const std::string> command, const DiscoveryRequest& request,
                                            std::string_view workDirectory, std::chrono::milliseconds timeout);

} // namespace lspmcpp::spec
