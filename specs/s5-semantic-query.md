# S5 — Semantic Queries for C++ Code

| | |
|---|---|
| Specification | S5 |
| Version | 0.1.0 |
| Status | Draft |
| Bindings | Model Context Protocol (2024-11-05, 2025-03-26, 2025-06-18); command line |
| License | Apache-2.0 |

## Abstract

This specification defines the questions a coding agent, a script or an editor asks a C++ language server about a workspace, and the shape of the answers: symbols found by name, position or identifier, their references, callers and callees, file outlines, modules and the module graph, a file's build context, and diagnostics computed for the content files have now. Results name locations the way a reader counts them, say which state of the workspace they describe, and say when they are incomplete. Section 6 binds the queries to the Model Context Protocol (MCP), section 7 to a command line. A later version adds the review of changes (findings, evidence and their LSP and SARIF forms); section 5 defines the finding data it will use.

## 1. Conventions

- The key words **MUST**, **MUST NOT**, **REQUIRED**, **SHALL**, **SHALL NOT**, **SHOULD**, **SHOULD NOT**, **RECOMMENDED**, **MAY** and **OPTIONAL** are to be interpreted as described in BCP 14 (RFC 2119, RFC 8174) when, and only when, they appear in all capitals.
- Structures are written as TypeScript interfaces. A field marked `?` may be absent.
- "Set", "role" and "module unit" have the meanings of [S1](s1-build-database.md); "status" and "context" those of [S3](s3-lsp-extensions.md). A module name is written `m`, a partition `m:p`.
- The *core engine* is the component that answers C++ semantics (clangd in mcppls); the *module index* is the server's own knowledge of module declarations and imports. A workspace may run without a core engine.

## 2. Result conventions

### 2.1 Locations

```ts
interface Location {
  file: string;       // relative to the workspace root, '/'-separated; absolute outside the root
  line: number;       // from 1
  column: number;     // from 1, in Unicode scalar values
  text: string;       // the whole line, without its line terminator
  endLine?: number;   // the end of a range, when the location is one
  endColumn?: number;
}
```

- Lines and columns **MUST** count from 1, a column counting Unicode scalar values, not bytes or UTF-16 code units. <a id="S5-2.1-1"></a><sup>S5-2.1-1</sup>
- A location **MUST** carry the text of its line, so a reader need not open the file to see what it names. <a id="S5-2.1-2"></a><sup>S5-2.1-2</sup>
- A file inside the workspace root **MUST** be named relative to the root with `/` separators on every platform. <a id="S5-2.1-3"></a><sup>S5-2.1-3</sup>

### 2.2 Snapshot

```ts
interface Snapshot {
  generation: number;   // advances whenever a document, a watched file, the project model or the plan changes
  overlays: string[];   // files whose unsaved content, not their content on disk, was read
  preparing: boolean;   // the project model or modules were still being prepared
  indexing: boolean;    // the core engine's index was still being built
}
```

- Every query result **MUST** carry the snapshot it was computed from. <a id="S5-2.2-1"></a><sup>S5-2.2-1</sup>
- A server **MUST** read a file's content on disk at the time of the query, unless a client of the same session holds unsaved content for it, which it then lists in `overlays`. <a id="S5-2.2-2"></a><sup>S5-2.2-2</sup>

### 2.3 Symbol identifiers

A symbol's `id` is the Unified Symbol Resolution string (USR) the core engine reports for it.

- A server **MUST** resolve an `id` produced by the same session without its name. <a id="S5-2.3-1"></a><sup>S5-2.3-1</sup>
- A caller outside that session **SHOULD** pass the symbol's name with its `id`; the server then finds the named symbols and keeps the one whose USR matches. <a id="S5-2.3-2"></a><sup>S5-2.3-2</sup>

### 2.4 Limits

Lists are bounded by `maxResults`. A bounded result carries `total`, the number of items before the bound, and `truncated`.

- A server **MUST** order list items deterministically: by module, then file, then line and column. <a id="S5-2.4-1"></a><sup>S5-2.4-1</sup>

### 2.5 Failures

