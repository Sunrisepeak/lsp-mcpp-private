# lsp-mcpp v1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the whole design of `2026-09-13-cxx-modules-unified-lsp-design.md` (v0.4) as a working, CI-verified lsp-mcpp v1 in one pull request.

**Architecture:** One mcpp package written only in C++23 modules produces three executables that share `src/` modules: the server `lsp-mcpp`, the conformance runner `lsp-mcpp-conformance` and the protocol generator `lsp-mcpp-lspgen`. The platform layer sits on openkal (`openkal.process` for child processes and pipes; the C++ standard library for threads, files and time), and platform facts come from a per-target `lspmcpp.os` package selected through `cfg` path dependencies and consumed with `if constexpr`. The server normalizes any build (mcpp, CMake, compile_commands.json, or nothing) into an engine database for a pinned clangd 23.1, adds module-level features from its own syntactic index, and ships with a thin VS Code extension that bundles clangd and the `lsp-mcpp-kit` semantic kits.

**Tech Stack:** mcpp (2026.9.14.1+), LLVM 22.1.8 via `openkal-llvm-runtime` 0.9.2, mcpp-index modular libraries `nlohmann.json` 3.12.0, `mcpplibs.cmdline` 0.0.2, `boost.ut` 2.3.1, openkal 0.12, clangd 23.1.0, TypeScript + vscode-languageclient + @vscode/test-electron, Python 3 for packaging scripts, GitHub Actions on ubuntu-24.04, macos-14 (arm64), windows-2022.

**Spec:** `.agents/docs/2026-09-13-cxx-modules-unified-lsp-design.md`, `.agents/docs/2026-09-13-cxx-module-build-database-ide-profile-spec.md`, evidence in `.agents/docs/2026-09-13-cxx-modules-lsp-experiments.md`.

## Global Constraints

- C++ code is C++23 modules only: no header files, no `#include`, no `#define`, no `#if`. Every module is an interface `.cppm` plus, when it has non-trivial bodies, an implementation unit `.cpp` beginning with `module <name>;`.
- Platform differences use `if constexpr` on constants from `import lspmcpp.os;`, never the preprocessor.
- Naming follows mcpp-style-ref: types `PascalCase`, functions `snake_case`, data members `camelCase`, private members end with `_`, constants `UPPER_SNAKE`, namespaces lowercase, `{}` initialisation with spaces, errors via `std::expected` / `std::optional`.
- Module names mirror directories: `src/<dir>/<file>.cppm` exports `lspmcpp.<dir>.<file>`.
- The semantic kit is named `lsp-mcpp-kit`. First-batch platforms: linux-x64, win32-x64, darwin-arm64. macOS has no x86_64 build.
- Semantic engine is clangd 23.1.x started with `--experimental-modules-support --use-dirty-headers --compile-commands-dir=<cache>`. Engine databases and caches live under the user cache directory, never inside the user project.
- Without a compiler the default standard library is libc++ matching clangd 23.1.
- Specs live in `specs/`; design and process documents live in `.agents/docs/`. Everything is Apache-2.0.
- VS Code extension identity: publisher `mcpp-community`, id `lsp-mcpp`, display name `C++ Modules`; 4 commands, 4 optional settings, language status item only, no walkthrough, `untrustedWorkspaces: limited`.
- Delivery: private repo `Sunrisepeak/lsp-mcpp-private`, `main` holds only the initial docs commit, all implementation lands in one PR from `feat/lsp-mcpp-v1`, every CI job green.

---

## File Structure

```
lsp-mcpp-private/
├── README.md, LICENSE, .gitignore
├── mcpp.toml                      package lsp-mcpp; 3 bin targets; openkal-llvm-runtime; cfg path deps on os/*
├── os/linux|macos|windows/        one tiny package each, all exporting module lspmcpp.os
│   ├── mcpp.toml
│   └── src/os.cppm
├── src/
│   ├── main.cpp                   lsp-mcpp CLI entry: serve | check | model | version
│   ├── base/                      error.cppm/.cpp  log.cppm/.cpp  text.cppm/.cpp  uri.cppm/.cpp
│   │   ├── platform/                  process  stdio  fs  env  dirs            (.cppm/.cpp each)
│   ├── lsp/                       jsonrpc.cppm/.cpp  protocol.cppm/.cpp (generated)  client.cppm/.cpp
│   ├── spec/                      database  metadata  kit  discovery        (.cppm/.cpp each)
│   ├── project/                   scan  compdb  detect  mcpp  cmake  infer  model
│   ├── toolchain/                 probe  discover
│   ├── normalize/                 gnu  msvc  plan
│   ├── engine/                    clangd.cppm/.cpp
│   ├── index/                     modules.cppm/.cpp
│   ├── server/                    router  session  cli
│   └── tools/                     conformance.cpp (bin main)  lspgen.cpp (bin main)
├── tests/                         one executable per file, discovered by `mcpp test`
├── conformance/
│   ├── fixtures/<name>/           project + scenario.json
│   └── README.md
├── specs/                         README, s1..s4 markdown, schema/*.json, CHANGELOG.md
├── editors/vscode/                package.json, tsconfig.json, src/*.ts, test/*.ts, .vscodeignore
├── packaging/
│   ├── payload.lock.json
│   ├── kits/                      kit.json templates for linux-x64, win32-x64, darwin-arm64
│   └── scripts/                   fetch_clangd.py  trim_clangd.py  build_kit.py  assemble_payload.py
└── .github/workflows/             ci.yml  release.yml
```

