import std;
import lspmcpp.testing;
import lspmcpp.base.path;
import lspmcpp.platform.fs;
import lspmcpp.platform.dirs;

namespace fs = lspmcpp::platform::fs;
namespace base = lspmcpp::base;

int main() {
    using namespace lspmcpp::testing;
    const std::string root { base::join_path(lspmcpp::platform::dirs::temp_directory(),
        std::format("lsp-mcpp-test-fs-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };

    "temporary directory is absolute"_test = [&] {
        expect(base::is_absolute_path(root)) << root;
    };

    "nested directories are created"_test = [&] {
        expect(fatal(fs::create_directories(base::join_path(root, "a/b/c")).has_value()));
        expect(fs::is_directory(base::join_path(root, "a/b/c")));
        expect(fs::create_directories(base::join_path(root, "a/b/c")).has_value());
    };

    "a small file is written and read"_test = [&] {
        const std::string file { base::join_path(root, "a/small.txt") };
        expect(fatal(fs::write_file(file, "first").has_value()));
        expect(fs::read_file(file).value_or("") == "first");
        expect(fs::is_regular_file(file));
        expect(!fs::is_directory(file));
    };

    "an atomic write creates a file"_test = [&] {
        const std::string file { base::join_path(root, "a/data.json") };
        expect(fatal(fs::write_file_atomic(file, "first").has_value()));
        expect(fs::read_file(file).value_or("") == "first");
    };

    "an atomic write replaces an existing file"_test = [&] {
        const std::string file { base::join_path(root, "a/data.json") };
        expect(fatal(fs::write_file_atomic(file, "second").has_value()));
        expect(fs::read_file(file).value_or("") == "second");
    };

    "a large file is written in one call"_test = [&] {
        const std::string file { base::join_path(root, "a/large.bin") };
        expect(fatal(fs::write_file(file, std::string(100000, 'x')).has_value()));
        auto content = fs::read_file(file);
        expect(content.has_value() && content->size() == 100000u);
    };

    "a large atomic write leaves no temporary file"_test = [&] {
        const std::string file { base::join_path(root, "a/data.json") };
        expect(fatal(fs::write_file_atomic(file, std::string(100000, 'y')).has_value()));
        auto content = fs::read_file(file);
        expect(content.has_value() && content->size() == 100000u);
        std::vector<std::string> names;
        for (const auto& entry : fs::list_directory(base::join_path(root, "a"))) names.emplace_back(base::file_name(entry));
        const std::vector<std::string> expected { "b", "data.json", "large.bin", "small.txt" };
        expect(names == expected) << std::format("{}", names);
    };

    "binary content survives"_test = [&] {
        const std::string file { base::join_path(root, "binary.bin") };
        std::string bytes;
        for (int i { 0 }; i < 256; ++i) bytes.push_back(static_cast<char>(i));
        bytes += "\r\n\n\r";
        expect(fatal(fs::write_file(file, bytes).has_value()));
        expect(fs::read_file(file).value_or("") == bytes);
    };

    "stamp changes when content changes"_test = [&] {
        const std::string file { base::join_path(root, "stamp.txt") };
        expect(fs::write_file(file, "1").has_value());
        const auto first = fs::stamp(file);
        expect(fatal(first.has_value()));
        expect(fs::write_file(file, "22").has_value());
        const auto second = fs::stamp(file);
        expect(fatal(second.has_value()));
        expect(second->size == 2u);
        expect(*first != *second);
        expect(!fs::stamp(base::join_path(root, "absent")).has_value());
    };

    "listing follows the skip rules"_test = [&] {
        for (std::string_view name : { "src/m.cppm", "src/main.cpp", "src/notes.txt", "target/x.cppm",
                                       ".git/y.cpp", "sub/node_modules/z.cpp", "sub/deep/w.CPP" }) {
            const std::string file { base::join_path(root, base::join_path("tree", name)) };
            expect(fs::create_directories(base::parent_path(file)).has_value());
            expect(fs::write_file(file, "x").has_value());
        }
        const std::array<std::string_view, 3> extensions { ".cpp", ".cppm", ".ixx" };
        const std::array<std::string_view, 2> skip { "target", "node_modules" };
        const auto files = fs::list_files(base::join_path(root, "tree"), extensions, skip);
        std::vector<std::string> relative;
        for (const auto& file : files) relative.push_back(base::relative_path(file, base::join_path(root, "tree")).value_or(file));
        const std::vector<std::string> expected { "src/m.cppm", "src/main.cpp", "sub/deep/w.CPP" };
        expect(relative == expected) << std::format("{}", relative);
    };

    "missing files are errors, not exceptions"_test = [&] {
        expect(!fs::read_file(base::join_path(root, "missing/file")).has_value());
        expect(!fs::exists(base::join_path(root, "missing")));
    };

    "current directory is absolute"_test = [] {
        expect(base::is_absolute_path(fs::current_directory())) << fs::current_directory();
    };

    fs::remove_all(root);
    "removal is complete"_test = [&] { expect(!fs::exists(root)); };

    return report();
}
