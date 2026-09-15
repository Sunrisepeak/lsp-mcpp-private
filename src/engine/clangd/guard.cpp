module mcppls.engine.clangd.guard;

import std;

namespace mcppls::engine::clangd {

GuardClock::time_point RestartGate::earliest(GuardClock::time_point now) const {
    const std::size_t count { recent(now) };
    if (count == 0) return now;
    const std::chrono::seconds gap { std::min<std::chrono::seconds::rep>(FIRST_GAP.count() << std::min<std::size_t>(count - 1, 16),
                                                                         std::chrono::duration_cast<std::chrono::seconds>(MAX_GAP).count()) };
    return std::max(now, restarts_.back() + gap);
}

void RestartGate::record(GuardClock::time_point now) {
    restarts_.push_back(now);
    while (!restarts_.empty() && now - restarts_.front() > WINDOW) restarts_.pop_front();
}

std::size_t RestartGate::recent(GuardClock::time_point now) const {
    return static_cast<std::size_t>(std::ranges::count_if(restarts_, [&](GuardClock::time_point at) { return now - at <= WINDOW; }));
}

Quarantine::Verdict Quarantine::timed_out(std::string_view uri, GuardClock::time_point sent, GuardClock::time_point now,
                                         std::optional<GuardClock::time_point> lastAnswer) {
    while (!unanswered_.empty() && now - std::get<0>(unanswered_.front()) > STALL_WINDOW) unanswered_.pop_front();
    if (lastAnswer && *lastAnswer >= sent) {
        // clangd kept answering others while this file waited: the file is what is stuck.
        auto& entry = entries_[std::string { uri }];
        if (++entry.timeouts < TIMEOUTS_BEFORE_QUARANTINE) return Verdict::wait;
        put(uri, now);
        return Verdict::quarantined;
    }
    unanswered_.emplace_back(now, sent, std::string { uri });
    std::set<std::string_view> files;
    for (const auto& [at, requested, file] : unanswered_) files.insert(file);
    if (files.size() >= 2) return Verdict::stalled;
    auto& entry = entries_[std::string { uri }];
    if (++entry.timeouts < TIMEOUTS_BEFORE_QUARANTINE) return Verdict::wait;
    put(uri, now);
    return Verdict::quarantined;
}

void Quarantine::answered(std::string_view uri) {
    if (const auto it = entries_.find(uri); it != entries_.end() && !it->second.until) it->second.timeouts = 0;
}

bool Quarantine::contains(std::string_view uri) const {
    const auto it = entries_.find(uri);
    return it != entries_.end() && it->second.until.has_value();
}

void Quarantine::put(std::string_view uri, GuardClock::time_point now) {
    auto& entry = entries_[std::string { uri }];
    const std::chrono::minutes term { std::min<std::chrono::minutes::rep>(FIRST_TERM.count() << std::min(entry.terms, 8), MAX_TERM.count()) };
    entry.until = now + term;
    entry.timeouts = 0;
    ++entry.terms;
    std::erase_if(unanswered_, [&](const auto& item) { return std::get<2>(item) == uri; });
}

bool Quarantine::release(std::string_view uri) {
    const auto it = entries_.find(uri);
    if (it == entries_.end() || !it->second.until) return false;
    it->second.until.reset();
    return true;
}

void Quarantine::release_all() {
    for (auto& [uri, entry] : entries_) {
        entry.until.reset();
        entry.timeouts = 0;
    }
    unanswered_.clear();
}

std::vector<std::string> Quarantine::due(GuardClock::time_point now) {
    std::vector<std::string> released;
    for (auto& [uri, entry] : entries_) {
        if (entry.until && *entry.until <= now) {
            entry.until.reset();
            released.push_back(uri);
        }
    }
    return released;
}

std::optional<std::string> Quarantine::first_stalled() const {
    if (unanswered_.empty()) return std::nullopt;
    const auto first = std::ranges::min_element(unanswered_, {}, [](const auto& item) { return std::get<1>(item); });
    return std::get<2>(*first);
}

std::size_t Quarantine::size() const {
    return static_cast<std::size_t>(std::ranges::count_if(entries_, [](const auto& item) { return item.second.until.has_value(); }));
}

std::vector<std::string> Quarantine::members() const {
    std::vector<std::string> files;
    for (const auto& [uri, entry] : entries_) {
        if (entry.until) files.push_back(uri);
    }
    return files;
}

LineLimiter::Decision LineLimiter::admit(GuardClock::time_point now) {
    Decision decision;
    if (!windowStart_ || now - *windowStart_ >= window_) {
        windowStart_ = now;
        inWindow_ = 0;
        decision.suppressedBefore = std::exchange(suppressed_, 0);
    }
    if (inWindow_ < burst_) {
        ++inWindow_;
        decision.forward = true;
    } else {
        ++suppressed_;
        decision.suppressedBefore = 0;
    }
    return decision;
}

std::size_t engine_workers(std::size_t hardwareThreads, bool macos) {
    const std::size_t threads { std::max<std::size_t>(1, hardwareThreads) };
    const std::size_t cores { macos ? threads : std::max<std::size_t>(1, threads / 2) };
    return std::max<std::size_t>(2, cores / 4);
}

std::size_t preparation_limit(std::size_t hardwareThreads, bool macos, std::size_t waitingFiles) {
    const std::size_t workers { engine_workers(hardwareThreads, macos) };
    return std::max<std::size_t>(1, waitingFiles > 0 ? workers / 4 : workers / 2);
}

} // namespace mcppls::engine::clangd
