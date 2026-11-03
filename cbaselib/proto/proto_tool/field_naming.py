"""Protocol field names: type prefix followed by PascalCase business words."""
import re


def FieldPrefix(type_name):
    name = re.sub(r"\s+", "", type_name).replace("std::", "")
    if name == "bool": return "b"
    if name == "float": return "f"
    if name == "double": return "d"
    if name == "string": return "s"
    if name.startswith("vector<"): return "v"
    if name.startswith("map<"): return "m"
    if re.fullmatch(r"u?int(8|16|32|64)_t|unsignedint|int|size_t", name): return "i"
    return "st"


def ValidFieldName(type_name, field_name):
    return re.fullmatch(FieldPrefix(type_name) + r"[A-Z][A-Za-z0-9]*", field_name) is not None
