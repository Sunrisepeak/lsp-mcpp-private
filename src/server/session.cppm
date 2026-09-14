// The language server session: one event loop feeding one or more workspace roots (usable plan
// W9.1), from the editor's standard input, each root's engine output, and background model loads
// (design sections 12.3 and 12.5). SessionOptions and the per-root WorkspaceRoot type live in
// lspmcpp.server.workspace, re-exported here so a caller needs only this one import.
export module lspmcpp.server.session;

import std;
export import lspmcpp.server.workspace;

export namespace lspmcpp::server {

// Serves the Language Server Protocol on standard input and output until the
// client exits. Returns the process exit code.
int run_session(const SessionOptions& options);

} // namespace lspmcpp::server
