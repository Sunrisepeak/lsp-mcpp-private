# mcppls-model

The reference model gateway for mcppls (overall design section 7.5, work
item M2). A separate mcpp package, built and versioned independently of the
language server: mcppls never talks to the network itself (openkal, which
the server is built on, deliberately has no DNS resolution and no TLS), so
when a user opts into a model source, mcppls starts this executable as a
child process and speaks the line protocol in [PROTOCOL.md](PROTOCOL.md)
with it over stdio.

```
mcppls-model --provider openai --endpoint http://127.0.0.1:11434/v1 --model qwen2.5-coder
```

talks to a local OpenAI-compatible server (Ollama, llama.cpp, vLLM, ...);
pointed at `https://api.openai.com/v1` or `https://api.anthropic.com/v1`
(the defaults when `--endpoint` is omitted) with `--api-key-env` naming a
variable that holds a real key, it talks to the corresponding cloud API. See
PROTOCOL.md for the full method list, error codes and configuration.

## Build form

Experiment X3 (design section 13) asked whether this package can be built
the way the main server is — LLVM 22.1.8 with the `openkal-llvm-runtime`
dependency (openkal beneath musl, libc++ above it; see the root
`mcpp.toml`) — or needs the toolchain's normal C library instead.

**Both forms actually build on Linux.** Two throwaway packages depending on
exactly what this gateway uses (`mcpplibs.tinyhttps` 0.2.8, `nlohmann.json`
3.12.0, `mcpplibs.cmdline` 0.0.2) were built and run: one with
`openkal-llvm-runtime = "0.9.6"` added (matching the root package's
`[dependencies]`), one without it. Both compiled tinyhttps's mbedTLS
dependency and linked cleanly:

```
# with openkal-llvm-runtime:
Target x86_64-unknown-linux-gnu
       compiler-runtime  compiler-rt    (openkal-llvm-runtime@0.9.6, graph)
       kernel-abi        openkal        (openkal-linux@0.12.0, graph)
       c-abi             musl           (openkal-musl@0.13.5, graph)
       c++-abi           libc++         (openkal-llvm-runtime@0.9.6, graph)
  Finished dev [unoptimized + debuginfo] in 2.06s   # statically linked, runs

# without it (the toolchain's normal glibc/libc++):
Target x86_64-unknown-linux-gnu
  Finished dev [unoptimized + debuginfo] in 1.54s   # dynamically linked against xim-x-glibc, runs
```

(mbedTLS under musl prints a handful of benign upstream `-Wshift-op-parentheses`
warnings from `openkal-musl`'s `endian.h`; no errors either way.)

**Chosen: the toolchain's normal C library (no `openkal-llvm-runtime`
dependency)** — `model-gateway/mcpp.toml` sets `[toolchain] default =
"llvm@22.1.8"` and stops there. Reasons, since a Linux-only build success on
both sides does not by itself settle it:

1. **The reason the server uses openkal does not apply here.** Its whole
   point for the server is cross-compiling one Linux CI host's build for
   every platform ("one source builds every target", per the root
   `README.md`). This package "only needs to build and run natively on each
   host (it is not cross-built from Linux like the server)" — every host in
   CI builds its own binary — so that benefit is moot, and it is the *only*
   reason to prefer openkal over the toolchain's own, better-trodden path
   for a general-purpose library like tinyhttps that is not written
   specifically for openkal.
2. **openkal's release-profile defects land squarely on a tool like this
   one.** Recorded from this project's own probes (`.agents` /
   `cpp-module-toolchain-pitfalls`): release (`-O2`) builds over
   openkal-windows truncate argv and produce no stdout, and over
   openkal-macos segfault; dev builds are fine. A gateway that is *entirely*
   "read argv, write stdout" (PROTOCOL.md) is exactly what those defects
   break, and a payload or release CI job would very plausibly hit them
   rather than route around them by accident.
3. **The normal form on Windows is already known to work in this
   repository.** From the same probes (`msvc-family-probe-facts`):
   `llvm@22.1.8` without the openkal runtime, on the default MSVC target
   with MSVC STL, already builds `import std` cleanly on Windows — this is
   not a leap into untested territory, it is the form this project has
   already exercised there.

So this package is built and tested with each host's own C library: glibc
on Linux, the system libraries on macOS, MSVC's ucrt on Windows — not
openkal, and not cross-built.

One knock-on effect: `mcpplibs::tinyhttps` (0.2.3 through at least 0.3.0,
checked directly) refuses every URL scheme but `https` before opening a
socket, including for `127.0.0.1`. That is unrelated to the openkal-vs-normal
question above — the same restriction holds in both forms — but it does mean
neither `mcpplibs::llmapi` nor `mcpplibs::tinyhttps::HttpClient` alone can
reach the plain-HTTP local endpoints PROTOCOL.md's `--endpoint` example and
this package's own tests need. See "Dependencies" below.

## Dependencies

- `nlohmann.json` 3.12.0 (`import nlohmann.json;`) for every JSON value:
  requests and responses on the wire, and the OpenAI/Anthropic request and
  response bodies.
