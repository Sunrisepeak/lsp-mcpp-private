// Dialect translation rules (design section 14.4) and engine database plans.
import std;
import lspmcpp.testing;
import nlohmann.json;
import lspmcpp.base.path;
import lspmcpp.platform.fs;
import lspmcpp.platform.dirs;
import lspmcpp.spec.database;
import lspmcpp.spec.kit;
import lspmcpp.spec.metadata;
import lspmcpp.toolchain.probe;
import lspmcpp.project.scan;
import lspmcpp.normalize.gnu;
import lspmcpp.normalize.msvc;
import lspmcpp.normalize.plan;

namespace s = lspmcpp::spec;
namespace n = lspmcpp::normalize;
namespace p = lspmcpp::project;
using lspmcpp::toolchain::ToolchainFacts;

namespace {

bool contains(const std::vector<std::string>& words, std::string_view word) {
    return std::ranges::find(words, word) != words.end();
}

bool contains_prefix(const std::vector<std::string>& words, std::string_view prefix) {
    return std::ranges::any_of(words, [&](const std::string& word) { return word.starts_with(prefix); });
}

ToolchainFacts gcc_facts(std::string target = "x86_64-linux-gnu") {
    ToolchainFacts facts;
    facts.toolchain.family = s::Family::gcc;
    facts.toolchain.version = "16.1.0";
    facts.toolchain.driver = "/opt/gcc/bin/g++";
    facts.toolchain.target = target;
    facts.toolchain.stdlib = s::Stdlib { "libstdc++", "16.1.0", "/opt/gcc/lib64/libstdc++.modules.json" };
    if (target.find("mingw") != std::string::npos) facts.mingwRoot = "/opt/mingw";
    else facts.gccInstallDirectory = "/opt/gcc/lib/gcc/x86_64-linux-gnu/16.1.0";
    return facts;
}

} // namespace

