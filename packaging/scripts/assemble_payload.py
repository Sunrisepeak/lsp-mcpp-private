#!/usr/bin/env python3
"""Put the server, clangd and the semantic kit into the payload layout.

    assemble_payload.py --platform linux-x64 --server target/.../bin/lsp-mcpp \\
                        --clangd DIR --kit DIR --out editors/vscode/payload
    assemble_payload.py --verify editors/vscode/payload

The layout is the contract the server and the VS Code extension rely on:

    <payload>/
      payload.json                  versions and relative paths of the three parts
      bin/lsp-mcpp[.exe]
      clangd/bin/clangd[.exe]
      clangd/lib/clang/<major>/include/...
      kit/kit.json + kit data        (spec S4)
      licenses/                      lsp-mcpp and LLVM license texts

--clangd is a directory produced by trim_clangd.py and --kit one produced by
build_kit.py. Assembling always ends with the same verification --verify runs.
"""
import argparse
import json
import os
import re
import shutil
import stat
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import fetch  # noqa: E402  (same directory)

PLATFORMS = ("linux-x64", "win32-x64", "darwin-arm64")
PAYLOAD_VERSION = 1


def log(message):
    print(f"assemble_payload: {message}", file=sys.stderr, flush=True)


def suffix(platform):
    return ".exe" if platform == "win32-x64" else ""


def server_version_from_manifest():
    with open(os.path.join(REPO, "mcpp.toml"), encoding="utf-8") as f:
        text = f.read()
    package = re.search(r"^\[package\](.*?)(^\[|\Z)", text, flags=re.S | re.M)
    match = re.search(r'^\s*version\s*=\s*"([^"]+)"', package.group(1) if package else "", flags=re.M)
    if not match:
        raise SystemExit("assemble_payload: no [package] version in mcpp.toml; pass --server-version")
    return match.group(1)


def make_executable(path):
    if os.name != "nt":
        os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def tree_size(path):
    total, count = 0, 0
    for base, _, files in os.walk(path):
        for name in files:
            total += os.path.getsize(os.path.join(base, name))
            count += 1
    return total, count


def verify(payload_dir):
    """Returns a list of problems; empty means the payload is complete."""
    problems = []

    def need_file(relative, what):
        path = os.path.join(payload_dir, *relative.split("/"))
        if not os.path.isfile(path):
            problems.append(f"{what} is missing: {relative}")
            return None
        return path

    manifest_path = os.path.join(payload_dir, "payload.json")
    if not os.path.isfile(manifest_path):
        return [f"payload.json is missing in {payload_dir}"]
    with open(manifest_path, encoding="utf-8") as f:
        manifest = json.load(f)
    if manifest.get("payload-version") != PAYLOAD_VERSION:
        problems.append(f"payload-version is {manifest.get('payload-version')!r}, expected {PAYLOAD_VERSION}")
    platform = manifest.get("platform")
    if platform not in PLATFORMS:
        problems.append(f"unknown platform {platform!r}")
        return problems
    exe = suffix(platform)
    for part, expected in (("server", f"bin/lsp-mcpp{exe}"), ("clangd", f"clangd/bin/clangd{exe}")):
        entry = manifest.get(part) or {}
        if entry.get("path") != expected:
            problems.append(f"{part}.path is {entry.get('path')!r}, expected {expected!r}")
        if not entry.get("version"):
            problems.append(f"{part}.version is empty")
        path = need_file(expected, part)
        if path and platform != "win32-x64" and os.name != "nt" and not os.access(path, os.X_OK):
            problems.append(f"{expected} is not executable")

    major = str(manifest.get("clangd", {}).get("version", "0")).split(".")[0]
    need_file(f"clangd/lib/clang/{major}/include/stddef.h", "clang builtin headers")

    kit_entry = manifest.get("kit") or {}
    if kit_entry.get("path") != "kit":
        problems.append(f"kit.path is {kit_entry.get('path')!r}, expected 'kit'")
    kit_json_path = need_file("kit/kit.json", "kit manifest")
    if kit_json_path:
        with open(kit_json_path, encoding="utf-8") as f:
            kit = json.load(f)
        kit_dir = os.path.dirname(kit_json_path)
        if kit.get("name") != kit_entry.get("name"):
            problems.append(f"kit name {kit.get('name')!r} differs from payload.json {kit_entry.get('name')!r}")
        if kit.get("kit-version") != 1:
            problems.append(f"kit-version is {kit.get('kit-version')!r}, expected 1")
        for directory in kit.get("system-include-directories", []):
            if not os.path.isdir(os.path.join(kit_dir, *directory.split("/"))):
                problems.append(f"kit include directory is missing: {directory}")
        if kit.get("sysroot") and not os.path.isdir(os.path.join(kit_dir, kit["sysroot"])):
            problems.append(f"kit sysroot is missing: {kit['sysroot']}")
        for license_file in kit.get("licenses", []):
            if not os.path.isfile(os.path.join(kit_dir, *license_file.split("/"))):
                problems.append(f"kit license is missing: {license_file}")
        metadata = kit.get("stdlib", {}).get("module-metadata", "")
        metadata_path = os.path.join(kit_dir, *metadata.split("/"))
        if not os.path.isfile(metadata_path):
            problems.append(f"kit module manifest is missing: {metadata}")
        else:
            with open(metadata_path, encoding="utf-8") as f:
                modules = json.load(f).get("modules", [])
            base = os.path.dirname(metadata_path)
            names = set()
            for module in modules:
                names.add(module.get("logical-name"))
                if not os.path.isfile(os.path.normpath(os.path.join(base, *module["source-path"].split("/")))):
                    problems.append(f"module {module.get('logical-name')} source is missing: {module['source-path']}")
                for directory in module.get("local-arguments", {}).get("system-include-directories", []):
                    if not os.path.isdir(os.path.normpath(os.path.join(base, *directory.split("/")))):
                        problems.append(f"module {module.get('logical-name')} include directory is missing: {directory}")
            if "std" not in names:
                problems.append("the kit's module manifest does not provide std")

    for name in ("lsp-mcpp-LICENSE.txt", "LLVM-LICENSE.TXT"):
        need_file(f"licenses/{name}", "license")

    # A VSIX cannot hold two paths that differ only in case, and neither can
    # the file systems of Windows and macOS.
    seen = {}
    for base, _, files in os.walk(payload_dir):
        for name in files:
            rel = os.path.relpath(os.path.join(base, name), payload_dir).replace(os.sep, "/")
            other = seen.setdefault(rel.lower(), rel)
            if other != rel:
                problems.append(f"paths differ only in case: {other} and {rel}")
    return problems


