# Conformance

The executable part of the specifications: each fixture is a small project and
a `scenario.json` that drives a language server through the Language Server
Protocol and checks what comes back. The same fixtures run locally and in CI.

```bash
mcpp build
bin=target/<triple>/<fingerprint>/bin
$bin/lsp-mcpp-conformance run --server $bin/lsp-mcpp --fixture conformance/fixtures/inferred \
    --payload editors/vscode/payload            # or --clangd PATH --kit DIR
```

The runner copies the fixture to a scratch directory, runs its `prepare`
commands there, starts the server with a private cache directory
(`LSP_MCPP_CACHE_DIR`), advertises `experimental.cxxModules`, and prints one
`PASS`/`FAIL` line per check. It exits non-zero when a check fails. Once the
server has exited, or has left two requests in a row unanswered, the remaining
checks fail at once with that reason instead of each waiting out its timeout.

## Fixtures

| Fixture | What it covers |
|---|---|
| `inferred` | Loose module sources, no build system and no compiler: the semantic kit provides libc++ semantics (design 13.5) |
| `untrusted` | An mcpp package in an untrusted workspace: nothing is executed, the kit answers, the status is `degraded` with the reason |
| `mcpp-gcc` | mcpp with GCC 16: GCC arguments translated for clangd (P1), libstdc++'s `std` injected |
| `mcpp-llvm` | mcpp with LLVM 22: libc++ selected through include paths, BMI arguments removed (P3) |
| `mcpp-emit` | mcpp's `emit build-database --format json` (mcpp-community/mcpp#636), simulated by `lsp-mcpp-mock-mcpp` from `mcpp-mock.json`: an S1 level 3 model and an unchanged workspace |
| `mcpp-emit-package-std` | The same with `std` and `std.compat` provided by a dependency package's translation units instead of the toolchain's manifest |
| `mingw` | A compile database for `x86_64-windows-gnu`: MinGW-w64 GCC semantics through `--sysroot` (P2) |
| `cmake-clang` | CMake 3.28+ with `FILE_SET CXX_MODULES`, Clang and Ninja: `@modmap` expansion, partitions |
| `cmake-msvc` | The same project built by cl.exe on Windows: cl.exe arguments translated into clang++ arguments, `-interface`, `-ifcOutput` and `-reference` removed from the expanded `.modmap` files, the Visual Studio toolset and Windows SDK passed explicitly (P7) |
| `cmake-msvc-std` | CMake's `import std` with cl.exe and an `.ixx` interface: the build's own `std.ixx` units are replaced by the MSVC STL manifest's (P7) |
| `cmake-clangxx-msvc` | CMake modules built by clang++ for the MSVC ABI (P5) |
| `compdb-clangxx-msvc-std` | clang++ for the MSVC ABI with `import std`, built by the fixture's own script (P5) |
| `compdb-clang-cl-std` | clang-cl with `import std`, built by the fixture's own script (P6) |
| `mcpp-msvc` | mcpp with `msvc@system` and `import std` |
| `mcpp-llvm-msvc` | mcpp's default Windows toolchain, LLVM for `x86_64-windows-msvc`, with the MSVC STL (P5) |
| `inferred-msvc` | Loose module sources on a machine with Visual Studio: MSVC STL semantics without a build system (design 9.3, D27) |
| `timing` | Startup timing (usable plan W7): the `inferred` project opened and navigated at once; run cold, then warm with the same workspace and cache |
| `s1-two-sets` | A workspace carrying its own S1 build database (`--database`, usable plan W9.2): two sets compile the same file under `-DVARIANT=1` and `-DVARIANT=2`; `cxxModules/setContext` switches which one answers |
| `watch-polling` | Run with `--no-dynamic-watch` (usable plan W9.3): a new module interface written straight into the workspace must still reach the module graph within seconds, through the polling fallback rather than a client-driven `workspace/didChangeWatchedFiles` |
| `payload-corrupt` | Its `prepare` step copies the payload the runner was given and truncates clangd in the copy (usable plan W9.4); `server-arguments` then points `--payload` at that broken copy, and status must reach `error` with issue `payload-corrupt` |
| `multi-root` | Two workspace folders (usable plan W9.1): an `inferred` root and an mcpp-built `mcpp-llvm` root, each getting its own project model and clangd, each `cxxModules/status` telling them apart by `project.root` |

On Windows every server runs without a developer environment, as it does when an editor starts it. Fixtures whose own build needs one declare `"prepare-environment": "msvc"`, and the runner is given the environment with `--msvc-env FILE` (`NAME=value` lines, the output of `set` after `vcvars64.bat`); only the prepare steps see it. `--no-dynamic-watch` makes the runner declare no `workspace.didChangeWatchedFiles.dynamicRegistration` support at all, the way an editor without it would, exercising the server's own polling fallback (usable plan W9.3) instead of dynamic registration.

## Startup timing

