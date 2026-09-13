module lspmcpp.platform.process;

import std;
import openkal.types;
import openkal.fs;
import openkal.stream;
import openkal.process;
import openkal.timeout;
import lspmcpp.base.error;
import lspmcpp.base.path;
import lspmcpp.base.text;
import lspmcpp.platform.env;
import lspmcpp.platform.fs;

namespace lspmcpp::platform {

namespace {

// Bit positions from openkal/process.h. The C header states them as macros,
// which a module does not carry; the layout is frozen by the specification.
constexpr kal_uintptr SPAWN_BOUND_LIFETIME { kal_uintptr { 1 } << 0 };
constexpr kal_uintptr PROP_BOUND_LIFETIME { kal_uintptr { 1 } << 5 };

struct Preopen {
    kal_dir directory {};
    std::string name;
};

const std::vector<Preopen>& preopens() {
    static const std::vector<Preopen> table { [] {
        std::vector<Preopen> result;
        const kal_uintptr count { kal_fs_preopen_count() };
        for (kal_uintptr i { 0 }; i < count; ++i) {
            kal_dir dir {};
            std::array<char, 1024> name {};
            kal_uintptr length { 0 };
            if (kal_fs_preopen(i, &dir, name.data(), name.size(), &length) != kal_ok) continue;
            if (length > name.size()) continue;
            result.push_back(Preopen { dir, std::string { name.data(), static_cast<std::size_t>(length) } });
        }
        return result;
    }() };
    return table;
}

bool is_separator(char c) { return c == '/' || c == '\\'; }

struct Resolved {
    kal_dir directory {};
    std::string name;
    std::string remainder;
};

std::optional<Resolved> resolve(std::string_view rawPath) {
    const std::string path { base::normalize_path(rawPath) };
    std::optional<Resolved> best;
    for (const auto& preopen : preopens()) {
        std::string prefix { base::normalize_path(preopen.name) };
        if (prefix.empty()) continue;
        const bool prefixIsRoot { is_separator(prefix.back()) };
        bool matches { false };
        if (lspmcpp::base::NATIVE_PATH_STYLE == lspmcpp::base::PathStyle::windows) {
            matches = path.size() >= prefix.size()
                   && base::iequals_ascii(std::string_view { path }.substr(0, prefix.size()), prefix);
        } else {
            matches = path.starts_with(prefix);
        }
        if (!matches) continue;
        if (!prefixIsRoot && path.size() > prefix.size() && !is_separator(path[prefix.size()])) continue;
        if (best && best->name.size() >= prefix.size()) continue;
        std::string_view rest { std::string_view { path }.substr(prefix.size()) };
        while (!rest.empty() && is_separator(rest.front())) rest.remove_prefix(1);
        best = Resolved { preopen.directory, prefix, std::string { rest } };
    }
    return best;
}

std::string describe(int code) {
    switch (code) {
    case kal_err_invalid: return "invalid argument";
    case kal_err_again: return "would block";
    case kal_err_io: return "input/output error";
    case kal_err_no_memory: return "out of memory";
    case kal_err_permission: return "permission denied";
    case kal_err_not_supported: return "not supported";
    case kal_err_closed: return "closed";
    case kal_err_not_found: return "not found";
    default: return std::format("openkal error {}", code);
    }
}

} // namespace

std::optional<PreopenMatch> match_preopen(std::string_view absolutePath) {
    auto resolved = resolve(absolutePath);
    if (!resolved) return std::nullopt;
    return PreopenMatch { resolved->name, resolved->remainder };
}

struct Process::State {
    kal_process handle {};
    kal_stream input {};
    kal_stream output {};
    kal_stream error {};
    bool inputOpen { false };
    bool outputOpen { false };
    bool errorOpen { false };
    bool exited { false };
    int exitCode { -1 };
    std::mutex writeMutex;
    std::mutex waitMutex;
};

Process::Process() = default;
Process::Process(Process&& other) noexcept = default;
Process& Process::operator=(Process&& other) noexcept = default;

Process::~Process() {
    if (!state_) return;
    close_input();
    if (!state_->exited) {
        kal_process_terminate(state_->handle);
        int status { 0 };
        int terminated { 0 };
        kal_process_wait(state_->handle, &status, &terminated);
    }
    if (state_->outputOpen) kal_process_channel_close(state_->output);
    if (state_->errorOpen) kal_process_channel_close(state_->error);
    kal_process_close(state_->handle);
}

bool Process::valid() const { return static_cast<bool>(state_); }

base::Result<Process> Process::spawn(const SpawnOptions& options) {
    if (!base::is_absolute_path(options.program)) {
        return base::fail("spawn-program", std::format("program path must be absolute: {}", options.program));
    }
    const auto program = resolve(options.program);
    if (!program) {
        return base::fail("spawn-program", std::format("program is outside every preopened directory: {}", options.program));
    }
    const std::string workPath { options.workDirectory.empty() ? fs::current_directory() : options.workDirectory };
    const auto work = resolve(workPath);
    if (!work) {
        return base::fail("spawn-directory", std::format("work directory is outside every preopened directory: {}", workPath));
    }

    kal_dir workDir { work->directory };
    bool closeWorkDir { false };
    if (!work->remainder.empty()) {
        const int opened { kal_fs_open_dir(work->directory, work->remainder.data(), work->remainder.size(), &workDir) };
        if (opened != kal_ok) {
            return base::fail("spawn-directory", std::format("cannot open work directory {}: {}", workPath, describe(opened)));
        }
        closeWorkDir = true;
    }
    auto closeWork = [&] { if (closeWorkDir) kal_fs_close_dir(workDir); };

    auto state = std::make_unique<State>();
    kal_stream childIn {};
    kal_stream childOut {};
    kal_stream childErr {};
    // A channel is one-way: the first stream reads what the second writes.
    if (options.pipeInput) {
        if (kal_process_channel(&childIn, &state->input) != kal_ok) {
            closeWork();
            return base::fail("spawn-channel", "cannot create the input channel");
        }
        state->inputOpen = true;
    }
    if (options.pipeOutput) {
        if (kal_process_channel(&state->output, &childOut) != kal_ok) {
            closeWork();
            return base::fail("spawn-channel", "cannot create the output channel");
        }
        state->outputOpen = true;
    }
    if (options.pipeError) {
        if (kal_process_channel(&state->error, &childErr) != kal_ok) {
            closeWork();
            return base::fail("spawn-channel", "cannot create the error channel");
        }
        state->errorOpen = true;
    }

    std::vector<std::string> argvStorage;
    argvStorage.reserve(options.arguments.size() + 1);
    argvStorage.push_back(options.program);
    for (const auto& argument : options.arguments) argvStorage.push_back(argument);
    std::vector<const char*> argv;
    std::vector<kal_uintptr> argvLengths;
    for (const auto& argument : argvStorage) {
        argv.push_back(argument.data());
        argvLengths.push_back(argument.size());
    }

    // A null environment starts the child with an empty one, so inheritance is explicit.
    const std::vector<std::string> environmentStorage { options.environment ? *options.environment : env::variables() };
    std::vector<const char*> envp;
    std::vector<kal_uintptr> envpLengths;
    for (const auto& variable : environmentStorage) {
        envp.push_back(variable.data());
        envpLengths.push_back(variable.size());
    }

    const kal_uintptr flags { (kal_process_props() & PROP_BOUND_LIFETIME) != 0 ? SPAWN_BOUND_LIFETIME : kal_uintptr { 0 } };
    const kal_spawn how { program->directory, workDir, nullptr, nullptr, 0, flags };
    // A zero stream inherits the parent's; kal_stdin() is zero on openkal-linux, which is the same act.
    const kal_spawn_streams streams {
        options.pipeInput ? childIn : kal_stream {},
        options.pipeOutput ? childOut : kal_stderr(),
        options.pipeError ? childErr : kal_stderr(),
    };
    const int result { kal_process_spawn(&how, program->remainder.data(), program->remainder.size(),
                                         argv.data(), argvLengths.data(), argv.size(),
                                         envp.data(), envpLengths.data(), envp.size(),
                                         &streams, &state->handle) };
    if (options.pipeInput) kal_process_channel_close(childIn);
    if (options.pipeOutput) kal_process_channel_close(childOut);
    if (options.pipeError) kal_process_channel_close(childErr);
    closeWork();
    if (result != kal_ok) {
        if (state->inputOpen) kal_process_channel_close(state->input);
        if (state->outputOpen) kal_process_channel_close(state->output);
        if (state->errorOpen) kal_process_channel_close(state->error);
        return base::fail("spawn-failed", std::format("cannot start {}: {}", options.program, describe(result)));
    }

    Process process;
    process.state_ = std::move(state);
    return process;
}

base::Result<void> Process::write(std::string_view bytes) {
    if (!state_ || !state_->inputOpen) return base::fail("process-write", "input is not open");
    std::lock_guard lock { state_->writeMutex };
    std::size_t done { 0 };
    while (done < bytes.size()) {
        const kal_intptr written { kal_stream_write(state_->input, bytes.data() + done, bytes.size() - done) };
        if (written <= 0) {
            return base::fail("process-write", std::format("write failed: {}", describe(static_cast<int>(-written))));
        }
        done += static_cast<std::size_t>(written);
    }
    return {};
}

namespace {

base::Result<std::string> read_stream(kal_stream stream, bool open) {
    if (!open) return std::string {};
    std::array<char, 16384> buffer {};
    const kal_intptr got { kal_stream_read(stream, buffer.data(), buffer.size()) };
    if (got < 0) {
        if (static_cast<int>(-got) == kal_err_closed) return std::string {};
        return base::fail("process-read", std::format("read failed: {}", describe(static_cast<int>(-got))));
    }
    return std::string { buffer.data(), static_cast<std::size_t>(got) };
}

} // namespace

base::Result<std::string> Process::read_output() {
    if (!state_) return std::string {};
    return read_stream(state_->output, state_->outputOpen);
}

base::Result<std::string> Process::read_error() {
    if (!state_) return std::string {};
    return read_stream(state_->error, state_->errorOpen);
}

void Process::close_input() {
    if (!state_) return;
    std::lock_guard lock { state_->writeMutex };
    if (state_->inputOpen) {
        kal_process_channel_close(state_->input);
        state_->inputOpen = false;
    }
}

base::Result<int> Process::wait() {
    if (!state_) return base::fail("process-wait", "no process");
    std::lock_guard lock { state_->waitMutex };
    if (state_->exited) return state_->exitCode;
    int status { 0 };
    int terminated { 0 };
    const int result { kal_process_wait(state_->handle, &status, &terminated) };
    if (result != kal_ok) return base::fail("process-wait", describe(result));
    state_->exited = true;
    state_->exitCode = terminated != 0 ? -1 : status;
    return state_->exitCode;
}

base::Result<std::optional<int>> Process::wait_for(std::chrono::milliseconds timeout) {
    if (!state_) return base::fail("process-wait", "no process");
    std::lock_guard lock { state_->waitMutex };
    if (state_->exited) return std::optional<int> { state_->exitCode };
    int status { 0 };
    int terminated { 0 };
    const auto nanoseconds = std::max<std::int64_t>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count());
    const int result { kal_timeout_wait_process(state_->handle, static_cast<kal_u64>(nanoseconds), &status, &terminated) };
    if (result == kal_err_again) return std::optional<int> {};
    if (result != kal_ok) return base::fail("process-wait", describe(result));
    state_->exited = true;
    state_->exitCode = terminated != 0 ? -1 : status;
    return std::optional<int> { state_->exitCode };
}

void Process::terminate() {
    if (!state_) return;
    std::lock_guard lock { state_->waitMutex };
    if (!state_->exited) kal_process_terminate(state_->handle);
}

base::Result<RunResult> run(SpawnOptions options, std::chrono::milliseconds timeout) {
    options.pipeInput = true;
    options.pipeOutput = true;
    options.pipeError = true;
    auto process = Process::spawn(options);
    if (!process) return std::unexpected { process.error() };
    process->close_input();

    RunResult result;
    std::jthread outputReader { [&] {
        while (true) {
            auto chunk = process->read_output();
            if (!chunk || chunk->empty()) break;
            result.output += *chunk;
        }
    } };
    std::jthread errorReader { [&] {
        while (true) {
            auto chunk = process->read_error();
            if (!chunk || chunk->empty()) break;
            result.error += *chunk;
        }
    } };

    auto waited = process->wait_for(timeout);
    if (!waited) return std::unexpected { waited.error() };
    if (!waited->has_value()) {
        result.timedOut = true;
        process->terminate();
        auto code = process->wait();
        result.exitCode = code.value_or(-1);
    } else {
        result.exitCode = **waited;
    }
    outputReader.join();
    errorReader.join();
    return result;
}

} // namespace lspmcpp::platform
