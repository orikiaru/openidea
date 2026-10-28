from common import loginfo
from serialize_data_parse import ClassMember
from serialize_data_parse import SerializeData
from serialize_data_parse import SerializeDataType

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

def ParseRpcData(lex):
    name = lex.ident()
    if name is None:
        return None
    is_key = False
    if lex.peek_ident() == "key":
        lex.ident()
        is_key = True
    if not lex.punct("{"):
        return None
    data = SerializeData(SerializeDataType.RPC_DATA, None)
    data.name = name
    data.is_key = is_key
    while not lex.eof() and lex.peek_char() != "}":
        seq = lex.number()
        if seq is None:
            return None
        typ = ParseType(lex)
        field = lex.ident()
        if typ is None or field is None or not lex.punct(";"):
            return None
        member = ClassMember()
        member.name = field
        member.type = typ
        member.seq = seq
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
    for need in ("id", "res_id", "request", "response"):
        if need not in fields:
            loginfo("rpc missing", need, "name=", name)
            return None
    data = SerializeData(SerializeDataType.RPC, None)
    data.name = name
    data.id = fields["id"]
    data.res_id = fields["res_id"]
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

def ParseProtoText(text):
    lex = ProtoLexer(text)
    datas = []
    while not lex.eof():
        kind = lex.ident()
        if kind == "rpcdata":
            data = ParseRpcData(lex)
        elif kind == "rpc":
            data = ParseRpc(lex)
        else:
            loginfo("proto line", lex.line, "expect rpcdata or rpc")
            return None
        if data is None:
            return None
        datas.append(data)
    return datas
