module lspmcpp.platform.fs;

import std;
import lspmcpp.base.error;
import lspmcpp.base.path;
import lspmcpp.base.text;

namespace lspmcpp::platform::fs {

namespace {

std::filesystem::path native(std::string_view path) { return std::filesystem::path { std::string { path } }; }

std::string from_native(const std::filesystem::path& path) { return base::normalize_path(path.generic_string()); }

std::atomic<std::uint64_t> gTemporaryCounter { 0 };

} // namespace

bool exists(std::string_view path) {
    std::error_code error;
    return std::filesystem::exists(native(path), error);
}

bool is_directory(std::string_view path) {
    std::error_code error;
    return std::filesystem::is_directory(native(path), error);
}

bool is_regular_file(std::string_view path) {
    std::error_code error;
    return std::filesystem::is_regular_file(native(path), error);
}

std::optional<FileStamp> stamp(std::string_view path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(native(path), error);
    if (error) return std::nullopt;
    const auto time = std::filesystem::last_write_time(native(path), error);
    if (error) return std::nullopt;
    return FileStamp { static_cast<std::uint64_t>(size),
                       static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count()) };
}

base::Result<std::string> read_file(std::string_view path) {
    std::ifstream stream { native(path), std::ios::binary };
    if (!stream) return base::fail("read-file", std::format("cannot open {}", path));
    std::string content { std::istreambuf_iterator<char> { stream }, std::istreambuf_iterator<char> {} };
    if (stream.bad()) return base::fail("read-file", std::format("cannot read {}", path));
    return content;
}

base::Result<void> write_file(std::string_view path, std::string_view content) {
    std::ofstream stream { native(path), std::ios::binary | std::ios::trunc };
    if (!stream) return base::fail("write-file", std::format("cannot create {}", path));
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    stream.flush();
    if (!stream) return base::fail("write-file", std::format("cannot write {}", path));
    return {};
}

base::Result<void> write_file_atomic(std::string_view path, std::string_view content) {
    const std::string temporary { std::format("{}.tmp-{}-{}", path,
        std::chrono::steady_clock::now().time_since_epoch().count(), gTemporaryCounter.fetch_add(1)) };
    if (auto written = write_file(temporary, content); !written) return written;
    std::error_code error;
    std::filesystem::rename(native(temporary), native(path), error);
    if (error) {
        // Some systems refuse to replace an existing file by rename.
        std::filesystem::remove(native(path), error);
        error.clear();
        std::filesystem::rename(native(temporary), native(path), error);
    }
    if (error) {
        std::filesystem::remove(native(temporary), error);
        return base::fail("write-file", std::format("cannot replace {}", path));
    }
    return {};
}

base::Result<void> create_directories(std::string_view path) {
    if (is_directory(path)) return {};
    // Created one component at a time: a volume root has no parent to create.
    const std::string normalized { base::normalize_path(path) };
    std::string parent { base::parent_path(normalized) };
    if (!parent.empty() && parent != normalized && !is_directory(parent)) {
        if (auto created = create_directories(parent); !created) return created;
    }
    std::error_code error;
    std::filesystem::create_directory(native(normalized), error);
    if (error && !is_directory(normalized)) {
        return base::fail("create-directory", std::format("cannot create {}: {}", path, error.message()));
    }
    return {};
}

void remove_all(std::string_view path) {
    std::error_code error;
    std::filesystem::remove_all(native(path), error);
}

std::vector<std::string> list_files(std::string_view root, std::span<const std::string_view> extensions,
                                    std::span<const std::string_view> skipDirectories) {
    std::vector<std::string> result;
    std::vector<std::string> pending { base::normalize_path(root) };
    while (!pending.empty()) {
        std::string directory { std::move(pending.back()) };
        pending.pop_back();
        std::error_code error;
        std::filesystem::directory_iterator iterator { native(directory), error };
        if (error) continue;
        for (const auto& entry : iterator) {
            const std::string name { entry.path().filename().generic_string() };
            const std::string full { base::join_path(directory, name) };
            std::error_code statusError;
            if (entry.is_directory(statusError)) {
                if (name.starts_with('.')) continue;
                if (std::ranges::any_of(skipDirectories, [&](std::string_view skip) { return name == skip; })) continue;
                if (entry.is_symlink(statusError)) continue;
                pending.push_back(full);
            } else if (entry.is_regular_file(statusError)) {
                const std::string_view suffix { base::extension(name) };
                if (extensions.empty()
                    || std::ranges::any_of(extensions, [&](std::string_view ext) { return base::iequals_ascii(ext, suffix); })) {
                    result.push_back(full);
                }
            }
        }
    }
    std::ranges::sort(result);
    return result;
}

std::vector<std::string> list_directory(std::string_view path) {
    std::vector<std::string> result;
    std::error_code error;
    std::filesystem::directory_iterator iterator { native(path), error };
    if (error) return result;
    for (const auto& entry : iterator) {
        result.push_back(base::join_path(path, entry.path().filename().generic_string()));
    }
    std::ranges::sort(result);
    return result;
}

std::string current_directory() {
    std::error_code error;
    const auto path = std::filesystem::current_path(error);
    if (error) return {};
    return from_native(path);
}

} // namespace lspmcpp::platform::fs
