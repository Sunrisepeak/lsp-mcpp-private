#!/usr/bin/env python3
"""Assemble an `lsp-mcpp-kit` semantic kit (spec S4) for one platform.

    build_kit.py --platform linux-x64    --out DIR [--cache DIR] [--work DIR] [--jobs N]
    build_kit.py --platform darwin-arm64 --out DIR [...]          # on a macOS host
    build_kit.py --platform win32-x64    --out DIR [...]          # on any host

A kit is data only: the standard library headers, the `std` module sources, a
P3286-shaped module manifest, C library headers where they may be shipped, the
upstream license texts and kit.json. clangd needs nothing else to give full
module semantics on a machine without a compiler.

Recipes, chosen by payload.lock.json:

    libcxx-source  LLVM's runtimes build from llvm-project-<v>.src.tar.xz, configured
                   for libc++ and libc++abi and installed as headers and module
                   sources only (nothing is compiled). Needs cmake, ninja and a
                   working C/C++ compiler for the configure checks. linux-x64 adds
                   glibc and Linux kernel headers from the host as the sysroot;
                   darwin-arm64 declares that it requires the macOS SDK instead,
                   which Apple's license does not allow a kit to carry.
    llvm-mingw     generic-w64-mingw32/include (without *.idl, *.tlb, *.def),
                   share/libc++/v1 and the x86_64-w64-mingw32 module manifest,
                   extracted from the llvm-mingw release built on the same LLVM.

Environment overrides: CMAKE, NINJA, CC, CXX.
"""
import argparse
import fnmatch
import json
import os
import posixpath
import shutil
import subprocess
import sys
import tarfile
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fetch  # noqa: E402  (same directory)

PLATFORMS = ("linux-x64", "win32-x64", "darwin-arm64")
KIT_VERSION = 1


def log(message):
    print(f"build_kit: {message}", file=sys.stderr, flush=True)


# --------------------------------------------------------------------------
# Archive extraction: stdlib only, one streaming pass, links become copies.
# --------------------------------------------------------------------------

def _safe_relative(name):
    parts = [p for p in name.split("/") if p not in ("", ".")]
    if not parts or any(p == ".." for p in parts) or name.startswith("/"):
        return None
    return "/".join(parts)


def _write_member(archive_file, member, destination):
    os.makedirs(os.path.dirname(destination), exist_ok=True)
    source = archive_file.extractfile(member)
    if source is None:
        return False
    with source, open(destination, "wb") as sink:
        shutil.copyfileobj(source, sink, length=1 << 20)
    if member.mode & 0o111 and os.name != "nt":
        os.chmod(destination, 0o755)
    return True