def assemble(args):
    exe = suffix(args.platform)
    lock = fetch.load_lock()
    out = os.path.abspath(args.out)
    for label, path, is_dir in (("--server", args.server, False), ("--clangd", args.clangd, True), ("--kit", args.kit, True)):
        if (is_dir and not os.path.isdir(path)) or (not is_dir and not os.path.isfile(path)):
            raise SystemExit(f"assemble_payload: {label} {path} does not exist")
    kit_json = os.path.join(args.kit, "kit.json")
    if not os.path.isfile(kit_json):
        raise SystemExit(f"assemble_payload: {args.kit} has no kit.json")
    with open(kit_json, encoding="utf-8") as f:
        kit = json.load(f)
    clangd_binary = os.path.join(args.clangd, "bin", f"clangd{exe}")
    if not os.path.isfile(clangd_binary):
        raise SystemExit(f"assemble_payload: {args.clangd} has no bin/clangd{exe}; is it trimmed for {args.platform}?")

    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, "bin"))
    server_target = os.path.join(out, "bin", f"lsp-mcpp{exe}")
    shutil.copyfile(args.server, server_target)
    make_executable(server_target)

    shutil.copytree(args.clangd, os.path.join(out, "clangd"), ignore=shutil.ignore_patterns("LICENSE.TXT"))
    make_executable(os.path.join(out, "clangd", "bin", f"clangd{exe}"))
    shutil.copytree(args.kit, os.path.join(out, "kit"))

    licenses = os.path.join(out, "licenses")
    os.makedirs(licenses)
    shutil.copyfile(os.path.join(REPO, "LICENSE"), os.path.join(licenses, "lsp-mcpp-LICENSE.txt"))
    clangd_license = os.path.join(args.clangd, "LICENSE.TXT")
    if not os.path.isfile(clangd_license):
        raise SystemExit(f"assemble_payload: {args.clangd} has no LICENSE.TXT")
    shutil.copyfile(clangd_license, os.path.join(licenses, "LLVM-LICENSE.TXT"))

    manifest = {
        "payload-version": PAYLOAD_VERSION,
        "platform": args.platform,
        "server": {"version": args.server_version or server_version_from_manifest(), "path": f"bin/lsp-mcpp{exe}"},
        "clangd": {"version": lock["clangd-version"], "path": f"clangd/bin/clangd{exe}"},
        "kit": {"name": kit["name"], "path": "kit"},
    }
    with open(os.path.join(out, "payload.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    host = {"linux": "linux-x64", "darwin": "darwin-arm64", "win32": "win32-x64"}.get(
        "linux" if sys.platform.startswith("linux") else sys.platform)
    if host == args.platform:
        clangd = os.path.join(out, "clangd", "bin", f"clangd{exe}")
        reported = subprocess.run([clangd, "--version"], capture_output=True, text=True).stdout
        if lock["clangd-version"] not in reported:
            raise SystemExit(f"assemble_payload: clangd reports {reported.strip()!r}, the lock says {lock['clangd-version']}")
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--platform", choices=PLATFORMS)
    parser.add_argument("--server", help="the lsp-mcpp executable built for --platform")
    parser.add_argument("--clangd", help="directory produced by trim_clangd.py")
    parser.add_argument("--kit", help="directory produced by build_kit.py")
    parser.add_argument("--out", help="payload directory to create (replaced if it exists)")
    parser.add_argument("--server-version", help="defaults to [package] version in mcpp.toml")
    parser.add_argument("--verify", metavar="PAYLOAD", help="only verify an assembled payload")
    args = parser.parse_args()

    if args.verify:
        target = os.path.abspath(args.verify)
    else:
        missing = [name for name in ("platform", "server", "clangd", "kit", "out") if not getattr(args, name)]
        if missing:
            parser.error("assembling needs " + ", ".join(f"--{m}" for m in missing))
        target = assemble(args)

    problems = verify(target)
    if problems:
        for problem in problems:
            log(f"problem: {problem}")
        raise SystemExit(1)
    size, count = tree_size(target)
    log(f"payload verified: {count} files, {size / 1e6:.1f} MB -> {target}")
    print(target)


if __name__ == "__main__":
    main()
