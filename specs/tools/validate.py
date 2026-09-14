#!/usr/bin/env python3
"""Validates the specifications' machine-readable parts (specs/README.md: "CI validates every example against its schema").

    python3 specs/tools/validate.py            # from the repository root; needs the jsonschema package

Checks: every JSON file parses and every schema is valid draft 2020-12; examples, JSON blocks in the text and the
simulated producer data of conformance fixtures validate; semantic rules a schema cannot express; negative cases the
schemas must reject; relative links resolve. Exits non-zero on any failure.
"""
import copy, json, pathlib, re, sys
from jsonschema import Draft202012Validator

root = pathlib.Path(__file__).resolve().parent.parent
repository = root.parent
failures = 0

def check(label, ok, detail=""):
    global failures
    print(f"{'PASS' if ok else 'FAIL'}  {label}{('  ' + detail) if detail and not ok else ''}")
    if not ok:
        failures += 1

def load(path):
    return json.loads(path.read_text(encoding="utf-8"))

# 1. every JSON file parses; every schema is a valid draft 2020-12 schema
schemas = {}
for path in sorted(root.rglob("*.json")):
    try:
        doc = load(path)
        check(f"parse {path.relative_to(root)}", True)
    except Exception as e:
        check(f"parse {path.relative_to(root)}", False, str(e))
        continue
    if path.parent.name == "schema":
        try:
            Draft202012Validator.check_schema(doc)
            check(f"schema is valid 2020-12: {path.name}", True)
        except Exception as e:
            check(f"schema is valid 2020-12: {path.name}", False, str(e))
        schemas[path.name] = Draft202012Validator(doc)

s1, s2, s4 = (schemas["s1-build-database.schema.json"], schemas["s2-discovery.schema.json"],
              schemas["s4-kit.schema.json"])

def validate(label, validator, doc, expect_valid=True):
    errors = sorted(validator.iter_errors(doc), key=lambda e: list(e.path))
    ok = (not errors) if expect_valid else bool(errors)
    detail = "; ".join(f"{list(e.path)}: {e.message}" for e in errors[:3]) if expect_valid else "accepted an invalid document"
    check(label, ok, detail)

ex = root / "examples"
# 2. examples validate
for name in ["s1-level3-gcc.json", "s1-level2-clang-two-sets.json"]:
    validate(f"S1 example validates: {name}", s1, load(ex / name))
validate("S2 request validates", s2, load(ex / "s2-request.json"))
envelope = load(ex / "s2-envelope.json")
validate("S2 single-document envelope validates", s2, envelope)
validate("S2 envelope carries a valid S1 database", s1, envelope["data"]["database"])
lines = [l for l in (ex / "s2-messages.jsonl").read_text(encoding="utf-8").splitlines() if l.strip()]
for i, line in enumerate(lines):
    validate(f"S2 message line {i + 1} validates", s2, json.loads(line))
check("S2 stream ends with a terminal message", json.loads(lines[-1])["kind"] in ("finished", "error"))
for name in ["s4-kit-linux-x64.json", "s4-kit-win32-x64.json", "s4-kit-darwin-arm64.json"]:
    validate(f"S4 example validates: {name}", s4, load(ex / name))

# 3. semantic checks the schema cannot express (S1 sections 7, 8.2, 10; S4 rule 4)
IMPORTABLE = {"module-interface", "module-partition-interface", "module-partition-implementation"}
def s1_semantics(name, doc):
    tool_ids = set(doc["ide"]["toolchains"])
    set_names = [s["name"] for s in doc["sets"]]
    check(f"S1 {name}: set names unique", len(set_names) == len(set(set_names)))
    for s in doc["sets"]:
        check(f"S1 {name}: {s['name']} toolchain id exists", s["ide"]["toolchain"] in tool_ids)
        check(f"S1 {name}: {s['name']} visible sets exist", all(v in set_names for v in s["visible-sets"]))
        for tu in s["translation-units"]:
            role = tu["ide"]["role"]
            provides = tu.get("provides", {})
            check(f"S1 {name}: {tu['source']} provides iff importable role",
                  bool(provides) == (role in IMPORTABLE))
            for module in provides:
                partition = ":" in module
                check(f"S1 {name}: {tu['source']} partition role matches name",
                      partition == (role in {"module-partition-interface", "module-partition-implementation"}))
    # every requirement resolves by section 10
    by_set = {s["name"]: s for s in doc["sets"]}
    for s in doc["sets"]:
        tool = doc["ide"]["toolchains"][s["ide"]["toolchain"]]
        for tu in s["translation-units"]:
            for req in tu.get("requires", []):
                in_set = any(req in u.get("provides", {}) for u in s["translation-units"])
                in_visible = any(req in u.get("provides", {}) and not u.get("private", False)
                                 for v in s["visible-sets"] for u in by_set[v]["translation-units"])
                stdlib = req in ("std", "std.compat") and "module-metadata" in tool.get("stdlib", {})
                check(f"S1 {name}: {tu['source']} requirement {req} resolves", in_set or in_visible or stdlib)

for name in ["s1-level3-gcc.json", "s1-level2-clang-two-sets.json"]:
    s1_semantics(name, load(ex / name))