def extract_selected(archive, out_dir, select):
    """Extracts the members whose path below the archive's top directory
    satisfies `select(path)`. Symbolic and hard links inside the selection are
    materialized as copies of what they point at, wherever that is in the
    archive, so the result is the same on hosts that cannot create links.
    Returns the list of written relative paths."""
    meta = {}          # relative path -> (kind, link target as an archive-relative path)
    written = set()
    links = []         # (relative path of the link, member)
    top = None
    with tarfile.open(archive, "r|*") as tf:
        for member in tf:
            name = member.name
            if top is None:
                top = name.split("/", 1)[0]
            if not name.startswith(top + "/"):
                continue
            rel = _safe_relative(name[len(top) + 1:])
            if rel is None:
                continue
            if member.issym():
                target = posixpath.normpath(posixpath.join(posixpath.dirname(rel), member.linkname))
                meta[rel] = ("link", target)
            elif member.islnk():
                linked = member.linkname
                if linked.startswith(top + "/"):
                    linked = linked[len(top) + 1:]
                meta[rel] = ("link", posixpath.normpath(linked))
            elif member.isdir():
                meta[rel] = ("dir", None)
            elif member.isfile():
                meta[rel] = ("file", None)
            else:
                continue
            if not select(rel):
                continue
            if member.isfile():
                if _write_member(tf, member, os.path.join(out_dir, *rel.split("/"))):
                    written.add(rel)
            elif member.issym() or member.islnk():
                links.append(rel)

    def resolve(rel, depth=0):
        if depth > 40:
            raise SystemExit(f"build_kit: link cycle at {rel} in {archive}")
        # Resolve every leading component, since a directory on the way may be a link.
        parts = rel.split("/")
        for i in range(1, len(parts) + 1):
            prefix = "/".join(parts[:i])
            kind = meta.get(prefix, (None, None))
            if kind[0] == "link":
                rest = "/".join(parts[i:])
                joined = posixpath.normpath(posixpath.join(kind[1], rest)) if rest else kind[1]
                return resolve(joined, depth + 1)
        return rel

    # Work out what each link needs: a file, or every file beneath a directory.
    needed = {}        # link path -> list of (source archive path, destination path)
    for link in links:
        target = resolve(link)
        kind = meta.get(target, (None, None))[0]
        pairs = []
        if kind == "file":
            pairs.append((target, link))
        elif kind == "dir":
            for path, (path_kind, _) in meta.items():
                if path_kind == "file" and path.startswith(target + "/"):
                    pairs.append((path, link + path[len(target):]))
            for path, (path_kind, _) in meta.items():
                if path_kind == "link" and path.startswith(target + "/"):
                    real = resolve(path)
                    if meta.get(real, (None, None))[0] == "file":
                        pairs.append((real, link + path[len(target):]))
        else:
            log(f"warning: {link} points at {target}, which is not in the archive; skipped")
        needed[link] = pairs

    missing = {source for pairs in needed.values() for source, _ in pairs if source not in written}
    fetched_dir = None
    if missing:
        fetched_dir = tempfile.mkdtemp(prefix="build-kit-links-")
        with tarfile.open(archive, "r|*") as tf:
            for member in tf:
                if not member.name.startswith(top + "/") or not member.isfile():
                    continue
                rel = _safe_relative(member.name[len(top) + 1:])
                if rel in missing:
                    _write_member(tf, member, os.path.join(fetched_dir, *rel.split("/")))
    for pairs in needed.values():
        for source, destination in pairs:
            base = out_dir if source in written else fetched_dir
            source_path = os.path.join(base, *source.split("/"))
            destination_path = os.path.join(out_dir, *destination.split("/"))
            if not os.path.isfile(source_path):
                log(f"warning: could not materialize {destination} from {source}")
                continue
            os.makedirs(os.path.dirname(destination_path), exist_ok=True)
            shutil.copyfile(source_path, destination_path)
            written.add(destination)
    if fetched_dir:
        shutil.rmtree(fetched_dir, ignore_errors=True)
    return sorted(written)


# --------------------------------------------------------------------------
# Manifest and kit.json
# --------------------------------------------------------------------------

