// Project detection, inference from sources and compile databases, and model loading.
import std;
import lspmcpp.testing;
import nlohmann.json;
import lspmcpp.base.path;
import lspmcpp.base.text;
import lspmcpp.platform.fs;
import lspmcpp.platform.dirs;
import lspmcpp.spec.database;
import lspmcpp.spec.kit;
import lspmcpp.spec.metadata;
import lspmcpp.toolchain.probe;
import lspmcpp.project.detect;
import lspmcpp.project.infer;
import lspmcpp.project.mcpp;
import lspmcpp.project.model;
import lspmcpp.project.compdb;

namespace fs = lspmcpp::platform::fs;
namespace b = lspmcpp::base;
namespace p = lspmcpp::project;
namespace s = lspmcpp::spec;

namespace {

std::string make_root(std::string_view name) {
    const std::string root { b::join_path(lspmcpp::platform::dirs::temp_directory(),
        std::format("lsp-mcpp-test-project-{}-{}", name, std::chrono::steady_clock::now().time_since_epoch().count())) };
    (void)fs::create_directories(root);
    return root;
}

void write(const std::string& root, std::string_view relative, std::string_view content) {
    const std::string path { b::join_path(root, relative) };
    (void)fs::create_directories(b::parent_path(path));
    (void)fs::write_file(path, content);
}

void write_fixture(const std::string& root) {
    write(root, "src/main.cpp", "import std;\nimport hello.greet;\n\nint main() {\n    std::println(\"{}\", hello::greet(\"mcpp\"));\n}\n");
    write(root, "src/greet/greet.cppm", "export module hello.greet;\nexport import :detail;\nimport std;\n");
    write(root, "src/greet/detail.cppm", "export module hello.greet:detail;\nimport std;\n");
    write(root, "target/obj/generated.cppm", "export module should.be.skipped;\n");
    write(root, ".hidden/x.cpp", "import hidden;\n");
}

} // namespace