- `mcpplibs.cmdline` 0.0.2 (`import mcpplibs.cmdline;`) for `--provider` /
  `--endpoint` / `--model` / `--api-key-env` / `--timeout`.
- `mcpplibs.tinyhttps` 0.2.8 (`import mcpplibs.tinyhttps;`), used directly
  rather than through `mcpplibs.llmapi`:
  - `tinyhttps::HttpClient` sends every `https://` request (the default
    OpenAI/Anthropic base URLs, and any `https://` endpoint a user
    configures).
  - `tinyhttps::Socket` — tinyhttps's own raw-TCP class, exported from its
    top-level module alongside `HttpClient` — is the transport for
    `http://` endpoints. `mcppls.model.transport` adds only the thin,
    deliberately minimal HTTP/1.1 request/response framing on top (always
    sends `Connection: close`, so there is no chunked-transfer decoder to
    write), and polls for cancellation in ~100ms slices while waiting on
    I/O, so a `cancel` notification reaches a request blocked reading a slow
    reply promptly (exercised by the "slow" case in `tests/`).

  `mcpplibs.llmapi` 0.2.8 was tried first, per the task, and set aside: its
  `OpenAI`/`Anthropic` provider classes call straight into
  `tinyhttps::HttpClient::send`, with no seam to redirect a request through
  anything else — so the `https`-only restriction above is not something a
  caller of llmapi can route around, only something a caller of tinyhttps's
  own lower-level pieces can. llmapi also vendors its own copy of
  `nlohmann::json` as `mcpplibs.llmapi.nlohmann.json` (a separate module from
  the top-level `nlohmann.json` this package also needs); using tinyhttps
  directly avoids depending on both.

**Known trap** (see `.agents` project memory, confirmed while writing this
package): `nlohmann::json x { otherJson }` — braced-init with one `json`
argument — selects the `initializer_list<json>` constructor and produces a
one-element *array* wrapping `otherJson`, not a copy of it. Every JSON
value built here uses `=`, not `{ }`, whenever the right-hand side is
itself a `nlohmann::json` value (an empty `Json::array()`/`Json::object()`,
or a whole request/response body assembled in one expression).

## Layout

C++23 named modules only (`.cppm` interfaces + `.cpp` implementation units,
no headers of our own, no macros — a `module;` global fragment is used only
for `<winsock2.h>`/POSIX socket headers `mcpplibs::tinyhttps::Socket` itself
needs, the same pattern tinyhttps uses). Every module is `mcppls.model.<name>`;
every one opens `namespace mcppls::model` (the src/-directory convention the
main server's modules already use — see the overall design's section 4.2).

| File | Module | Role |
|---|---|---|
| `src/protocol.cppm`/`.cpp` | `mcppls.model.protocol` | The `{id, result\|error}` envelope and error codes |
| `src/config.cppm`/`.cpp` | `mcppls.model.config` | `--flag` / `MCPPLS_MODEL_*` resolution |
| `src/schema.cppm`/`.cpp` | `mcppls.model.schema` | The `required`-keys / top-level-`type`s check for `complete`'s `schema` |
| `src/transport.cppm`/`.cpp` | `mcppls.model.transport` | One POST-JSON call, over `http://` or `https://` |
| `src/provider.cppm`/`.cpp` | `mcppls.model.provider` | OpenAI-compatible and Anthropic request building / response parsing |
| `src/gateway.cppm`/`.cpp` | `mcppls.model.gateway` | The stdio loop: reads and dispatches, threads a `cancel` through to an in-flight call |
| `src/main.cpp` | — | Entry point (not a module, like the server's own `src/main.cpp`) |

## Building

```bash
cd model-gateway
mcpp build          # target/<host-triple>/<hash>/bin/mcppls-model
```

Native only, on whichever host runs it — see "Build form" above.

## Tests

Python standard library only, no real provider and no API key (PROTOCOL.md's
error paths need a provider that misbehaves on command, which no real one
offers to do):

- `tests/mock_openai.py` — an OpenAI-compatible `POST /v1/chat/completions`
  server on `127.0.0.1`, port chosen by the OS and printed on its first
  stdout line. The reply is chosen by the last user message's content:
  `plain`, `json-ok`, `json-bad`, `http-500`, `slow` (sleeps 4s, for the
  cancel test).
- `tests/run.py --gateway <built executable>` — starts the mock, runs the
  built gateway against it, and checks: `initialize`; a plain completion
  with usage; a schema completion returning `result.json`; error `1004` for
  `json-bad`; error `1001` for `http-500`; `cancel` answering `1003`
  promptly (well before the mock's 4s sleep ends); `shutdown`; a clean exit
  at end of input. Prints `PASS`/`FAIL` per check and exits non-zero on the
  first failure.

```bash
mcpp build
python3 tests/run.py --gateway target/x86_64-linux-gnu/<hash>/bin/mcppls-model
```

All 25 checks pass; run five times in a row locally with no flakiness
(the cancel check is the only timing-sensitive one — it asserts the
response lands within 3 seconds of the `cancel` notification, well short of
the mock's 4-second sleep).
