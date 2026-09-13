# S2 — Build Database Discovery Protocol

| | |
|---|---|
| Specification | S2 |
| Version | 0.1.0 |
| Status | Draft |
| Schema | [`schema/s2-discovery.schema.json`](schema/s2-discovery.schema.json) |
| Examples | [`examples/s2-request.json`](examples/s2-request.json), [`examples/s2-messages.jsonl`](examples/s2-messages.jsonl) |
| License | Apache-2.0 |

## Abstract

This specification defines how a consumer — typically a language server — locates an [S1](s1-build-database.md) build database, how it asks a producer to write or refresh one, and how it learns when the database must be read again. The command form follows the shape of rust-analyzer's project discovery command and Go's `GOPACKAGESDRIVER`: a child process, one JSON request on standard input, and a stream of JSON messages on standard output.

## 1. Conventions

- The key words **MUST**, **MUST NOT**, **REQUIRED**, **SHALL**, **SHALL NOT**, **SHOULD**, **SHOULD NOT**, **RECOMMENDED**, **MAY** and **OPTIONAL** are to be interpreted as described in BCP 14 (RFC 2119, RFC 8174) when, and only when, they appear in all capitals.
- JSON is as defined by RFC 8259 and encoded in UTF-8. A JSONL stream is a sequence of JSON objects, each serialized on a single line and terminated by a line feed (U+000A); a carriage return before the line feed is permitted and ignored.
- "Consumer" is the tool that starts the command and reads the database. "Producer" is the discovery command.

## 2. Locating a database

A consumer looks for a database in this order and uses the first that applies:

1. A path the user configured explicitly.
2. The path returned by a discovery command (section 3).
3. A `build_database.json` in a build directory the consumer knows for the project's build system, for example a CMake binary directory.

A producer **SHOULD** update a database atomically, by writing a temporary file in the same directory and renaming it over the database.

## 3. Discovery command

### 3.1 Invocation

The command line is configured by the user or is a convention of a producer, for example `mcpp emit build-database --format jsonl`. The consumer:

1. starts the command with the workspace root as its working directory;
2. writes exactly one request object (section 3.2) to its standard input as a single JSON line;
3. closes its standard input;
4. reads messages (section 3.3) from its standard output until the terminal message or the end of the stream.

Standard error is free-form diagnostic text. A consumer **MAY** log it and **MUST NOT** interpret it.

### 3.2 Request

| Field | Type | Requirement | Description |
|---|---|---|---|
| `workspace` | string | MUST | Absolute path of the workspace root. |
| `files` | string[] | SHOULD | Absolute paths of the files the consumer currently needs, for example the files open in the editor. A producer MAY use them to prioritize work and MUST NOT limit the database to them. An empty array means no particular file. |
| `configuration` | string | MAY | The build configuration the consumer wants, for example `debug`. Absent means the producer's default. |
| `profile-version` | string | MUST | The highest S1 profile version the consumer understands, for example `"0.2.0"`. |

Example:

```json
{ "workspace": "/abs/path/to/workspace",
  "files": ["/abs/path/to/workspace/src/greet/greet.cppm"],
  "configuration": "debug",
  "profile-version": "0.2.0" }
```

### 3.3 Messages

Every line of standard output is one message object with a `kind` field.

| `kind` | Fields | Meaning |
|---|---|---|
| `progress` | `message` (string, MUST); `done`, `total` (non-negative integers, MAY) | Work is under way. |
| `finished` | `database` (string, MUST); `watch` (string[], MUST); `profile-version` (string, SHOULD) | The database at `database` is complete. `profile-version` is the S1 version of the written document. |
| `error` | `message` (string, MUST); `code` (string, MAY) | Discovery failed. |

The last message **MUST** be a terminal message, `finished` or `error`, and a producer **MUST NOT** write anything after it. `database` **MUST** be an absolute path. Each element of `watch` is an absolute path or a glob pattern relative to `workspace`, using the glob syntax of LSP 3.18 (`*`, `**`, `?`, `{a,b}`, `[...]`).

Example stream:

```text
{"kind": "progress", "message": "configuring"}
{"kind": "progress", "message": "scanning module sources", "done": 12, "total": 40}
{"kind": "finished", "database": "/abs/path/to/workspace/target/build_database.json", "watch": ["mcpp.toml", "mcpp.lock", "src/**/*.cppm"], "profile-version": "0.2.0"}
```

## 4. Producer requirements

A producer:

- **MUST NOT** require a full build. It performs only what is needed to know the translation units, their arguments and the module graph: configuration and dependency scanning.
- **MUST** write the database before emitting `finished`, and **SHOULD** write it atomically (section 2).
- **MUST** write a database that conforms to S1 at level 1 or higher, with a `profile-version` no higher than the request's when it can write that version.
- **MUST** list in `watch` every input whose change can change the database, such as build description files, lock files and module sources whose declarations determine the graph.
- **SHOULD** emit a `progress` message whenever work takes noticeably long, so that a consumer can show that discovery is alive.
- **MUST** exit with status 0 after `finished` and with a non-zero status after `error`.

## 5. Consumer requirements

A consumer:

- **MUST** watch the `database` file and every `watch` entry, and when any of them changes, run the discovery command again or reload the database. It **SHOULD** debounce bursts of changes.
- **MUST** treat the end of the stream without a terminal message, a line that is not a valid message, or a `finished` message whose database cannot be read as an error.
- **MUST** bound the time a discovery command may run, **MUST** terminate the command when the bound expires and treat the expiry as an error. A default of 120 seconds is RECOMMENDED; progress messages **MAY** extend the bound.
- **MUST** ignore unknown fields in messages and **MUST** treat a message of unknown `kind` that is not the last line as progress without a message.
- **SHOULD** keep using the last database that loaded successfully while discovery is running or after it fails, and tell the user that the model may be stale.

## 6. Security considerations

- A discovery command is an arbitrary program chosen by project content or user configuration. A consumer **MUST** run it only in a workspace the user trusts.
- A consumer **MUST NOT** pass secrets in the request and **SHOULD** start the command with the environment of the editor session, not with extra credentials.
- Paths returned in `database` and `watch` may point outside the workspace. A consumer **MUST** only read and watch them, never write to them.
