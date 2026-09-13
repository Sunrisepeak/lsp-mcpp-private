#!/usr/bin/env python3
"""Reduce an official clangd release archive to what the payload ships.

    trim_clangd.py --platform linux-x64 --out DIR [--zip clangd-linux-23.1.0.zip] [--cache DIR] [--no-strip]

Keeps bin/clangd[.exe], lib/clang/<major>/include (clang's builtin headers,
which clangd finds relative to its own executable) and LICENSE.TXT. Everything
else in the release -- sanitizer runtimes, lib/clang/<major>/lib and share -- is
never used by clangd and is dropped.

    linux-x64      symbols are stripped when `strip` or `llvm-strip` is on PATH
    darwin-arm64   the universal binary is thinned to arm64 with `lipo`, stripped
                   with `strip -x` and re-signed ad hoc; this needs a macOS host
    win32-x64      files are selected only

Without --zip the archive recorded for the platform in payload.lock.json is
fetched and verified first.
"""
import argparse
import os
import platform as host_platform
import shutil
import stat
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fetch  # noqa: E402  (same directory)

PLATFORMS = ("linux-x64", "win32-x64", "darwin-arm64")


def executable_name(platform):
    return "clangd.exe" if platform == "win32-x64" else "clangd"


def run(command):
    print("trim_clangd: " + " ".join(command), file=sys.stderr)
    subprocess.run(command, check=True)


def extract(archive, out, platform):
    exe = executable_name(platform)
    kept = 0
    with zipfile.ZipFile(archive) as zf:
        names = zf.namelist()
        roots = {name.split("/", 1)[0] for name in names if "/" in name}
        if len(roots) != 1:
            raise SystemExit(f"trim_clangd: expected one top-level directory in {archive}, found {sorted(roots)}")
        root = roots.pop() + "/"
        majors = sorted({name[len(root):].split("/")[2] for name in names
                         if name.startswith(root + "lib/clang/") and name.count("/") >= 4})
        if len(majors) != 1:
            raise SystemExit(f"trim_clangd: expected one lib/clang/<major> in {archive}, found {majors}")
        include_prefix = f"{root}lib/clang/{majors[0]}/include/"
        wanted_files = {root + "bin/" + exe, root + "LICENSE.TXT"}
        for info in zf.infolist():
            name = info.filename
            if info.is_dir():
                continue
            if name not in wanted_files and not name.startswith(include_prefix):
                continue
            relative = name[len(root):]
            target = os.path.join(out, *relative.split("/"))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with zf.open(info) as source, open(target, "wb") as sink:
                shutil.copyfileobj(source, sink, length=1 << 20)
            kept += 1
    binary = os.path.join(out, "bin", exe)
    if not os.path.isfile(binary):
        raise SystemExit(f"trim_clangd: {archive} has no bin/{exe}")
    if platform != "win32-x64":
        mode = os.stat(binary).st_mode
        os.chmod(binary, mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return binary, majors[0], kept


def strip_linux(binary):
    tool = shutil.which("llvm-strip") or shutil.which("strip")
    if not tool:
        print("trim_clangd: no strip tool on PATH; the binary keeps its symbols", file=sys.stderr)
        return False
    run([tool, "--strip-all", binary])
    return True


def thin_and_strip_macos(binary):
    if sys.platform != "darwin":
        raise SystemExit("trim_clangd: darwin-arm64 needs a macOS host (lipo, strip -x and codesign)")
    thin = binary + ".arm64"
    run(["lipo", "-thin", "arm64", binary, "-output", thin])
    os.replace(thin, binary)
    run(["strip", "-x", binary])
    # Modifying a Mach-O invalidates its signature, and arm64 macOS refuses to
    # run an arm64 executable without a valid one.
    run(["codesign", "--force", "--sign", "-", binary])


def dir_size(path):
    total = 0
    for base, _, files in os.walk(path):
        for name in files:
            total += os.path.getsize(os.path.join(base, name))
    return total


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--platform", required=True, choices=PLATFORMS)
    parser.add_argument("--out", required=True)
    parser.add_argument("--zip", help="clangd release archive; fetched from the lock when omitted")
    parser.add_argument("--cache", default=fetch.DEFAULT_CACHE)
    parser.add_argument("--no-strip", action="store_true")
    args = parser.parse_args()

    lock = fetch.load_lock()
    archive = args.zip or fetch.fetch(lock["platforms"][args.platform]["clangd"], cache=args.cache, lock=lock)
    out = os.path.abspath(args.out)
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    binary, major, kept = extract(archive, out, args.platform)

    if not args.no_strip:
        if args.platform == "linux-x64":
            strip_linux(binary)
        elif args.platform == "darwin-arm64":
            thin_and_strip_macos(binary)

    host_runs_it = ((args.platform == "linux-x64" and sys.platform.startswith("linux") and host_platform.machine() in ("x86_64", "AMD64"))
                    or (args.platform == "darwin-arm64" and sys.platform == "darwin" and host_platform.machine() == "arm64")
                    or (args.platform == "win32-x64" and sys.platform == "win32"))
    if host_runs_it:
        version = subprocess.run([binary, "--version"], check=True, capture_output=True, text=True).stdout.strip()
        print(f"trim_clangd: {version.splitlines()[0] if version else 'clangd runs'}", file=sys.stderr)
    print(f"trim_clangd: {kept} files, lib/clang/{major}/include, {dir_size(out) / 1e6:.1f} MB -> {out}", file=sys.stderr)
    print(out)


if __name__ == "__main__":
    main()
