"""Protocol field naming and stable JSON/wire-number regressions."""
import sys
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


if __name__ == "__main__":
    unittest.main()
