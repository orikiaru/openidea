import re
from common import loginfo
from serialize_data_parse import ClassMember
from serialize_data_parse import SerializeData
from serialize_data_parse import SerializeDataType
from serialize_data_parse import NormalizeDbType

class ProtoLexer:
    def __init__(self, text):
        self.text = text
        self.n = len(text)
        self.i = 0
        self.line = 1

    def fail(self, what):
        loginfo("proto line", self.line, what)
        return None

    def skip(self):
        while self.i < self.n:
            ch = self.text[self.i]
            if ch in " \t\r":
                self.i += 1
            elif ch == "\n":
                self.line += 1
                self.i += 1
            elif self.text.startswith("//", self.i):
                while self.i < self.n and self.text[self.i] != "\n":
                    self.i += 1
            else:
                return

    def eof(self):
        self.skip()
        return self.i >= self.n

    def peek_char(self):
        self.skip()
        if self.i >= self.n:
            return ""
        return self.text[self.i]

    def peek_ident(self):
        self.skip()
        i = self.i
        if i >= self.n or not (self.text[i].isalpha() or self.text[i] == "_"):
            return None
        j = i + 1
        while j < self.n and (self.text[j].isalnum() or self.text[j] in "_:"):
            j += 1
        return self.text[i:j]

    def ident(self):
        word = self.peek_ident()
        if word is None:
            return self.fail("expect name")
        self.i += len(word)
        return word

    def number(self):
        self.skip()
        if self.i >= self.n or not self.text[self.i].isdigit():
            return self.fail("expect field number")
        start = self.i
        while self.i < self.n and self.text[self.i].isdigit():
            self.i += 1
        return int(self.text[start:self.i])

    def signed_number(self):
        negative = False
        if self.peek_char() == "-":
            self.i += 1
            negative = True
        value = self.number()
        if value is None:
            return None
        return -value if negative else value

    def punct(self, ch):
        if self.peek_char() != ch:
            return self.fail("expect " + ch)
        self.i += 1
        return True

def NormalizeTypeName(name):
    if name == "string" or name == "std::string":
        return "std::string"
    if name == "vector" or name == "std::vector":
        return "std::vector"
    if name == "map" or name == "std::map":
        return "std::map"
    return name

def ParseType(lex):
    name = lex.ident()
    if name is None:
        return None
    name = NormalizeTypeName(name)
    if lex.peek_char() != "<":
        return name
    lex.i += 1
    first = ParseType(lex)
    if first is None:
        return None
    if lex.peek_char() == ",":
        lex.i += 1
        second = ParseType(lex)
        if second is None or not lex.punct(">"):
            return None
        if name != "std::map":
            return lex.fail("only map takes two type arguments")
        return "std::map<" + first + ", " + second + ">"
    if not lex.punct(">"):
        return None
    if name != "std::vector":
        return lex.fail("only vector takes one type argument")
    return "std::vector<" + first + ">"

FIELD_MARKERS = ("json", "nojson", "db", "nodb", "dbjson", "dbkey", "dbtype")

# 名字可省略。省略时序列化名就是字段名，由后面的标记收尾补上。
def ParseOptionalName(lex):
    word = lex.peek_ident()
    if word is None or word in FIELD_MARKERS:
        return ""
    return lex.ident()

def ApplyFieldMarker(lex, member, word):
    if word == "json":
        if member.no_json or member.json_marked or member.json_name:
            return lex.fail("repeated json marker")
        member.json_marked = True
        name = ParseOptionalName(lex)
        if name is None:
            return None
        if name:
            member.json_name = name
        return True
    if word == "nojson":
        if member.no_json or member.json_marked or member.json_name:
            return lex.fail("json and nojson")
        member.no_json = True
        return True
    if word == "db":
        if member.no_db or member.db_marked or member.db_name:
            return lex.fail("db and nodb" if member.no_db else "repeated db marker")
        member.db_marked = True
        name = ParseOptionalName(lex)
        if name is None:
            return None
        if name:
            member.db_name = name
        return True
    if word == "nodb":
        if member.no_db or member.db_marked or member.db_name or member.db_json or member.db_key or member.db_type:
            return lex.fail("db and nodb")
        member.no_db = True
        return True
    if word == "dbjson":
        if member.no_db or member.db_json:
            return lex.fail("db and nodb" if member.no_db else "repeated dbjson marker")
        member.db_json = True
        return True
    if word == "dbkey":
        if member.no_db or member.db_key:
            return lex.fail("db and nodb" if member.no_db else "repeated dbkey marker")
        member.db_key = True
        return True
    if word == "dbtype":
        # [dbtype VARBINARY 64]、[dbtype LONGTEXT]：类型名，带长度的类型后面跟长度。
        if member.no_db or member.db_type:
            return lex.fail("db and nodb" if member.no_db else "repeated dbtype marker")
        type_name = lex.ident()
        if type_name is None:
            return None
        length = None
        if lex.peek_char().isdigit():
            length = lex.number()
        member.db_type = NormalizeDbType(type_name, length)
        if member.db_type is None:
            return lex.fail("invalid dbtype " + type_name + ("" if length is None else " " + str(length)))
        return True
    return lex.fail("unknown field marker " + word)