Responsibilities are one per module; later tasks only rely on the `Produces` blocks of earlier tasks.

---

## Milestone 0 — Repository, skeleton and CI backbone

### Task 0.1: Create the repository with the docs-only main branch

**Files:** Create `README.md` (rewrite), `LICENSE`, `.gitignore`; keep `.agents/docs/**`; remove `hello/` (its content becomes a conformance fixture in Task 5.1).

- [ ] `git init -b main`, add README, LICENSE (Apache-2.0 text), `.gitignore` (`target/`, `node_modules/`, `editors/vscode/out/`, `editors/vscode/payload/`, `*.vsix`, `.lsp-mcpp/`, `__pycache__/`), `.agents/docs/`.
- [ ] `gh repo create Sunrisepeak/lsp-mcpp-private --private --source . --push`.
- [ ] `git switch -c feat/lsp-mcpp-v1`.

### Task 0.2: Package skeleton, `lspmcpp.os`, first CI

**Files:** Create `mcpp.toml`, `os/{linux,macos,windows}/{mcpp.toml,src/os.cppm}`, `src/main.cpp` (prints version), `src/tools/conformance.cpp`, `src/tools/lspgen.cpp` (stubs returning 0), `tests/test_os.cpp`, `.github/workflows/ci.yml`.

**Produces:**

```cpp
export module lspmcpp.os;
import std;
export namespace lspmcpp::os {
enum class Family { linux, macos, windows };
inline constexpr Family FAMILY { /* per package */ };
inline constexpr std::string_view EXECUTABLE_SUFFIX { /* "" or ".exe" */ };
inline constexpr char PATH_LIST_SEPARATOR { /* ':' or ';' */ };
inline constexpr std::string_view VSCODE_TARGET { /* "linux-x64" | "darwin-arm64" | "win32-x64" */ };
}
```

- [ ] Test `tests/test_os.cpp`: `if constexpr (FAMILY == Family::windows)` requires `EXECUTABLE_SUFFIX == ".exe"` and `PATH_LIST_SEPARATOR == ';'`, else `""` and `':'`.
- [ ] `mcpp build && mcpp test` locally; cross `mcpp build --target x86_64-windows` and `--target aarch64-macos`.
- [ ] CI `ci.yml` job `build-test` on the three runners: install xlings and pinned mcpp (retry loop as in mcpplibs/openkal), `mcpp build`, `mcpp test`; job `cross-build` on ubuntu builds the Windows and macOS targets.
- [ ] Push; CI green before Milestone 1.

---

## Milestone 1 — Foundations

### Task 1.1: `lspmcpp.base.*`

**Produces:**

```cpp
export module lspmcpp.base.error;   // struct Error { std::string code; std::string message; };
                                    // template<class T> using Result = std::expected<T, Error>;
                                    // Error make_error(std::string_view code, std::string message);
export module lspmcpp.base.log;     // enum class Level { debug, info, warning, error };
                                    // void set_level(Level); void write(Level, std::string_view); (to stderr, thread-safe)
export module lspmcpp.base.text;    // std::vector<std::string_view> split_lines(std::string_view);
                                    // std::string_view trim(std::string_view); bool starts_with_word(...);
                                    // struct Position { int line; int character; };  (UTF-16 columns)
                                    // Position position_at(std::string_view text, std::size_t byteOffset);
                                    // std::size_t offset_at(std::string_view text, Position);
export module lspmcpp.base.uri;     // Result<std::string> uri_to_path(std::string_view uri);
                                    // std::string path_to_uri(std::string_view path);
                                    // std::string normalize_path(std::string_view path);  ('/' separators, '.'/'..' folded, drive letter upper-case)
```

- [ ] Tests `tests/test_text.cpp`, `tests/test_uri.cpp`: UTF-16 columns for `"a😀b"` (byte 5 → character 3), `file:///home/u/a%20b.cpp` ↔ `/home/u/a b.cpp`, Windows form `file:///c%3A/Users/x/m.cppm` ↔ `C:/Users/x/m.cppm` checked under `if constexpr (FAMILY == Family::windows)` and as pure string rules on every platform.

### Task 1.2: JSON, command line and tests from the mcpp ecosystem

JSON is not written by hand. Design D24 and §12.8: general-purpose libraries come from mcpp-index modular packages, never compat packages.

**Produces:** dependencies in `mcpp.toml`:

