#!/usr/bin/env python3
"""THROWAWAY SPIKE — not production code.

Minimal LSP client that probes clangd's C++20 modules behaviour on the
fixture project (hello + module hello.greet with partition :detail).

    lsp_probe.py <clangd> <project_dir> <cdb_dir> [extra clangd args...]

Checks (printed as PASS/FAIL lines, see experiments doc):
  C1 diagnostics of src/main.cpp are empty
  C2 go-to-definition hello::greet lands in greet.cppm
  C3 hover hello::greet names the providing module unit
  C4 go-to-definition std::println lands in the toolchain's own std library
  C5 go-to-definition on the module name in `import hello.greet;`
  C6 completion after `hello::` offers greet
  C7 unsaved edit in greet.cppm (adds greet2) is visible to main.cpp
  C8 saved edit in greet.cppm is visible to main.cpp
  C9 find-references of hello::greet spans main.cpp and greet.cppm
The fixture's greet.cppm is restored on exit.
"""
import json, os, pathlib, queue, subprocess, sys, threading, time

clangd, proj, cdb_dir = sys.argv[1], os.path.abspath(sys.argv[2]), os.path.abspath(sys.argv[3])
extra = sys.argv[4:]
MAIN = os.path.join(proj, "src/main.cpp")
GREET = os.path.join(proj, "src/greet/greet.cppm")
uri = lambda p: pathlib.Path(p).as_uri()

proc = subprocess.Popen([clangd, "--log=error", f"--compile-commands-dir={cdb_dir}", *extra],
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
inbox, diags, lock = queue.Queue(), {}, threading.Lock()


def send(msg):
    data = json.dumps(msg).encode()
    with lock:
        proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(data) + data)
        proc.stdin.flush()


def reader():
    while True:
        headers = {}
        while True:
            line = proc.stdout.readline()
            if not line:
                return
            line = line.decode().strip()
            if not line:
                break
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()
        msg = json.loads(proc.stdout.read(int(headers["content-length"])))
        if msg.get("method") == "textDocument/publishDiagnostics":
            diags[msg["params"]["uri"]] = msg["params"]["diagnostics"]
        elif "method" in msg and msg.get("id") is not None:
            send({"jsonrpc": "2.0", "id": msg["id"], "result": None})
        else:
            inbox.put(msg)


threading.Thread(target=reader, daemon=True).start()
_id = [0]


TIMEOUT = float(os.environ.get("PROBE_TIMEOUT", "180"))  # clangd 23.1 may never answer (E13)


def request(method, params, timeout=TIMEOUT):
    _id[0] += 1
    rid = _id[0]
    send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
    end = time.time() + timeout
    while time.time() < end:
        try:
            msg = inbox.get(timeout=0.5)
        except queue.Empty:
            continue
        if msg.get("id") == rid:
            return msg.get("result")
    return None


def notify(method, params):
    send({"jsonrpc": "2.0", "method": method, "params": params})


def wait_diags(u, timeout=TIMEOUT):
    end = time.time() + timeout
    while time.time() < end:
        if u in diags:
            return diags.pop(u)
        time.sleep(0.1)
    return None


def locs(res):
    res = res if isinstance(res, list) else ([res] if res else [])
    return [f"{(l.get('uri') or l.get('targetUri')).replace('file://', '')}:"
            f"{(l.get('range') or l.get('targetSelectionRange'))['start']['line'] + 1}" for l in res]


def labels(res):
    items = (res or {}).get("items", []) if isinstance(res, dict) else (res or [])
    return sorted({i["label"].strip() for i in items})


def report(check, ok, detail):
    print(f"{check} {'PASS' if ok else 'FAIL'}  {detail}")


doc = lambda p: {"textDocument": {"uri": uri(p)}}
main_text, greet_text = open(MAIN).read(), open(GREET).read()
try:
    t0 = time.time()
    request("initialize", {"processId": os.getpid(), "rootUri": uri(proj),
                           "capabilities": {"textDocument": {"hover": {"contentFormat": ["plaintext"]}}}})
    notify("initialized", {})
    notify("textDocument/didOpen", {"textDocument": {"uri": uri(MAIN), "languageId": "cpp", "version": 1,
                                                     "text": main_text}})
    d = wait_diags(uri(MAIN))
    report("C1", d == [], f"{time.time() - t0:.1f}s first diagnostics: {[x['message'] for x in d or []]}")

    r = locs(request("textDocument/definition", {**doc(MAIN), "position": {"line": 4, "character": 31}}))
    report("C2", any("greet.cppm:" in x for x in r), r)
    h = request("textDocument/hover", {**doc(MAIN), "position": {"line": 4, "character": 31}}) or {}
    hv = h.get("contents", {}).get("value", "") if isinstance(h, dict) else ""
    report("C3", "greet.cppm" in hv, hv.splitlines()[:3])
    r = locs(request("textDocument/definition", {**doc(MAIN), "position": {"line": 4, "character": 10}}))
    report("C4", len(r) > 0, r)
    r = locs(request("textDocument/definition", {**doc(MAIN), "position": {"line": 1, "character": 9}}))
    report("C5", any("greet.cppm" in x for x in r), r)

    lines = main_text.splitlines()
    probe = "\n".join(lines[:5] + ["    hello::"] + lines[5:]) + "\n"
    notify("textDocument/didChange", {"textDocument": {"uri": uri(MAIN), "version": 2},
                                      "contentChanges": [{"text": probe}]})
    c = labels(request("textDocument/completion", {**doc(MAIN), "position": {"line": 5, "character": 11}}))
    report("C6", any(x.startswith("greet(") for x in c), c)

    dirty = greet_text.replace("export namespace hello {", "export namespace hello {\n  int greet2() { return 2; }")
    notify("textDocument/didOpen", {"textDocument": {"uri": uri(GREET), "languageId": "cpp", "version": 1,
                                                     "text": dirty}})
    wait_diags(uri(GREET))
    notify("textDocument/didChange", {"textDocument": {"uri": uri(MAIN), "version": 3},
                                      "contentChanges": [{"text": probe + " "}]})
    time.sleep(1)
    c = labels(request("textDocument/completion", {**doc(MAIN), "position": {"line": 5, "character": 11}}))
    report("C7", "greet2()" in c, c)

    # C8 must not be satisfied by the open buffer: discard it, then change the file on disk.
    notify("textDocument/didClose", doc(GREET))
    time.sleep(1)
    with open(GREET, "w") as f:
        f.write(dirty)
    notify("textDocument/didSave", doc(GREET))
    notify("workspace/didChangeWatchedFiles", {"changes": [{"uri": uri(GREET), "type": 2}]})
    notify("textDocument/didChange", {"textDocument": {"uri": uri(MAIN), "version": 4},
                                      "contentChanges": [{"text": probe + "  "}]})
    time.sleep(2)
    c = labels(request("textDocument/completion", {**doc(MAIN), "position": {"line": 5, "character": 11}}))
    report("C8", "greet2()" in c, c)

    r = locs(request("textDocument/references", {**doc(MAIN), "position": {"line": 4, "character": 31},
                                                 "context": {"includeDeclaration": True}}))
    report("C9", any("main.cpp" in x for x in r) and any("greet.cppm" in x for x in r), r)
    request("shutdown", None, timeout=10)
    notify("exit", None)
finally:
    with open(GREET, "w") as f:
        f.write(greet_text)
    proc.kill()
