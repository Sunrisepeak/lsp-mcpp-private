// S1 database, P3286 metadata, S4 kits and S2 discovery.
import std;
import lspmcpp.testing;
import nlohmann.json;
import lspmcpp.base.path;
import lspmcpp.platform.fs;
import lspmcpp.platform.dirs;
import lspmcpp.spec.database;
import lspmcpp.spec.metadata;
import lspmcpp.spec.kit;
import lspmcpp.spec.discovery;


namespace fs = lspmcpp::platform::fs;
namespace base = lspmcpp::base;
using Json = nlohmann::json;

namespace {

std::string repository_root() {
    std::string directory { fs::current_directory() };
    while (true) {
        if (fs::is_regular_file(base::join_path(directory, "specs/README.md"))) return directory;
        const std::string parent { base::parent_path(directory) };
        if (parent == directory) return fs::current_directory();
        directory = parent;
    }
}

std::string scratch(std::string_view name) {
    const std::string directory { base::join_path(lspmcpp::platform::dirs::temp_directory(),
        std::format("lsp-mcpp-test-spec-{}-{}", name, std::chrono::steady_clock::now().time_since_epoch().count())) };
    (void)fs::create_directories(directory);
    return directory;
}

} // namespace

int main() {
    using namespace lspmcpp::testing;
    const std::string root { repository_root() };

    "the level 3 GCC example loads"_test = [&] {
        auto database = lspmcpp::spec::load_database(base::join_path(root, "specs/examples/s1-level3-gcc.json"));
        expect(fatal(database.has_value())) << (database ? "" : database.error().message);
        expect(database->profileVersion == "0.2.0");
        expect(lspmcpp::spec::conformance_level(*database) == 3_i);
        expect(fatal(database->sets.size() == 1u));
        const auto& set = database->sets.front();
        expect(set.units.size() == 3u);
        expect(set.units[1].role == std::optional<lspmcpp::spec::Role> { lspmcpp::spec::Role::module_interface });
        expect(set.units[2].isPrivate);
        const auto* toolchain = lspmcpp::spec::find_toolchain(*database, set.toolchain);
        expect(fatal(toolchain != nullptr));
        expect(toolchain->family == lspmcpp::spec::Family::gcc);
        expect(toolchain->stdlib.has_value() && toolchain->stdlib->name == "libstdc++");
        expect(lspmcpp::spec::absolute_source(set.units[0]) == "/home/u/hello/src/greet/detail.cppm") << lspmcpp::spec::absolute_source(set.units[0]);
    };

    "a database round-trips through JSON"_test = [&] {
        auto first = lspmcpp::spec::load_database(base::join_path(root, "specs/examples/s1-level2-clang-two-sets.json"));
        expect(fatal(first.has_value()));
        const Json written = Json::parse(lspmcpp::spec::to_json(*first).dump());
        auto second = lspmcpp::spec::from_json(written);
        expect(fatal(second.has_value()));
        expect(lspmcpp::spec::to_json(*second) == lspmcpp::spec::to_json(*first));
        expect(lspmcpp::spec::conformance_level(*second) == 2_i);
    };

    "module names resolve in the specified order"_test = [&] {
        auto database = lspmcpp::spec::load_database(base::join_path(root, "specs/examples/s1-level2-clang-two-sets.json"));
        expect(fatal(database.has_value()));
        const auto shapes = lspmcpp::spec::find_set(*database, "shapes@Debug");
        const auto app = lspmcpp::spec::find_set(*database, "app@Debug");
        expect(fatal(shapes.has_value() && app.has_value()));
        std::vector<std::string> manifestsRead;
        const lspmcpp::spec::MetadataReader reader = [&](std::string_view path) {
            manifestsRead.emplace_back(path);
            return std::vector<lspmcpp::spec::ModuleEntry> { { "std", "/opt/llvm/share/libc++/v1/std.cppm", true, {}, {} },
                                                    { "std.compat", "/opt/llvm/share/libc++/v1/std.compat.cppm", true, {}, {} } };
        };
        const auto inSet = lspmcpp::spec::resolve_module(*database, *shapes, "demo.shapes:circle", reader);
        expect(inSet.from == lspmcpp::spec::ResolvedFrom::set);
        expect(inSet.providers.size() == 1u && inSet.providers[0].source == "/work/demo/shapes/circle.cppm");
        const auto visible = lspmcpp::spec::resolve_module(*database, *app, "demo.shapes", reader);
        expect(visible.from == lspmcpp::spec::ResolvedFrom::visible_set);
        expect(visible.providers.size() == 1u && visible.providers[0].set == "shapes@Debug");
        expect(manifestsRead.empty());
        const auto stdlib = lspmcpp::spec::resolve_module(*database, *app, "std", reader);
        expect(stdlib.from == lspmcpp::spec::ResolvedFrom::stdlib);
        expect(stdlib.providers.size() == 1u);
        const auto missing = lspmcpp::spec::resolve_module(*database, *app, "nowhere", reader);
        expect(missing.from == lspmcpp::spec::ResolvedFrom::unresolved && missing.providers.empty());
    };

    "private units are not visible and duplicates are ambiguous"_test = [] {
        auto database = lspmcpp::spec::from_json(Json::parse(R"({
          "version": 1, "revision": 0,
          "sets": [
            { "name": "a", "visible-sets": ["b", "c"], "translation-units": [
              { "source": "main.cpp", "work-directory": "/p", "arguments": ["cc"], "requires": ["m", "hidden"] } ] },
            { "name": "b", "translation-units": [
              { "source": "b/m.cppm", "work-directory": "/p", "arguments": ["cc"], "provides": { "m": "" } },
              { "source": "b/h.cppm", "work-directory": "/p", "arguments": ["cc"], "provides": { "hidden": "" }, "private": true } ] },
            { "name": "c", "translation-units": [
              { "source": "c/m.cppm", "work-directory": "/p", "arguments": ["cc"], "provides": { "m": "" } } ] }
          ] })"));
        expect(fatal(database.has_value()));
        expect(lspmcpp::spec::conformance_level(*database) == 1_i);
        const lspmcpp::spec::MetadataReader none = [](std::string_view) { return std::vector<lspmcpp::spec::ModuleEntry> {}; };
        const auto m = lspmcpp::spec::resolve_module(*database, 0, "m", none);
        expect(m.from == lspmcpp::spec::ResolvedFrom::visible_set);
        expect(m.ambiguous());
        const auto hidden = lspmcpp::spec::resolve_module(*database, 0, "hidden", none);
        expect(hidden.from == lspmcpp::spec::ResolvedFrom::unresolved);
        const auto commands = lspmcpp::spec::to_compile_commands(*database);
        expect(commands.size() == 4u);
        expect(commands[0]["directory"] == "/p");
    };

    "invalid databases are errors"_test = [] {
        expect(!lspmcpp::spec::from_json(Json::parse(R"({"sets": []})")).has_value());
        expect(!lspmcpp::spec::from_json(Json::parse(R"({"version": 1, "sets": [ { "translation-units": [] } ]})")).has_value());
        expect(!lspmcpp::spec::from_json(Json::parse(R"({"version": 1, "sets": [ { "name": "s", "translation-units": [ { "source": "x" } ] } ]})")).has_value());
        expect(!lspmcpp::spec::from_json(Json::parse("[]")).has_value());
    };

    "module metadata paths resolve against the manifest"_test = [] {
        const std::string directory { scratch("metadata") };
        const std::string lib { base::join_path(directory, "lib/x86_64-unknown-linux-gnu") };
        expect(fs::create_directories(lib).has_value());
        const std::string manifest { base::join_path(lib, "libc++.modules.json") };
        expect(fs::write_file(manifest, R"({"version":1,"revision":1,"modules":[
            {"logical-name":"std","source-path":"../../share/libc++/v1/std.cppm","is-std-library":true,
             "local-arguments":{"system-include-directories":["../../share/libc++/v1"]}},
            {"logical-name":"std.compat","source-path":"../../share/libc++/v1/std.compat.cppm","is-std-library":true,
             "local-arguments":{"definitions":[{"name":"X","value":"1"}]}}]})").has_value());
        auto entries = lspmcpp::spec::read_module_metadata(manifest);
        expect(fatal(entries.has_value()));
        expect(fatal(entries->size() == 2u));
        expect((*entries)[0].source == base::join_path(directory, "share/libc++/v1/std.cppm")) << (*entries)[0].source;
        expect((*entries)[0].systemIncludeDirectories.size() == 1u);
        expect((*entries)[1].definitions.size() == 1u && (*entries)[1].definitions[0].value == std::optional<std::string> { "1" });
        const std::string gcc { base::join_path(directory, "gcc.json") };
        expect(fs::write_file(gcc, R"({"version":1,"revision":1,"modules":[{"logical-name":"std","source-path":"/usr/include/c++/16/bits/std.cc","is-std-library":true}]})").has_value());
        auto gccEntries = lspmcpp::spec::read_module_metadata(gcc);
        expect(fatal(gccEntries.has_value() && gccEntries->size() == 1u));
        expect((*gccEntries)[0].source == "/usr/include/c++/16/bits/std.cc");
        expect(!lspmcpp::spec::read_module_metadata(base::join_path(directory, "absent.json")).has_value());
        fs::remove_all(directory);
    };

    "kit examples load and bad kits are refused"_test = [&] {
        for (std::string_view name : { "s4-kit-linux-x64.json", "s4-kit-win32-x64.json", "s4-kit-darwin-arm64.json" }) {
            auto text = fs::read_file(base::join_path(root, base::join_path("specs/examples", name)));
            expect(fatal(text.has_value()));
            auto kit = lspmcpp::spec::parse_kit(Json::parse(*text), "/kits/k");
            expect(kit.has_value()) << name << (kit ? "" : kit.error().message);
            if (kit) {
                expect(!kit->systemIncludeDirectories.empty());
                expect(kit->moduleMetadata.starts_with("/kits/k/"));
                expect(lspmcpp::spec::requires_macos_sdk(*kit) == (name == "s4-kit-darwin-arm64.json"));
            }
        }
        auto valid = Json::parse(R"({"kit-version":1,"name":"k","target":"t",
            "stdlib":{"name":"libc++","version":"23.1.0","module-metadata":"m.json"},
            "system-include-directories":["include"],"sysroot":null,"licenses":[]})");
        expect(lspmcpp::spec::parse_kit(valid, "/k").has_value());
        auto escaping = valid;
        escaping["system-include-directories"] = Json::array({ "../outside" });
        expect(!lspmcpp::spec::parse_kit(escaping, "/k").has_value());
        auto absolute = valid;
        absolute["stdlib"]["module-metadata"] = "C:/m.json";
        expect(!lspmcpp::spec::parse_kit(absolute, "/k").has_value());
        auto unknownRequirement = valid;
        unknownRequirement["requires"] = Json::array({ Json { { "kind", "quantum-sdk" } } });
        expect(!lspmcpp::spec::parse_kit(unknownRequirement, "/k").has_value());
        auto future = valid;
        future["kit-version"] = 2;
        expect(!lspmcpp::spec::parse_kit(future, "/k").has_value());
    };

    "discovery output is interpreted"_test = [&] {
        auto text = fs::read_file(base::join_path(root, "specs/examples/s2-messages.jsonl"));
        expect(fatal(text.has_value()));
        auto result = lspmcpp::spec::parse_discovery_output(*text);
        expect(fatal(result.has_value()));
        expect(result->database == "/home/u/hello/target/build_database.json");
        expect(result->watch.size() == 4u);
        expect(result->progress.size() == 2u);
        expect(!lspmcpp::spec::parse_discovery_output(R"({"kind":"progress","message":"x"})").has_value());
        auto failed = lspmcpp::spec::parse_discovery_output(R"({"kind":"error","message":"no toolchain"})");
        expect(!failed.has_value() && failed.error().message == "no toolchain");
        expect(!lspmcpp::spec::parse_discovery_output("not json").has_value());
        const Json request = lspmcpp::spec::make_discovery_request({ "/w", { "/w/a.cppm" }, "debug" });
        expect(request["profile-version"] == "0.2.0");
        expect(request["files"].size() == 1u);
    };

    return report();
}
