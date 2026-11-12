"""Protocol field naming and stable JSON/wire-number regressions."""
import sys
import subprocess
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
        self.assertIn("ToDbBlob(vNumbers)", generated)
        self.assertIn("ToDbBlob(mFlags)", generated)
        self.assertIn("ToDbBlob(stChild)", generated)
        self.assertIn("FromDbBlob(*stCell, stDecoded.vNumbers)", generated)
        self.assertIn("FromDbInteger(*stCell, stDecoded.iSecret)", generated)
        self.assertNotIn("stDecoded.sCache", generated)

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
        self.assertFalse(self.load_proto("rpcdata Child { 0 int32_t iNumber; } rpcdata Row json { 0 Child stChild; }"))
        self.assertFalse(self.load_proto("rpcdata Child { 0 int32_t iNumber; } rpcdata Row json db { 0 Child stChild; }"))
        self.assertTrue(self.load_proto("rpcdata Child json { 0 int32_t iNumber; } rpcdata Row json db { 0 Child stChild; }"))
        self.assertTrue(self.load_proto("rpcdata Child json { 0 int32_t iNumber; } rpcdata Row json { 0 Child stChild; 1 [nojson] vector<int32_t> vSkip; }"))
        self.assertTrue(self.load_proto("rpcdata Key key { 0 int32_t iNumber; } rpcdata Row json { 0 int32_t iId; 1 [nojson] map<Key, int32_t> mKeyed; }"))
        self.assertFalse(self.load_proto("rpcdata Key key { 0 int32_t iNumber; } rpcdata Row json { 0 map<Key, int32_t> mKeyed; }"))
        json_db = ParseProtoText("rpcdata Child json { 0 int32_t iNumber; } rpcdata Row db { 0 [dbjson] vector<int32_t> vNumbers; 1 [dbjson] Child stChild; 2 vector<int32_t> vRaw; }")[1]
        generated = SerializeDataFileGen(json_db).GenDataFile()
        self.assertIn("ToJsonText(vNumbers)", generated)
        self.assertIn("ToJsonText(stChild)", generated)
        self.assertIn("FromJsonText", generated)
        self.assertIn("ToDbBlob(vRaw)", generated)
        self.assertNotIn("ToDbBlob(vNumbers)", generated)
        self.assertTrue(self.load_proto("rpcdata Child json { 0 int32_t iNumber; } rpcdata Row db { 0 [dbjson] Child stChild; }"))
        self.assertFalse(self.load_proto("rpcdata Child { 0 int32_t iNumber; } rpcdata Row db { 0 [dbjson] Child stChild; }"))
        self.assertFalse(self.load_proto("rpcdata Row db { 0 [dbjson] int32_t iId; }"))
        self.assertFalse(self.load_proto("rpcdata Row db { 0 [dbjson] vector<bool> vFlags; }"))
        self.assertFalse(self.load_proto("rpcdata Row json { 0 [dbjson] vector<int32_t> vNumbers; }"))
        self.assertTrue(self.load_proto("rpcdata Row db { 0 [db id] [dbjson] vector<int32_t> vNumbers; 1 [nodb] string sCache; }"))
        self.assertIsNone(ParseProtoText("rpcdata Bad json db { 0 [json] [nojson] int32_t iOne; }"))

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


class ModuleGeneratorTest(unittest.TestCase):
    def run_generator(self, modules, split_proto=False):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            module_file = temp / "module.proto"
            component_file = temp / "component.proto"
            manifest = temp / "modules.cmake"
            module_file.write_text(modules, encoding="utf-8")
            component_file.write_text("", encoding="utf-8")
            proto_dir = root / "modules/protoconfig"
            proto_files = [str(proto_dir / name) for name in ("db.proto", "json.proto", "rpc.proto")]
            command = [sys.executable, str(root / "cbaselib/proto/proto_tool/main.py")]
            tail = [str(module_file), str(component_file), str(temp / "include"),
                    str(temp / "modules"), str(temp / "deploy"), str(manifest)]
            if split_proto:
                command.extend(tail + ["--"] + proto_files)
            else:
                command.extend([proto_files[-1]] + tail)
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
                       "--data-only", str(root / "cbaselib/net/protocol/work.proto"), str(output)]
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
            left = Path(temp) / "left.proto"
            right = Path(temp) / "right.proto"
            left.write_text("rpcdata Child { 0 int32_t iNumber; }\n", encoding="utf-8")
            right.write_text("rpcdata Row { 0 Child stChild; }\nrpcdata Req { 0 int32_t iNumber; }\nrpcdata Res { 0 int32_t iNumber; }\nrpc Echo { id 9; request Req; response Res; }\n", encoding="utf-8")
            multi = subprocess.run([sys.executable, str(root / "cbaselib/proto/proto_tool/main.py"),
                                    "--data-only", str(output), "--", str(left), str(right)],
                                   cwd=root, capture_output=True, text=True, errors="replace")
            self.assertEqual(multi.returncode, 0, multi.stdout + multi.stderr)
            self.assertIn('#include "oi_rpcdata_child.h"', (output / "oi_rpcdata_row.h").read_text(encoding="utf-8"))
            self.assertIn("::OI::RpcData::Req", (output / "rpc_ids.h").read_text(encoding="utf-8"))

    def test_plain_json_keeps_field_name(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "generated"
            proto_dir = root / "modules/protoconfig"
            command = [sys.executable, str(root / "cbaselib/proto/proto_tool/main.py"),
                       "--data-only", str(output), "--",
                       str(proto_dir / "db.proto"), str(proto_dir / "json.proto"), str(proto_dir / "rpc.proto")]
            result = subprocess.run(command, cwd=root, capture_output=True, text=True, errors="replace")
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
