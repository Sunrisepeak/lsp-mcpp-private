#!/usr/bin/env python3
"""THROWAWAY SPIKE — not production code.

"No compiler, no build system" provider (experiments E15/E16): infer a
clangd-ready compile database for loose C++ module sources from a headers-only
semantic kit described by a kit.json manifest.

    infer_cdb.py <project_dir> <kit_root> <out_dir> [--std c++23]

kit.json (paths relative to the kit root):

    {
      "kit-version": 1,
      "target": "x86_64-w64-mingw32",
      "stdlib": { "name": "libc++", "version": "23.1.0",
                  "module-metadata": "x86_64-w64-mingw32/lib/libc++.modules.json" },
      "system-include-directories": ["generic-w64-mingw32/include/c++/v1",
                                     "generic-w64-mingw32/include"],
      "sysroot": null,
      "arguments": ["-nostdinc++", "-nostdlibinc"]
    }

The driver path written into the database deliberately does not exist:
clangd only needs it to pick the driver mode, it never executes it.
"""
import argparse, glob, json, os, re

IMPORTABLE_RE = re.compile(r"^\s*(export\s+module\s+[\w.]+(:[\w.]+)?|module\s+[\w.]+:[\w.]+)\s*;", re.M)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("project"), ap.add_argument("kit"), ap.add_argument("out")
    ap.add_argument("--std", default="c++23")
    a = ap.parse_args()
    proj, kit = os.path.abspath(a.project), os.path.abspath(a.kit)
    spec = json.load(open(os.path.join(kit, "kit.json")))
    at = lambda p: os.path.normpath(os.path.join(kit, p))
    base = ["/nonexistent/lsp-mcpp-semantic-kit/clang++", "--no-default-config",
            f"--target={spec['target']}", f"-std={a.std}", *spec.get("arguments", [])]
    for d in spec["system-include-directories"]:
        base += ["-isystem", at(d)]
    if spec.get("sysroot"):
        base.append(f"--sysroot={at(spec['sysroot'])}")
    cdb = []
    sources = [p for ext in ("cpp", "cc", "cxx", "cppm", "ccm", "cxxm", "ixx")
               for p in glob.glob(f"{proj}/**/*.{ext}", recursive=True)
               if "/.lsp-mcpp/" not in p and "/target/" not in p]
    for src in sorted(sources):
        with open(src, errors="replace") as f:
            text = re.sub(r"//.*?$|/\*.*?\*/", "", f.read(), flags=re.S | re.M)
        mode = ["-x", "c++-module"] if IMPORTABLE_RE.search(text) else []
        cdb.append({"directory": proj, "file": src, "arguments": base + mode + ["-c", src]})
    manifest = at(spec["stdlib"]["module-metadata"])
    mdir = os.path.dirname(manifest)
    std_units = 0
    for m in json.load(open(manifest))["modules"]:
        src = os.path.normpath(os.path.join(mdir, m["source-path"]))
        extra = []
        for d in m.get("local-arguments", {}).get("system-include-directories", []):
            extra += ["-isystem", os.path.normpath(os.path.join(mdir, d))]
        cdb.append({"directory": proj, "file": src,
                    "arguments": base + extra + ["-Wno-reserved-module-identifier", "-x", "c++-module", "-c", src]})
        std_units += 1
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "compile_commands.json"), "w") as f:
        json.dump(cdb, f, indent=1)
    print(f"inferred {len(cdb) - std_units} project units + {std_units} std units "
          f"(target {spec['target']}, {spec['stdlib']['name']}) -> {a.out}/compile_commands.json")


if __name__ == "__main__":
    main()
