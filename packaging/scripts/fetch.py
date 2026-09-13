#!/usr/bin/env python3
"""Download inputs recorded in packaging/payload.lock.json, verified by sha256.

    fetch.py <entry> [<entry> ...] [--cache DIR] [--lock FILE]
    fetch.py --all [--cache DIR]
    fetch.py --platform linux-x64 [--cache DIR]      # the inputs one platform needs

Prints the absolute path of each verified file, one per line. A cached file
whose hash matches is not downloaded again; a mismatching one is replaced.
Standard library only, so it runs unchanged on every CI runner.
"""
import argparse
import hashlib
import json
import os
import shutil
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_LOCK = os.path.normpath(os.path.join(HERE, "..", "payload.lock.json"))
DEFAULT_CACHE = os.environ.get("LSP_MCPP_PAYLOAD_CACHE") or os.path.join(
    os.path.expanduser("~"), ".cache", "lsp-mcpp-payload")
PLATFORMS = ("linux-x64", "win32-x64", "darwin-arm64")


def load_lock(path=DEFAULT_LOCK):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def inputs_for_platform(lock, platform):
    if platform not in lock["platforms"]:
        raise SystemExit(f"unknown platform {platform!r}; known: {', '.join(lock['platforms'])}")
    spec = lock["platforms"][platform]
    return [spec["clangd"], spec["kit"]["source"]]


def fetch(name, cache=DEFAULT_CACHE, lock=None, retries=4, quiet=False):
    """Returns the path of the verified file for lock entry `name`."""
    lock = lock or load_lock()
    if name not in lock["entries"]:
        raise SystemExit(f"unknown lock entry {name!r}; known: {', '.join(lock['entries'])}")
    entry = lock["entries"][name]
    os.makedirs(cache, exist_ok=True)
    target = os.path.join(cache, entry["file"])
    if os.path.isfile(target):
        if sha256_of(target) == entry["sha256"]:
            return os.path.abspath(target)
        if not quiet:
            print(f"fetch: cached {entry['file']} does not match the lock; downloading again", file=sys.stderr)
        os.remove(target)

    partial = target + ".partial"
    last_error = None
    for attempt in range(1, retries + 1):
        try:
            if not quiet:
                print(f"fetch: {entry['url']} (attempt {attempt})", file=sys.stderr)
            request = urllib.request.Request(entry["url"], headers={"User-Agent": "lsp-mcpp-packaging"})
            with urllib.request.urlopen(request, timeout=120) as response, open(partial, "wb") as out:
                shutil.copyfileobj(response, out, length=1 << 20)
            actual = sha256_of(partial)
            if actual != entry["sha256"]:
                raise ValueError(f"sha256 mismatch for {entry['file']}: expected {entry['sha256']}, got {actual}")
            if "size" in entry and os.path.getsize(partial) != entry["size"]:
                raise ValueError(f"size mismatch for {entry['file']}")
            os.replace(partial, target)
            return os.path.abspath(target)
        except Exception as error:  # network errors and verification failures are retried alike
            last_error = error
            if os.path.exists(partial):
                os.remove(partial)
            if attempt < retries:
                time.sleep(min(30, 3 * attempt))
    raise SystemExit(f"fetch: giving up on {name}: {last_error}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("entries", nargs="*")
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--platform", choices=PLATFORMS)
    parser.add_argument("--cache", default=DEFAULT_CACHE)
    parser.add_argument("--lock", default=DEFAULT_LOCK)
    args = parser.parse_args()
    lock = load_lock(args.lock)
    names = list(args.entries)
    if args.all:
        names += list(lock["entries"])
    if args.platform:
        names += inputs_for_platform(lock, args.platform)
    if not names:
        parser.error("name at least one entry, --platform or --all")
    seen = set()
    for name in names:
        if name in seen:
            continue
        seen.add(name)
        print(fetch(name, cache=args.cache, lock=lock))


if __name__ == "__main__":
    main()