int main() {
    using namespace lspmcpp::testing;

    "detection order"_test = [] {
        const std::string mcpp { make_root("mcpp") };
        write(mcpp, "mcpp.toml", "[package]\nname = \"hello\"\n");
        write(mcpp, "CMakeLists.txt", "project(x)\n");
        expect(p::detect_project(mcpp).kind == p::SourceKind::mcpp);

        const std::string cmake { make_root("cmake") };
        write(cmake, "CMakeLists.txt", "project(x)\n");
        write(cmake, "build/CMakeCache.txt", "");
        write(cmake, "build/compile_commands.json", "[]");
        const auto cmakeDetection = p::detect_project(cmake);
        expect(cmakeDetection.kind == p::SourceKind::cmake);
        expect(cmakeDetection.buildDirectory == b::join_path(cmake, "build"));
        expect(cmakeDetection.compileCommands == b::join_path(cmake, "build/compile_commands.json"));

        const std::string commands { make_root("compdb") };
        write(commands, "compile_commands.json", "[]");
        expect(p::detect_project(commands).kind == p::SourceKind::compile_commands);

        const std::string loose { make_root("loose") };
        write(loose, "a.cppm", "export module a;\n");
        expect(p::detect_project(loose).kind == p::SourceKind::inferred);
        expect(p::detect_project(loose, "db.json").kind == p::SourceKind::build_database);
        for (const auto& root : { mcpp, cmake, commands, loose }) fs::remove_all(root);
    };

    "inference from sources"_test = [] {
        const std::string root { make_root("infer") };
        write_fixture(root);
        const auto inferred = p::infer_database(root, p::InferOptions {}, p::file_scanner());
        expect(fatal(inferred.database.sets.size() == 1u));
        const auto& set = inferred.database.sets.front();
        expect(set.units.size() == 3u) << "target/ and dot directories are skipped";
        std::map<std::string, const s::TranslationUnit*> byName;
        for (const auto& unit : set.units) byName[std::string { b::file_name(unit.source) }] = &unit;
        expect(fatal(byName.contains("greet.cppm") && byName.contains("detail.cppm") && byName.contains("main.cpp")));
        expect(byName["greet.cppm"]->role == std::optional<s::Role> { s::Role::module_interface });
        expect(byName["detail.cppm"]->providedModules.front().first == "hello.greet:detail");
        expect(byName["greet.cppm"]->requiredModules == std::vector<std::string> { "hello.greet:detail", "std" });
        expect(byName["main.cpp"]->role == std::optional<s::Role> { s::Role::non_module });
        expect(byName["main.cpp"]->arguments.back() == byName["main.cpp"]->source);
        expect(set.toolchain.empty());
        const auto resolution = s::resolve_module(inferred.database, 0, "hello.greet", [](std::string_view) { return std::vector<s::ModuleEntry> {}; });
        expect(resolution.from == s::ResolvedFrom::set);
        fs::remove_all(root);
    };

    "compile commands become sets per toolchain"_test = [] {
        const std::string root { make_root("sets") };
        write_fixture(root);
        std::vector<p::CompileCommand> commands;
        for (std::string_view file : { "src/main.cpp", "src/greet/greet.cppm", "src/greet/detail.cppm" }) {
            const std::string path { b::join_path(root, file) };
            commands.push_back(p::CompileCommand { root, path, "", { "/opt/gcc/bin/g++", "-std=c++23", "-c", path } });
        }
        commands.push_back(p::CompileCommand { root, b::join_path(root, "src/c.c"), "", { "/opt/gcc/bin/gcc", "-c", "src/c.c" } });
        const p::Prober prober = [](std::string_view driver, std::span<const std::string>) -> std::optional<lspmcpp::toolchain::ToolchainFacts> {
            lspmcpp::toolchain::ToolchainFacts facts;
            facts.toolchain.family = s::Family::gcc;
            facts.toolchain.version = "16.1.0";
            facts.toolchain.driver = std::string { driver };
            facts.toolchain.target = "x86_64-linux-gnu";
            return facts;
        };
        const auto result = p::database_from_commands(commands, "hello", p::file_scanner(), prober);
        expect(fatal(result.database.sets.size() == 1u));
        expect(result.database.sets.front().units.size() == 3u) << "C sources are not part of the C++ model";
        expect(result.database.sets.front().toolchain == "gcc-16.1.0-x86_64-linux-gnu");
        expect(result.facts.size() == 1u);
        expect(s::conformance_level(result.database) == 2_i);
        fs::remove_all(root);
    };

    "an untrusted workspace still gets a model"_test = [] {
        const std::string root { make_root("untrusted") };
        write_fixture(root);
        write(root, "mcpp.toml", "[package]\nname = \"hello\"\nversion = \"0.1.0\"\n");
        s::Kit kit;
        kit.name = "k";
        kit.target = "x86_64-unknown-linux-gnu";
        kit.stdlibName = "libc++";
        kit.stdlibVersion = "23.1.0";
        p::LoadOptions options;
        options.trusted = false;
        options.kit = &kit;
        options.cacheDirectory = b::join_path(root, ".cache-dir");
        const auto model = p::load_project(root, options);
        expect(model.source == p::SourceKind::inferred);
        expect(model.usesKit);
        expect(model.profile.kind == "semantic-kit" && model.profile.stdlib == "libc++ 23.1.0");
        expect(std::ranges::any_of(model.issues, [](const p::ModelIssue& issue) { return issue.code == "mcpp-no-database"; }));
        expect(!model.watch.empty());
        expect(model.database.sets.front().units.size() == 3u);
        fs::remove_all(root);
    };

    "a producer's build database is completed, not replaced"_test = [] {
        const std::string root { make_root("database") };
        write_fixture(root);
        const std::string main { b::join_path(root, "src/main.cpp") };
        const std::string greet { b::join_path(root, "src/greet/greet.cppm") };
        const std::string detail { b::join_path(root, "src/greet/detail.cppm") };
        nlohmann::json document = nlohmann::json::parse(std::format(R"({{
          "version": 1, "revision": 0,
          "sets": [ {{ "name": "hello", "translation-units": [
            {{ "source": "{}", "work-directory": "{}", "arguments": ["clang++", "-c", "{}"] }},
            {{ "source": "{}", "work-directory": "{}", "arguments": ["clang++", "-c", "{}"] }},
            {{ "source": "{}", "work-directory": "{}", "arguments": ["clang++", "-c", "{}"], "ide": {{ "role": "module-partition-interface" }},
               "provides": {{ "hello.greet:detail": "" }}, "requires": ["std"] }}
          ] }} ] }})", main, root, main, greet, root, greet, detail, root, detail));
        write(root, "build/db.json", document.dump());
        p::LoadOptions options;
        options.trusted = false;
        const auto model = p::load_project(root, p::LoadOptions { options.trusted, {}, "build/db.json" });
        expect(model.source == p::SourceKind::build_database);
        expect(fatal(model.database.sets.size() == 1u && model.database.sets[0].units.size() == 3u));
        const auto& units = model.database.sets[0].units;
        expect(units[1].role == std::optional<s::Role> { s::Role::module_interface }) << "scanned";
        expect(units[1].providedModules.size() == 1u && units[1].providedModules[0].first == "hello.greet");
        expect(units[2].requiredModules == std::vector<std::string> { "std" }) << "kept as the producer wrote it";
        fs::remove_all(root);
    };

    "mcpp manifests"_test = [] {
        expect(p::mcpp_package_name("# c\n[package]\nname        = \"lsp-mcpp\"\nversion = \"0.1.0\"\n[dependencies]\nname = \"x\"\n") == "lsp-mcpp");
        expect(p::mcpp_package_name("[dependencies]\nname = \"x\"\n").empty());
    };

    "a package's std comes from mcpp's std build record"_test = [] {
        const std::string root { make_root("mcpp-std") };
        const std::string build { b::join_path(root, "target/x86_64-linux-gnu/3ea09e2fa2b8f653") };
        const std::string cache { b::join_path(root, "build-cache/v1/std/32dc1b80167fa105") };
        const std::string runtime { b::join_path(root, "xpkgs/openkal-llvm-runtime-0.9.6") };
        write(build, "build.ninja", std::format("cxxflags = -std=c++23\nbuild pcm.cache/std.pcm : stage_file {0}/pcm.cache/std.pcm\n"
                                                "build pcm.cache/std.compat.pcm : stage_file {0}/pcm.cache/std.compat.pcm | pcm.cache/std.pcm\n", cache));
        nlohmann::json record {
            { "schema", 1 },
            { "std_module_source", runtime + "/llvm-generated/std.cppm" },
            { "std_compat_source", runtime + "/llvm-generated/std.compat.cppm" },
            { "std_build_commands", nlohmann::json::array({
                std::format("cd '{0}' && env LD_LIBRARY_PATH='/llvm/lib' '/llvm/bin/clang++' -std=c++23 -Wno-reserved-module-identifier '-nostdinc++' "
                            "-isystem '{1}/llvm/libcxx/include' '-D_XOPEN_SOURCE=700' --precompile '{1}/llvm-generated/std.cppm' -o 'pcm.cache/std.pcm' 2>&1", cache, runtime),
                std::format("cd '{0}' && env LD_LIBRARY_PATH='/llvm/lib' '/llvm/bin/clang++' -std=c++23 'pcm.cache/std.pcm' -c -o std.o 2>&1", cache) }) },
            { "std_compat_build_commands", nlohmann::json::array({
                std::format("env LD_LIBRARY_PATH='/llvm/lib' '/llvm/bin/clang++' -std=c++23 -fmodule-file=std={0}/pcm.cache/std.pcm "
                            "--precompile '{1}/llvm-generated/std.compat.cppm' -o '{0}/pcm.cache/std.compat.pcm' 2>&1", cache, runtime) }) },
        };
        write(cache, "std-module.json", record.dump(2));
        p::CompileCommand user;
        user.directory = root;
        user.file = b::join_path(root, "src/main.cpp");
        user.arguments = { "/llvm/bin/clang++", "-std=c++23", std::format("-fmodule-file=std={}/pcm.cache/std.pcm", build), "-c", user.file };
        const std::vector<p::CompileCommand> commands { user };
        const auto units = p::mcpp_standard_units(commands);
        expect(fatal(units.size() == 2u));
        expect(units[0].file == runtime + "/llvm-generated/std.cppm");
        expect(units[0].directory == cache);
        expect(units[0].arguments == std::vector<std::string> { "/llvm/bin/clang++", "-std=c++23", "-Wno-reserved-module-identifier", "-nostdinc++", "-isystem",
                                                                runtime + "/llvm/libcxx/include", "-D_XOPEN_SOURCE=700", runtime + "/llvm-generated/std.cppm" })
            << b::join(units[0].arguments, " ");
        expect(units[1].file == runtime + "/llvm-generated/std.compat.cppm");
        expect(units[1].directory == cache) << "a command without cd runs where the record is";
        expect(std::ranges::none_of(units[1].arguments, [](const std::string& word) { return word.starts_with("-fmodule-file=") || word == "-o"; }));

        user.arguments = { "/llvm/bin/clang++", "-std=c++23", "-c", user.file };
        expect(p::mcpp_standard_units(std::vector<p::CompileCommand> { user }).empty()) << "no staged std BMI, nothing to find";
        fs::remove_all(root);
    };

    "workspace keys are stable and distinct"_test = [] {
        expect(p::workspace_key("/home/u/project") == p::workspace_key("/home/u/project/"));
        expect(p::workspace_key("/home/u/project") != p::workspace_key("/home/u/project2"));
        expect(p::workspace_key("/home/u/project").starts_with("project-"));
    };

    return report();
}
