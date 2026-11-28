"""Protocol field naming and stable JSON/wire-number regressions."""
import sys
import subprocess

from textio import use_utf8_children
use_utf8_children()
import tempfile
import unittest
from pathlib import Path
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "cbaselib/proto/proto_tool"))
from field_naming import ValidFieldName
from proto_parse import ProtocolGen
from proto_text import ParseProtoText
from serialize_data_parse import SerializeData, SerializeDataType
from serialize_data_file_gen import SerializeDataFileGen
from module_data_parse import Module, Component, ModuleGen
from module_file_gen import ModuleFileGen


from check_field_names import class_key


class FieldNamesTest(unittest.TestCase):
    def test_fixed_width_and_bytes_types(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "types.proto"
            path.write_text("rpcdata Bad { 0 long stValue; }", encoding="utf-8")
            self.assertFalse(ProtocolGen(path).Gen())
            path.write_text("rpcdata Bad { 0 vector<long> vValues; }", encoding="utf-8")
            self.assertFalse(ProtocolGen(path).Gen())
            for bad_type, field_name in (("size_t", "stValue"), ("Mystery", "stValue"), ("vector<Mystery>", "vValues")):
                path.write_text("rpcdata Bad { 0 %s %s; }" % (bad_type, field_name), encoding="utf-8")
                self.assertFalse(ProtocolGen(path).Gen(), bad_type)
            path.write_text("rpcdata Good { 0 int64_t iValue; 1 Bytes stData; 2 OI::Base::Bytes stOther; 3 ::OI::Base::Bytes stThird; 4 std::uint32_t iCount; }", encoding="utf-8")
            gen = ProtocolGen(path)
            self.assertTrue(gen.Gen())
            generated = SerializeDataFileGen(gen.serialize_datas[0]).GenDataFile()
            self.assertNotIn("OI::Base::::OI::Base::Bytes", generated)
            self.assertGreaterEqual(generated.count("::OI::Base::Bytes"), 3)
            self.assertIn("uint32_t iCount", generated)

    def test_component_declaration_and_reference(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "module.proto"
            proto = Path(tmp) / "rpc.proto"
            proto.write_text("rpcdata Req {} rpcdata Res {} rpc Echo { id 101; request Req; response Res; }", encoding="utf-8")
            protocol = ProtocolGen(proto)
            self.assertTrue(protocol.Gen())
            path.write_text("module App { component Sample; } component Sample Server { rpc Echo; }", encoding="utf-8")
            gen = ModuleGen(protocol, path)
            self.assertTrue(gen.Gen())
            self.assertIn("Sample", gen.modules["app"].components)
            self.assertIn("Echo", gen.modules["app"].components["Sample"].protos)
            self.assertIn("Echo", gen.modules["app"].protos)
            path.write_text("component Sample Server { rpc Missing; } module App { component Sample; }", encoding="utf-8")
            self.assertFalse(ModuleGen(ProtocolGen([]), path).Gen())
            path.write_text("component Sample BadType { } module App { component Sample; }", encoding="utf-8")
            self.assertFalse(ModuleGen(ProtocolGen([]), path).Gen())

    def test_declaration_names_and_enum_bounds(self):
        for text in ("rpcdata class {}", "enum class { ITEM = 1; }", "enum Good { ITEM = 9223372036854775808; }"):
            self.assertIsNone(ParseProtoText(text), text)
        self.assertIsNotNone(ParseProtoText("enum Good { ITEM = 2147483648; }"))

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
        data = ParseProtoText("rpcdata Example json { 7 [json count] int32_t iCount; 20 string sName; }")[0]
        generated = SerializeDataFileGen(data).GenDataFile()
        self.assertIn("os.WritePb(7, iCount)", generated)
        self.assertIn("os.WritePb(20, sName)", generated)
        self.assertIn('FromJsonValue(in["count"], decoded.iCount)', generated)
        self.assertIn('ToJsonValue(out["count"], iCount)', generated)
        self.assertIn('ToJsonValue(out["sName"], sName)', generated)
        self.assertNotIn('out["iCount"]', generated)
        self.assertNotIn("oi_db_row.h", generated)
        plain = ParseProtoText("rpcdata Example json db { 0 [json] [db] int32_t iCount; }")[0]
        self.assertEqual(plain.members[0].json_name, "iCount")
        self.assertEqual(plain.members[0].db_name, "iCount")

    def test_container_deserialize_does_not_clear_existing_values(self):
        data = ParseProtoText("rpcdata Example { 0 vector<int32_t> vNumbers; 1 map<string, int32_t> mFlags; 2 int32_t iSibling; }")[0]
        generated = SerializeDataFileGen(data).GenDataFile()
        self.assertIn("os.WritePb(0, vNumbers)", generated)
        self.assertIn("os.WritePb(1, mFlags)", generated)
        self.assertIn("static constexpr std::array<::OI::Base::PbField, 3> fields", generated)
        self.assertIn("std::array<uint8_t, 3> seen{};", generated)
        for name, sequence in (("vNumbers", 0), ("mFlags", 1), ("iSibling", 2)):
            self.assertIn(f"OI_PB_FIELD(Example, {sequence}, {name})", generated)
        self.assertIn("return os.ReadFields(this, fields, seen);", generated)
        self.assertNotIn("switch (field_seq)", generated)
        self.assertNotIn("while (os.RemainSize()", generated)
        self.assertNotIn("vNumbers.clear()", generated)
        self.assertNotIn("mFlags.clear()", generated)

    def test_generated_serialization_overrides(self):
        for text in ("rpcdata Empty { }", "rpcdata Example { 0 int32_t iCount; }"):
            generated = SerializeDataFileGen(ParseProtoText(text)[0]).GenDataFile()
            self.assertIn("::OI::Base::BytesStream& serialize(::OI::Base::BytesStream& os) const override", generated)
            self.assertIn("::OI::Base::BytesStream& deserialize(::OI::Base::BytesStream& os) override", generated)
            self.assertNotIn("virtual ::OI::Base::BytesStream&", generated)

    def test_deserialize_field_table_is_sorted_and_empty_is_supported(self):
        data = ParseProtoText("rpcdata Example { 20 int32_t iLast; 7 string sFirst; 13 vector<int32_t> vMiddle; }")[0]
        generated = SerializeDataFileGen(data).GenDataFile()
        entries = ["OI_PB_FIELD(Example, 7, sFirst)",
                   "OI_PB_FIELD(Example, 13, vMiddle)",
                   "OI_PB_FIELD(Example, 20, iLast)"]
        positions = [generated.index(entry) for entry in entries]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("static constexpr std::array<::OI::Base::PbField, 3> fields", generated)
        self.assertNotIn("switch (field_seq)", generated)
        self.assertNotIn("while (os.RemainSize()", generated)
        empty = SerializeDataFileGen(ParseProtoText("rpcdata Empty { }")[0]).GenDataFile()
        self.assertIn("static constexpr std::array<::OI::Base::PbField, 0> fields", empty)
        self.assertIn("std::array<uint8_t, 0> seen{};", empty)
        self.assertIn("return os.ReadFields(this, fields, seen);", empty)
        self.assertNotIn("switch (field_seq)", empty)
        self.assertNotIn("while (os.RemainSize()", empty)

    def test_dispatch_variants(self):
        data = ParseProtoText("rpcdata Example { 20 int32_t iLast; 7 string sFirst; 13 vector<int32_t> vMiddle; }")[0]
        macro = SerializeDataFileGen(data, "macro").GetPbDeserialize()
        switch = SerializeDataFileGen(data, "switch").GetPbDeserialize()
        self.assertEqual(macro, SerializeDataFileGen(data).GetPbDeserialize())
        self.assertIn("OI_PB_FIELD(Example, 7, sFirst)", macro)
        self.assertIn("return os.ReadFields(this, fields, seen);", macro)
        self.assertNotIn("OI_PB_FIELD", switch)
        self.assertIn("std::array<uint8_t, 3> seen{};", switch)
        self.assertIn("switch (field_seq)", switch)
        self.assertEqual([switch.index("case %d:" % seq) for seq in (7, 13, 20)],
                         sorted(switch.index("case %d:" % seq) for seq in (7, 13, 20)))
        for index, name in enumerate(("sFirst", "vMiddle", "iLast")):
            self.assertIn("if (seen[%d] != 0)" % index, switch)
            self.assertIn("seen[%d] = 1;" % index, switch)
            self.assertIn("os.ReadPb(field_size, %s);" % name, switch)
        self.assertIn("throw ::OI::Base::ParserException();", switch)
        self.assertIn("default:\n                os.SkipPb(field_size);", switch)
        empty = SerializeDataFileGen(ParseProtoText("rpcdata Empty { }")[0], "switch").GetPbDeserialize()
        self.assertNotIn("seen", empty)
        self.assertNotIn("case ", empty)
        self.assertNotIn("switch (", empty)
        self.assertIn("os.ReadPbHeader(field_seq, field_size);", empty)
        self.assertIn("os.SkipPb(field_size);", empty)

    def test_codec_markers_skip_fields_and_blob_complex_values(self):
        text = "rpcdata Child json { 0 int32_t iNumber; } rpcdata Example json db { 7 [json count, db id] int32_t iCount; 20 string sName; 3 [nojson] int32_t iSecret; 4 [nodb] string sCache; 5 vector<int32_t> vNumbers; 6 map<string, int32_t> mFlags; 8 [db child] Child stChild; }"
        data = ParseProtoText(text)[1]
        generated = SerializeDataFileGen(data).GenDataFile()
        self.assertNotIn("decoded.iSecret", generated)
        self.assertNotIn('out["iSecret"]', generated)
        self.assertIn('ToJsonValue(out["sCache"], sCache)', generated)
        self.assertNotIn("`sCache`", generated)
        self.assertIn("`iSecret`", generated)
        self.assertIn("`id`", generated)
        self.assertIn("`child`", generated)
        self.assertIn('"id"', generated)
        self.assertIn('pMember == &Example::iCount', generated)
        self.assertIn('"vNumbers"', generated)
        self.assertIn('pMember == &Example::vNumbers', generated)
        self.assertIn('"mFlags"', generated)
        self.assertIn('pMember == &Example::mFlags', generated)
        self.assertIn('"child"', generated)
        self.assertIn('pMember == &Example::stChild', generated)
        self.assertIn('"iSecret"', generated)
        self.assertIn('pMember == &Example::iSecret', generated)
        self.assertIn("::OI::Db::FromRow(*this, vColumns, vRow)", generated)
        self.assertIn("::OI::Db::AppendRowParameters(*this, vParameters)", generated)
        self.assertIn("::OI::Db::ToParameterMap(*this)", generated)
        self.assertIn("::OI::Db::FromCellMap(*this, mCells)", generated)
        self.assertNotIn("pMember == &Example::sCache", generated)
        self.assertIn('#include "oi_db_row.h"', generated)

    def test_default_db_table_uses_lowercase_structure_name(self):
        plain, row = ParseProtoText("rpcdata Plain {} rpcdata HTTP2_Log db { 0 [dbkey] uint32_t iId; }")
        generated = SerializeDataFileGen(row).GenDataFile()
        self.assertIn('static constexpr const char* DbTableName()', generated)
        self.assertIn('return "http2_log";', generated)
        self.assertNotIn('DbTableName', SerializeDataFileGen(plain).GenDataFile())

    def test_invalid_and_duplicate_names(self):
        self.assertIsNone(ParseProtoText("rpcdata Bad { 0 int32_t count; }"))
        self.assertIsNone(ParseProtoText("rpcdata Bad json { 0 [json same] int32_t iOne; 1 [json same] int32_t iTwo; }"))
        self.assertIsNone(ParseProtoText("rpcdata Bad { 0 int32_t iOne; 0 int32_t iTwo; }"))
        self.assertIsNone(ParseProtoText("rpcdata Bad { 0 [nojson] int32_t iOne; }"))
        self.assertIsNone(ParseProtoText("rpcdata Bad key json { 0 int32_t iOne; }"))
        self.assertIsNone(ParseProtoText("rpcdata Bad json { 0 [nojson] int32_t iOne; }"))
        self.assertIsNone(ParseProtoText("rpcdata Bad db { 0 [db same] int32_t iOne; 1 [db same] int32_t iTwo; }"))
        self.assertIsNone(ParseProtoText("rpcdata Bad db { 0 [nodb] int32_t iOne; }"))
        self.assertIsNone(ParseProtoText("rpcdata Bad json { 0 int32_t iOne json c; }"))
        self.assertIsNone(ParseProtoText("rpcdata Bad json db { 0 [json c, nojson] int32_t iOne; }"))
        self.assertIsNone(ParseProtoText("rpcdata Plain { 0 [db] int32_t iId; }"))
        self.assertIsNone(ParseProtoText("rpcdata Plain { 0 [db id] int32_t iId; }"))
        self.assertIsNone(ParseProtoText("rpcdata Plain { 0 [nodb] int32_t iId; }"))
        self.assertIsNone(ParseProtoText("rpcdata Plain { 0 [dbjson] vector<int32_t> vNumbers; }"))
        self.assertIsNone(ParseProtoText("rpcdata Plain json { 0 [dbjson] vector<int32_t> vNumbers; }"))
        self.assertIsNone(ParseProtoText("rpcdata Row db { 0 [nodb] [db id] int32_t iId; }"))
        self.assertIsNone(ParseProtoText("rpcdata Row db { 0 [db id] [nodb] int32_t iId; }"))
        self.assertIsNone(ParseProtoText("rpcdata Row db { 0 [nodb] [dbjson] vector<int32_t> vNumbers; }"))
        self.assertIsNone(ParseProtoText("rpcdata Row db { 0 [dbjson] [nodb] vector<int32_t> vNumbers; }"))
        self.assertIsNone(ParseProtoText("rpcdata Row db { 0 [dbjson, nodb] vector<int32_t> vNumbers; }"))

    def load_proto(self, text):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "sample.proto"
            path.write_text(text, encoding="utf-8")
            return ProtocolGen(str(path)).Gen()

    def load_files(self, texts):
        with tempfile.TemporaryDirectory() as temp:
            paths = []
            for index, text in enumerate(texts):
                path = Path(temp) / ("part%d.proto" % index)
                path.write_text(text, encoding="utf-8")
                paths.append(str(path))
            return ProtocolGen(paths).Gen()

    def test_multi_proto_files(self):
        child = "rpcdata Child { 0 int32_t iNumber; }"
        row = "rpcdata Row db { 0 int64_t iId; 1 Child stChild; }"
        self.assertTrue(self.load_files([child, row]))
        self.assertTrue(self.load_files([row, child]))
        self.assertFalse(self.load_files([child, child]))
        req = "rpcdata Req { 0 int32_t iNumber; }"
        res = "rpcdata Res { 0 int32_t iNumber; } rpc Echo { id 9; request Req; response Res; }"
        self.assertTrue(self.load_files([req, res]))
        self.assertFalse(self.load_files([res]))
        same_id = "rpcdata Req2 { 0 int32_t iNumber; } rpcdata Res2 { 0 int32_t iNumber; } rpc Other { id 9; request Req2; response Res2; }"
        self.assertFalse(self.load_files([req, res, same_id]))
        self.assertFalse(self.load_files(["enum One { ITEM = 1; }", "enum Two { ITEM = 2; }"]))

    def test_db_top_level_accepts_complex_values_as_blob(self):
        self.assertTrue(self.load_proto("rpcdata Child { 0 int32_t iNumber; } rpcdata Row db { 0 int64_t iId; 1 vector<int32_t> vNumbers; 2 Child stChild; 3 map<string, int32_t> mFlags; }"))
        self.assertFalse(self.load_proto("rpcdata Bad db { 0 Missing stChild; }"))
        self.assertFalse(self.load_proto("rpcdata Row db { 0 vector<bool> vFlags; }"))
        self.assertFalse(self.load_proto("rpcdata Row { 0 vector<bool> vFlags; }"))
        self.assertFalse(self.load_proto("rpcdata Row { 0 map<int32_t, vector<bool>> mFlags; }"))
        self.assertFalse(self.load_proto("rpcdata Row { 0 vector<vector<bool>> vFlags; }"))
        self.assertFalse(self.load_proto("rpcdata Child { 0 int32_t iNumber; } rpcdata Row json { 0 Child stChild; }"))
        self.assertFalse(self.load_proto("rpcdata Child { 0 int32_t iNumber; } rpcdata Row json db { 0 Child stChild; }"))
        self.assertTrue(self.load_proto("rpcdata Child json { 0 int32_t iNumber; } rpcdata Row json db { 0 Child stChild; }"))
        self.assertTrue(self.load_proto("rpcdata Child json { 0 int32_t iNumber; } rpcdata Row json { 0 Child stChild; 1 [nojson] vector<int32_t> vSkip; }"))
        self.assertTrue(self.load_proto("rpcdata Key key { 0 int32_t iNumber; } rpcdata Row json { 0 int32_t iId; 1 [nojson] map<Key, int32_t> mKeyed; }"))
        self.assertFalse(self.load_proto("rpcdata Key key { 0 int32_t iNumber; } rpcdata Row json { 0 map<Key, int32_t> mKeyed; }"))
        json_db = ParseProtoText("rpcdata Child json { 0 int32_t iNumber; } rpcdata Row db { 0 [dbjson] vector<int32_t> vNumbers; 1 [dbjson] Child stChild; 2 vector<int32_t> vRaw; }")[1]
        generated = SerializeDataFileGen(json_db).GenDataFile()
        self.assertRegex(generated, r"std::array\s*<\s*(?:::OI::Db::)?Field\s*<\s*Row\s*>\s*,\s*3\s*>")
        self.assertIn("DbFindField(", generated)
        self.assertNotIn("std::make_tuple", generated)
        for member, codec in (("vNumbers", "Json"), ("stChild", "Json"), ("vRaw", "Value")):
            self.assertIn(f"EncodeDb{codec}(stRow.{member})", generated)
            self.assertIn(f"DecodeDb{codec}(sBytes, stRow.{member})", generated)
        self.assertIn('"vNumbers"', generated)
        self.assertIn('pMember == &Row::vNumbers', generated)
        self.assertIn('"stChild"', generated)
        self.assertIn('pMember == &Row::stChild', generated)
        self.assertIn('"vRaw"', generated)
        self.assertIn('pMember == &Row::vRaw', generated)
        self.assertIn('#include "oi_db_json.h"', generated)
        self.assertTrue(self.load_proto("rpcdata Child json { 0 int32_t iNumber; } rpcdata Row db { 0 [dbjson] Child stChild; }"))
        self.assertFalse(self.load_proto("rpcdata Child { 0 int32_t iNumber; } rpcdata Row db { 0 [dbjson] Child stChild; }"))
        self.assertFalse(self.load_proto("rpcdata Row db { 0 [dbjson] int32_t iId; }"))
        self.assertFalse(self.load_proto("rpcdata Row db { 0 [dbjson] vector<bool> vFlags; }"))
        self.assertFalse(self.load_proto("rpcdata Row json { 0 [dbjson] vector<int32_t> vNumbers; }"))
        self.assertTrue(self.load_proto("rpcdata Row db { 0 [db id] [dbjson] vector<int32_t> vNumbers; 1 [nodb] string sCache; }"))
        self.assertIsNone(ParseProtoText("rpcdata Bad json db { 0 [json] [nojson] int32_t iOne; }"))

    def test_db_key_and_type_markers(self):
        row = ParseProtoText("rpcdata Child json { 0 int32_t iNumber; } rpcdata Row json db { 0 [dbkey] int64_t iId; 1 [dbkey, dbtype varbinary 32] string sZone; 2 [dbtype TEXT] string sNote; 3 [dbtype MEDIUMBLOB] Bytes stRaw; 4 [db child] [dbjson] [dbtype MEDIUMTEXT] Child stChild; 5 int32_t iCount; }")[1]
        generated = SerializeDataFileGen(row).GenDataFile()
        for member, sql_type, key in (("sZone", "VARBINARY(32)", "true"),
                                      ("sNote", "TEXT", "false"),
                                      ("stRaw", "MEDIUMBLOB", "false"),
                                      ("stChild", "MEDIUMTEXT", "false")):
            codec = "Json" if member == "stChild" else "Value"
            suffix = ', false, " DEFAULT (0x7B7D)"' if member == "stChild" else ""
            self.assertIn(f'DecodeDb{codec}(sBytes, stRow.{member});\n                }}, "{sql_type}", {key}{suffix}}}', generated)
        self.assertIn('"iId"', generated)
        self.assertIn('pMember == &Row::iId', generated)
        self.assertIn('"sZone"', generated)
        self.assertIn('pMember == &Row::sZone', generated)
        self.assertIn('"sNote"', generated)
        self.assertIn('pMember == &Row::sNote', generated)
        self.assertIn('"stRaw"', generated)
        self.assertIn('pMember == &Row::stRaw', generated)
        self.assertIn('"child"', generated)
        self.assertIn('pMember == &Row::stChild', generated)
        self.assertIn('"iCount"', generated)
        self.assertIn('pMember == &Row::iCount', generated)
        self.assertTrue(self.load_proto("rpcdata Row db { 0 [dbkey] bool bFlag; 1 [dbkey] uint32_t iShard; 2 [dbkey] [dbtype BINARY 8] Bytes stCode; }"))
        self.assertFalse(self.load_proto("rpcdata Row db { 0 [dbkey] [dbtype CHAR 8] Bytes stCode; }"))
        self.assertTrue(self.load_proto("rpcdata Row db { 0 [dbtype VARBINARY 16] vector<int32_t> vNumbers; }"))
        # 主键只能是整数、bool、定长 string / Bytes。
        self.assertFalse(self.load_proto("rpcdata Row db { 0 [dbkey] double dScore; }"))
        self.assertFalse(self.load_proto("rpcdata Row db { 0 [dbkey] string sName; }"))
        self.assertFalse(self.load_proto("rpcdata Row db { 0 [dbkey] [dbtype BLOB] string sName; }"))
        self.assertFalse(self.load_proto("rpcdata Row db { 0 [dbkey] vector<int32_t> vNumbers; }"))
        self.assertFalse(self.load_proto("rpcdata Child json { 0 int32_t iNumber; } rpcdata Row json db { 0 [dbkey] [dbjson] Child stChild; }"))
        # utf8mb4 InnoDB 主键整体不超过 3072 字节。文本每字符 4 字节，变长列再加长度前缀，多列合计。
        self.assertTrue(self.load_proto("rpcdata Row db { 0 [dbkey] [dbtype VARCHAR 767] string sName; }"))
        self.assertFalse(self.load_proto("rpcdata Row db { 0 [dbkey] [dbtype VARCHAR 768] string sName; }"))
        self.assertTrue(self.load_proto("rpcdata Row db { 0 [dbkey] [dbtype VARBINARY 3070] Bytes stCode; }"))
        self.assertFalse(self.load_proto("rpcdata Row db { 0 [dbkey] [dbtype VARBINARY 3072] Bytes stCode; }"))
        self.assertFalse(self.load_proto("rpcdata Row db { 0 [dbkey] [dbtype VARBINARY 65535] Bytes stCode; }"))
        self.assertFalse(self.load_proto("rpcdata Row db { 0 [dbkey] int64_t iId; 1 [dbkey] [dbtype VARBINARY 3072] Bytes stCode; }"))
        self.assertTrue(self.load_proto("rpcdata Row db { 0 [dbtype VARBINARY 65535] Bytes stRaw; }"))
        # 列类型要和字段对得上：数值不能换，二进制值不能存文本，JSON 不能存二进制。
        self.assertFalse(self.load_proto("rpcdata Row db { 0 [dbtype VARCHAR 8] int32_t iCount; }"))
        self.assertFalse(self.load_proto("rpcdata Row db { 0 [dbtype TEXT] Bytes stRaw; }"))
        self.assertFalse(self.load_proto("rpcdata Row db { 0 [dbtype TEXT] vector<int32_t> vNumbers; }"))
        self.assertFalse(self.load_proto("rpcdata Row json db { 0 [dbjson] [dbtype BLOB] vector<int32_t> vNumbers; }"))
        # 类型名、长度和标记组合。
        self.assertIsNone(ParseProtoText("rpcdata Row db { 0 [dbtype VARCHAR] string sName; }"))
        self.assertIsNone(ParseProtoText("rpcdata Row db { 0 [dbtype VARCHAR 0] string sName; }"))
        self.assertIsNone(ParseProtoText("rpcdata Row db { 0 [dbtype CHAR 256] string sName; }"))
        self.assertIsNone(ParseProtoText("rpcdata Row db { 0 [dbtype TEXT 5] string sName; }"))
        self.assertIsNone(ParseProtoText("rpcdata Row db { 0 [dbtype JSON] string sName; }"))
        self.assertIsNone(ParseProtoText("rpcdata Row db { 0 [dbkey] [dbkey] int32_t iId; }"))
        self.assertIsNone(ParseProtoText("rpcdata Row db { 0 [dbtype TEXT] [dbtype BLOB] string sName; }"))
        self.assertIsNone(ParseProtoText("rpcdata Row db { 0 int32_t iId; 1 [nodb] [dbkey] int32_t iOther; }"))
        self.assertIsNone(ParseProtoText("rpcdata Row db { 0 int32_t iId; 1 [dbtype TEXT] [nodb] string sOther; }"))
        self.assertIsNone(ParseProtoText("rpcdata Row { 0 [dbkey] int32_t iId; }"))
        self.assertIsNone(ParseProtoText("rpcdata Row { 0 [dbtype TEXT] string sName; }"))

    def test_errordef_int32_bounds(self):
        for value in (1, 2147483647):
            with self.subTest(value=value):
                self.assertIsNotNone(ParseProtoText("errordef Errors { ERR_VALUE = %d; }" % value))
        for value in (0, -1, 2147483648, 4294967296):
            with self.subTest(value=value):
                self.assertIsNone(ParseProtoText("errordef Errors { ERR_VALUE = %d; }" % value))
        self.assertIsNotNone(ParseProtoText("enum Ordinary { ZERO = 0; NEGATIVE = -1; LARGE = 2147483648; }"))

    def test_xml_alias(self):
        node = ET.fromstring('<rpcdata name="Example" json="true" db="true"><member name="iCount" type="int32_t" seq="7" json="count"/><member name="sCache" type="string" seq="8" nodb="true"/></rpcdata>')
        data = SerializeData(SerializeDataType.RPC_DATA, node)
        self.assertTrue(data.Parse())
        self.assertEqual(data.members[0].json_name, "count")
        self.assertEqual(data.members[0].db_name, "iCount")
        self.assertIsNone(data.members[1].db_name)
        bare = ET.fromstring('<rpcdata name="Example"><member name="iCount" type="int32_t" seq="7" json="count"/></rpcdata>')
        self.assertFalse(SerializeData(SerializeDataType.RPC_DATA, bare).Parse())
        node[0].set("name", "count")
        self.assertFalse(SerializeData(SerializeDataType.RPC_DATA, node).Parse())
        keyed = ET.fromstring('<rpcdata name="Example" db="true"><member name="iId" type="int64_t" seq="0" dbkey="true"/><member name="sName" type="string" seq="1" dbtype="varbinary(16)"/></rpcdata>')
        data = SerializeData(SerializeDataType.RPC_DATA, keyed)
        self.assertTrue(data.Parse())
        self.assertTrue(data.members[0].db_key)
        self.assertEqual(data.members[1].db_type, "VARBINARY(16)")
        keyed[1].set("dbtype", "VARBINARY")
        self.assertFalse(SerializeData(SerializeDataType.RPC_DATA, keyed).Parse())
        keyed[1].set("dbtype", "TEXT")
        keyed[0].set("dbkey", "maybe")
        self.assertFalse(SerializeData(SerializeDataType.RPC_DATA, keyed).Parse())


class ModuleGeneratorTest(unittest.TestCase):
    def test_generated_main_catches_component_initialization(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            module = Module()
            module.name = "Example"
            component = Component()
            component.name = "Worker"
            component.type = "Server"
            module.components[component.name] = component
            (root / "example/src").mkdir(parents=True)
            generator = ModuleFileGen(module, str(root), str(root))
            self.assertTrue(generator.GenModuleFile())
            main = (root / "example/src/main.cpp").read_text(encoding="utf-8")
            self.assertRegex(main, r'StartFramework\("\.\./config/example\.toml",\s*\w+,\s*\w+\)')
            startup = main.index("return ::OI::Base::RunFramework([]() -> int32_t")
            logger = main.index("::OI::Log::Init();", startup)
            component = main.index("Worker::GetInstance().Init();", logger)
            self.assertIn("return iCode;", main[logger:component])
            self.assertIn("return iCode;", main[component:main.index("return OI_OK;", component)])
            self.assertNotIn("::OI::Base::StopFramework();", main)

    def test_dispatch_command_line(self):
        root = Path(__file__).resolve().parents[1]
        main = root / "cbaselib/proto/proto_tool/main.py"
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            proto = temp / "example.proto"
            proto.write_text("rpcdata Example { 7 int32_t iCount; }\n", encoding="utf-8")
            module = temp / "module.proto"
            module.write_text("module Empty { }\n", encoding="utf-8")
            for mode in ("macro", "switch"):
                for form in ("data-old", "data-new", "module-old", "module-new"):
                    output = temp / (mode + "-" + form)
                    flag = "--pb-dispatch=" + mode
                    if form == "data-old":
                        args = ["--data-only", flag, str(proto), str(output)]
                    elif form == "data-new":
                        args = [flag, "--data-only", str(output), "--", str(proto)]
                    else:
                        tail = [str(module), str(output), str(temp / "modules"),
                                str(temp / "deploy"), str(temp / "manifest.cmake")]
                        args = [str(proto), flag] + tail if form == "module-old" else tail + [flag, "--", str(proto)]
                    result = subprocess.run([sys.executable, str(main)] + args,
                                            cwd=root, capture_output=True, text=True, encoding="utf-8", errors="replace")
                    self.assertEqual(result.returncode, 0, (mode, form, result.stdout, result.stderr))
                    generated = (output / "oi_rpcdata_example.h").read_text(encoding="utf-8")
                    self.assertEqual("switch (field_seq)" in generated, mode == "switch")
                    self.assertEqual("OI_PB_FIELD(Example, 7, iCount)" in generated, mode == "macro")
            invalid = subprocess.run([sys.executable, str(main), "--pb-dispatch=invalid",
                                      "--data-only", str(proto), str(temp / "invalid")],
                                     cwd=root, capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(invalid.returncode, 1)
            self.assertIn("invalid --pb-dispatch:", invalid.stdout)
            after_delimiter = temp / "--pb-dispatch=switch"
            after_delimiter.write_text(proto.read_text(encoding="utf-8"), encoding="utf-8")
            result = subprocess.run([sys.executable, str(main), "--data-only", str(temp / "boundary"),
                                     "--", after_delimiter.name],
                                    cwd=temp, capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            generated = (temp / "boundary/oi_rpcdata_example.h").read_text(encoding="utf-8")
            self.assertIn("OI_PB_FIELD(Example, 7, iCount)", generated)

    def run_generator(self, modules, split_proto=False):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            module_file = temp / "module.proto"
            manifest = temp / "modules.cmake"
            module_file.write_text(modules, encoding="utf-8")
            proto_dir = root / "modules/protoconfig"
            proto_files = [str(proto_dir / name) for name in ("db.proto", "json.proto", "rpc.proto")]
            command = [sys.executable, str(root / "cbaselib/proto/proto_tool/main.py")]
            tail = [str(module_file), str(temp / "include"),
                    str(temp / "modules"), str(temp / "deploy"), str(manifest)]
            if split_proto:
                command.extend(tail + ["--"] + proto_files)
            else:
                command.extend([proto_files[-1]] + tail)
            result = subprocess.run(command, cwd=root, capture_output=True, text=True, encoding="utf-8", errors="replace")
            output = manifest.read_text(encoding="utf-8") if manifest.exists() else None
            if result.returncode == 0:
                repeated = subprocess.run(command, cwd=root, capture_output=True, text=True, encoding="utf-8", errors="replace")
                self.assertEqual(repeated.returncode, 0, repeated.stdout + repeated.stderr)
                self.assertEqual(manifest.read_text(encoding="utf-8"), output)
            return result, output

    def test_data_only_generator(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "generated"
            command = [sys.executable, str(root / "cbaselib/proto/proto_tool/main.py"),
                       "--data-only", str(root / "cbaselib/proto/protocol/framework.proto"), str(output)]
            result = subprocess.run(command, cwd=root, capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            generated = output / "oi_rpcdata_workcompleted.h"
            enums = output / "rpc_enums.h"
            self.assertTrue(generated.exists())
            self.assertTrue(enums.exists())
            self.assertIn("iOperation", generated.read_text(encoding="utf-8"))
            self.assertIn("WORK_PROTOCOL_COMPLETED", enums.read_text(encoding="utf-8"))
            before = {path.name: path.read_bytes() for path in output.iterdir() if path.is_file()}
            repeated = subprocess.run(command, cwd=root, capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(repeated.returncode, 0, repeated.stdout + repeated.stderr)
            self.assertEqual(before, {path.name: path.read_bytes() for path in output.iterdir() if path.is_file()})
            bad = subprocess.run([sys.executable, str(root / "cbaselib/proto/proto_tool/main.py"),
                                  "--data-only", str(Path(temp) / "missing.xml"), str(output)],
                                 cwd=root, capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertNotEqual(bad.returncode, 0)
            left = Path(temp) / "left.proto"
            right = Path(temp) / "right.proto"
            left.write_text("rpcdata Child { 0 int32_t iNumber; }\n", encoding="utf-8")
            right.write_text("rpcdata Row { 0 Child stChild; }\nrpcdata Req { 0 int32_t iNumber; }\nrpcdata Res { 0 int32_t iNumber; }\nrpc Echo { id 9; request Req; response Res; }\n", encoding="utf-8")
            multi = subprocess.run([sys.executable, str(root / "cbaselib/proto/proto_tool/main.py"),
                                    "--data-only", str(output), "--", str(left), str(right)],
                                   cwd=root, capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(multi.returncode, 0, multi.stdout + multi.stderr)
            self.assertIn('#include "oi_rpcdata_child.h"', (output / "oi_rpcdata_row.h").read_text(encoding="utf-8"))
            self.assertIn("::OI::RpcData::Req", (output / "rpc_ids.h").read_text(encoding="utf-8"))

    def test_generator_uses_actual_type_dependencies(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            proto = temp / "types.proto"
            proto.write_text("rpcdata Foo { 0 BarExtra stValue; }\n"
                             "rpcdata Bar { 0 Foo stValue; }\n"
                             "rpcdata BarExtra { 0 int32_t iValue; }\n"
                             "rpcdata Container { 0 vector<BarExtra> vItems; }\n", encoding="utf-8")
            output = temp / "out"
            result = subprocess.run([sys.executable, str(root / "cbaselib/proto/proto_tool/main.py"),
                                     "--data-only", str(proto), str(output)], cwd=root,
                                    capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            foo = (output / "oi_rpcdata_foo.h").read_text(encoding="utf-8")
            bar = (output / "oi_rpcdata_bar.h").read_text(encoding="utf-8")
            container = (output / "oi_rpcdata_container.h").read_text(encoding="utf-8")
            self.assertIn('#include "oi_rpcdata_barextra.h"', foo)
            self.assertNotIn('#include "oi_rpcdata_bar.h"', foo)
            self.assertIn('#include "oi_rpcdata_foo.h"', bar)
            self.assertIn('#include "oi_rpcdata_barextra.h"', container)
            self.assertNotIn('#include "oi_rpcdata_bar.h"', container)

    def test_generator_rejects_direct_value_cycles_before_writing(self):
        generator = Path(__file__).resolve().parents[1] / "cbaselib/proto/proto_tool/main.py"
        for schema, cycle in (("rpcdata Row { 0 Row stParent; }", "Row -> Row"),
                              ("rpcdata Row { 0 Child stChild; }\nrpcdata Child { 0 Row stParent; }",
                               "Row -> Child -> Row"),
                              ("rpcdata Row { 0 vector<Row> vChildren; }", None)):
            with self.subTest(schema=schema), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                spec, output = root / "types.proto", root / "out"
                spec.write_text(schema, encoding="utf-8")
                result = subprocess.run([sys.executable, str(generator), "--data-only", str(spec), str(output)],
                                        capture_output=True, text=True, encoding="utf-8")
                if cycle:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(cycle, result.stdout + result.stderr)
                    self.assertFalse(list(output.glob("*.h")))
                else:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertTrue((output / "oi_rpcdata_row.h").is_file())

    def test_generator_rejects_case_insensitive_output_collision(self):
        root = Path(__file__).resolve().parents[1]
        for split in (False, True):
            with self.subTest(split=split), tempfile.TemporaryDirectory() as directory:
                temp = Path(directory)
                left = temp / "left.proto"
                right = temp / "right.proto"
                first = "rpcdata UserID { 0 int32_t iFirst; }\n"
                second = "rpcdata UserId { 0 int32_t iSecond; }\n"
                left.write_text(first if split else first + second, encoding="utf-8")
                right.write_text(second, encoding="utf-8")
                command = [sys.executable, str(root / "cbaselib/proto/proto_tool/main.py"),
                           "--data-only", str(temp / "out"), "--", str(left)]
                if split:
                    command.append(str(right))
                result = subprocess.run(command, cwd=root, capture_output=True, text=True,
                                        encoding="utf-8", errors="replace")
                output = result.stdout + result.stderr
                self.assertNotEqual(result.returncode, 0, output)
                for expected in ("UserID", "UserId", "oi_rpcdata_userid.h"):
                    self.assertIn(expected, output)

    def test_plain_json_keeps_field_name(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "generated"
            proto_dir = root / "modules/protoconfig"
            command = [sys.executable, str(root / "cbaselib/proto/proto_tool/main.py"),
                       "--data-only", str(output), "--",
                       str(proto_dir / "db.proto"), str(proto_dir / "json.proto"), str(proto_dir / "rpc.proto")]
            result = subprocess.run(command, cwd=root, capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            header = (output / "oi_rpcdata_jsonall.h").read_text(encoding="utf-8")
            plain = (output / "oi_rpcdata_jsonplain.h").read_text(encoding="utf-8")
            bare = (output / "oi_rpcdata_jsondefault.h").read_text(encoding="utf-8")
            for key in ("iPlain", "sPlain", "vPlain", "mPlain", "stPlain", "iDefault", "sDefault", "vDefault", "mDefault", "stDefault", "gap", "c"):
                self.assertIn('out["%s"]' % key, header)
            self.assertNotIn('out["plain"]', header)
            self.assertIn('out["iNumber"]', plain)
            self.assertIn('out["sText"]', plain)
            self.assertNotIn('out["n"]', plain)
            self.assertIn('out["iNumber"]', bare)
            self.assertIn('out["sText"]', bare)
            self.assertNotIn('out["n"]', bare)
            self.assertIn("iSkip", header)
            self.assertIn("iSkip", bare)
            self.assertNotIn('out["iSkip"]', header)
            self.assertNotIn('in["iSkip"]', header)
            self.assertNotIn('out["iSkip"]', bare)
            self.assertNotIn('in["iSkip"]', bare)

    def test_invalid_module_names(self):
        for module in ("module {\n}\n", 'module "../bad" {\n}\n',
                       "module bad name {\n}\n", "module é {\n}\n",
                       "module -bad {\n}\n",
                       "module Echo {\n}\nmodule eCHO {\n}\n"):
            with self.subTest(module=module):
                result, _ = self.run_generator(module)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_manifest_uses_generated_directory_names(self):
        result, manifest = self.run_generator("module LogD {\n}\nmodule Test-2_ABC {\n}\n", split_proto=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIsNotNone(manifest)
        self.assertEqual(manifest.split(), ["set(OI_MODULES", "logd", "test-2_abc", ")"])


if __name__ == "__main__":
    unittest.main()