```toml
[dependencies]
cmdline = "0.0.2"                 # import mcpplibs.cmdline;

[dependencies.nlohmann]
json = "3.12.0"                   # import nlohmann.json;

[dev-dependencies.boost-ext]
ut = "2.3.1"                      # import boost.ut;
```

- [ ] Every module that handles JSON uses `nlohmann::json` (`nlohmann::ordered_json` where key order is visible to users, e.g. S1 output); errors from `parse` are caught at the module boundary and converted to `base::Result`.
- [ ] Unit tests use `boost.ut`.
- [ ] Cross-building nlohmann.json for `x86_64-windows` and `aarch64-macos` needs openkal-llvm-runtime 0.9.2 (design §12.9 K1). Until mcpp-index carries 0.9.2 the dependency points at the fix branch.

### Task 1.3: Platform layer

**Produces:**

```cpp
export module lspmcpp.platform.process;
export namespace lspmcpp::platform {
struct SpawnOptions { std::string program; std::vector<std::string> arguments; std::string workDirectory;
                      bool pipeInput { true }; bool pipeOutput { true }; bool pipeError { false }; };
class Process {   // move-only; destructor terminates and reaps a running child
public:
    static base::Result<Process> spawn(const SpawnOptions& options);
    base::Result<void> write(std::string_view bytes);
    base::Result<std::string> read_output();    // "" means end of stream
    base::Result<std::string> read_error();
    void close_input();
    base::Result<int> wait();
    void terminate();
};
struct RunResult { int exitCode; std::string output; std::string error; bool timedOut; };
base::Result<RunResult> run(const SpawnOptions& options, std::chrono::milliseconds timeout);
}
export module lspmcpp.platform.stdio;   // base::Result<std::string> read_input(); base::Result<void> write_output(std::string_view); (raw bytes over openkal.stream)
export module lspmcpp.platform.env;     // std::optional<std::string> get(std::string_view); std::optional<std::string> find_executable(std::string_view name);
export module lspmcpp.platform.fs;      // base::Result<std::string> read_file(std::string_view); base::Result<void> write_file_atomic(std::string_view, std::string_view);
                                        // std::vector<std::string> list_files(std::string_view root, std::span<const std::string_view> extensions);  (skips .git target build node_modules .lsp-mcpp and dot-dirs)
export module lspmcpp.platform.dirs;    // std::string cache_dir();  (XDG_CACHE_HOME|~/.cache, ~/Library/Caches, %LOCALAPPDATA%) + "/lsp-mcpp"
```

- [ ] Program paths are resolved against openkal preopens by longest prefix (spike-proven); input channel gives the child the reading end.
- [ ] Tests `tests/test_process.cpp`: echo round-trip through a child (`/bin/sh -c cat` on Unix, `cmd.exe /c more` selected with `if constexpr` on Windows), exit status propagation, `run` timeout kills a sleeping child; `tests/test_fs.cpp`: atomic write then read, list_files skip rules; `tests/test_env.cpp`: `find_executable` finds the test's own runner shell.

---

## Milestone 2 — Protocol and specifications

### Task 2.1: LSP JSON-RPC and generated protocol constants

**Produces:**

```cpp
export module lspmcpp.lsp.jsonrpc;
export namespace lspmcpp::lsp {
class FrameReader { public: void feed(std::string_view bytes); std::optional<base::Result<json::Value>> next(); };
std::string encode_frame(const json::Value& message);
enum class Kind { request, notification, response, invalid };
Kind kind_of(const json::Value& message);
json::Value make_request(json::Value id, std::string_view method, json::Value params);
json::Value make_notification(std::string_view method, json::Value params);
json::Value make_result(json::Value id, json::Value result);
json::Value make_error(json::Value id, int code, std::string_view message);
}
export module lspmcpp.lsp.protocol;     // generated: namespace lspmcpp::lsp::method { inline constexpr std::string_view TEXT_DOCUMENT_DEFINITION {...}; ... }
                                        // struct MethodInfo { std::string_view name; bool isRequest; bool clientToServer; };
                                        // const MethodInfo* find_method(std::string_view name);   (table in protocol.cpp)
export module lspmcpp.lsp.client;       // class Endpoint: owns a Process, reader thread, request ids, callbacks (used by engine and conformance runner)
```

- [ ] `src/tools/lspgen.cpp` reads `tools/lspgen/metaModel-3.18.json` (vendored, MIT notice kept) and writes `src/lsp/protocol.cppm` and `.cpp`; CI regenerates and runs `git diff --exit-code src/lsp`.
- [ ] Tests `tests/test_jsonrpc.cpp`: split frames across feeds, two frames in one feed, bad header error; `find_method("textDocument/definition")->isRequest == true`.

### Task 2.2: S1 database, P3286 metadata, S4 kit, S2 discovery

**Produces:**

