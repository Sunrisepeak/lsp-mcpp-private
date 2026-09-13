module lspmcpp.spec.discovery;

import std;
import nlohmann.json;
import lspmcpp.base.error;
import lspmcpp.base.text;
import lspmcpp.platform.process;
import lspmcpp.spec.database;

namespace lspmcpp::spec {

nlohmann::json make_discovery_request(const DiscoveryRequest& request) {
    nlohmann::json value = nlohmann::json::object();
    value["workspace"] = request.workspace;
    value["files"] = request.files;
    if (!request.configuration.empty()) value["configuration"] = request.configuration;
    value["profile-version"] = std::string { PROFILE_VERSION };
    return value;
}

base::Result<DiscoveryResult> parse_discovery_output(std::string_view output) {
    DiscoveryResult result;
    for (auto line : base::split_lines(output)) {
        line = base::trim(line);
        if (line.empty()) continue;
        nlohmann::json message = nlohmann::json::parse(line, nullptr, false);
        if (message.is_discarded() || !message.is_object()) {
            return base::fail("discovery-protocol", std::format("not a JSON object line: {}", line));
        }
        const std::string kind { message.value("kind", std::string {}) };
        if (kind == "progress") {
            result.progress.push_back(message.value("message", std::string {}));
        } else if (kind == "finished") {
            result.database = message.value("database", std::string {});
            if (result.database.empty()) return base::fail("discovery-protocol", "finished without a database path");
            for (const auto& path : message.value("watch", nlohmann::json::array())) {
                if (path.is_string()) result.watch.push_back(path.get<std::string>());
            }
            return result;
        } else if (kind == "error") {
            return base::fail("discovery-failed", message.value("message", std::string { "the producer reported an error" }));
        }
        // Unknown kinds are ignored, as consumers must ignore unknown fields.
    }
    return base::fail("discovery-protocol", "the producer ended without finished or error");
}

base::Result<DiscoveryResult> run_discovery(std::span<const std::string> command, const DiscoveryRequest& request,
                                            std::string_view workDirectory, std::chrono::milliseconds timeout) {
    if (command.empty()) return base::fail("discovery-command", "empty discovery command");
    platform::SpawnOptions options {
        .program = command.front(),
        .arguments = { command.begin() + 1, command.end() },
        .workDirectory = std::string { workDirectory },
        .pipeInput = true,
        .pipeOutput = true,
        .pipeError = true,
    };
    auto process = platform::Process::spawn(options);
    if (!process) return std::unexpected { process.error() };
    std::string output;
    std::string errors;
    std::jthread outputReader { [&] {
        while (true) {
            auto chunk = process->read_output();
            if (!chunk || chunk->empty()) break;
            output += *chunk;
        }
    } };
    std::jthread errorReader { [&] {
        while (true) {
            auto chunk = process->read_error();
            if (!chunk || chunk->empty()) break;
            errors += *chunk;
        }
    } };
    (void)process->write(make_discovery_request(request).dump() + "\n");
    process->close_input();
    auto waited = process->wait_for(timeout);
    if (!waited || !waited->has_value()) {
        process->terminate();
        (void)process->wait();
        outputReader.join();
        errorReader.join();
        return base::fail("discovery-timeout", std::format("{} did not finish in {} ms", command.front(), timeout.count()));
    }
    outputReader.join();
    errorReader.join();
    auto result = parse_discovery_output(output);
    if (!result && **waited != 0) {
        return base::fail("discovery-failed", std::format("{} exited with {}: {}", command.front(), **waited, base::trim(errors)));
    }
    return result;
}

} // namespace lspmcpp::spec
