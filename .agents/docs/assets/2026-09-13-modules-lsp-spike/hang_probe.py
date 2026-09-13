#!/usr/bin/env python3
"""THROWAWAY SPIKE: reproduces experiment E13 (clangd request hang when a module
interface has an unresolvable import).

    hang_probe.py <clangd> <project_dir_with_raw_compile_commands> <log_file>

Run it on the gcc@16.1.0 copy produced by run.sh, using the *raw* build CDB.
"""
import json, os, pathlib, queue, subprocess, sys, threading, time

clangd, proj, log = sys.argv[1], sys.argv[2], sys.argv[3]
uri = lambda p: pathlib.Path(p).as_uri()
MAIN, GREET = f"{proj}/src/main.cpp", f"{proj}/src/greet/greet.cppm"
p = subprocess.Popen([clangd, "--log=verbose", f"--compile-commands-dir={proj}", "--experimental-modules-support", "--use-dirty-headers"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=open(log, "w"))
q, diags = queue.Queue(), {}
def rd():
    while True:
        h = {}
        while True:
            l = p.stdout.readline()
            if not l: return
            l = l.decode().strip()
            if not l: break
            k, v = l.split(":", 1); h[k.lower()] = v.strip()
        m = json.loads(p.stdout.read(int(h["content-length"])))
        if m.get("method") == "textDocument/publishDiagnostics": diags[m["params"]["uri"]] = (time.time(), len(m["params"]["diagnostics"]))
        elif "method" in m and "id" in m: send({"jsonrpc": "2.0", "id": m["id"], "result": None})
        else: q.put(m)
def send(m):
    d = json.dumps(m).encode(); p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(d) + d); p.stdin.flush()
threading.Thread(target=rd, daemon=True).start()
T0 = time.time()
def req(i, method, params, to=30):
    send({"jsonrpc": "2.0", "id": i, "method": method, "params": params}); end = time.time() + to
    while time.time() < end:
        try: m = q.get(timeout=0.2)
        except queue.Empty: continue
        if m.get("id") == i: return f"answered after {time.time()-T0:.1f}s"
    return f"NO ANSWER within {to}s"
print("init:", req(1, "initialize", {"processId": os.getpid(), "rootUri": uri(proj), "capabilities": {}}))
send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {"uri": uri(MAIN), "languageId": "cpp", "version": 1, "text": open(MAIN).read()}}})
print("completion main.cpp:", req(2, "textDocument/completion", {"textDocument": {"uri": uri(MAIN)}, "position": {"line": 4, "character": 31}}))
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {"uri": uri(GREET), "languageId": "cpp", "version": 1, "text": open(GREET).read() + "\n"}}})
time.sleep(10)
print("diagnostics seen:", {k.split('/')[-1]: f"{v[1]} diags at {v[0]-T0:.1f}s" for k, v in diags.items()})
print("completion main.cpp after opening greet.cppm:", req(3, "textDocument/completion", {"textDocument": {"uri": uri(MAIN)}, "position": {"line": 4, "character": 31}}))
print("hover greet.cppm:", req(4, "textDocument/hover", {"textDocument": {"uri": uri(GREET)}, "position": {"line": 5, "character": 20}}))
p.kill()
