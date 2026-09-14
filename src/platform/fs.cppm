// Files and directories. Paths are strings in the normalized '/' form of
// lspmcpp.base.path; Windows paths keep their drive ("C:/Users/x").
export module lspmcpp.platform.fs;

import std;
import lspmcpp.base.error;

export namespace lspmcpp::platform::fs {

struct FileStamp {
    std::uint64_t size { 0 };
    std::int64_t modified { 0 };   // nanoseconds since the file-clock epoch; only compared for equality
    auto operator<=>(const FileStamp&) const = default;
};

bool exists(std::string_view path);
bool is_directory(std::string_view path);
bool is_regular_file(std::string_view path);
std::optional<FileStamp> stamp(std::string_view path);

base::Result<std::string> read_file(std::string_view path);
base::Result<void> write_file(std::string_view path, std::string_view content);
// Writes a sibling temporary file and renames it over `path`.
base::Result<void> write_file_atomic(std::string_view path, std::string_view content);
base::Result<void> create_directories(std::string_view path);
void remove_all(std::string_view path);

// Regular files under `root` whose extension (".cppm") is in `extensions`,
// sorted. Directories named in `skipDirectories` and directories starting with
// '.' are not entered. An empty `extensions` accepts every file.
std::vector<std::string> list_files(std::string_view root, std::span<const std::string_view> extensions,
                                    std::span<const std::string_view> skipDirectories);
// Immediate children (files and directories) of a directory, sorted.
std::vector<std::string> list_directory(std::string_view path);

std::string current_directory();

// The name a file has once every symbolic link on the way to it is followed,
// for comparing names that reach one file by different routes: /var and
// /private/var on macOS. The part of a path that does not exist is kept as
// written. On Windows the path is only normalized.
std::string canonical_path(std::string_view path);

} // namespace lspmcpp::platform::fs