```cpp
export module lspmcpp.spec.database;
export namespace lspmcpp::spec {
enum class Family { gcc, clang, msvc, clang_cl, other };
enum class Role { module_interface, module_partition_interface, module_partition_implementation, module_implementation, non_module, unknown };
struct Stdlib { std::string name; std::string version; std::string moduleMetadata; };
struct Toolchain { Family family; std::string version; std::string buildId; std::string driver; std::string target;
                   std::string sysroot; std::optional<Stdlib> stdlib; std::vector<std::string> configFiles; };
struct TranslationUnit { std::string source; std::string workDirectory; std::vector<std::string> arguments;
                         std::vector<std::string> localArguments; std::string object; bool isPrivate { false };
                         std::vector<std::pair<std::string, std::string>> providedModules; std::vector<std::string> requiredModules;
                         Role role { Role::unknown }; };   // enumerators snake_case, members camelCase
struct Set { std::string name; std::string familyName; std::vector<std::string> visibleSets; std::vector<std::string> baselineArguments;
             std::string toolchain; std::string configuration; std::string kind; std::vector<std::string> moduleMetadata;
             std::vector<TranslationUnit> units; };
struct Database { int version { 1 }; int revision { 0 }; std::string profileVersion { "0.2.0" };
                  std::vector<std::pair<std::string, Toolchain>> toolchains; std::vector<Set> sets; };
base::Result<Database> from_json(const json::Value& document);
json::Value to_json(const Database& database);
int conformance_level(const Database& database);                  // 1..3
json::Value to_compile_commands(const Database& database, std::string_view setName);
enum class ResolvedFrom { set, visible_set, module_metadata, stdlib, unresolved, ambiguous };
struct Resolution { ResolvedFrom from; std::vector<std::string> providers; };   // absolute source paths
Resolution resolve_module(const Database& database, std::size_t setIndex, std::string_view moduleName,
                          const std::function<std::vector<std::pair<std::string, std::string>>(std::string_view)>& metadataReader);
}
export module lspmcpp.spec.metadata;    // struct ModuleEntry { std::string logicalName; std::string source; bool isStdLibrary; std::vector<std::string> systemIncludeDirectories; };
                                        // base::Result<std::vector<ModuleEntry>> read_module_metadata(std::string_view manifestPath);  (paths made absolute)
export module lspmcpp.spec.kit;         // struct Kit { std::string root, name, target, stdlibName, stdlibVersion, moduleMetadata; std::vector<std::string> systemIncludeDirectories, arguments; std::optional<std::string> sysroot; bool requiresMacosSdk; };
                                        // base::Result<Kit> load_kit(std::string_view kitRoot);
export module lspmcpp.spec.discovery;   // struct DiscoveryResult { std::string database; std::vector<std::string> watch; };
                                        // base::Result<DiscoveryResult> run_discovery(std::span<const std::string> command, const json::Value& request, std::chrono::milliseconds timeout);
```

- [ ] Tests `tests/test_database.cpp` (spec §15 example parses, level 3, round-trips, resolution order set → visible set → metadata → stdlib, ambiguity), `tests/test_metadata.cpp` (libc++ and libstdc++ manifests from experiment E3 as inline JSON written to a temp dir), `tests/test_kit.cpp` (E16 kit.json), `tests/test_discovery.cpp` (a shell script producer emitting progress then finished).

### Task 2.3: `specs/`

**Files:** `specs/README.md`, `specs/s1-build-database.md`, `specs/s2-discovery.md`, `specs/s3-lsp-extensions.md`, `specs/s4-semantic-kit.md`, `specs/schema/s1-build-database.schema.json`, `specs/schema/s2-discovery.schema.json`, `specs/schema/s4-kit.schema.json`, `specs/CHANGELOG.md`.

- [ ] Port S1/S2 from the spec draft and S3/S4 from design §8–§9; kit name `lsp-mcpp-kit`.
- [ ] `tests/test_schemas.cpp` parses every schema and every JSON example block extracted into `specs/examples/*.json`.

---

## Milestone 3 — Project model, toolchains, normalization

### Task 3.1: `lspmcpp.project.scan`

**Produces:**

```cpp
export module lspmcpp.project.scan;
export namespace lspmcpp::project {
struct Range { base::Position start; base::Position end; };
struct ModuleDeclaration { std::string module; std::string partition; bool isExported { false }; Range nameRange; };
struct ImportDeclaration { std::string module; std::string partition; bool isExported { false }; bool isHeaderUnit { false }; Range nameRange; };
struct ScanResult { std::optional<ModuleDeclaration> declaration; std::vector<ImportDeclaration> imports; bool uncertain { false }; };
ScanResult scan_source(std::string_view text);
spec::Role role_of(const ScanResult& result);
std::string provided_name(const ScanResult& result);         // "m" or "m:p", empty for non-module units
std::vector<std::string> required_names(const ScanResult& result);   // partitions expanded to "m:p"
}
```

