#!/usr/bin/env python3
"""Split release payloads into xlings-res artifacts and render the xpkg descriptors.

    xlings_artifacts.py --payloads DIR --out DIR [--version V] [--kit-version K] [--clangd-version C]

DIR holds payload-<platform>.tar.gz from CI (each with payload/ inside). For each
platform this writes, following the XLINGS_RES naming convention
`{name}-{version}-{os}-{arch}.tar.gz`:

    mcpp-language-server-<V>-<os>-<arch>.tar.gz   mcpp-language-server-<V>-<os>-<arch>/bin/mcppls[.exe], LICENSE
    mcppls-kit-<K>-<os>-<arch>.tar.gz    mcppls-kit-<K>-<os>-<arch>/kit.json, ...

and renders packaging/xlings/*.lua.in into mcpp-language-server.lua and mcppls-kit.lua with
the sha256 of each archive. Versions default to mcpp.toml and payload.lock.json.
"""
import argparse
import hashlib
import io
import json
import os
import re
import shutil
import sys
import tarfile
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
PLATFORMS = {
    "linux-x64": ("linux", "x86_64"),
    "darwin-arm64": ("macosx", "aarch64"),
    "win32-x64": ("windows", "x86_64"),
}


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def package_version():
    with open(os.path.join(ROOT, "mcpp.toml"), encoding="utf-8") as stream:
        match = re.search(r'^version\s*=\s*"([^"]+)"', stream.read(), re.M)
    return match.group(1)


def lock():
    with open(os.path.join(ROOT, "packaging", "payload.lock.json"), encoding="utf-8") as stream:
        return json.load(stream)


def write_archive(out_path, root_name, entries):
    """entries: list of (source path, archive-relative path)."""
    with tarfile.open(out_path, "w:gz") as archive:
        for source, relative in entries:
            archive.add(source, arcname=f"{root_name}/{relative}", recursive=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--payloads", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--version")
    parser.add_argument("--kit-version")
    parser.add_argument("--clangd-version")
    args = parser.parse_args()

    version = args.version or package_version()
    locked = lock()
    kit_version = args.kit_version or locked.get("libcxx-version")
    clangd_version = args.clangd_version or locked.get("clangd-version")
    os.makedirs(args.out, exist_ok=True)
    values = {"@VERSION@": version, "@KIT_VERSION@": kit_version, "@CLANGD_VERSION@": clangd_version}

    for platform, (os_name, arch) in PLATFORMS.items():
        tarball = os.path.join(args.payloads, f"payload-{platform}.tar.gz")
        if not os.path.isfile(tarball):
            sys.exit(f"xlings_artifacts: missing {tarball}")
        with tempfile.TemporaryDirectory() as scratch:
            with tarfile.open(tarball) as archive:
                archive.extractall(scratch, filter="data") if hasattr(tarfile, "data_filter") else archive.extractall(scratch)
            payload = os.path.join(scratch, "payload")
            exe = ".exe" if platform == "win32-x64" else ""
            server_name = f"mcpp-language-server-{version}-{os_name}-{arch}"
            server_archive = os.path.join(args.out, f"{server_name}.tar.gz")
            license_file = os.path.join(ROOT, "LICENSE")
            write_archive(server_archive, server_name, [(os.path.join(payload, "bin", f"mcppls{exe}"), f"bin/mcppls{exe}"),
                                                        (license_file, "LICENSE")])
            kit_name = f"mcppls-kit-{kit_version}-{os_name}-{arch}"
            kit_archive = os.path.join(args.out, f"{kit_name}.tar.gz")
            kit_dir = os.path.join(payload, "kit")
            write_archive(kit_archive, kit_name, [(os.path.join(kit_dir, name), name) for name in sorted(os.listdir(kit_dir))])
        key = f"{os_name}_{arch}".upper()
        values[f"@SHA256_{key}@"] = sha256(server_archive)
        values[f"@KIT_SHA256_{key}@"] = sha256(kit_archive)
        print(f"{platform}: {os.path.basename(server_archive)} {values[f'@SHA256_{key}@']}")
        print(f"{platform}: {os.path.basename(kit_archive)} {values[f'@KIT_SHA256_{key}@']}")

    for template in ("mcpp-language-server.lua.in", "mcppls-kit.lua.in"):
        with open(os.path.join(ROOT, "packaging", "xlings", template), encoding="utf-8") as stream:
            text = stream.read()
        for placeholder, value in values.items():
            text = text.replace(placeholder, value)
        leftover = re.findall(r"@[A-Z0-9_]+@", text)
        if leftover:
            sys.exit(f"xlings_artifacts: unrendered {sorted(set(leftover))} in {template}")
        target = os.path.join(args.out, template[: -len(".in")])
        with open(target, "w", encoding="utf-8") as stream:
            stream.write(text)
        print(f"rendered {target}")


if __name__ == "__main__":
    main()
