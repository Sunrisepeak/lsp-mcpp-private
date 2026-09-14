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

On Windows every server runs without a developer environment, as it does when an editor starts it. Fixtures whose own build needs one declare `"prepare-environment": "msvc"`, and the runner is given the environment with `--msvc-env FILE` (`NAME=value` lines, the output of `set` after `vcvars64.bat`); only the prepare steps see it.

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
`{env:NAME|fallback}` to an environment variable, `{workspace}` to the fixture's scratch copy and
`{runner-dir}` to the directory of the runner executable. Positions are `[line, character]`,
zero-based, UTF-16. A check with `"text"` opens its file with that unsaved content;
a check with `"optional": true` reports `SKIP` instead of failing.

| Kind | Passes when |
|---|---|
| `status` | `cxxModules/status` reaches `ready` or `degraded` and matches `source`, `profile-kind`, `state`, `level` when given, and a `profile-compiler` prefix |
| `workspace-unchanged` | no file under the workspace was added, changed or removed after the prepare steps |
| `diagnostics-empty` | the file's diagnostics, after the engine has published them, contain no errors |
| `diagnostic-code` | a diagnostic with code `expect` is published for the file |
| `definition` / `declaration` | a location ends with `expect` |
| `definition-any` | there is at least one location |
| `hover-contains` | the hover text contains `expect` |
| `completion-contains` | a completion label starts with `expect`; `insert: [line, text]` adds a line first, `edit` changes another open buffer without saving it |
| `references-span` | the references include every path in `expect` |
| `document-symbol-contains` | the outline has a top-level symbol named `expect` |
| `module-graph-contains` | `cxxModules/graph` lists module `expect` |

The check identifiers C1–C9 follow experiment E6 in
`.agents/docs/2026-09-13-cxx-modules-lsp-experiments.md`; M-checks cover the
module features of S3 section 8.5.