- [ ] Tests `tests/test_scan.cpp`: the fixture sources, comments and strings containing `import x;`, raw strings, `export import :detail;` resolving to `hello.greet:detail`, header units flagged, `#if`-guarded import sets `uncertain`, UTF-16 ranges.

### Task 3.2: Compile databases and toolchain probing

**Produces:**

```cpp
export module lspmcpp.project.compdb;   // struct CompileCommand { std::string directory, file, output; std::vector<std::string> arguments; };
                                        // base::Result<std::vector<CompileCommand>> read_compile_commands(std::string_view path);
                                        // std::vector<std::string> split_command(std::string_view command, bool windowsRules);
                                        // std::vector<std::string> expand_response_files(std::span<const std::string> arguments, std::string_view directory);
export module lspmcpp.toolchain.probe;  // spec::Family classify_driver(std::string_view driverPath);
                                        // using Runner = std::function<base::Result<std::string>(std::span<const std::string> argv)>;
                                        // base::Result<spec::Toolchain> probe_toolchain(std::string_view driverPath, const Runner& runner);
                                        // Runner process_runner();  (platform::run with 20 s timeout, cached by path+size+mtime)
export module lspmcpp.toolchain.discover; // std::vector<std::string> discover_compilers();  (PATH, xlings/mcpp payloads, Homebrew LLVM, vswhere)
```

- [ ] Tests `tests/test_compdb.cpp` (arguments and command forms, Windows quoting, `@modmap` expansion), `tests/test_probe.cpp` with a fake `Runner` replaying outputs recorded in E5/E10/E14 for gcc 16, mingw gcc 16, clang 22 (libc++ manifest) and a cl.exe path layout.

### Task 3.3: Normalizers

**Produces:**

```cpp
export module lspmcpp.normalize.gnu;
export namespace lspmcpp::normalize {
struct UnitInput { const spec::TranslationUnit* unit; const spec::Toolchain* toolchain; bool importable; std::string engineDriver; };
std::vector<std::string> translate_gnu(const UnitInput& input);
}
export module lspmcpp.normalize.msvc;   // std::vector<std::string> translate_msvc(const UnitInput& input);   (engine driver clang-cl, --driver-mode=cl)
export module lspmcpp.normalize.plan;
export namespace lspmcpp::normalize {
struct EngineEntry { std::string directory; std::string file; std::vector<std::string> arguments; };
struct PlanIssue { std::string code; std::string message; std::string file; };
struct EnginePlan { std::vector<EngineEntry> entries; std::vector<PlanIssue> issues; std::string contextSet; };
struct PlanInput { const spec::Database* database; std::string contextSet; std::optional<spec::Kit> kit;
                   std::function<project::ScanResult(std::string_view path)> scanner; };
EnginePlan plan_engine(const PlanInput& input);
base::Result<void> write_engine_database(std::string_view directory, const EnginePlan& plan);
}
```

- [ ] Rules exactly as design §14.4 (GCC Linux `--gcc-install-dir`, MinGW `--sysroot`, Clang strip BMI flags and expand `@modmap`, MSVC strip `/reference` `/ifcOutput` `/ifcSearchDir` `/interface` `/internalPartition` `/headerUnit*` `/scanDependencies` `/sourceDependencies*`, kits from `kit.json`).
- [ ] std / std.compat units injected once per options group from the toolchain manifest or kit; imports without providers produce `unresolved-module` issues and the importing unit is still written but its unresolvable module unit is not.
- [ ] Tests `tests/test_normalize_gnu.cpp`, `tests/test_normalize_msvc.cpp`, `tests/test_plan.cpp` (visible-set filtering, std injection count, unresolved issue, atomic write).

### Task 3.4: Project detection and providers

**Produces:**

```cpp
export module lspmcpp.project.detect;   // enum class Kind { mcpp, cmake, compile_commands, inferred };
                                        // struct Detection { Kind kind; std::string root; std::string buildDirectory; std::string compileCommands; };
                                        // Detection detect_project(std::string_view root);
export module lspmcpp.project.infer;    // spec::Database infer_database(std::string_view root, const std::optional<spec::Toolchain>& toolchain, const std::optional<spec::Kit>& kit);
export module lspmcpp.project.mcpp;     // base::Result<spec::Database> load_mcpp(std::string_view root, const toolchain::Runner&, bool trusted);
                                        //   emit build-database when available, else `mcpp build --configure-only` + compile_commands.json + scan + probe
export module lspmcpp.project.cmake;    // base::Result<spec::Database> load_cmake(const Detection&, std::string_view cacheDir, const toolchain::Runner&, bool trusted);
                                        //   build_database.json → compile_commands.json (+modmap) → private configure
export module lspmcpp.project.model;    // struct ProjectModel { spec::Database database; Kind source; int level; std::vector<std::string> watch; std::vector<std::string> issues; };
                                        // ProjectModel load_project(std::string_view root, const LoadOptions&);  (never fails: falls back to inferred)
```

