import re
from enum import Enum
from typing import Dict
from common import loginfo
from serialize_data_parse import SerializeData
from proto_parse import ProtocolGen
from proto_text import ProtoLexer, ValidDeclName
from dataclasses import dataclass

NAME_RE = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_-]*")

def ReadDeclName(lex):
    lex.skip()
    match = NAME_RE.match(lex.text, lex.i)
    if not match:
        return lex.fail("expect name")
    lex.i = match.end()
    return match.group(0)

def ParseModuleText(text):
    lex = ProtoLexer(text)
    components = []
    modules = []
    while not lex.eof():
        kind = lex.ident()
        if kind is None:
            return None
        if kind == "component":
            name = ReadDeclName(lex)
            type_name = lex.ident()
            if name is None or type_name is None or not lex.punct("{"):
                return None
            rpc_names = []
            while not lex.eof() and lex.peek_char() != "}":
                kind = lex.ident()
                if kind not in ("rpc", "notify"):
                    return lex.fail("expect rpc or notify")
                rpc_name = lex.ident()
                if rpc_name is None:
                    return None
                binder = None
                if lex.peek_char() != ";":
                    if lex.ident() != "bind":
                        return lex.fail("expect ';' or 'bind <method>'")
                    binder = lex.ident()
                    if binder is None or not ValidDeclName(binder):
                        return lex.fail("invalid registration method")
                if not lex.punct(";"):
                    return None
                rpc_names.append((kind, rpc_name, binder))
            if not lex.punct("}"):
                return None
            components.append((name, type_name, rpc_names))
            continue
        if kind != "module":
            loginfo("proto line", lex.line, "expect module")
            return None
        name = ReadDeclName(lex)
        if name is None or not lex.punct("{"):
            return None
        component_names = []
        while not lex.eof() and lex.peek_char() != "}":
            word = lex.ident()
            if word is None:
                return None
            if word != "component":
                loginfo("proto line", lex.line, "expect component")
                return None
            component_name = ReadDeclName(lex)
            if component_name is None or not lex.punct(";"):
                return None
            component_names.append(component_name)
        if not lex.punct("}"):
            return None
        modules.append((name, component_names))
    if lex.unclosed_comment:
        return None
    return components, modules

class ComponentType(Enum):
    Server = 1
    Client = 2

@dataclass
class ProtocolHandler:
    kind: str
    protocol: SerializeData
    binder: str = None


class Component:
    def __init__(self) -> None:
        self.name = ""          #组件名
        self.type = ""
        self.protos = {}        #type:Dict[str,SerializeData] #组件所涉及的协议

    def GetType(self):
        if(self.type == "Server"):
            return ComponentType.Server
        return ComponentType.Client

    def Parse(self, name, type_name, rpc_names, proto_gen:ProtocolGen):
        self.name = name
        self.type = type_name
        if(not self.name or not self.type):
            loginfo("ParseComponent Error NoName Or NoType Component = {} type = {}".format(self.name, self.type))
            return False
        if(self.type != "Server" and self.type != "Client"):
            loginfo("ParseComponent Error Type Component = {} type = {}".format(self.name, self.type))
            return False

        for kind, pname, binder in rpc_names:
            if(pname in self.protos):
                loginfo("ParseComponent Error Protocol Repeat Component=", self.name, "Protocol=", pname)
                return False
            proto = proto_gen.GetProtocol(pname)
            if(not proto):
                loginfo("ParseComponent Error NoProtocolExsit Component=", self.name, "Protocol=", pname)
                return False
            self.protos[pname] = ProtocolHandler(kind, proto, binder)
        return True


class Module:
    def __init__(self) -> None:
        self.name = ""          #模块名
        self.components = {}    #type:Dict[str,Component]       #模块所涉及的所有组件
        self.protos = {}        #type:Dict[str,SerializeData]   #模块涉及的所有协议,自然排重,方便生成

    def Parse(self, component_names, component_map:Dict[str,Component]):
        for c_name in component_names:
            if c_name.lower() in {name.lower() for name in self.components}:
                loginfo("Duplicate component source in process", self.name, c_name)
                return False
            if(not component_map.get(c_name)):
                loginfo("ParseModule Error NoComponent Module=", self.name, "Component=", c_name)
                return False
            self.components[c_name] = component_map[c_name]
            for name, handler in self.components[c_name].protos.items():
                self.protos[name] = handler.protocol
        return True


class ModuleGen:
    def __init__(self, proto_gen: ProtocolGen, module_path:str) -> None:
        self.proto_gen = proto_gen
        self.modules = {}       #type:Dict[str,Module]
        self.components = {}    #type:Dict[str,Component]
        self.module_path = module_path

    def Gen(self):
        if(not self.Parse()):
            return False
        return True

    def Parse(self):
        try:
            with open(self.module_path, encoding="utf-8") as fp:
                module_text = fp.read()
        except OSError as exc:
            loginfo("ParseModule Error", exc)
            return False
        declarations = ParseModuleText(module_text)
        if declarations is None:
            return False
        components, modules = declarations
        for name, type_name, rpc_names in components:
            if not self.ParseComponent(name, type_name, rpc_names):
                return False
        for name, component_names in modules:
            if(not self.ParseModule(name, component_names)):
                return False
        return True

    def ParseModule(self, name, component_names):
        if not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_-]*", name):
            loginfo("ParseModule Error Invalid Module=", name)
            return False
        module_dir = name.lower()
        if module_dir in self.modules:
            loginfo("ParseModule Error Repeat Module=", name)
            return False
        module = Module()
        module.name = name
        if(not module.Parse(component_names, self.components)):
            loginfo("{} {}".format(module.name, "[Module ParseFault]"))
            return False
        self.modules[module_dir] = module
        loginfo("{} {}".format("[Module ParseSuccess]", module.name))
        return True

    def ParseComponent(self, name, type_name, rpc_names):
        if not ValidDeclName(name) or name.lower() in ("main", "oi_module_components", "modulecomponents"):
            loginfo("Invalid C++ component name", name)
            return False
        component = Component()
        if(not component.Parse(name, type_name, rpc_names, self.proto_gen)):
            loginfo("{}{}".format(component.name, "[Component ParseFault]"))
            return False
        if(component.name in self.components.keys()):
            loginfo("ParseComponent Error Repeat Component=", component.name)
            return False
        self.components[component.name] = component
        loginfo("{} {}".format("[Component ParseSuccess]", component.name))
        return True