A run can reuse its workspace and the server's cache, so a second run measures a warm start:

```bash
$bin/lsp-mcpp-conformance run --server $bin/lsp-mcpp --payload payload --fixture conformance/fixtures/timing \
    --workspace-dir /tmp/timing/workspace --cache-dir /tmp/timing/cache --measure cold.json --navigation-budget 15
$bin/lsp-mcpp-conformance run --server $bin/lsp-mcpp --payload payload --fixture conformance/fixtures/timing \
    --workspace-dir /tmp/timing/workspace --cache-dir /tmp/timing/cache --measure warm.json --navigation-budget 2 --expect-warm
```

| Option | Effect |
|---|---|
| `--workspace-dir DIR` | The fixture is copied to `DIR/<name>` and prepared once; later runs use it as it is |
| `--cache-dir DIR` | The server's cache directory, instead of a fresh one per run |
| `--measure FILE` | JSON with seconds from `initialize` to the first `ready` state, the first diagnostics and the first navigation that answered, and each check's duration |
| `--navigation-budget SECONDS` | The run fails when the first navigation took longer |
| `--expect-warm` | A `module-cache-reused` check fails unless an earlier run left the module's files in the cache |

CI runs the pair on every host with budgets of 15 and 2 seconds and uploads the measure files; nightly records three runs per host.

## Scenario format

```json
{
  "name": "inferred",
  "prepare": [["cmake", "-S", ".", "-B", "build"]],
  "remove": ["mcpp.toml"],
  "server-arguments": ["--no-discover"],
  "checks": [ { "id": "C2", "kind": "definition", "file": "src/main.cpp", "at": [4, 31], "expect": "src/greet/greet.cppm" } ]
}
```

In `prepare` and `server-arguments`, `{exe}` expands to the platform executable suffix,
`{env:NAME|fallback}` to an environment variable, `{workspace}` to the fixture's scratch copy,
`{runner-dir}` to the directory of the runner executable, and `{payload}` to the runner's own
`--payload` directory (usable plan W9.4: a fixture's `prepare` step can copy and mutate it, then
point `server-arguments`' own `--payload` at the mutated copy — a later `--payload` wins). Positions
are `[line, character]`, zero-based, UTF-16. A check with `"text"` opens its file with that unsaved
content; a check with `"optional": true` reports `SKIP` instead of failing, and `"timeout": SECONDS`
waits less than the run's `--timeout`. `"file"` and `"folder"` on a check, like every other path a
scenario names, are relative to the fixture's own root, never to a specific workspace folder.

A fixture with more than one workspace folder (usable plan W9.1) names them, relative to its own
root, in a top-level `"folders"` array; without one, the fixture's root is the only folder, as it
always has been.

```json
{ "name": "multi-root", "folders": ["inferred", "mcpp-llvm"], "checks": [
  { "id": "S1", "kind": "status", "folder": "mcpp-llvm", "state": "ready" } ] }
```

| Kind | Passes when |
|---|---|
| `status` | `cxxModules/status` reaches `ready`, `degraded` or `error` and matches `source`, `profile-kind`, `state`, `level`, `issue-code` when given, and a `profile-compiler` prefix; `"folder"` picks one root's own status in a multi-root fixture (usable plan W9.1), absent picks whichever root's arrived most recently |
| `workspace-unchanged` | no file under the workspace was added, changed or removed after the prepare steps |
| `module-cache-reused` | every file clangd published for `module` (default `std`) before the server started is still there unchanged, and none was added (SC4); passes on a cold start unless `--expect-warm` |
| `diagnostics-empty` | the file's diagnostics, after the engine has published them, contain no errors |
| `diagnostic-code` | a diagnostic with code `expect` is published for the file |
| `definition` / `declaration` | a location ends with `expect` |
| `definition-any` | there is at least one location |
| `hover-contains` | the hover text contains `expect` |
| `completion-contains` | a completion label starts with `expect`; `insert: [line, text]` adds a line first, `edit` changes another open buffer without saving it |
| `references-span` | the references include every path in `expect` |
| `document-symbol-contains` | the outline has a top-level symbol named `expect` |
| `module-graph-contains` | `cxxModules/graph` lists module `expect`; retries within the check's own timeout, so it doubles as "a change reaches the graph within N seconds" (usable plan W9.3's `watch-polling`) |
| `set-context` | sends `cxxModules/setContext` with `"context"` (usable plan W9.2), then a hover at `"at"` contains `expect`, retried the same way as `hover-contains` |
| `write-file` | writes `"content"` (default: a fresh `export module <module>;`) to `"file"` directly, the way a file system watcher — or, without one, the server's own polling fallback — would notice it, without the runner opening it as a document (usable plan W9.3) |

The check identifiers C1–C9 follow experiment E6 in
`.agents/docs/2026-09-13-cxx-modules-lsp-experiments.md`; M-checks cover the
module features of S3 section 8.5.