darwin = load(ex / "s4-kit-darwin-arm64.json")
check("S4 darwin kit declares macos-sdk", {"kind": "macos-sdk"} in darwin.get("requires", []))
for name in ["s4-kit-linux-x64.json", "s4-kit-win32-x64.json", "s4-kit-darwin-arm64.json"]:
    kit = load(ex / name)
    check(f"S4 {name}: libc++ version equals pinned clangd 23.1.0", kit["stdlib"]["version"] == "23.1.0")

# 4. negative cases: schemas reject what the text forbids
base_kit = load(ex / "s4-kit-linux-x64.json")
for label, mutate in [
    ("absolute include directory", lambda k: k["system-include-directories"].append("/usr/include")),
    ("parent traversal", lambda k: k["system-include-directories"].append("include/../../etc")),
    ("drive prefix", lambda k: k.update({"sysroot": "C:/sdk"})),
    ("backslash separator", lambda k: k["licenses"].append("licenses\\X.TXT")),
    ("unknown kit-version", lambda k: k.update({"kit-version": 2})),
    ("missing licenses", lambda k: k.pop("licenses")),
]:
    kit = copy.deepcopy(base_kit); mutate(kit)
    validate(f"S4 schema rejects {label}", s4, kit, expect_valid=False)
kit = copy.deepcopy(base_kit); kit["system-include-directories"].append("include/..hidden/v1")
validate("S4 schema accepts a component that merely starts with '..'", s4, kit)

base_db = load(ex / "s1-level3-gcc.json")
for label, mutate in [
    ("unknown role", lambda d: d["sets"][0]["translation-units"][0]["ide"].update({"role": "interface"})),
    ("unknown family", lambda d: d["ide"]["toolchains"]["gcc-16.1.0-x86_64-linux-gnu"].update({"family": "icc"})),
    ("set ide without toolchain", lambda d: d["sets"][0]["ide"].pop("toolchain")),
    ("macro with define and undefine", lambda d: d["sets"][0]["ide"]["options"]["macros"].append({"define": "A", "undefine": "A"})),
    ("module name with space", lambda d: d["sets"][0]["translation-units"][2]["requires"].append("bad name")),
    ("missing sets", lambda d: d.pop("sets")),
]:
    db = copy.deepcopy(base_db); mutate(db)
    validate(f"S1 schema rejects {label}", s1, db, expect_valid=False)
db = copy.deepcopy(base_db); db["sets"][0]["translation-units"][2]["requires"].append("módulo.ñ")
validate("S1 schema accepts non-ASCII module names", s1, db)
db = copy.deepcopy(base_db); db["unknown-top-level"] = {"x": 1}; db["sets"][0]["ide"]["vendor"] = True
validate("S1 schema accepts unknown fields", s1, db)

for label, doc in [
    ("finished without watch", {"kind": "finished", "database": "/x/db.json"}),
    ("unknown kind", {"kind": "done"}),
    ("request without profile-version", {"workspace": "/w"}),
]:
    validate(f"S2 schema rejects {label}", s2, doc, expect_valid=False)
s2_request_only = Draft202012Validator({"$ref": "#/$defs/request",
                                        "$defs": load(root / "schema" / "s2-discovery.schema.json")["$defs"]})
validate("S2 request definition rejects a request carrying kind", s2_request_only,
         {"workspace": "/w", "profile-version": "0.2.0", "kind": "progress"}, expect_valid=False)

# 5. JSON blocks embedded in the specification text
def json_blocks(md):
    return re.findall(r"```json\n(.*?)```", md.read_text(encoding="utf-8"), flags=re.S)
s1_blocks = json_blocks(root / "s1-build-database.md")
check("S1 text: complete example equals examples/s1-level3-gcc.json", json.loads(s1_blocks[0]) == base_db)
s2_blocks = json_blocks(root / "s2-discovery.md")
validate("S2 text: request block validates", s2, json.loads(s2_blocks[0]))
s2_text = (root / "s2-discovery.md").read_text(encoding="utf-8")
stream = re.search(r"```text\n(.*?)```", s2_text, flags=re.S).group(1)
for i, line in enumerate(l for l in stream.splitlines() if l.strip()):
    validate(f"S2 text: stream line {i + 1} validates", s2, json.loads(line))
s4_blocks = json_blocks(root / "s4-semantic-kit.md")
check("S4 text: example equals examples/s4-kit-win32-x64.json", json.loads(s4_blocks[0]) == load(ex / "s4-kit-win32-x64.json"))

# 6. relative links in markdown resolve
for md in sorted(root.glob("*.md")):
    for target in re.findall(r"\]\(([^)#:]+)(?:#[^)]*)?\)", md.read_text(encoding="utf-8")):
        check(f"link in {md.name}: {target}", (root / target).exists())


# 7. the simulated producer data of conformance fixtures (mcpp-community/mcpp#636) is S1
for mock in sorted((repository / "conformance" / "fixtures").glob("*/mcpp-mock.json")):
    data = load(mock)
    if "database" not in data:
        continue
    text = json.dumps(data["database"]).replace("${root}", "/workspace")
    text = re.sub(r"\$\{env:[^}|]*(\|[^}]*)?\}", "/env", text)
    database = json.loads(text)
    validate(f"S1 fixture data validates: {mock.parent.name}", s1, database)
    s1_semantics(mock.parent.name, database)

print(f"\n{failures} failure(s)")
sys.exit(1 if failures else 0)
