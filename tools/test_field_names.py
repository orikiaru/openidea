"""Protocol field naming and stable JSON/wire-number regressions."""
import sys
import subprocess
import tempfile
import unittest
from pathlib import Path
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "cbaselib/proto/proto_tool"))
from field_naming import ValidFieldName
from proto_text import ParseProtoText
from serialize_data_parse import SerializeData, SerializeDataType
from serialize_data_file_gen import SerializeDataFileGen


from check_field_names import class_key


class FieldNamesTest(unittest.TestCase):
    def test_attributed_template_class(self):
        self.assertEqual(class_key("template < typename T > class [ [ nodiscard ] ] Result {".split()), "class")
        self.assertEqual(class_key("template < class T > struct [ [ nodiscard ] ] Result {".split()), "struct")
        self.assertEqual(class_key("template < class T = Box < int >> class Result {".split()), "class")


    def test_prefixes(self):
        for typ, name in [("int32_t", "iCount"), ("uint64_t", "iTaskId"),
                          ("bool", "bIsActive"), ("string", "sName"),
                          ("std::vector<int32_t>", "vRewards"),
                          ("std::map<int32_t, string>", "mItems"),
                          ("BaseInfo", "stBaseInfo"), ("float", "fRatio"), ("double", "dValue")]:
            self.assertTrue(ValidFieldName(typ, name), (typ, name))
            self.assertFalse(ValidFieldName(typ, "_" + name))
        self.assertFalse(ValidFieldName("int32_t", "sCount"))
        self.assertFalse(ValidFieldName("int32_t", "icount"))

    def test_alias_preserves_json_and_field_number(self):
        data = ParseProtoText("rpcdata Example { 7 int32_t iCount json count; 20 string sName; }")[0]
        generated = SerializeDataFileGen(data).GenDataFile()
        self.assertIn("os.WritePb(7, iCount)", generated)
        self.assertIn("os.WritePb(20, sName)", generated)
        self.assertIn('FromJsonValue(in["count"], decoded.iCount)', generated)
        self.assertIn('ToJsonValue(out["count"], iCount)', generated)
        self.assertIn('ToJsonValue(out["sName"], sName)', generated)
        self.assertNotIn('out["iCount"]', generated)

    def test_invalid_and_duplicate_names(self):
        self.assertIsNone(ParseProtoText("rpcdata Bad { 0 int32_t count; }"))
        self.assertIsNone(ParseProtoText("rpcdata Bad { 0 int32_t iOne json same; 1 int32_t iTwo json same; }"))
        self.assertIsNone(ParseProtoText("rpcdata Bad { 0 int32_t iOne; 0 int32_t iTwo; }"))

    def test_errordef_int32_bounds(self):
        for value in (1, 2147483647):
            with self.subTest(value=value):
                self.assertIsNotNone(ParseProtoText("errordef Errors { ERR_VALUE = %d; }" % value))
        for value in (0, -1, 2147483648, 4294967296):
            with self.subTest(value=value):
                self.assertIsNone(ParseProtoText("errordef Errors { ERR_VALUE = %d; }" % value))
        self.assertIsNotNone(ParseProtoText("enum Ordinary { ZERO = 0; NEGATIVE = -1; LARGE = 2147483648; }"))

    def test_xml_alias(self):
        node = ET.fromstring('<rpcdata name="Example"><member name="iCount" type="int32_t" seq="7" json="count"/></rpcdata>')
        data = SerializeData(SerializeDataType.RPC_DATA, node)
        self.assertTrue(data.ParseClassMember())
        self.assertEqual(data.members[0].json_name, "count")
        node[0].set("name", "count")
        self.assertFalse(SerializeData(SerializeDataType.RPC_DATA, node).ParseClassMember())


class ModuleGeneratorTest(unittest.TestCase):
    def run_generator(self, modules):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            module_file = temp / "module.xml"
            component_file = temp / "components.xml"
            manifest = temp / "modules.cmake"
            module_file.write_text("<Modules>" + modules + "</Modules>", encoding="utf-8")
            component_file.write_text("<Components/>", encoding="utf-8")
            command = [sys.executable, str(root / "cbaselib/proto/proto_tool/main.py"),
                 str(root / "modules/xmlconfig/proto.xml"), str(module_file),
                 str(component_file), str(temp / "include"), str(temp / "modules"),
                 str(temp / "deploy"), str(manifest)]
            result = subprocess.run(command, cwd=root, capture_output=True, text=True, errors="replace")
            output = manifest.read_text(encoding="utf-8") if manifest.exists() else None
            if result.returncode == 0:
                repeated = subprocess.run(command, cwd=root, capture_output=True, text=True, errors="replace")
                self.assertEqual(repeated.returncode, 0, repeated.stdout + repeated.stderr)
                self.assertEqual(manifest.read_text(encoding="utf-8"), output)
            return result, output

    def test_data_only_generator(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "generated"
            command = [sys.executable, str(root / "cbaselib/proto/proto_tool/main.py"),
                       "--data-only", str(root / "cbaselib/net/protocol/proto.xml"), str(output)]
            result = subprocess.run(command, cwd=root, capture_output=True, text=True, errors="replace")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            generated = output / "oi_rpcdata_workcompleted.h"
            enums = output / "rpc_enums.h"
            self.assertTrue(generated.exists())
            self.assertTrue(enums.exists())
            self.assertIn("iOperation", generated.read_text(encoding="utf-8"))
            self.assertIn("WORK_PROTOCOL_COMPLETED", enums.read_text(encoding="utf-8"))
            before = {path.name: path.read_bytes() for path in output.iterdir() if path.is_file()}
            repeated = subprocess.run(command, cwd=root, capture_output=True, text=True, errors="replace")
            self.assertEqual(repeated.returncode, 0, repeated.stdout + repeated.stderr)
            self.assertEqual(before, {path.name: path.read_bytes() for path in output.iterdir() if path.is_file()})
            bad = subprocess.run([sys.executable, str(root / "cbaselib/proto/proto_tool/main.py"),
                                  "--data-only", str(Path(temp) / "missing.xml"), str(output)],
                                 cwd=root, capture_output=True, text=True, errors="replace")
            self.assertNotEqual(bad.returncode, 0)

    def test_invalid_module_names(self):
        for module in ('<module/>', '<module name=""/>', '<module name="../bad"/>',
                       '<module name="bad name"/>', '<module name="é"/>',
                       '<module name="-bad"/>',
                       '<module name="Echo"/><module name="eCHO"/>'):
            with self.subTest(module=module):
                result, _ = self.run_generator(module)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_manifest_uses_generated_directory_names(self):
        result, manifest = self.run_generator('<module name="LogD"/><module name="Test-2_ABC"/>')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIsNotNone(manifest)
        self.assertEqual(manifest.split(), ["set(OI_MODULES", "logd", "test-2_abc", ")"])


if __name__ == "__main__":
    unittest.main()
