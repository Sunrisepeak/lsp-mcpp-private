// Diagnostic logging to standard error. The protocol owns standard output, so
// nothing here may ever write there.
export module lspmcpp.base.log;

import std;

export namespace lspmcpp::base::log {

enum class Level { debug, info, warning, error, off };

void set_level(Level level);
Level level();
bool enabled(Level level);
void write(Level level, std::string_view message);
std::optional<Level> parse_level(std::string_view name);

template <class... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::debug)) write(Level::debug, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::info)) write(Level::info, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void warning(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::warning)) write(Level::warning, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::error)) write(Level::error, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace lspmcpp::base::log
