// lsp-mcpp-mock-mcpp: stands in for mcpp in conformance fixtures. It answers the
// machine-output contract proposed in mcpp-community/mcpp#636 from data a fixture
// recorded in mcpp-mock.json, so the consumer side is exercised before mcpp
// ships the command.
//
//   lsp-mcpp-mock-mcpp --protocol-version
//   lsp-mcpp-mock-mcpp emit build-database --format json
//
// In mcpp-mock.json every string may use ${root} (the directory the command runs
// in) and ${env:NAME} or ${env:NAME|fallback}. {"database": <S1>, "watch": [...]}
// is answered as an envelope; {"diagnostics": [...]} as a failure with exit 1.
import std;
import nlohmann.json;
import lspmcpp.base.path;
import lspmcpp.base.text;
import lspmcpp.platform.env;
import lspmcpp.platform.fs;

namespace base = lspmcpp::base;
namespace fs = lspmcpp::platform::fs;
using Json = nlohmann::ordered_json;

namespace {

constexpr std::string_view VERSION { "0.0.0-mock" };

std::string expand(std::string_view text, const std::string& root) {
    std::string out;
    for (std::size_t i { 0 }; i < text.size();) {
        if (text.substr(i).starts_with("${")) {
            const std::size_t close { text.find('}', i) };
            if (close != std::string_view::npos) {
                const std::string_view name { text.substr(i + 2, close - i - 2) };
                if (name == "root") {
                    out += root;
                } else if (name.starts_with("env:")) {
                    std::string_view variable { name.substr(4) };
                    std::string fallback;
                    if (const std::size_t bar { variable.find('|') }; bar != std::string_view::npos) {
                        fallback = std::string { variable.substr(bar + 1) };
                        variable = variable.substr(0, bar);
                    }
                    out += base::normalize_path(lspmcpp::platform::env::get(variable).value_or(fallback));
                } else {
                    out += text.substr(i, close - i + 1);
                }
                i = close + 1;
                continue;
            }
        }
        out += text[i++];
    }
    return out;
}

void expand_all(Json& value, const std::string& root) {
    if (value.is_string()) {
        value = expand(value.get<std::string>(), root);
    } else if (value.is_array() || value.is_object()) {
        for (auto& child : value) expand_all(child, root);
    }
}

Json envelope(std::string_view kind, Json data, Json diagnostics, Json effects) {
    Json document = Json::object();
    document["schemaVersion"] = 1;
    document["kind"] = kind;
    document["kindVersion"] = 1;
    document["effects"] = std::move(effects);
    document["mcpp"] = Json { { "version", VERSION }, { "protocol", Json { { "min", 1 }, { "max", 1 } } } };
    if (!data.is_null()) document["data"] = std::move(data);
    document["diagnostics"] = std::move(diagnostics);
    return document;
}

// FNV-1a over the files a real producer would read, so the answer changes when they do.
std::string fingerprint(const std::string& root) {
    std::uint64_t hash { 1469598103934665603ull };
    auto mix = [&](std::string_view bytes) {
        for (const unsigned char c : bytes) {
            hash ^= c;
            hash *= 1099511628211ull;
        }
    };
    static constexpr std::array<std::string_view, 5> SOURCES { ".cppm", ".cpp", ".ixx", ".cc", ".cxx" };
    for (const auto& file : fs::list_files(root, SOURCES, std::array<std::string_view, 1> { "target" })) {
        mix(file);
        mix(fs::read_file(file).value_or(""));
    }
    mix(fs::read_file(base::join_path(root, "mcpp.toml")).value_or(""));
    return std::format("fnv1a:{:016x}", hash);
}

int protocol_version() {
    Json document = Json::object();
    document["schemaVersion"] = 1;
    document["kind"] = "mcpp.protocol";
    document["envelope"] = Json { { "min", 1 }, { "max", 1 } };
    document["kinds"] = Json { { "mcpp.build-database", 1 } };
    document["commands"] = Json { { "emit build-database", Json { { "effects", Json::array({ "read-project", "network", "write-global-cache", "exec-build-script" }) } } } };
    document["mcpp"] = Json { { "version", VERSION } };
    std::println("{}", document.dump(2));
    return 0;
}

int emit_build_database(std::span<const std::string> arguments) {
    std::string format;
    for (std::size_t i { 0 }; i < arguments.size(); ++i) {
        if (arguments[i] == "--format" && i + 1 < arguments.size()) format = arguments[++i];
        else if (arguments[i].starts_with("--format=")) format = arguments[i].substr(9);
    }
    if (format != "json") {
        std::println(std::cerr, "error: unsupported --format '{}'; expected: json", format);
        return 2;
    }
    const std::string root { base::normalize_path(fs::current_directory()) };
    auto text = fs::read_file(base::join_path(root, "mcpp-mock.json"));
    if (!text) {
        const Json diagnostics = Json::array({ Json { { "code", "MCPP_MOCK_NO_DATA" }, { "severity", "error" }, { "source", "mcpp" },
                                                      { "message", "this fixture recorded no mcpp-mock.json" } } });
        std::println("{}", envelope("mcpp.build-database", nullptr, diagnostics, Json::array({ "read-project" })).dump(2));
        return 1;
    }
    Json recorded = Json::parse(*text, nullptr, false);
    if (recorded.is_discarded() || !recorded.is_object()) {
        std::println(std::cerr, "error: mcpp-mock.json is not a JSON object");
        return 70;
    }
    expand_all(recorded, root);
    if (!recorded.contains("database")) {
        std::println("{}", envelope("mcpp.build-database", nullptr, recorded.value("diagnostics", Json::array()), Json::array({ "read-project" })).dump(2));
        return 1;
    }
    Json data = Json::object();
    data["database"] = recorded["database"];
    data["watch"] = recorded.value("watch", Json::array({ "mcpp.toml", "mcpp.lock", "src/**/*.cppm", "src/**/*.cpp" }));
    data["inputs-fingerprint"] = fingerprint(root);
    std::println("{}", envelope("mcpp.build-database", std::move(data), Json::array(), Json::array({ "read-project" })).dump(2));
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    const std::vector<std::string> arguments { argv + 1, argv + argc };
    if (arguments.size() == 1 && arguments[0] == "--protocol-version") return protocol_version();
    if (arguments.size() >= 2 && arguments[0] == "emit" && arguments[1] == "build-database") {
        return emit_build_database(std::span<const std::string> { arguments }.subspan(2));
    }
    if (!arguments.empty() && arguments[0] == "--version") {
        std::println("mcpp {}", VERSION);
        return 0;
    }
    // Anything else, notably `build --configure-only`, is what the consumer must not need. A build
    // leaves the mark a real one would, a compile_commands.json in the project, so that a fixture's
    // workspace-unchanged check sees it was run.
    if (!arguments.empty() && arguments[0] == "build") (void)fs::write_file(base::join_path(fs::current_directory(), "compile_commands.json"), "[]\n");
    std::println(std::cerr, "lsp-mcpp-mock-mcpp: '{}' is not simulated", base::join(arguments, " "));
    return arguments.empty() ? 2 : 127;
}
