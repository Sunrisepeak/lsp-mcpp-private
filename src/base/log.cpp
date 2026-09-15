module mcppls.base.log;

import std;
import openkal.stream;

namespace mcppls::base::log {

namespace {

std::atomic<Level> gLevel { Level::info };
std::mutex gWriteMutex;
std::function<void(std::string_view)> gSink;

std::string_view name_of(Level level) {
    switch (level) {
    case Level::debug: return "debug";
    case Level::info: return "info";
    case Level::warning: return "warning";
    case Level::error: return "error";
    case Level::off: return "off";
    }
    return "?";
}

} // namespace

void set_level(Level level) { gLevel.store(level); }

void set_sink(std::function<void(std::string_view line)> sink) {
    std::lock_guard lock { gWriteMutex };
    gSink = std::move(sink);
}

Level level() { return gLevel.load(); }

bool enabled(Level level) {
    return level != Level::off && static_cast<int>(level) >= static_cast<int>(gLevel.load());
}

void write(Level level, std::string_view message) {
    const std::string line { std::format("mcppls [{}] {}\n", name_of(level), message) };
    std::lock_guard lock { gWriteMutex };
    if (gSink) {
        gSink(line);
        return;
    }
    std::size_t done { 0 };
    while (done < line.size()) {
        const auto written = kal_stream_write(kal_stderr(), line.data() + done, line.size() - done);
        if (written <= 0) break;
        done += static_cast<std::size_t>(written);
    }
    kal_stream_flush(kal_stderr());
}

std::optional<Level> parse_level(std::string_view name) {
    if (name == "debug" || name == "verbose") return Level::debug;
    if (name == "info") return Level::info;
    if (name == "warning" || name == "warn") return Level::warning;
    if (name == "error") return Level::error;
    if (name == "off") return Level::off;
    return std::nullopt;
}

} // namespace mcppls::base::log
