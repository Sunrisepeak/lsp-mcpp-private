# mcppls with coding agents

mcppls integrates with coding agents through standard protocols: LSP today, for
editor-style navigation and post-edit diagnostics; MCP once the query layer lands, for
structured, symbol-oriented queries. This page covers what is available now and what
to expect next. Background: `.agents/docs/2026-09-15-mcppls-overall-design.md` §8.2,
§10.2, work items A1/A2.

## Requirement: `mcppls` on PATH

Every integration below starts the server as `mcppls serve` (LSP over stdio) — the
`mcppls` binary must resolve on `PATH` wherever the agent runs.

- **Once published**, install it through xlings: `xlings install mcpp-language-server -y -g`
  (xlings package `mcpp-language-server`, program `mcppls`). Not yet published as of this
  writing — see the design doc §12 for the rename and release plan.
- **Or build it locally**: `mcpp build` from a clone of this repository, binary at
  `target/<triple>/<fingerprint>/bin/mcppls` (for example
  `target/x86_64-linux-gnu/<fingerprint>/bin/mcppls` on Linux); put that `bin/` directory
  on `PATH`.
- **Or assemble a runnable payload** (server + clangd + a semantic kit) as described in
  `../../packaging/README.md`, and put its `bin/` on `PATH`.

## Claude Code

The plugin at `../claude-code/mcppls-lsp` (marketplace `mcppls`, `editors/claude-code`)
registers `mcppls serve` as the language server for C and C++ sources, including the
C++20/23 module extensions (`.cppm`, `.ccm`, `.cxxm`, `.c++m`, `.ixx`, `.mpp`, `.mxx`) next
to the traditional ones. It **replaces** the official `clangd-lsp` code-intelligence
plugin for a project — the two should not both be enabled; see that plugin's own README.

From a clone of this repository:

```
git clone https://github.com/Sunrisepeak/lsp-mcpp-private
/plugin marketplace add lsp-mcpp-private/editors/claude-code
/plugin install mcppls-lsp@mcppls
```

If this repository is already the workspace open in Claude Code, the marketplace path is
just `editors/claude-code`. Either way, the same steps work non-interactively:

```
claude plugin marketplace add editors/claude-code
claude plugin install mcppls-lsp@mcppls
```

See `../claude-code/mcppls-lsp/README.md` for a raw-URL add that skips cloning, and for
`claude plugin validate`, which checks both manifests without installing anything.

Once enabled and `mcppls` is on `PATH`, Claude Code gets automatic diagnostics after every
edit and LSP-based navigation (definition, references, hover, document symbols,
implementations, call hierarchy) across modules and partitions, the same way it does for
any other code-intelligence plugin.

## GitHub Copilot CLI

`../copilot-cli/lsp.json` registers `mcppls serve` the same way, in Copilot CLI's own
schema (`lspServers.<name>.command` / `args` / `fileExtensions` / ...), documented at
<https://docs.github.com/en/copilot/how-tos/copilot-cli/set-up-copilot-cli/add-lsp-servers>.
Use it one of two ways:

- **Per project**: copy it to `.github/lsp.json` in the project you want mcppls in.

  ```
  cp editors/copilot-cli/lsp.json /path/to/project/.github/lsp.json
  ```

- **For yourself, everywhere**: merge its `lspServers.mcppls` entry into
  `~/.copilot/lsp-config.json`.

Once the directory is trusted, Copilot CLI starts `mcppls` in the background and uses it
for definitions, references, hover, rename, document symbols, workspace symbols,
implementations and call hierarchy — the same eight operations it uses any other language
server for.

## Any MCP client

`mcppls mcp` is planned (design §7.6, roadmap item A6) but **not available yet** — it
arrives with the agent query layer (`ai/query`, items A4/A5). Once it exists, any MCP
client will be able to reach mcppls the way it reaches other MCP servers, with no
project-specific LSP registration needed. There is nothing to configure for this today;
this section is a placeholder until A6 lands.
