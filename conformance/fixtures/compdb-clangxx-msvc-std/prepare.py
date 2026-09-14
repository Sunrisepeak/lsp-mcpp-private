"""Builds the fixture with clang++ for the MSVC ABI and the MSVC STL std module, as a build tool would, and writes compile_commands.json."""
import json, os, pathlib, shutil, subprocess, sys

root = pathlib.Path.cwd()
build = root / "build"
build.mkdir(exist_ok=True)
tools = pathlib.Path(os.environ["VCToolsInstallDir"])
clangxx = shutil.which("clang++") or sys.exit("clang++ is not on PATH")
common = [clangxx, "--target=x86_64-pc-windows-msvc", "-std=c++23", "-c"]
steps = [
    (None, [*common, "-Wno-reserved-module-identifier", "-Wno-include-angled-in-module-purview", "-x", "c++-module",
            str(tools / "modules" / "std.ixx"), f"-fmodule-output={build / 'std.pcm'}", "-o", str(build / "std.obj")]),
    (root / "src" / "greet.cppm", [*common, f"-fmodule-file=std={build / 'std.pcm'}", f"-fmodule-output={build / 'greet.pcm'}",
                                   str(root / "src" / "greet.cppm"), "-o", str(build / "greet.obj")]),
    (root / "src" / "main.cpp", [*common, f"-fmodule-file=std={build / 'std.pcm'}", f"-fmodule-file=greet={build / 'greet.pcm'}",
                                 str(root / "src" / "main.cpp"), "-o", str(build / "main.obj")]),
]
database = []
for source, argv in steps:
    print("+", subprocess.list2cmdline(argv), flush=True)
    subprocess.run(argv, cwd=root, check=True)
    if source is not None:
        database.append({"directory": str(root), "file": str(source), "arguments": argv})
(root / "compile_commands.json").write_text(json.dumps(database, indent=2), encoding="utf-8")
