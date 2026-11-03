"""Audit owned C++ members, parameters and locals using libclang.

Requires: python -m pip install libclang
Usage: python tools/check_field_names.py --build-dir ~/openidea-build-gcc13
"""
import argparse
from concurrent.futures import ProcessPoolExecutor
import json
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def owned(path):
    if not path:
        return False
    path = Path(path)
    return ROOT in path.parents and "3rd" not in path.parts and not {"port", "win"}.issubset(path.parts)


def prefix(typ):
    from clang.cindex import TypeKind as K
    declared_name = re.sub(r"^const ", "", typ.spelling)
    typ = typ.get_canonical()
    if typ.kind in (K.LVALUEREFERENCE, K.RVALUEREFERENCE):
        return prefix(typ.get_pointee())
    name = re.sub(r"^const ", "", typ.spelling)
    if not name.startswith("std::") and declared_name.startswith("std::") and "<" in declared_name:
        name = declared_name
    if name.startswith("std::atomic<"):
        return prefix(typ.get_template_argument_type(0))
    if typ.kind == K.POINTER:
        return {"fn"} if typ.get_pointee().kind in (K.FUNCTIONPROTO, K.FUNCTIONNOPROTO) else {"p"}
    if typ.kind == K.BOOL: return {"b"}
    if typ.kind == K.FLOAT: return {"f"}
    if typ.kind in (K.DOUBLE, K.LONGDOUBLE): return {"d"}
    if typ.kind == K.ENUM or ("<" not in name and re.search(r"\b(char|short|int|long)\b", name) and "[" not in name): return {"i"}
    if re.match(r"std::(shared_ptr|unique_ptr|weak_ptr)<", name): return {"p"}
    if re.match(r"std::(basic_string|basic_string_view)<", name): return {"s"}
    if re.match(r"std::(map|unordered_map|multimap|unordered_multimap)<", name): return {"m"}
    if "[" in name or re.match(r"std::(vector|list|deque|array|set|unordered_set|multiset|queue|priority_queue)<", name): return {"v"}
    if name.startswith("std::function<") or "lambda at " in name: return {"fn"}
    if typ.kind == K.UNEXPOSED: return {"st", "fn"}
    return {"st"}


def class_key(tokens):
    """Read the record keyword outside template parameters; attributes may follow it."""
    depth = 0
    for token in tokens:
        if token == "<": depth += 1
        elif token in (">", ">>"): depth = max(0, depth - len(token))
        elif depth == 0 and token in ("class", "struct", "union"):
            return token
    return None


def inspect(command):
    from clang import cindex as c
    args = shlex.split(command["command"])
    compiler = args.pop(0)
    filtered = []
    skip = False
    for arg in args:
        if skip:
            skip = False
            continue
        if arg == "-o":
            skip = True
            continue
        if arg in ("-c", command["file"]) or arg.startswith(("-W", "-g", "-O")):
            continue
        filtered.append(arg)
    # libclang pip wheels have no GCC standard-library search configuration.
    probe = subprocess.run([compiler, "-E", "-x", "c++", "-", "-v"], input="", text=True, capture_output=True, check=True)
    active = False
    for line in probe.stderr.splitlines():
        if "search starts here:" in line: active = True
        elif "End of search list." in line: active = False
        elif active and Path(line.strip()).is_dir(): filtered += ["-isystem", line.strip()]
    filtered += ["-UNDEBUG", "-Wself-assign", "-Wself-assign-field"]
    tu = c.Index.create().parse(command["file"], args=filtered)
    issues = [str(d) for d in tu.diagnostics if d.severity >= c.Diagnostic.Error or (d.option.startswith("-Wself-assign") and d.location.file and owned(str(d.location.file)))]
    fields = set()
    functions = {c.CursorKind.FUNCTION_DECL, c.CursorKind.FUNCTION_TEMPLATE,
                 c.CursorKind.CXX_METHOD, c.CursorKind.CONSTRUCTOR,
                 c.CursorKind.DESTRUCTOR, c.CursorKind.LAMBDA_EXPR}
    records = {c.CursorKind.STRUCT_DECL, c.CursorKind.CLASS_DECL, c.CursorKind.CLASS_TEMPLATE}
    def walk(node, in_function=False):
        file = str(node.location.file) if node.location.file else None
        if file and not owned(file): return
        in_function = in_function or node.kind in functions
        parent = node.semantic_parent
        is_field = node.kind == c.CursorKind.FIELD_DECL or (node.kind == c.CursorKind.VAR_DECL and parent.kind in records)
        is_variable = node.kind == c.CursorKind.PARM_DECL or (node.kind == c.CursorKind.VAR_DECL and in_function and not is_field)
        generated = file and ("/modules/include/" in file or "/oi_rpcdata_" in file)
        if file and node.spelling and not generated and (is_field or is_variable):
            allowed = prefix(node.type)
            is_class = parent.kind == c.CursorKind.CLASS_DECL or (parent.kind == c.CursorKind.CLASS_TEMPLATE and class_key(t.spelling for t in parent.get_tokens()) == "class")
            if is_field and not is_class:
                for child in node.get_children(): walk(child, in_function)
                return
            fields.add((file, node.location.line, node.spelling))
            private = is_field and is_class and node.access_specifier == c.AccessSpecifier.PRIVATE
            marker = "m_" if private else ""
            iterator = not is_field and "iterator" in (node.type.spelling + node.type.get_canonical().spelling).lower()
            iterator_name = iterator and re.fullmatch(r"(?:it|[a-z][A-Za-z0-9]*It)", node.spelling)
            if not iterator_name and not any(re.fullmatch(marker + p + r"[A-Z][A-Za-z0-9]*", node.spelling) for p in allowed):
                issues.append(f"{file}:{node.location.line}: {node.spelling}: expected {marker}{sorted(allowed)} prefix ({node.type.spelling})")
        for child in node.get_children(): walk(child, in_function)

    walk(tu.cursor)
    return fields, issues


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4)
    options = parser.parse_args()
    commands = json.loads((options.build_dir / "compile_commands.json").read_text())
    commands = [x for x in commands if owned(x["file"]) and x["file"].endswith(".cpp")]
    if not commands: parser.error("no owned C++ compilation units found")
    fields, issues = set(), set()
    with ProcessPoolExecutor(max_workers=options.jobs) as pool:
        for found, errors in pool.map(inspect, commands):
            fields.update(found)
            issues.update(errors)
    for issue in sorted(issues): print(issue)
    print(f"naming audit: declarations={len(fields)} issues={len(issues)} units={len(commands)}")
    raise SystemExit(bool(issues))