def ParseMarkerGroup(lex, member):
    if not lex.punct("["):
        return None
    if lex.peek_char() == "]":
        return lex.fail("empty field markers")
    while True:
        word = lex.ident()
        if word is None:
            return None
        if word not in FIELD_MARKERS:
            return lex.fail("unknown field marker " + word)
        if not ApplyFieldMarker(lex, member, word):
            return None
        if lex.peek_char() == ",":
            lex.punct(",")
            continue
        break
    return lex.punct("]")

# 每个 [] 是一组标记，可以连着写。json、db 后面的名字可省略。
def ParseFieldMarkers(lex, member):
    while lex.peek_char() == "[":
        if not ParseMarkerGroup(lex, member):
            return None
    return True

def ParseRpcData(lex):
    name = lex.ident()
    if name is None:
        return None
    flags = set()
    while lex.peek_ident() in ("key", "json", "db"):
        flag = lex.ident()
        if flag in flags:
            return lex.fail("duplicate rpcdata flag " + flag)
        flags.add(flag)
    if not lex.punct("{"):
        return None
    data = SerializeData(SerializeDataType.RPC_DATA, None)
    data.name = name
    data.is_key = "key" in flags
    data.has_json = "json" in flags
    data.has_db = "db" in flags
    while not lex.eof() and lex.peek_char() != "}":
        seq = lex.number()
        if seq is None:
            return None
        member = ClassMember()
        member.seq = seq
        if ParseFieldMarkers(lex, member) is None:
            return None
        member.type = ParseType(lex)
        if member.type is None:
            return None
        field = lex.ident()
        if field is None:
            return None
        member.name = field
        if not lex.punct(";"):
            return None
        for old in data.members:
            if old.GetName() == field:
                loginfo("duplicate field proto=", name, ",field=", field)
                return None
        data.members.append(member)
    if not lex.punct("}"):
        return None
    if not data.AssignFieldSeq():
        return None
    return data

def ParseRpc(lex):
    name = lex.ident()
    if name is None or not lex.punct("{"):
        return None
    fields = {}
    while not lex.eof() and lex.peek_char() != "}":
        key = lex.ident()
        if key is None:
            return None
        if key in ("id", "res_id", "timeout"):
            num = lex.number()
            if num is None:
                return None
            fields[key] = str(num)
        elif key in ("request", "response"):
            val = lex.ident()
            if val is None:
                return None
            fields[key] = val
        else:
            return lex.fail("unknown rpc field " + key)
        if not lex.punct(";"):
            return None
    if not lex.punct("}"):
        return None
    for need in ("id", "request", "response"):
        if need not in fields:
            loginfo("rpc missing", need, "name=", name)
            return None
    if fields["request"] == fields["response"]:
        loginfo("rpc request and response reuse the same rpcdata name=", name)
        return None
    data = SerializeData(SerializeDataType.RPC, None)
    data.name = name
    data.id = fields["id"]
    data.res_id = fields.get("res_id", fields["id"])
    if "timeout" in fields:
        data.timeout = fields["timeout"]
    req = ClassMember()
    req.name = "req"
    req.type = fields["request"]
    res = ClassMember()
    res.name = "res"
    res.type = fields["response"]
    data.members.append(req)
    data.members.append(res)
    return data

ENUM_ITEM_NAME = re.compile(r"[A-Z][A-Z0-9_]*")
# 框架错误码是负数（oi_errno.h），业务错误码用正数。

# enum 和 errordef 都是 `名字 = 值;`，值必须手写。
def ParseEnum(lex, data_type):
    name = lex.ident()
    if name is None or not lex.punct("{"):
        return None
    data = SerializeData(data_type, None)
    data.name = name
    values = set()
    while not lex.eof() and lex.peek_char() != "}":
        item = lex.ident()
        if item is None or not lex.punct("="):
            return None
        value = lex.signed_number()
        if value is None or not lex.punct(";"):
            return None
        if not ENUM_ITEM_NAME.fullmatch(item):
            return lex.fail("enum item must be UPPER_SNAKE name=" + item)
        if data_type == SerializeDataType.ERROR_DEF and not 1 <= value <= 2147483647:
            return lex.fail("errordef value must be in 1..2147483647 name=%s value=%d" % (item, value))
        if value in values:
            return lex.fail("duplicate enum value name=" + item)
        values.add(value)
        member = ClassMember()
        member.name = item
        member.value = value
        data.members.append(member)
    if not lex.punct("}"):
        return None
    if len(data.members) == 0:
        return lex.fail("empty enum name=" + name)
    return data

def ParseProtoText(text):
    lex = ProtoLexer(text)
    datas = []
    while not lex.eof():
        kind = lex.ident()
        if kind == "rpcdata":
            data = ParseRpcData(lex)
        elif kind == "rpc":
            data = ParseRpc(lex)
        elif kind == "enum":
            data = ParseEnum(lex, SerializeDataType.ENUM)
        elif kind == "errordef":
            data = ParseEnum(lex, SerializeDataType.ERROR_DEF)
        else:
            loginfo("proto line", lex.line, "expect rpcdata, rpc, enum or errordef")
            return None
        if data is None:
            return None
        datas.append(data)
    return datas