- [ ] Tests `tests/test_detect.cpp`, `tests/test_infer.cpp` (fixture tree → roles, provides, requires), `tests/test_model_compdb.cpp` (a recorded GCC compile_commands.json from E1 converts to a level-2 database with gcc toolchain).

---

## Milestone 4 — Engine, index, server

### Task 4.1: `lspmcpp.engine.clangd`

**Produces:**

```cpp
export module lspmcpp.engine.clangd;
export namespace lspmcpp::engine {
struct ClangdConfig { std::string executable; std::string databaseDirectory; std::vector<std::string> extraArguments;
                      std::chrono::milliseconds requestTimeout { 8000 }; };
class Clangd {
public:
    using MessageHandler = std::function<void(json::Value)>;       // notifications and server requests from clangd
    base::Result<void> start(const ClangdConfig& config, MessageHandler onMessage, std::function<void(int)> onExit);
    void request(std::string_view method, json::Value params, std::function<void(base::Result<json::Value>)> onResult);
    void notify(std::string_view method, json::Value params);
    void stop();
    bool running() const;
};
}
```

- [ ] Timeouts answer `onResult` with an `engine-timeout` error; after 3 timeouts for one URI the session restarts the engine and replays open documents.
- [ ] Test `tests/test_clangd_engine.cpp` runs only when `LSPMCPP_CLANGD` is set (runtime check): initialize, open fixture file, definition returns a location.

### Task 4.2: `lspmcpp.index.modules`

**Produces:**

```cpp
export module lspmcpp.index.modules;
export namespace lspmcpp::index {
struct Location { std::string path; project::Range range; };
struct ModuleUnit { std::string path; std::string name; spec::Role role; project::Range declaration; };
class ModuleIndex {
public:
    void update(std::string_view path, std::string_view text);
    void remove(std::string_view path);
    std::vector<ModuleUnit> providers(std::string_view moduleName) const;
    std::optional<Location> definition(std::string_view path, base::Position position) const;
    std::optional<json::Value> completion(std::string_view path, std::string_view text, base::Position position) const;
    std::optional<std::string> hover(std::string_view path, base::Position position) const;
    json::Value diagnostics(std::string_view path) const;          // LSP Diagnostic[]; source "lsp-mcpp"
    json::Value document_symbols(std::string_view path) const;
    json::Value workspace_symbols(std::string_view query) const;
    json::Value graph() const;                                     // S3 cxxModules/graph result
};
}
```

- [ ] Tests `tests/test_index.cpp`: definition on `hello.greet` in `import hello.greet;` → greet.cppm declaration; `export import :detail;` → detail.cppm; completion after `import hel` lists `hello.greet`; unresolved import diagnostic; partition import from another module flagged.

### Task 4.3: `lspmcpp.server.router` and `lspmcpp.server.session`

**Produces:**

```cpp
export module lspmcpp.server.router;    // enum class Route { local, engine, merged };
                                        // Route route_for(std::string_view method, const json::Value& params, const index::ModuleIndex& idx, std::string_view text);
export module lspmcpp.server.session;   // struct SessionOptions { std::string payload; std::string clangd; bool trusted; };
                                        // int run_session(const SessionOptions& options);   (stdio LSP server; returns exit code)
export module lspmcpp.server.cli;       // int run_cli(std::span<const std::string_view> arguments);
```

- [ ] Session event loop: one queue fed by stdin reader, engine reader, workers and timers; states starting → loading → preparing → ready / degraded / error; `cxxModules/status` on every change when the client advertised `experimental.cxxModules.status`; `cxxModules/graph`, `moduleInfo`, `contexts`, `setContext`; registers `workspace/didChangeWatchedFiles` for `mcpp.toml`, `CMakeLists.txt`, `compile_commands.json`, `**/*.cppm`.
- [ ] CLI: `lsp-mcpp` / `lsp-mcpp serve [--payload DIR] [--clangd PATH]`, `lsp-mcpp check <file>`, `lsp-mcpp model [--root DIR] [--export compile-commands]`, `lsp-mcpp version`.
- [ ] Tests `tests/test_router.cpp`, `tests/test_cli.cpp` (`model` on the inferred fixture prints a valid level-2 S1 document).

---

## Milestone 5 — Conformance

### Task 5.1: Fixtures and runner

**Files:** `conformance/fixtures/{mcpp-gcc,mcpp-llvm,mingw,inferred,cmake-clang,msvc}/`, each with `scenario.json`; `src/tools/conformance.cpp`.

scenario.json:

