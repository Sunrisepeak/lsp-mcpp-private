"""Copies the payload the runner itself was given (its own --payload, via the runner's {payload}
placeholder) into the workspace, then truncates the copy's clangd executable so its size no
longer matches payload.json (usable plan W9.4). scenario.json's own server-arguments then passes
--payload pointing at this corrupted copy, overriding the runner's --payload (mcpplibs.cmdline
keeps the last of a repeated option): the server started on it should report status "error" with
issue "payload-corrupt", not silently run clangd off a broken binary or hang. Linux only.

payload.json's own "files" entries (assemble_payload.py) are filled in here for whichever of
clangd/kit.json a source payload does not already declare, computed before truncation, so the
fixture proves the check even against a payload assembled before usable plan W9.4 existed.
"""
import hashlib
import json
import os
import shutil
import sys

source = sys.argv[1] if len(sys.argv) > 1 else ""
if not source or not os.path.isdir(source):
    sys.exit("payload-corrupt: pass the runner's --payload directory (the {payload} placeholder)")

target = os.path.join(os.getcwd(), "payload-copy")
if os.path.exists(target):
    shutil.rmtree(target)
shutil.copytree(source, target)


def sha256_of(path):
    hasher = hashlib.sha256()
    size = 0
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 16), b""):
            hasher.update(block)
            size += len(block)
    return size, hasher.hexdigest()


manifest_path = os.path.join(target, "payload.json")
with open(manifest_path, encoding="utf-8") as f:
    manifest = json.load(f)
manifest.setdefault("files", {})
clangd = os.path.join(target, "clangd", "bin", "clangd")
kit_json = os.path.join(target, "kit", "kit.json")
for relative, path in (("clangd/bin/clangd", clangd), ("kit/kit.json", kit_json)):
    if relative not in manifest["files"] and os.path.isfile(path):
        size, digest = sha256_of(path)
        manifest["files"][relative] = {"size": size, "sha256": digest}
with open(manifest_path, "w", encoding="utf-8") as f:
    json.dump(manifest, f, indent=2)

# Corrupt it now, after the manifest above was computed from the still-intact file.
with open(clangd, "r+b") as f:
    f.truncate(1024)
