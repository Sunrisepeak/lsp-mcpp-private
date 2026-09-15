// What keeps one fault of clangd from becoming the whole workspace's (robustness design C4, C6, C7):
// restarts spaced out, files clangd stopped answering for set aside one by one, its log forwarded
// without flooding, and module preparation within a share of the machine.
export module mcppls.engine.clangd.guard;

import std;

export namespace mcppls::engine::clangd {

using GuardClock = std::chrono::steady_clock;

// Restarts in a row are spaced out: the first at once, then at least FIRST_GAP after the previous
// one, doubling while restarts keep coming within WINDOW, never more than MAX_GAP apart.
class RestartGate {
public:
    static constexpr std::chrono::seconds FIRST_GAP { 10 };
    static constexpr std::chrono::minutes MAX_GAP { 5 };
    static constexpr std::chrono::minutes WINDOW { 10 };

    // When the next restart may happen.
    GuardClock::time_point earliest(GuardClock::time_point now) const;
    void record(GuardClock::time_point now);
    std::size_t recent(GuardClock::time_point now) const;

private:
    std::deque<GuardClock::time_point> restarts_;
};

// Files whose requests clangd stopped answering. clangd 23.1 can stop answering for one file while it
// answers others (experiments S2, S12); such a file is set aside for a while, answered by mcppls's
// own engine, and handed back when it changes or its time is up. When clangd answers nobody, that is
// the engine stalling, not a file: the caller restarts it.
class Quarantine {
public:
    enum class Verdict { wait, quarantined, stalled };
    static constexpr int TIMEOUTS_BEFORE_QUARANTINE { 2 };
    static constexpr std::chrono::minutes FIRST_TERM { 2 };
    static constexpr std::chrono::minutes MAX_TERM { 16 };
    static constexpr std::chrono::seconds STALL_WINDOW { 60 };

    // A request about `uri`, sent at `sent`, timed out at `now`; `lastAnswer` is when clangd last
    // answered any request.
    Verdict timed_out(std::string_view uri, GuardClock::time_point sent, GuardClock::time_point now,
                      std::optional<GuardClock::time_point> lastAnswer);
    // clangd answered a request about `uri`: its timeouts start over.
    void answered(std::string_view uri);
    bool contains(std::string_view uri) const;
    // Set aside now, for a term that doubles each time the same file comes back.
    void put(std::string_view uri, GuardClock::time_point now);
    // The file changed, or the engine restarted: it goes back to clangd.
    bool release(std::string_view uri);
    void release_all();
    // Files whose term is over, released.
    std::vector<std::string> due(GuardClock::time_point now);
    // Of the files that timed out while clangd answered nobody, the one asked about first.
    std::optional<std::string> first_stalled() const;
    std::size_t size() const;

private:
    struct Entry {
        int timeouts { 0 };
        int terms { 0 };
        std::optional<GuardClock::time_point> until;
    };
    std::map<std::string, Entry, std::less<>> entries_;
    std::deque<std::tuple<GuardClock::time_point, GuardClock::time_point, std::string>> unanswered_;   // (timed out, sent, uri)
};

// Up to `burst` lines in each window; the first line of the next window carries how many were left out.
class LineLimiter {
public:
    LineLimiter(std::size_t burst, std::chrono::milliseconds window) : burst_ { burst }, window_ { window } {}
    struct Decision {
        bool forward { false };
        std::size_t suppressedBefore { 0 };   // lines left out in the windows before this line
    };
    Decision admit(GuardClock::time_point now);

private:
    std::size_t burst_;
    std::chrono::milliseconds window_;
    std::optional<GuardClock::time_point> windowStart_;
    std::size_t inWindow_ { 0 };
    std::size_t suppressed_ { 0 };
};

// How many modules are prepared at once: a quarter of the physical cores (hardware threads count as
// two per core except on macOS), at least one, and half of that while a person waits for a file.
std::size_t preparation_limit(std::size_t hardwareThreads, bool macos, std::size_t waitingFiles);

} // namespace mcppls::engine::clangd