```json
{
  "open": "src/main.cpp",
  "checks": [
    { "id": "C1", "kind": "diagnostics-empty", "file": "src/main.cpp" },
    { "id": "C2", "kind": "definition", "file": "src/main.cpp", "at": [4, 31], "expect": "src/greet/greet.cppm" },
    { "id": "C3", "kind": "hover-contains", "file": "src/main.cpp", "at": [4, 31], "expect": "greet" },
    { "id": "C4", "kind": "definition-any", "file": "src/main.cpp", "at": [4, 10] },
    { "id": "C5", "kind": "definition", "file": "src/main.cpp", "at": [1, 9], "expect": "src/greet/greet.cppm" },
    { "id": "C6", "kind": "completion-contains", "file": "src/main.cpp", "insert": [5, "    hello::"], "at": [5, 11], "expect": "greet" },
    { "id": "C7", "kind": "unsaved-edit-visible", "edit": "src/greet/greet.cppm", "expect": "greet2" },
    { "id": "C9", "kind": "references-span", "file": "src/main.cpp", "at": [4, 31], "expect": ["src/main.cpp", "src/greet/greet.cppm"] }
  ]
}
```

- [ ] Runner: `lsp-mcpp-conformance --server <lsp-mcpp> --payload <dir> --fixture <dir> [--clangd <path>]` spawns the server over openkal, advertises `experimental.cxxModules`, waits for `ready`, executes checks, prints one line per check and exits non-zero on failure. All nine checks including C5 must pass.

---

## Milestone 6 — Payload, VS Code extension, packaging

### Task 6.1: Payload scripts

**Files:** `packaging/payload.lock.json`, `packaging/scripts/{fetch_clangd.py,trim_clangd.py,build_kit.py,assemble_payload.py}`, `packaging/kits/{linux-x64,win32-x64,darwin-arm64}.kit.json`.

- [ ] clangd 23.1.0 from `github.com/clangd/clangd/releases`: keep `bin/clangd[.exe]` and `lib/clang/23/include`, strip on Linux, `lipo -thin arm64` on macOS.
- [ ] `lsp-mcpp-kit`: Linux and macOS configure libc++ 23.1.0 headers and module sources from `llvm-project-23.1.0.src` (runtimes build, install headers and modules only); Linux adds glibc and kernel headers from the runner; Windows extracts llvm-mingw 20260826 `generic-w64-mingw32/include`, `share/libc++/v1`, `x86_64-w64-mingw32/lib/libc++.modules.json`.
- [ ] Layout `payload/bin/lsp-mcpp[.exe]`, `payload/clangd/...`, `payload/kit/kit.json`.

### Task 6.2: VS Code extension

**Files:** `editors/vscode/{package.json,tsconfig.json,.vscodeignore,src/extension.ts,src/status.ts,src/conflicts.ts,src/commands.ts,src/payload.ts,test/runTest.ts,test/suite/index.ts,test/suite/modules.test.ts}`.

- [ ] Language client over stdio with `--payload <extension>/payload`; language status item driven by `cxxModules/status`; commands select context, show module graph, restart, show logs; settings `lspMcpp.compiler`, `lspMcpp.semanticKit`, `lspMcpp.detectConflicts`, `lspMcpp.trace.server`.
- [ ] E2E test on the `inferred` fixture: definition, module-name definition, completion, hover, references, zero notifications from this extension.
- [ ] `vsce package --target <platform>` produces the VSIX.

---

## Milestone 7 — CI, release, external PRs

### Task 7.1: `ci.yml`

Jobs, all required:

| Job | Runners | Content |
|---|---|---|
| `build-test` | ubuntu-24.04, macos-14, windows-2022 | mcpp build, mcpp test, lspgen freshness |
| `cross-build` | ubuntu-24.04 | `mcpp build --target x86_64-windows` and `aarch64-macos` for all three executables |
| `conformance` | ubuntu-24.04 (mcpp-gcc, mcpp-llvm, mingw, inferred, cmake-clang), macos-14 (inferred, mcpp-llvm), windows-2022 (inferred, msvc) | build payload, run runner |
| `vscode-e2e` | ubuntu-24.04 (xvfb), macos-14, windows-2022 | assemble payload, npm ci, npm test, vsce package |

### Task 7.2: `release.yml`

- [ ] Tag `v*`: build payload per platform, package VSIX per platform, attach to GitHub Release; publishing steps to Marketplace, Open VSX, xlings-res and the xim-pkgindex bump run only when their secrets exist.

### Task 7.3: External PRs (only when needed by lsp-mcpp)

- [ ] `mcpp`: `mcpp emit build-database` (S1 level 3, S2 JSONL) — lsp-mcpp keeps the compile_commands fallback so the main PR does not depend on it.
- [ ] `xim-pkgindex`: `lsp-mcpp-kit` data package and `llvm-tools@23.1.0` once release artifacts exist.

---

## Execution Strategy

Tightly coupled C++ milestones (0–4) run inline in this session with a build and `mcpp test` after every task; isolated work (specs port, VS Code extension, payload scripts) runs through subagents with the interfaces above. Every milestone ends with a push and a green CI run on the PR branch before the next milestone starts.

---

## Execution Record

Delivered as Sunrisepeak/lsp-mcpp-private#1 (`feat/lsp-mcpp-v1` → `main`). The C++ source is 99 files (43 `.cppm` interfaces, 56 `.cpp` implementation units) with no header files and no preprocessor directives. All milestones were implemented, and the design document follows the implementation (§12.9, §13, §14.3, §19).

