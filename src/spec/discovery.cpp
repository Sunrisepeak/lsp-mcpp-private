module lspmcpp.spec.discovery;

import std;
import nlohmann.json;
import lspmcpp.base.error;
import lspmcpp.base.text;
import lspmcpp.platform.process;
import lspmcpp.spec.database;

namespace lspmcpp::spec {

namespace {

std::vector<std::string> string_list(const nlohmann::json& value) {
    std::vector<std::string> result;
    if (!value.is_array()) return result;
    for (const auto& item : value) {
        if (item.is_string()) result.push_back(item.get<std::string>());
    }
    return result;
}

} // namespace

base::Result<ProducerProtocol> parse_producer_protocol(std::string_view output) {
    const nlohmann::json document = nlohmann::json::parse(output, nullptr, false);
    if (document.is_discarded() || !document.is_object()) return base::fail("producer-protocol", "the protocol description is not a JSON object");
    ProducerProtocol protocol;
    if (const auto kinds = document.find("kinds"); kinds != document.end() && kinds->is_object()) {
        for (const auto& item : kinds->items()) {
            if (item.value().is_number_integer()) protocol.kinds.emplace(item.key(), item.value().get<int>());
        }
    }
    if (const auto commands = document.find("commands"); commands != document.end() && commands->is_object()) {
        for (const auto& item : commands->items()) {
            if (item.value().is_object()) protocol.commandEffects.emplace(item.key(), string_list(item.value().value("effects", nlohmann::json::array())));
        }
    }
    return protocol;
}

base::Result<DatabaseDocument> parse_database_envelope(std::string_view output) {
    const nlohmann::json envelope = nlohmann::json::parse(output, nullptr, false);
    if (envelope.is_discarded() || !envelope.is_object()) return base::fail("discovery-protocol", "the producer's output is not one JSON object");
    if (envelope.value("schemaVersion", 0) != 1) return base::fail("discovery-protocol", "unsupported envelope schemaVersion");
    DatabaseDocument document;
    document.effects = string_list(envelope.value("effects", nlohmann::json::array()));
    for (const auto& diagnostic : envelope.value("diagnostics", nlohmann::json::array())) {
        if (!diagnostic.is_object()) continue;
        document.diagnostics.push_back(EnvelopeDiagnostic { diagnostic.value("code", std::string {}), diagnostic.value("severity", std::string {}),
                                                            diagnostic.value("message", std::string {}) });
    }
    const std::string kind { envelope.value("kind", std::string {}) };
    const auto data = envelope.find("data");
    if (!kind.ends_with(BUILD_DATABASE_KIND_SUFFIX) || data == envelope.end() || !data->is_object() || !data->contains("database")) {
        std::string reason { "the producer answered without a database" };
        for (const auto& diagnostic : document.diagnostics) {
            if (diagnostic.severity == "error") reason = std::format("{}: {}", diagnostic.code, diagnostic.message);
        }
        return base::fail("discovery-failed", reason);
    }
    document.database = (*data)["database"];
    if (!document.database.is_object()) return base::fail("discovery-protocol", "data.database is not an object");
    document.watch = string_list(data->value("watch", nlohmann::json::array()));
    document.inputsFingerprint = data->value("inputs-fingerprint", std::string {});
    return document;
}

base::Result<DatabaseDocument> run_database_command(std::span<const std::string> command, std::string_view workDirectory,
                                                    std::chrono::milliseconds timeout) {
    if (command.empty()) return base::fail("discovery-command", "empty discovery command");
    platform::SpawnOptions options {
        .program = command.front(),
        .arguments = { command.begin() + 1, command.end() },
        .workDirectory = std::string { workDirectory },
        .pipeInput = true,
        .pipeOutput = true,
        .pipeError = true,
    };
    auto result = platform::run(std::move(options), timeout);
    if (!result) return std::unexpected { result.error() };
    if (result->timedOut) return base::fail("discovery-timeout", std::format("{} did not finish in {} ms", command.front(), timeout.count()));
    auto document = parse_database_envelope(result->output);
    if (!document && base::trim(result->output).empty()) {
        return base::fail("discovery-failed", std::format("{} exited with {}: {}", command.front(), result->exitCode, base::trim(result->error)));
    }
    return document;
}

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