def check_manifest(kit_dir, manifest_rel):
    """Every source-path and include directory of the manifest must resolve
    inside the kit. A path that does not is rewritten to the file of the same
    name under share/libc++/v1 when one exists; anything else is an error."""
    manifest_path = os.path.join(kit_dir, *manifest_rel.split("/"))
    with open(manifest_path, encoding="utf-8") as f:
        manifest = json.load(f)
    base = os.path.dirname(manifest_path)
    kit_real = os.path.realpath(kit_dir)
    changed = False

    def inside(path):
        real = os.path.realpath(path)
        return real == kit_real or real.startswith(kit_real + os.sep)

    def relocate(value, want_dir):
        candidate = os.path.normpath(os.path.join(base, *value.split("/")))
        ok = os.path.isdir(candidate) if want_dir else os.path.isfile(candidate)
        if ok and inside(candidate):
            return value
        fallback = os.path.join(kit_dir, "share", "libc++", "v1")
        if not want_dir:
            fallback = os.path.join(fallback, posixpath.basename(value))
        exists = os.path.isdir(fallback) if want_dir else os.path.isfile(fallback)
        if not exists:
            raise SystemExit(f"build_kit: manifest {manifest_rel} refers to {value}, which is not in the kit")
        return os.path.relpath(fallback, base).replace(os.sep, "/")

    for module in manifest.get("modules", []):
        new_source = relocate(module["source-path"], want_dir=False)
        if new_source != module["source-path"]:
            module["source-path"], changed = new_source, True
        local = module.get("local-arguments", {})
        directories = local.get("system-include-directories", [])
        for i, directory in enumerate(directories):
            new_directory = relocate(directory, want_dir=True)
            if new_directory != directory:
                directories[i], changed = new_directory, True
    if changed:
        with open(manifest_path, "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=2)
            f.write("\n")
        log(f"rewrote paths in {manifest_rel} so they resolve inside the kit")
    return [m["logical-name"] for m in manifest.get("modules", [])]


def write_kit_json(kit_dir, data):
    for key in ("system-include-directories",):
        for directory in data[key]:
            if not os.path.isdir(os.path.join(kit_dir, *directory.split("/"))):
                raise SystemExit(f"build_kit: include directory {directory} is missing from the kit")
    if data.get("sysroot") and not os.path.isdir(os.path.join(kit_dir, data["sysroot"])):
        raise SystemExit(f"build_kit: sysroot {data['sysroot']} is missing from the kit")
    for license_file in data["licenses"]:
        if not os.path.isfile(os.path.join(kit_dir, *license_file.split("/"))):
            raise SystemExit(f"build_kit: license {license_file} is missing from the kit")
    modules = check_manifest(kit_dir, data["stdlib"]["module-metadata"])
    if "std" not in modules:
        raise SystemExit("build_kit: the module manifest does not provide std")
    with open(os.path.join(kit_dir, "kit.json"), "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
        f.write("\n")


# --------------------------------------------------------------------------
# Recipe: libc++ from the LLVM source release
# --------------------------------------------------------------------------

SOURCE_SUBTREES = ("runtimes/", "cmake/", "libcxx/", "libcxxabi/", "llvm/cmake/", "llvm/utils/llvm-lit/")


def find_tool(env_name, *names):
    explicit = os.environ.get(env_name)
    if explicit:
        return explicit
    for name in names:
        path = shutil.which(name)
        if path:
            return path
    raise SystemExit(f"build_kit: {names[0]} not found on PATH (or set {env_name})")


def run(command, cwd=None):
    log(" ".join(command))
    subprocess.run(command, check=True, cwd=cwd)


def copy_tree(source, destination):
    shutil.copytree(source, destination, dirs_exist_ok=True)


def sysroot_headers_from_dpkg():
    """glibc and Linux kernel headers as the Debian packages install them."""
    if not shutil.which("dpkg"):
        raise SystemExit("build_kit: dpkg is not available; pass --sysroot-include DIR with the C library headers")
    listing = subprocess.run(["dpkg", "-L", "libc6-dev", "linux-libc-dev"], check=True,
                             capture_output=True, text=True).stdout.splitlines()
    files = [p for p in listing if p.startswith("/usr/include/") and os.path.isfile(p)]
    licenses = [p for p in ("/usr/share/doc/libc6-dev/copyright", "/usr/share/doc/linux-libc-dev/copyright")
                if os.path.isfile(p)]
    return files, licenses


def recipe_libcxx_source(args, lock, spec, kit_dir, work_dir):
    target = spec["target"]
    version = lock["libcxx-version"]
    if args.platform == "darwin-arm64" and sys.platform != "darwin":
        raise SystemExit("build_kit: darwin-arm64 configures libc++ for Apple platforms and needs a macOS host")
    if args.platform == "linux-x64" and not sys.platform.startswith("linux"):
        raise SystemExit("build_kit: linux-x64 takes the C library headers from a Linux host")

    archive = args.source or fetch.fetch(spec["source"], cache=args.cache, lock=lock)
    source_dir = os.path.join(work_dir, "llvm-project")
    if not os.path.isfile(os.path.join(source_dir, "runtimes", "CMakeLists.txt")):
        log(f"extracting {', '.join(SOURCE_SUBTREES)} from {os.path.basename(archive)}")
        extract_selected(archive, source_dir, lambda rel: rel.startswith(SOURCE_SUBTREES))

    cmake = find_tool("CMAKE", "cmake")
    ninja = find_tool("NINJA", "ninja")
    cc = find_tool("CC", "clang", "cc", "gcc")
    cxx = find_tool("CXX", "clang++", "c++", "g++")
    build_dir = os.path.join(work_dir, "build")
    stage_dir = os.path.join(work_dir, "stage")
    shutil.rmtree(build_dir, ignore_errors=True)
    shutil.rmtree(stage_dir, ignore_errors=True)
    configure = [
        cmake, "-G", "Ninja", "-S", os.path.join(source_dir, "runtimes"), "-B", build_dir,
        f"-DCMAKE_MAKE_PROGRAM={ninja}",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_INSTALL_PREFIX={stage_dir}",
        f"-DCMAKE_C_COMPILER={cc}",
        f"-DCMAKE_CXX_COMPILER={cxx}",
        "-DLLVM_ENABLE_RUNTIMES=libcxx;libcxxabi",
        "-DLIBCXXABI_USE_LLVM_UNWINDER=OFF",
        "-DLIBCXX_INSTALL_MODULES=ON",
        "-DLIBCXX_INCLUDE_TESTS=OFF",
        "-DLIBCXX_INCLUDE_BENCHMARKS=OFF",
        "-DLIBCXXABI_INCLUDE_TESTS=OFF",
        "-DLLVM_INCLUDE_TESTS=OFF",
        "-DLLVM_INCLUDE_DOCS=OFF",
    ]
    if args.platform == "linux-x64":
        configure += ["-DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=ON", f"-DLLVM_DEFAULT_TARGET_TRIPLE={target}"]
    else:
        configure += ["-DCMAKE_OSX_ARCHITECTURES=arm64"]
    run(configure)
    run([ninja, "-C", build_dir, "install-cxx-headers", "install-cxxabi-headers", "install-cxx-modules"])

    for part in ("include", "share", "lib"):
        if os.path.isdir(os.path.join(stage_dir, part)):
            copy_tree(os.path.join(stage_dir, part), os.path.join(kit_dir, part))

    def relative(path):
        return os.path.relpath(path, kit_dir).replace(os.sep, "/")

    manifests = []
    for base, _, files in os.walk(os.path.join(kit_dir, "lib")):
        manifests += [os.path.join(base, f) for f in files if f == "libc++.modules.json"]
    if len(manifests) != 1:
        raise SystemExit(f"build_kit: expected one libc++.modules.json in the install, found {manifests}")
    include_dirs = ["include/c++/v1"]
    per_target = os.path.join(kit_dir, "include", target, "c++", "v1")
    if os.path.isdir(per_target):
        include_dirs.append(relative(per_target))
    elif not os.path.isdir(os.path.join(kit_dir, "include", "c++", "v1")):
        raise SystemExit("build_kit: the install has no include/c++/v1")
    config_sites = [os.path.join(d, "__config_site") for d in (os.path.join(kit_dir, *p.split("/")) for p in include_dirs)]
    if not any(os.path.isfile(p) for p in config_sites):
        raise SystemExit("build_kit: the install has no __config_site")

    licenses_dir = os.path.join(kit_dir, "licenses")
    os.makedirs(licenses_dir, exist_ok=True)
    shutil.copyfile(os.path.join(source_dir, "libcxx", "LICENSE.TXT"), os.path.join(licenses_dir, "LLVM-LICENSE.TXT"))
    licenses = ["licenses/LLVM-LICENSE.TXT"]

    data = {
        "kit-version": KIT_VERSION,
        "name": f"lsp-mcpp-kit-libcxx-{version}-{target}",
        "target": target,
        "stdlib": {"name": "libc++", "version": version, "module-metadata": relative(manifests[0])},
        "system-include-directories": include_dirs,
        "sysroot": None,
        "arguments": ["-nostdinc++"],
        "licenses": licenses,
    }

    if args.platform == "linux-x64":
        if args.sysroot_include:
            header_root = os.path.abspath(args.sysroot_include)
            destination = os.path.join(kit_dir, "sysroot", "usr", "include")
            copy_tree(header_root, destination)
            extra_licenses = list(args.sysroot_license or [])
        else:
            files, extra_licenses = sysroot_headers_from_dpkg()
            for path in files:
                destination = os.path.join(kit_dir, "sysroot", *path.lstrip("/").split("/"))
                os.makedirs(os.path.dirname(destination), exist_ok=True)
                shutil.copyfile(path, destination)
            log(f"copied {len(files)} glibc and Linux kernel headers into sysroot/usr/include")
        for i, path in enumerate(extra_licenses):
            package = os.path.basename(os.path.dirname(path)) if os.path.basename(path) == "copyright" else f"sysroot-{i}"
            name = f"{package}-copyright.txt" if os.path.basename(path) == "copyright" else os.path.basename(path)
            shutil.copyfile(path, os.path.join(licenses_dir, name))
            licenses.append(f"licenses/{name}")
        data["sysroot"] = "sysroot"
    else:
        # Apple's SDK license does not allow redistributing the C library headers.
        data["requires"] = [{"kind": "macos-sdk"}]
    write_kit_json(kit_dir, data)
    return data


# --------------------------------------------------------------------------
# Recipe: llvm-mingw
# --------------------------------------------------------------------------

MINGW_EXCLUDED = ("*.idl", "*.tlb", "*.def")
MINGW_TRIPLE = "x86_64-w64-mingw32"


def recipe_llvm_mingw(args, lock, spec, kit_dir, work_dir):
    entry = lock["entries"][spec["source"]]
    version = entry.get("llvm-version", lock["libcxx-version"])
    archive = args.source or fetch.fetch(spec["source"], cache=args.cache, lock=lock)
    manifest_rel = f"{MINGW_TRIPLE}/lib/libc++.modules.json"
    copyright_prefix = f"{MINGW_TRIPLE}/share/mingw32/"

    def select(rel):
        if rel.startswith("generic-w64-mingw32/include/"):
            return not any(fnmatch.fnmatch(posixpath.basename(rel), pattern) for pattern in MINGW_EXCLUDED)
        return (rel.startswith("share/libc++/v1/") or rel == manifest_rel or rel == "LICENSE.TXT"
                or (rel.startswith(copyright_prefix) and posixpath.basename(rel).startswith("COPYING")))

    log(f"extracting headers, module sources and manifest from {os.path.basename(archive)}")
    written = extract_selected(archive, kit_dir, select)

    # Windows file systems ignore case; two names that differ only in case
    # would silently collapse into one when the kit is unpacked there.
    by_key = {}
    for rel in written:
        by_key.setdefault(rel.lower(), []).append(rel)
    collisions = [names for names in by_key.values() if len(names) > 1]
    for names in collisions:
        contents = set()
        for name in names:
            with open(os.path.join(kit_dir, *name.split("/")), "rb") as f:
                contents.add(f.read())
        if len(contents) == 1:
            for extra in sorted(names)[1:]:
                os.remove(os.path.join(kit_dir, *extra.split("/")))
        else:
            log(f"warning: names differing only in case with different content: {names}")

    licenses_dir = os.path.join(kit_dir, "licenses")
    os.makedirs(licenses_dir, exist_ok=True)
    shutil.move(os.path.join(kit_dir, "LICENSE.TXT"), os.path.join(licenses_dir, "LLVM-LICENSE.TXT"))
    licenses = ["licenses/LLVM-LICENSE.TXT"]
    copyright_dir = os.path.join(kit_dir, *copyright_prefix.rstrip("/").split("/"))
    if os.path.isdir(copyright_dir):
        os.makedirs(os.path.join(licenses_dir, "mingw-w64"), exist_ok=True)
        for name in sorted(os.listdir(copyright_dir)):
            shutil.move(os.path.join(copyright_dir, name), os.path.join(licenses_dir, "mingw-w64", name))
            licenses.append(f"licenses/mingw-w64/{name}")
        shutil.rmtree(os.path.join(kit_dir, MINGW_TRIPLE, "share"))

    data = {
        "kit-version": KIT_VERSION,
        "name": f"lsp-mcpp-kit-libcxx-{version}-{MINGW_TRIPLE}",
        "target": MINGW_TRIPLE,
        "stdlib": {"name": "libc++", "version": version, "module-metadata": manifest_rel},
        "system-include-directories": ["generic-w64-mingw32/include/c++/v1", "generic-w64-mingw32/include"],
        "sysroot": None,
        "arguments": ["-nostdinc++", "-nostdlibinc"],
        "licenses": licenses,
    }
    write_kit_json(kit_dir, data)
    return data


RECIPES = {"libcxx-source": recipe_libcxx_source, "llvm-mingw": recipe_llvm_mingw}


def tree_size(path):
    total, count = 0, 0
    for base, _, files in os.walk(path):
        for name in files:
            total += os.path.getsize(os.path.join(base, name))
            count += 1
    return total, count


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--platform", required=True, choices=PLATFORMS)
    parser.add_argument("--out", required=True, help="kit directory to create (replaced if it exists)")
    parser.add_argument("--cache", default=fetch.DEFAULT_CACHE, help="download cache")
    parser.add_argument("--work", help="scratch directory for sources and the configure tree")
    parser.add_argument("--source", help="use this archive instead of fetching the lock entry")
    parser.add_argument("--sysroot-include", help="linux-x64: C library headers to use instead of the host's dpkg packages")
    parser.add_argument("--sysroot-license", action="append", help="linux-x64: license file for --sysroot-include (repeatable)")
    args = parser.parse_args()

    lock = fetch.load_lock()
    spec = lock["platforms"][args.platform]["kit"]
    kit_dir = os.path.abspath(args.out)
    if os.path.exists(kit_dir):
        shutil.rmtree(kit_dir)
    os.makedirs(kit_dir)
    work_dir = os.path.abspath(args.work) if args.work else tempfile.mkdtemp(prefix="lsp-mcpp-kit-")
    os.makedirs(work_dir, exist_ok=True)

    data = RECIPES[spec["recipe"]](args, lock, spec, kit_dir, work_dir)
    size, count = tree_size(kit_dir)
    log(f"{data['name']}: {count} files, {size / 1e6:.1f} MB -> {kit_dir}")
    if not args.work:
        shutil.rmtree(work_dir, ignore_errors=True)
    print(kit_dir)


if __name__ == "__main__":
    main()