int main() {
    using namespace lspmcpp::testing;

    "P1: GCC on Linux"_test = [] {
        const auto facts = gcc_facts();
        const std::vector<std::string> arguments { "/opt/gcc/bin/g++", "-std=c++23", "-fmodules", "-fmodule-mapper=/p/m.map", "-O0", "-g",
            "--sysroot=/opt/subos", "-B/opt/binutils/bin", "-fdeps-format=p1689r5", "-MD", "-MF", "x.d", "-c", "src/greet/greet.cppm", "-o", "obj/greet.o" };
        const auto out = n::translate_gnu(n::GnuInput { arguments, "/p/src/greet/greet.cppm", "/p", &facts, true });
        expect(contains(out, "-std=c++23") && contains(out, "-O0") && contains(out, "--sysroot=/opt/subos"));
        expect(!contains(out, "-fmodules") && !contains_prefix(out, "-fmodule-mapper") && !contains_prefix(out, "-B") && !contains_prefix(out, "-fdeps"));
        expect(!contains(out, "-c") && !contains(out, "-o") && !contains(out, "obj/greet.o") && !contains(out, "src/greet/greet.cppm"));
        expect(!contains(out, "-MD") && !contains(out, "-MF") && !contains(out, "x.d"));
        expect(contains(out, "--no-default-config") && contains(out, "--target=x86_64-linux-gnu") && contains(out, "-stdlib=libstdc++"));
        expect(contains(out, "--gcc-install-dir=/opt/gcc/lib/gcc/x86_64-linux-gnu/16.1.0"));
        expect(out.size() >= 2 && out[out.size() - 2] == "-x" && out.back() == "c++-module");
    };

    "P2: GCC for MinGW uses the sysroot"_test = [] {
        const auto facts = gcc_facts("x86_64-w64-mingw32");
        const std::vector<std::string> arguments { "/opt/mingw/bin/x86_64-w64-mingw32-g++", "-std=c++23", "-c", "/p/main.cpp" };
        const auto out = n::translate_gnu(n::GnuInput { arguments, "/p/main.cpp", "/p", &facts, false });
        expect(contains(out, "--sysroot=/opt/mingw"));
        expect(!contains_prefix(out, "--gcc-install-dir"));
        expect(!contains(out, "c++-module"));
    };

    "P3: Clang strips BMI arguments and keeps the rest"_test = [] {
        ToolchainFacts facts;
        facts.toolchain.family = s::Family::clang;
        facts.toolchain.driver = "/llvm/bin/clang++";
        const std::vector<std::string> arguments { "/llvm/bin/clang++", "-std=c++23", "-stdlib=libc++", "-fmodule-file=a=/b/a.pcm",
            "-fmodule-output=/b/m.pcm", "-fprebuilt-module-path=/b", "-fmodules-reduced-bmi", "@CMakeFiles/m.dir/m.cppm.o.modmap",
            "-x", "c++-module", "--precompile", "-DX=1", "-I/p/include", "-c", "/p/m.cppm", "-o", "m.o" };
        const auto out = n::translate_gnu(n::GnuInput { arguments, "/p/m.cppm", "/p/build", &facts, true });
        expect(contains(out, "-stdlib=libc++") && contains(out, "-DX=1") && contains(out, "-I/p/include"));
        expect(!contains_prefix(out, "-fmodule-file") && !contains_prefix(out, "-fmodule-output") && !contains_prefix(out, "-fprebuilt"));
        expect(!contains(out, "-fmodules-reduced-bmi") && !contains(out, "--precompile") && !contains_prefix(out, "@"));
        expect(std::ranges::count(out, std::string { "c++-module" }) == 1);
        expect(!contains(out, "--no-default-config"));
    };

    "P6/P7: MSVC arguments for clang-cl"_test = [] {
        const std::vector<std::string> arguments { "cl.exe", "/std:c++latest", "/EHsc", "/interface", "/ifcOutput", "out\\m.ifc",
            "/reference", "a=out\\a.ifc", "/ifcSearchDir", "out", "/scanDependencies-", "/sourceDependencies:directives", "deps.json",
            "/Foout\\m.obj", "/Fdout\\m.pdb", "/FS", "/c", "/DNAME=1", "src\\m.ixx" };
        const auto out = n::translate_msvc(n::MsvcInput { arguments, "C:/p/src/m.ixx", "C:/p", nullptr, true, "C:/VC", "C:/SDK" });
        expect(out.front() == "--driver-mode=cl");
        expect(contains(out, "/std:c++latest") && contains(out, "/EHsc") && contains(out, "/DNAME=1"));
        expect(!contains(out, "/interface") && !contains(out, "/ifcOutput") && !contains(out, "out\\m.ifc")) << std::format("{}", out);
        expect(!contains(out, "/reference") && !contains(out, "a=out\\a.ifc") && !contains(out, "/ifcSearchDir"));
        expect(!contains_prefix(out, "/Fo") && !contains_prefix(out, "/Fd") && !contains(out, "/c") && !contains(out, "src\\m.ixx") && !contains(out, "deps.json")) << std::format("{}", out);
        expect(contains(out, "/vctoolsdirC:/VC") && contains(out, "/winsdkdirC:/SDK"));
        expect(out.back() == "/clang:-xc++-module");
    };

    "kit arguments and semantic subsets"_test = [] {
        s::Kit kit;
        kit.target = "x86_64-w64-mingw32";
        kit.arguments = { "-nostdinc++", "-nostdlibinc" };
        kit.systemIncludeDirectories = { "/kit/include/c++/v1", "/kit/include" };
        const auto out = n::kit_arguments(kit, "c++26", "");
        const std::vector<std::string> expected { "--no-default-config", "--target=x86_64-w64-mingw32", "-std=c++26", "-nostdinc++",
                                                  "-nostdlibinc", "-isystem", "/kit/include/c++/v1", "-isystem", "/kit/include" };
        expect(out == expected) << std::format("{}", out);
        const std::vector<std::string> command { "cl.exe", "/IC:/inc", "/DA=1", "/std:c++20", "-O2", "/FIpch.h", "-I", "x" };
        const auto subset = n::semantic_subset(command);
        const std::vector<std::string> wanted { "-IC:/inc", "-DA=1", "-std=c++20", "-includepch.h", "-I", "x" };
        expect(subset == wanted) << std::format("{}", subset);
        expect(n::language_standard_of(command) == "c++20");
    };

    "a plan resolves, injects std once and leaves out what cannot resolve"_test = [] {
        const std::string root { lspmcpp::base::join_path(lspmcpp::platform::dirs::temp_directory(),
            std::format("lsp-mcpp-test-plan-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        (void)lspmcpp::platform::fs::create_directories(root);
        const std::map<std::string, std::string> sources {
            { "/p/src/main.cpp", "import std;\nimport hello.greet;\nimport missing.module;\nint main() {}\n" },
            { "/p/src/greet.cppm", "export module hello.greet;\nexport import :detail;\nimport std;\n" },
            { "/p/src/detail.cppm", "export module hello.greet:detail;\nimport std;\n" },
            { "/p/src/broken.cppm", "export module broken;\nimport nowhere;\n" },
            { "/p/src/user.cppm", "export module user;\nimport broken;\n" },
            { "/p/src/dup1.cppm", "export module dup;\n" },
            { "/p/src/dup2.cppm", "export module dup;\n" },
        };
        s::Database database;
        database.hasIde = true;
        s::Set set;
        set.name = "hello";
        set.hasIde = true;
        set.toolchain = "gcc-16.1.0-x86_64-linux-gnu";
        for (const auto& [path, text] : sources) {
            s::TranslationUnit unit;
            unit.source = path;
            unit.workDirectory = "/p";
            unit.arguments = { "/opt/gcc/bin/g++", "-std=c++23", "-fmodules", "-c", path };
            set.units.push_back(std::move(unit));
        }
        database.sets.push_back(set);
        std::map<std::string, ToolchainFacts, std::less<>> facts { { set.toolchain, gcc_facts() } };
        n::PlanInput input;
        input.database = &database;
        input.facts = &facts;
        input.engineDriverDirectory = "/payload/clangd/bin";
        input.scanner = [&](std::string_view path) {
            const auto it = sources.find(std::string { path });
            return it == sources.end() ? p::ScanResult {} : p::scan_source(it->second);
        };
        int manifestReads { 0 };
        input.metadataReader = [&](std::string_view manifest) {
            ++manifestReads;
            expect(manifest == "/opt/gcc/lib64/libstdc++.modules.json");
            return std::vector<s::ModuleEntry> { { "std", "/opt/gcc/include/c++/16/bits/std.cc", true, {}, {} },
                                                 { "std.compat", "/opt/gcc/include/c++/16/bits/std.compat.cc", true, {}, {} } };
        };
        const auto plan = n::plan_engine(input);
        std::set<std::string> files;
        for (const auto& entry : plan.entries) files.insert(entry.file);
        expect(files.contains("/p/src/main.cpp")) << "non-module units are always written";
        expect(files.contains("/p/src/greet.cppm") && files.contains("/p/src/detail.cppm"));
        expect(!files.contains("/p/src/broken.cppm")) << "an interface with an unresolvable import is left out";
        expect(!files.contains("/p/src/user.cppm")) << "and so is an interface importing it";
        expect(files.contains("/p/src/dup1.cppm") && !files.contains("/p/src/dup2.cppm"));
        expect(files.contains("/opt/gcc/include/c++/16/bits/std.cc") && files.contains("/opt/gcc/include/c++/16/bits/std.compat.cc"));
        expect(plan.stdUnits == 2u);
        const auto has_issue = [&](std::string_view code, std::string_view module) {
            return std::ranges::any_of(plan.issues, [&](const n::PlanIssue& issue) { return issue.code == code && issue.module == module; });
        };
        expect(has_issue("unresolved-module", "missing.module"));
        expect(has_issue("unresolved-module", "nowhere"));
        expect(has_issue("ambiguous-module", "dup"));
        for (const auto& entry : plan.entries) {
            if (entry.file == "/p/src/greet.cppm") {
                expect(entry.arguments.front() == "/payload/clangd/bin/clang++");
                expect(entry.arguments.back() == "/p/src/greet.cppm");
                expect(contains(entry.arguments, "c++-module"));
            }
            if (entry.file.ends_with("std.cc")) {
                expect(contains(entry.arguments, "-Wno-reserved-module-identifier") && contains(entry.arguments, "--gcc-install-dir=/opt/gcc/lib/gcc/x86_64-linux-gnu/16.1.0"));
            }
        }
        const auto written = n::write_engine_database(root, plan);
        expect(written.has_value());
        const auto text = lspmcpp::platform::fs::read_file(lspmcpp::base::join_path(root, "compile_commands.json"));
        expect(fatal(text.has_value()));
        expect(nlohmann::json::parse(*text).size() == plan.entries.size());
        lspmcpp::platform::fs::remove_all(root);
    };

    "a kit plan without a toolchain"_test = [] {
        s::Database database;
        s::Set set;
        set.name = "inferred";
        s::TranslationUnit unit;
        unit.source = "/w/main.cpp";
        unit.workDirectory = "/w";
        unit.arguments = { "clang++", "-std=c++23", "-I/w/include", "-c", "/w/main.cpp" };
        set.units.push_back(unit);
        database.sets.push_back(set);
        s::Kit kit;
        kit.target = "x86_64-unknown-linux-gnu";
        kit.moduleMetadata = "/kit/lib/libc++.modules.json";
        kit.arguments = { "-nostdinc++" };
        kit.systemIncludeDirectories = { "/kit/include/c++/v1" };
        kit.sysroot = "/kit/sysroot";
        n::PlanInput input;
        input.database = &database;
        input.kit = &kit;
        input.engineDriverDirectory = "/payload/clangd/bin";
        input.scanner = [](std::string_view) { return p::scan_source("import std;\n"); };
        input.metadataReader = [](std::string_view) {
            return std::vector<s::ModuleEntry> { { "std", "/kit/share/libc++/v1/std.cppm", true, { "/kit/share/libc++/v1" }, {} } };
        };
        const auto plan = n::plan_engine(input);
        expect(fatal(plan.entries.size() == 2u));
        const auto& main = plan.entries[0];
        expect(contains(main.arguments, "--sysroot=/kit/sysroot") && contains(main.arguments, "-I/w/include") && contains(main.arguments, "-std=c++23"));
        expect(std::ranges::count_if(main.arguments, [](const std::string& a) { return a.starts_with("-std="); }) == 1);
        const auto& std = plan.entries[1];
        expect(std.file == "/kit/share/libc++/v1/std.cppm");
        expect(contains(std.arguments, "/kit/share/libc++/v1"));
        expect(plan.issues.empty());
    };

    return report();
}