```ts
interface Failure {
  code: "invalid-arguments" | "not-found" | "ambiguous" | "unavailable" | "timeout" | "untrusted" | "unknown-tool";
  message: string;
  candidates?: Symbol[];   // for ambiguous
}
```

- A query that names more than one symbol where one is needed **MUST** fail with `ambiguous` and list the candidates with their identifiers. <a id="S5-2.5-1"></a><sup>S5-2.5-1</sup>
- A query that needs the core engine in a workspace without one **MUST** fail with `unavailable`, not return an empty result. <a id="S5-2.5-2"></a><sup>S5-2.5-2</sup>

## 3. Queries

### 3.1 Symbols

Input: `name` (unqualified, or qualified with `::`), or `id`, or `file`, `line` and `column`; optionally `kind` and `module` to narrow a name.

```ts
interface Symbol {
  id?: string;
  name: string;
  qualifiedName: string;
  kind: string;              // an LSP SymbolKind in kebab case: function, method, class, struct, namespace, ...; or module
  module?: string;           // of the unit that declares it
  declaration?: Location;
  definition?: Location;
  signature?: string;
  type?: string;             // a function's return type, or a variable's type
  documentation?: string;
}
interface Symbols { snapshot: Snapshot; symbols: Symbol[]; total: number; truncated: boolean }
```

- A name **MUST** match a symbol's own name exactly, and a qualified name also the scope that contains it. <a id="S5-3.1-1"></a><sup>S5-3.1-1</sup>
- `kind` values **MUST** distinguish a struct from a class. <a id="S5-3.1-2"></a><sup>S5-3.1-2</sup>
- `module` **MUST** name the partition when the declaration is in a partition. <a id="S5-3.1-3"></a><sup>S5-3.1-3</sup>
- A symbol defined in another unit of its module than the one declaring it (an implementation unit) **MUST** have that `definition`, also when it was found by a position in an importer. <a id="S5-3.1-4"></a><sup>S5-3.1-4</sup>
- A symbol the core engine's index names twice (by declaration and by definition) **MUST** be one result. <a id="S5-3.1-5"></a><sup>S5-3.1-5</sup>

### 3.2 References

```ts
interface ReferenceGroup { module?: string; file: string; references: { line: number; column: number; text: string }[] }
interface SearchScope { searchedFiles: number; complete: boolean; unsearched: string[] }
interface References { snapshot: Snapshot; symbol: Symbol; groups: ReferenceGroup[]; total: number; truncated: boolean; scope: SearchScope }
```

A core engine's index can miss references in units whose imports it does not build. The *search scope* of a symbol declared in module `m` is: the units of `m`, every unit that imports `m`, and, for every module that re-exports a module already in the scope, every unit that imports it.

- A server **MUST** find references in every unit of the search scope, within its budget, including units that reach the symbol only through a re-exporting module. <a id="S5-3.2-1"></a><sup>S5-3.2-1</sup>
- When the budget left part of the search scope unsearched, `scope.complete` **MUST** be false, with those units in `scope.unsearched`. <a id="S5-3.2-2"></a><sup>S5-3.2-2</sup>

### 3.3 Calls

Input: a symbol, and `direction`: `callers` or `callees`.

```ts
interface Call { symbol: Symbol; sites: Location[] }   // sites are in the caller
interface Calls { snapshot: Snapshot; symbol: Symbol; direction: "callers" | "callees"; calls: Call[]; total: number; truncated: boolean; scope: SearchScope }
```

A caller is the function, method or constructor that encloses a reference. Two programs of a workspace each have their own `main`, with one identifier between them; they are two callers.

- Callers **MUST** be searched in the search scope of 3.2, each enclosing function of a file its own caller. <a id="S5-3.3-1"></a><sup>S5-3.3-1</sup>
- A call's `qualifiedName` **MUST** include the scope the function is declared in. <a id="S5-3.3-2"></a><sup>S5-3.3-2</sup>

### 3.4 File outline

```ts
interface OutlineEntry { name: string; kind: string; line: number; detail?: string; children?: OutlineEntry[] }
interface Outline { snapshot: Snapshot; file: string; module?: string; symbols: OutlineEntry[] }
```

