# Changelog

All changes to the specifications in this directory. Each specification is versioned independently.

## All specifications

### 2026-09-15

- The reference implementation is renamed from lsp-mcpp to mcpp-language-server: its program, C++ modules and namespaces are `mcppls` and its semantic kit package is `mcppls-kit`. S3's example diagnostic `source` and S4's distribution package name follow. No normative change.

### 2026-09-14

- Rule identifiers `S<n>-<section>-<ordinal>` on every requirement of S1 to S4 (S3 added the same day), and `conformance/traceability.json` mapping each to its evidence (README, "Rule identifiers and traceability"). No normative change.

## S5 — Semantic Queries for C++ Code

### 0.1.0 — 2026-09-15 (Draft)

First draft (overall design 7.1, 7.2, 7.6, 8.1):

- Result conventions: locations counted from 1 in Unicode scalar values with their line's text, the snapshot a result describes, USR symbol identifiers, deterministic order, limits and failures.
- Queries: symbols by name, identifier or position; references and callers searched in a symbol's module and everything importing it, re-exports included; callees; file outlines; modules and the module graph; fresh diagnostics.
- The build context of a file, and the data of findings (rule, evidence, origin, fix, fingerprint) for the review of changes of a later version.
- Verification after an edit: changed files and the units importing them, built again; snippets checked in place without writing them.
- The review of a change: collection from git, semantic diff, impact, six deterministic rules with their evidence, and the LSP and SARIF 2.1.0 forms of findings.
- Bindings: MCP tools over standard input and output, and `mcppls query`, `diagnostics`, `verify`, `impact` and `review` with exit statuses.

## S1 — C++ Build Database: IDE Profile

### 0.2.0 — 2026-09-14 (Draft), revised 2026-09-14

Revision:

- `stdlib.module-metadata` may name the MSVC STL's own `modules.json` shape (`library`, `module-sources`).
- The standard library modules may be provided by translation units of a set every requiring set sees, for example a dependency package's `std.cppm`; `stdlib` is then optional, and units win over a manifest listing the same module.
- Informative, from mcpp's answers on mcpp-community/mcpp#636 (no normative change):
  - a build that resolves imports against one flat module graph may group units per package (`<package>`, `<package>:test`, and a set for the standard library modules such as `mcpp:std`), and each set's `visible-sets` then lists every other set (section 7);
  - a consumer, or a library it uses, may complete a level 2 document to level 3 by structuring `arguments`, and the options it derives restate the arguments rather than replace them, so a producer may write level 2 (sections 9 and 11.1);
  - the level 3 example (section 15, `examples/s1-level3-gcc.json`) names a neutral `example-producer` as its generator, since mcpp writes level 2.

First public draft, derived from the design draft of 2026-09-13.

- Self-contained definition of every field, including those shared with P2977R2; a conforming document remains a valid P2977R2 document.
- Document, set and translation-unit `ide` objects; Toolchain with explicit `stdlib` and `module-metadata`; unit roles determined from source content, with `unknown` for undecidable cases and `header-unit` reserved.
- SemanticOptions with defined merge rules.
- Module name resolution order: set, visible sets, set module metadata, toolchain standard library metadata; unresolved and ambiguous names must be reported.
- Producer conformance levels 1–4 and consumer requirements.
- Export to `compile_commands.json`.
- JSON Schema `schema/s1-build-database.schema.json` and examples.

## S2 — Build Database Discovery Protocol

### 0.2.0 — 2026-09-14 (Draft), revised 2026-09-14

Revision:

- Example `examples/s2-envelope.json` carries the database in the shape mcpp settled on in mcpp-community/mcpp#636: level 2, sets `hello`, `hello:test` and `mcpp:std`, each seeing every other. No normative change.

First draft of 0.2.0:

- Single-document mode (section 3.4): one envelope on standard output with the database inline, advertised through the producer's `--protocol-version`; the producer writes nothing into the workspace. It matches mcpp's machine-output protocol version 1 and mcpp-community/mcpp#636.
- Schema `envelope` definition and example `examples/s2-envelope.json`.
- Schema: a `finished` message's `database` is an absolute path, as section 3.3 requires.

### 0.1.0 — 2026-09-14 (Draft)

- Database location order: explicit configuration, discovery command, known build directory.
- Discovery command: one JSON request on standard input; JSONL `progress`, `finished` and `error` messages on standard output; watched paths; producer and consumer requirements, including timeouts and trust.
- JSON Schema `schema/s2-discovery.schema.json` and examples.

## S3 — Language Server Protocol Extensions for C++ Modules

### 2026-09-15 (protocol version 1, backward compatible)

- `cxxModules/status`: `engine.name` is any engine name, `"none"` when a root has no core semantic engine; the optional `engines` lists every engine serving the root with its role and state (section 4, S3-4-5). Clients already reading `engine` keep working.


### Protocol version 1 — 2026-09-14 (Draft)

- Capability negotiation through `experimental.cxxModules`.
- `cxxModules/status` notification; `cxxModules/graph`, `cxxModules/moduleInfo`, `cxxModules/contexts` and `cxxModules/setContext` requests.
- Module features mapped onto standard LSP messages, with diagnostic codes `unresolved-module`, `ambiguous-module` and `partition-outside-module`.
- Revision 2026-09-14: `cxxModules/status` gains optional `notices`, facts that reduce no feature; issue code `module-build-failed`.
- Revision 2026-09-14: a server managing more than one workspace root sends one `cxxModules/status` notification per root, told apart by the already-existing `project.root` (usable plan W9.1).
- Revision 2026-09-14: `project.root` is the workspace folder's URI exactly as the client sent it, not a canonical form of its path, so a client can match it to its own folder; issue code `model-stale`.

## S4 — Semantic Kit

### Kit version 1 — 2026-09-14 (Draft)

- Kit layout and the `kit.json` manifest; kit paths; the `macos-sdk` requirement kind.
- Rules: data only, libc++ version equal to the pinned engine version, no redistributed macOS SDK, MinGW-w64 semantics on Windows.
- Consumer procedure for turning a kit into engine compile commands.
- First-batch kits for linux-x64, win32-x64 and darwin-arm64, distributed as `mcppls-kit`.
- JSON Schema `schema/s4-kit.schema.json` and examples.
