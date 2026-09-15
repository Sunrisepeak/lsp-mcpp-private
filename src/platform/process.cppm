// Child processes over openkal.process: start a program, talk to it through
// pipes, wait for it with or without a bound, and end it.
export module mcppls.platform.process;

import std;
import mcppls.base.error;

export namespace mcppls::platform {

struct SpawnOptions {
    std::string program;                                   // absolute path of the executable
    std::vector<std::string> arguments;                    // without argv[0]; argv[0] is `program`
    std::string workDirectory;                             // absolute; empty means the current directory
    std::optional<std::vector<std::string>> environment;   // "NAME=value"; nullopt inherits this process's
    bool pipeInput { true };
    bool pipeOutput { true };                              // false: the child writes to this process's stderr,
                                                           // because this process's stdout carries the protocol
    bool pipeError { false };                              // false: the child writes to this process's stderr
};

class Process {
public:
    Process();
    Process(Process&& other) noexcept;
    Process& operator=(Process&& other) noexcept;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    ~Process();                                            // terminates and reaps a child still running

public:
    static base::Result<Process> spawn(const SpawnOptions& options);

    bool valid() const;
    base::Result<void> write(std::string_view bytes);      // thread-safe
    // Blocks until bytes arrive. An empty string means the stream ended.
    base::Result<std::string> read_output();
    base::Result<std::string> read_error();
    void close_input();
    base::Result<int> wait();
    // nullopt when the bound expired and the child is still running.
    base::Result<std::optional<int>> wait_for(std::chrono::milliseconds timeout);
    void terminate();

private:
    struct State;
    std::unique_ptr<State> state_;
};

struct RunResult {
    int exitCode { -1 };
    std::string output;
    std::string error;
    bool timedOut { false };
};

// Runs a program to completion with its standard output and error captured.
base::Result<RunResult> run(SpawnOptions options, std::chrono::milliseconds timeout);

// The openkal preopened directory that contains an absolute path, and the path
// beneath it. Exposed for tests and for callers that need to explain a failure.
struct PreopenMatch {
    std::string preopenName;
    std::string remainder;
};
std::optional<PreopenMatch> match_preopen(std::string_view absolutePath);

} // namespace mcppls::platform