- An outline of a module unit **MUST** include its module declaration. <a id="S5-3.4-1"></a><sup>S5-3.4-1</sup>

### 3.5 Modules

Input: a module `name` (a partition names its module), or a `file` of the module.

```ts
interface ModuleDescription {
  snapshot: Snapshot;
  name: string;
  external: boolean;                 // provided by a standard library or module metadata
  resolvedFrom: "set" | "stdlib" | "module-metadata";
  units: { file: string; name: string; role: string; sets?: string[] }[];
  partitions: string[];
  exportedPartitions: string[];      // re-exported by the primary interface
  imports: string[];                 // modules its units import, its own partitions excepted
  importedBy: string[];              // modules whose units import it
  importingFiles: string[];           // units that import it and belong to no module
}
interface ModuleGraph { snapshot: Snapshot; modules: { name: string; external: boolean; files: string[] }[]; imports: { from: string; to: string }[]; total: number; truncated: boolean }
```

- A description **MUST** list every unit of the module with its role, implementation units included. <a id="S5-3.5-1"></a><sup>S5-3.5-1</sup>
- Module queries **MUST** be answered without a core engine. <a id="S5-3.5-2"></a><sup>S5-3.5-2</sup>

### 3.6 Diagnostics

Input: `files`, and `fresh` (default true).

```ts
interface Diagnostic { severity: "error" | "warning" | "information" | "hint"; message: string; code?: string; source?: string; location: Location; related?: { location: Location; message: string }[] }
interface FileDiagnostics { file: string; complete: boolean; reason?: string; diagnostics: Diagnostic[] }
interface DiagnosticsReport { snapshot: Snapshot; files: FileDiagnostics[]; counts: { error: number; warning: number; information: number; hint: number }; semanticSource: string }
```

- With `fresh`, a server **MUST** wait, within its deadline, until the core engine's diagnostics are computed for the content each file has now. <a id="S5-3.6-1"></a><sup>S5-3.6-1</sup>
- A file whose diagnostics are not computed for its current content when the result is returned **MUST** have `complete: false` and a `reason`. <a id="S5-3.6-2"></a><sup>S5-3.6-2</sup>

## 4. Contexts

### 4.1 Build context

```ts
interface BuildContext {
  snapshot: Snapshot;
  file: string;
  module?: string;
  inModel: boolean;                                // some set of the project model builds the file
  sets: { name: string; kind: string; role: string }[];
  contextSet: string;                              // S3 context; "default" for all sets
  projectSource: string;                           // mcpp | cmake | compile-commands | build-database | inferred
  semanticSource: "build-toolchain" | "semantic-kit";
  compiler?: string;
  toolchain?: { family: string; version: string };
  target: string;
  stdlib: string;
  languageStandard?: string;
  macros: string[];                                // NAME, NAME=value, or -NAME for an undefinition
  includeDirectories: string[];
  excludedFromEngine: boolean;                     // left out of the core engine's database
  engine: string;                                  // "clangd 23.1.0", or "none"
  issues: { code: string; message: string }[];
}
```

- A build context **MUST** give the file's role in each set that builds it. <a id="S5-4.1-1"></a><sup>S5-4.1-1</sup>
- A file no set builds **MUST** have `inModel: false` and an issue `not-in-model`. <a id="S5-4.1-2"></a><sup>S5-4.1-2</sup>

### 4.2 Module interface

What `import m;` brings in, without implementations.

```ts
interface InterfaceDeclaration {
  kind: "function" | "class" | "struct" | "union" | "enum" | "concept" | "alias" | "variable" | "other";
  name: string;
  qualifiedName: string;
  declaration: string;       // without body or initializer, whitespace collapsed
  documentation?: string;    // the comment right above it
  unit: string;              // the unit that exports it: m, or m:p
  location: Location;
  conditional?: boolean;     // inside #if, #ifdef or #ifndef: the summary does not preprocess
}
interface ModuleInterface {
  module: string;
  files: string[];           // interface units read, the primary interface first
  reexports: string[];       // other modules `export import` brings in; not expanded
  declarations: InterfaceDeclaration[];
  total: number;
  truncated: boolean;
  documentationOmitted: boolean;
}
```

