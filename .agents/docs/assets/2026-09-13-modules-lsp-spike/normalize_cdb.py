#!/usr/bin/env python3
"""THROWAWAY SPIKE — not production code.

Normalize a build-produced compile_commands.json (mcpp / CMake, GCC or Clang
dialect) into a clangd-ready, compiler-neutral CDB for C++20 named modules.

    normalize_cdb.py <in_compile_commands.json> <out_dir> [--clang PATH]

Then run:  clangd --experimental-modules-support --use-dirty-headers \
                  --compile-commands-dir=<out_dir>

Pipeline (mirrors the design doc's "normalizer" layer):
  1. expand @response files (CMake .modmap for Clang)          -> raw argv
  2. detect build dialect from argv[0]                          -> gcc | clang | msvc
  3. strip build-BMI / scan flags (IDE must not consume build BMIs)
  4. translate GCC dialect to the clang driver, keep semantic options,
     make target/stdlib explicit so clang parses the *same* libstdc++
     (Linux: --gcc-install-dir; MinGW: --sysroot at the toolchain root)
  5. mark importable units (`export module` / `module X:Y`) with -x c++-module
  6. inject std / std.compat units from the toolchain's *.modules.json (P3286 shape)

Known simplifications (see design doc): MSVC dialect not implemented; one std
unit per output (real design: one per semantic-options group); GCC mapper files
are ignored (module graph is rediscovered by clangd's scanner).
"""
import json, os, re, shlex, shutil, subprocess, sys

GCC_STRIP_EXACT = {"-fmodules-ts", "-fmodules", "-fmodule-only", "-fmodule-lazy", "-fno-module-lazy"}
GCC_STRIP_PREFIX = ("-fmodule-mapper=", "-fdeps-format=", "-fdeps-file=", "-fdeps-target=",
                    "-fmodule-header", "-flang-info-")
CLANG_STRIP_PREFIX = ("-fmodule-file=", "-fprebuilt-module-path=", "-fmodule-output",
                      "-fmodules-reduced-bmi", "-fexperimental-modules-reduced-bmi")
CLANG_STRIP_EXACT = {"--precompile"}
TAKES_VALUE = {"-o", "-MF", "-MT", "-MQ", "-x"}
IMPORTABLE_RE = re.compile(r"^\s*(export\s+module\s+[\w.]+(:[\w.]+)?|module\s+[\w.]+:[\w.]+)\s*;", re.M)


def expand_response_files(args, cwd):
    out = []
    for a in args:
        if a.startswith("@"):
            path = os.path.join(cwd, a[1:])
            with open(path) as f:
                out += expand_response_files(shlex.split(f.read()), cwd)
        else:
            out.append(a)
    return out


def dialect(argv0):
    name = os.path.basename(argv0).lower()
    if name in ("cl", "cl.exe") or "clang-cl" in name:
        return "msvc"
    if "clang" in name:
        return "clang"
    if re.search(r"(^|-)(g\+\+|gcc|c\+\+)(-[\d.]+)?(\.exe)?$", name):
        return "gcc"
    return "unknown"


def query(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout.strip()


def is_importable(path):
    try:
        with open(path, errors="replace") as f:
            text = re.sub(r"//.*?$|/\*.*?\*/", "", f.read(), flags=re.S | re.M)
        return bool(IMPORTABLE_RE.search(text))
    except OSError:
        return False


def semantic_args(entry, clang):
    cwd = entry["directory"]
    args = entry.get("arguments") or shlex.split(entry["command"])
    args = expand_response_files(args, cwd)
    kind = dialect(args[0])
    if kind == "msvc":
        raise NotImplementedError("MSVC dialect: see design doc (clang-cl + std.ixx), not in spike")
    src = os.path.normpath(os.path.join(cwd, entry["file"]))
    # GCC commands are re-targeted to a clang driver; Clang commands keep their own
    # driver path (it may carry a clang++.cfg next to it).
    out, skip = [clang if kind == "gcc" else args[0]], False
    for a in args[1:]:
        if skip:
            skip = False
            continue
        if a in TAKES_VALUE:
            skip = True
            continue
        if a == "-c" or os.path.normpath(os.path.join(cwd, a)) == src:
            continue
        if kind == "gcc" and (a in GCC_STRIP_EXACT or a.startswith(GCC_STRIP_PREFIX) or a.startswith("-B")):
            continue
        if a in CLANG_STRIP_EXACT or a.startswith(CLANG_STRIP_PREFIX) or a.startswith("-x"):
            continue
        out.append(a)
    if kind == "gcc":
        # Make every implicit toolchain input explicit: a clang driver may ship a
        # clang++.cfg that silently selects libc++ (observed with mcpp's LLVM payload).
        target = query([args[0], "-dumpmachine"])
        out += ["--no-default-config", f"--target={target}", "-stdlib=libstdc++"]
        if "mingw" in target:
            # clang's MinGW driver ignores --gcc-install-dir (reports it unused);
            # it locates the GCC installation through --sysroot at the toolchain root.
            if not any(a.startswith("--sysroot") for a in out):
                out.append("--sysroot=" + os.path.normpath(os.path.join(os.path.dirname(args[0]), "..")))
        else:
            install_dir = os.path.dirname(query([args[0], "-print-libgcc-file-name"]))
            out.append(f"--gcc-install-dir={install_dir}")
        manifest = query([args[0], "-print-file-name=libstdc++.modules.json"])
    else:
        manifest = query([args[0], "-print-library-module-manifest-path"])
    return kind, src, out, manifest


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    in_cdb, out_dir = sys.argv[1], os.path.abspath(sys.argv[2])
    clang = sys.argv[sys.argv.index("--clang") + 1] if "--clang" in sys.argv else shutil.which("clang++")
    entries = json.load(open(in_cdb))
    result, template, manifest, proj_dir = [], None, None, None
    for e in entries:
        kind, src, args, manifest_ = semantic_args(e, clang)
        template, manifest, proj_dir = template or args, manifest or manifest_, proj_dir or e["directory"]
        mode = ["-x", "c++-module"] if is_importable(src) else []
        result.append({"directory": e["directory"], "file": src, "arguments": args + mode + ["-c", src]})
    if manifest and os.path.isfile(manifest):
        base = os.path.dirname(manifest)
        for m in json.load(open(manifest)).get("modules", []):
            src = os.path.normpath(os.path.join(base, m["source-path"]))
            extra = []
            for d in m.get("local-arguments", {}).get("system-include-directories", []):
                extra += ["-isystem", os.path.normpath(os.path.join(base, d))]
            result.append({"directory": proj_dir, "file": src,
                           "arguments": template + extra + ["-Wno-reserved-module-identifier",
                                                            "-x", "c++-module", "-c", src]})
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "compile_commands.json"), "w") as f:
        json.dump(result, f, indent=1)
    print(f"normalized {len(entries)} entries (+{len(result) - len(entries)} std units) "
          f"-> {out_dir}/compile_commands.json; std manifest: {manifest}")


if __name__ == "__main__":
    main()