### Where execution departed from the plan

| Plan | What was done | Why |
|---|---|---|
| Task 1.2: tests on `boost.ut` 2.3.1 | An in-repo `lspmcpp.testing` harness (`testing/`), a path dev-dependency | `boost.ut` crashes clang 22.1.8 on a Windows host and every test binary linking it segfaults at startup on macOS (design §12.9 K5) |
| Tech stack: `openkal-llvm-runtime` 0.9.2 | 0.9.5 | Defects found while building and testing lsp-mcpp were fixed upstream and released through the whole pinned chain (below) |
| Payloads carry a release server | Release, after running on dev while K7 and K13 were open | Both release-only defects are fixed in 0.9.5; CI runs the unit tests in both profiles on every host |
| Task 6.1: `fetch_clangd.py`, `packaging/kits/*.kit.json` templates | `packaging/scripts/fetch.py` fetches every lock entry; `build_kit.py` writes `kit.json`; `packaging/kits/README.md` documents each kit; `xlings_artifacts.py` splits release payloads for xlings | A generated manifest cannot drift from the files it describes |
| Task 7.1: Windows conformance `inferred, msvc` | Windows runs `inferred`, `untrusted`, `mingw` and `cmake-msvc`; macOS runs `inferred`, `untrusted`, `mcpp-llvm`; Linux runs all six fixtures and `mcpp-llvm` again through a symbolic link | The MSVC fixture is a CMake project built by cl.exe in a developer environment, so it measures P7. clang-cl (P6) has no fixture |
| Task 6.2: @vscode/test-electron 2.5 | 3.1 on Node 22, with a short `--user-data-dir` | VS Code 1.110 renamed the macOS executable, and macOS limits the IPC socket path to 104 bytes |
| Task 7.3: `mcpp emit build-database` upstream | Not proposed. The mcpp provider asks for it first (S2) and falls back to `mcpp build --configure-only` | lsp-mcpp does not depend on it, as the plan allowed |
| Task 7.3: xim-pkgindex descriptors | Rendered by `xlings_artifacts.py` in the release workflow, not submitted | They need published release archives |

### Upstream changes

Every defect is recorded in design §12.9 with its root cause. Version requirements are exact, so each fix was released by every package that pins it, mirrored to GitCode, and added to mcpp-index.

| Package | PRs and releases |
|---|---|
| openkal-llvm-runtime | #17 → 0.9.2 (`_GNU_SOURCE`, K1), #18 → 0.9.3, #19 → 0.9.4, #20 → 0.9.5 (with a release-build run on each host) |
| openkal-musl | #31 → 0.13.2 (C++ `pthread_t` on Windows, K6), #32 → 0.13.3 (detached thread exit overran the shared stack, K9), #33 → 0.13.4 |
| openkal-windows | #20 → 0.7.1 (loops became C runtime calls under `-O2`, K7), #21 → 0.7.2 (inheritable channel ends, K10), #22 → 0.7.3 (argument quoting, argument narrowing and the `\\?\` prefix, K11), #23 → 0.7.4 (environment values, K12) |
| openkal-macos | #20 → 0.9.1 (a returned register declared an input; release programs faulted before `main`, K13) |
| mcpp-index | #414 to #423 |

Recorded without a fix: K2 (MinGW `mm_malloc.h`), K5 (`boost.ut`), K8 (libc++ wide strings over 32-bit musl `wcslen`), and the requirements K3 and K4.

### Verification

Every job in `.github/workflows/ci.yml` passes on the pull request:

| Job | Hosts | What it establishes |
|---|---|---|
| build and unit tests | ubuntu-24.04, macos-14, windows-2022 | 15 test programs in the dev and the release profile; the three executables start; the generated protocol is fresh |
| cross-build from Linux | x86_64-windows-gnu, aarch64-macos | one Linux host builds every platform's executables |
| payload | linux-x64, win32-x64, darwin-arm64 | release server, trimmed clangd 23.1.0 and `lsp-mcpp-kit`, verified layout, no case-only name pairs |
| conformance | Linux: seven runs over six fixtures and a symbolic-link workspace; macOS: `inferred`, `untrusted`, `mcpp-llvm`; Windows: `inferred`, `untrusted`, `mingw`, `cmake-msvc` | the specifications against real toolchains, through the payload that ships |
| VS Code end to end | linux-x64, darwin-arm64, win32-x64 | the extension starts the payload's server in a downloaded VS Code, the module features answer, and the platform VSIX packages |

Found and fixed on the way, besides the upstream defects above: unsaved module edits did not reach importers when the workspace was reached by another name (a symbolic link on macOS, a short alias on Windows), a server that exited or hung made every later conformance check wait out its timeout, Linux kernel headers with case-only name pairs broke VSIX packaging, and VS Code 1.110's renamed macOS executable and 104-byte socket limit broke the end-to-end tests.