Input: a budget in tokens, four characters counting as one.

- A summary **MUST** include the declarations of every partition the primary interface re-exports, transitively, each with the unit that exports it. <a id="S5-4.2-1"></a><sup>S5-4.2-1</sup>
- When the budget does not hold every declaration with its documentation, documentation **MUST** be left out before any declaration is. <a id="S5-4.2-2"></a><sup>S5-4.2-2</sup>
- Without a core engine, or for a name the core engine does not know, a symbol lookup by name (3.1) **MUST** find the declarations module interfaces export under that name. <a id="S5-4.2-3"></a><sup>S5-4.2-3</sup>

## 5. Findings

The review of changes (a later version) reports findings. Their data is defined here so that every producer of findings, rules and models alike, agrees on it.

```ts
interface Evidence { id: string; kind: "reference" | "diff" | "diagnostic" | "module-graph" | "import" | "test"; location: Location; detail?: string }
interface Finding {
  id: string;
  rule: string;                                    // e.g. module/export-removed-in-use
  severity: "error" | "warning" | "information" | "hint";
  message: string;
  location: Location;
  evidence: Evidence[];
  origin: "rule" | "model";
  fix: null | { description: string; edits: { file: string; line: number; column: number; endLine: number; endColumn: number; newText: string }[] };
  fingerprint: string;                             // "sha256:" and 64 hexadecimal digits
  model?: { source: string; name: string; template: string };
  confidence?: number;
}
```

- A fingerprint **MUST** be computed from the rule, the file and the whitespace-normalized text of the location and of the evidence, so it does not change when lines move or are reindented. <a id="S5-5-1"></a><sup>S5-5-1</sup>

## 6. MCP binding

A server runs as an MCP server over standard input and output (`mcppls mcp`): one JSON-RPC message per line.

| Tool | Query |
|---|---|
| `cxx_symbol` | 3.1 |
| `cxx_references` | 3.2, and 3.3 with `direction` |
| `cxx_outline` | 3.4 |
| `cxx_module` | 3.5 with 4.2 as `interface` (unless `interface: false`); `graph: true` for the graph |
| `cxx_build_context` | 4.1 |
| `cxx_diagnostics` | 3.6 |

- Every tool **MUST** be annotated `readOnlyHint: true`, and leave every file of the workspace as it was. <a id="S5-6-1"></a><sup>S5-6-1</sup>
- A tool result **MUST** carry its S5 result as JSON text, and also as `structuredContent` when the negotiated protocol version is 2025-06-18 or later. <a id="S5-6-2"></a><sup>S5-6-2</sup>
- A failure of a query **MUST** be a tool result with `isError: true` and `{"error": Failure}`, not a JSON-RPC error; a JSON-RPC error is for an unknown tool or method. <a id="S5-6-3"></a><sup>S5-6-3</sup>
- A server **MUST** answer `initialize` with the client's protocol version when it supports it, and otherwise with the latest version it supports. <a id="S5-6-4"></a><sup>S5-6-4</sup>
- A server **MUST NOT** write anything but MCP messages to its standard output. <a id="S5-6-5"></a><sup>S5-6-5</sup>

## 7. Command-line binding

| Command | Query |
|---|---|
| `mcppls query symbol <name> [--at FILE:LINE:COLUMN] [--id ID] [--kind K] [--module M]` | 3.1 |
| `mcppls query refs <name> [--no-declaration]` | 3.2 |
| `mcppls query calls <name> [--callees]` | 3.3 |
| `mcppls query outline <file>` | 3.4 |
| `mcppls query module <name> [--file F] [--graph]` | 3.5 |
| `mcppls diagnostics <file>... [--no-fresh]` | 3.6 |

Every command takes `--root`, `--timeout` and `--format json|text`.

- With `--format json` (the default) a command **MUST** print exactly one JSON document: the S5 result, or `{"error": Failure}`. <a id="S5-7-1"></a><sup>S5-7-1</sup>
- A command **MUST** exit with 0 when it has a result, 1 when the query found nothing or diagnostics include an error, and 2 when the command itself failed, diagnostics that could not be completed in time included. <a id="S5-7-2"></a><sup>S5-7-2</sup>
