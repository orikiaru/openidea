from enum import Enum
from typing import List,Tuple,Dict,Union,Optional
from xml.etree.ElementTree import Element
from common import openxml
from common import loginfo
from serialize_data_parse import SerializeData
from proto_parse import ProtocolGen
class ComponentType(Enum):
    Server = 1
    Client = 2
    
class Component:
    def __init__(self) -> None:
        self.name = ""          #组件名
        self.type = ""
        self.protos = {}        #type:Dict[str,SerializeData] #组件所涉及的协议

    def GetType(self):
        if(self.type == "Server"):
            return ComponentType.Server
        return ComponentType.Client
        
    def Parse(self, c_xml:Element, proto_gen:ProtocolGen):
        self.name = c_xml.get("name")
        self.type = c_xml.get("type")
        protocols = c_xml.findall("protocol")
        if(not self.name or not self.type):
            #TODO 标志性错误提示,暂时这个日志提示不到出错位置?
            loginfo("ParseComponent Error NoName Or NoType Component = {} type = {}".format(self.name, self.type))
            return False
        if(self.type != "Server" and self.type != "Client"):
            loginfo("ParseComponent Error Type Component = {} type = {}".format(self.name, self.type))
            return False

        for p in protocols:
            pname = p.get("name")
            if(pname in self.protos):
                loginfo("ParseComponent Error Protocol Repeat Component=", self.name, "Protocol=", pname)
                return False
            proto = proto_gen.GetProtocol(pname)
            if(not proto):
                loginfo("ParseComponent Error NoProtocolExsit Component=", self.name, "Protocol=", pname)
                return False
            self.protos[pname] = proto
        return True


class Module:
    def __init__(self) -> None:
        self.name = ""          #模块名
        self.components = {}    #type:Dict[str,Component]       #模块所涉及的所有组件
        self.protos = {}        #type:Dict[str,SerializeData]   #模块涉及的所有协议,自然排重,方便生成
        
    def Parse(self, m_xml:Element, component_map:Dict[str,Component]):
        self.name = m_xml.get("name")
        coms = m_xml.findall("component")
        for c in coms:
            c_name = c.get("name")
            if(not component_map.get(c_name)):
                loginfo("ParseModule Error Component Repeat Module=", self.name, "Component=", c_name)
                return False
            self.components[c_name] = component_map[c_name]
            for p in self.components[c_name].protos.values():
                self.protos[p.GetName()] = p
        return True
                

        
class ModuleGen:
    def __init__(self, proto_gen: ProtocolGen, module_xml_path:str, component_xml_path:str) -> None:
        self.proto_gen = proto_gen
        self.modules = {}       #type:Dict[str,Module]
        self.components = {}    #type:Dict[str,Component]
        self.module_xml_path = module_xml_path
        self.component_xml_path = component_xml_path
        
    def Gen(self):
        if(not self.Parse()):
            return False
        return True
        
    def Parse(self):
        root = openxml(self.component_xml_path)
        comps = root.findall("component")
        for c in comps:
            if(not self.ParseComponent(c)):
                return False
            
        root = openxml(self.module_xml_path)
        modules = root.findall("module")
        for m_xml in modules:
            if(not self.ParseModule(m_xml)):
                return False
        return True
            
    def ParseModule(self, m_xml):
        module = Module()
        if(not module.Parse(m_xml, self.components)):
            loginfo("{} {}".format(module.name, "[Module ParseFault]"))
            return False
        if(module.name in self.modules.keys()):
            loginfo("ParseModule Error Repeat Module=", module.name)
            return False
        self.modules[module.name] = module
        loginfo("{} {}".format("[Module ParseSuccess]", module.name))
        return True
        

    def ParseComponent(self, c_xml):
        component = Component()
        if(not component.Parse(c_xml, self.proto_gen)):
            loginfo("{}{}".format(component.name, "[Component ParseFault]"))
            return False
        if(component.name in self.components.keys()):
            loginfo("ParseComponent Error Repeat Component=", component.name)
            return False
        self.components[component.name] = component
        loginfo("{} {}".format("[Component ParseSuccess]", component.name))
        return True

            
