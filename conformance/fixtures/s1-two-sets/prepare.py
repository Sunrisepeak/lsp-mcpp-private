"""Writes build_database.json: an S1 document the workspace carries itself (usable plan W9.2),
with two sets ("variant1", "variant2") that both compile src/main.cpp, one with -DVARIANT=1 and
the other with -DVARIANT=2. No build system runs; this is the database a producer would have
written. sys.argv[1] (scenario.json passes {env:CONFORMANCE_CLANGXX|clang++}, the same convention
cmake-clang's own prepare step uses) is a real, introspectable compiler, so the server's own
toolchain probing at load time (project/infer.cpp's enrich_database) resolves it instead of
guessing from a fake driver path.
"""
import json
import pathlib
import shutil
import sys

root = pathlib.Path.cwd()
clangxx = sys.argv[1] if len(sys.argv) > 1 else "clang++"
if not pathlib.Path(clangxx).is_absolute():
    found = shutil.which(clangxx)
    if not found:
        sys.exit(f"s1-two-sets: {clangxx} is not on PATH")
    clangxx = found
source = str(root / "src" / "main.cpp")


def translation_unit(variant):
    return {
        "source": source,
        "work-directory": str(root),
        "arguments": [clangxx, "-std=c++23", f"-DVARIANT={variant}", "-c", source,
                      "-o", str(root / f"main-{variant}.o")],
        "local-arguments": [f"-DVARIANT={variant}"],
    }


def set_for(variant):
    return {
        "name": f"variant{variant}",
        "family-name": "probe",
        "visible-sets": [],
        "baseline-arguments": ["-std=c++23"],
        "ide": {"toolchain": "llvm-22.1.8", "configuration": f"variant{variant}", "kind": "executable"},
        "translation-units": [translation_unit(variant)],
    }


database = {
    "version": 1,
    "revision": 0,
    "ide": {
        "profile-version": "0.2.0",
        "generator": {"name": "lsp-mcpp-conformance", "version": "0.0.0"},
        "toolchains": {
            "llvm-22.1.8": {
                "family": "clang",
                "version": "22.1.8",
                "driver": clangxx,
                "target": "x86_64-unknown-linux-gnu",
            },
        },
    },
    "sets": [set_for(1), set_for(2)],
}
(root / "build_database.json").write_text(json.dumps(database, indent=2), encoding="utf-8")
