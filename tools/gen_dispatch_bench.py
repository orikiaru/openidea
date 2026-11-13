# 生成三套字段分发的对照 cpp。参数 switch、map、array 三选一。
# 字段读写都走框架的 ReadPb；map 和 array 的查表也在框架里。没选中的那套不会出现在这个 cpp 里。
import argparse
from pathlib import Path


def WriteMembers(stOut, iFields):
    for iField in range(iFields):
        stOut.write("    int32_t f%d{};\n" % iField)
    stOut.write("    std::int64_t Check() const\n")
    stOut.write("    {\n")
    stOut.write("        return static_cast<std::int64_t>(f0) + static_cast<std::int64_t>(f%d);\n" % (iFields - 1))
    stOut.write("    }\n")


def WriteSerialize(stOut, iFields):
    stOut.write("    OI_NOINLINE ::OI::Base::BytesStream& serialize(::OI::Base::BytesStream& os) const\n")
    stOut.write("    {\n")
    for iField in range(iFields):
        stOut.write("        os.WritePb(%d, f%d);\n" % (iField, iField))
    stOut.write("        return os;\n")
    stOut.write("    }\n")


def WriteSwitch(stOut, iIndex, iFields):
    stOut.write("    OI_NOINLINE ::OI::Base::BytesStream& deserialize(::OI::Base::BytesStream& os)\n")
    stOut.write("    {\n")
    stOut.write("        std::array<uint8_t, %d> seen{};\n" % iFields)
    stOut.write("        while (os.RemainSize() > 0)\n")
    stOut.write("        {\n")
    stOut.write("            uint32_t field_seq = 0;\n")
    stOut.write("            uint32_t field_size = 0;\n")
    stOut.write("            os.ReadPbHeader(field_seq, field_size);\n")
    stOut.write("            switch (field_seq)\n")
    stOut.write("            {\n")
    for iField in range(iFields):
        stOut.write("            case %d:\n" % iField)
        stOut.write("                if (seen[%d] != 0)\n" % iField)
        stOut.write("                {\n")
        stOut.write("                    throw ::OI::Base::ParserException();\n")
        stOut.write("                }\n")
        stOut.write("                seen[%d] = 1;\n" % iField)
        stOut.write("                os.ReadPb(field_size, f%d);\n" % iField)
        stOut.write("                break;\n")
    stOut.write("            default:\n")
    stOut.write("                os.SkipPb(field_size);\n")
    stOut.write("                break;\n")
    stOut.write("            }\n")
    stOut.write("        }\n")
    stOut.write("        return os;\n")
    stOut.write("    }\n")


def WriteMap(stOut, iIndex, iFields):
    stOut.write("    OI_NOINLINE ::OI::Base::BytesStream& deserialize(::OI::Base::BytesStream& os)\n")
    stOut.write("    {\n")
    stOut.write("        static const std::map<uint32_t, ::OI::Base::PbReadFn> fields = {\n")
    for iField in range(iFields):
        stOut.write("            OI_PB_MAP_FIELD(P%d, %d, f%d),\n" % (iIndex, iField, iField))
    stOut.write("        };\n")
    stOut.write("        std::array<uint8_t, %d> seen{};\n" % iFields)
    stOut.write("        return os.ReadFieldsByMap(this, fields, seen);\n")
    stOut.write("    }\n")


def WriteArray(stOut, iIndex, iFields):
    stOut.write("    OI_NOINLINE ::OI::Base::BytesStream& deserialize(::OI::Base::BytesStream& os)\n")
    stOut.write("    {\n")
    stOut.write("        static constexpr std::array<::OI::Base::PbField, %d> fields = {{\n" % iFields)
    for iField in range(iFields):
        stOut.write("            OI_PB_FIELD(P%d, %d, f%d),\n" % (iIndex, iField, iField))
    stOut.write("        }};\n")
    stOut.write("        std::array<uint8_t, %d> seen{};\n" % iFields)
    stOut.write("        return os.ReadFields(this, fields, seen);\n")
    stOut.write("    }\n")


def WriteBench(stOut, sScheme, iProtocols, iFields, iHot):
    stOut.write("template <typename T>\n")
    stOut.write("std::int64_t DecodeOne(::OI::Base::BytesStream& stInput)\n")
    stOut.write("{\n")
    stOut.write("    T stValue{};\n")
    stOut.write("    stValue.deserialize(stInput);\n")
    stOut.write("    return stValue.Check();\n")
    stOut.write("}\n\n")
    stOut.write("int main()\n")
    stOut.write("{\n")
    stOut.write("    constexpr int kFields = %d;\n" % iFields)
    stOut.write("    constexpr int kProtos = %d;\n" % iProtocols)
    stOut.write("    constexpr int kHot = %d;\n" % iHot)
    stOut.write("    try\n")
    stOut.write("    {\n")
    stOut.write("        ::OI::Base::BytesStream stPacket;\n")
    stOut.write("        for (int iField = 0; iField < kFields; ++iField)\n")
    stOut.write("        {\n")
    stOut.write("            stPacket.WritePb(static_cast<uint32_t>(iField), static_cast<int32_t>(iField + 1));\n")
    stOut.write("        }\n")
    stOut.write("        stPacket.StartTransaction();\n")
    stOut.write("        const std::int64_t iOne = 1 + kFields;\n")
    stOut.write("        using DecodeFn = std::int64_t (*)(::OI::Base::BytesStream&);\n")
    stOut.write("        static const DecodeFn vFns[%d] = {\n" % iProtocols)
    for iIndex in range(iProtocols):
        stOut.write("            &DecodeOne<P%d>,\n" % iIndex)
    stOut.write("        };\n")
    stOut.write("        for (int iIndex = 0; iIndex < kProtos; ++iIndex)\n")
    stOut.write("        {\n")
    stOut.write("            stPacket.RollBack();\n")
    stOut.write("            (void)vFns[iIndex](stPacket);\n")
    stOut.write("        }\n")
    stOut.write("        P0 stValue{};\n")
    stOut.write("        stPacket.RollBack();\n")
    stOut.write("        stValue.deserialize(stPacket);\n")
    stOut.write("        ::OI::Base::BytesStream stOut;\n")
    stOut.write("        const auto t0 = std::chrono::steady_clock::now();\n")
    stOut.write("        for (int iIndex = 0; iIndex < kHot; ++iIndex)\n")
    stOut.write("        {\n")
    stOut.write("            stOut.Clear();\n")
    stOut.write("            stValue.serialize(stOut);\n")
    stOut.write("        }\n")
    stOut.write("        const auto t1 = std::chrono::steady_clock::now();\n")
    stOut.write("        std::int64_t iSum = 0;\n")
    stOut.write("        const auto t2 = std::chrono::steady_clock::now();\n")
    stOut.write("        for (int iIndex = 0; iIndex < kHot; ++iIndex)\n")
    stOut.write("        {\n")
    stOut.write("            stPacket.RollBack();\n")
    stOut.write("            stValue.deserialize(stPacket);\n")
    stOut.write("            iSum += stValue.Check();\n")
    stOut.write("        }\n")
    stOut.write("        const auto t3 = std::chrono::steady_clock::now();\n")
    stOut.write("        if (stOut.Size() != stPacket.Size() || stPacket.RemainSize() != 0 || iSum != iOne * kHot)\n")
    stOut.write("        {\n")
    stOut.write('            std::printf("bad scheme=%s out=%zu packet=%zu remain=%zu sum=%lld\\n", "' + sScheme + '", stOut.Size(), stPacket.Size(), stPacket.RemainSize(), static_cast<long long>(iSum));\n')
    stOut.write("            return 1;\n")
    stOut.write("        }\n")
    stOut.write("        const auto iSerNs = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();\n")
    stOut.write("        const auto iDeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();\n")
    stOut.write('        std::printf("scheme=%s protocols=%d fields=%d iters=%d ser_ns=%lld de_ns=%lld\\n", "' + sScheme + '", kProtos, kFields, kHot, static_cast<long long>(iSerNs / kHot), static_cast<long long>(iDeNs / kHot));\n')
    stOut.write("        return 0;\n")
    stOut.write("    }\n")
    stOut.write("    catch (const ::OI::Base::ParserException& stError)\n")
    stOut.write("    {\n")
    stOut.write('        std::printf("parse %s\\n", stError.what());\n')
    stOut.write("        return 2;\n")
    stOut.write("    }\n")
    stOut.write("}\n")


def Main():
    stParser = argparse.ArgumentParser()
    stParser.add_argument("scheme", choices=("switch", "map", "array"))
    stParser.add_argument("--protocols", type=int, default=1000)
    stParser.add_argument("--fields", type=int, default=128)
    stParser.add_argument("--hot", type=int, default=10000000)
    stParser.add_argument("--out", default="")
    stArgs = stParser.parse_args()
    if stArgs.protocols < 1 or stArgs.fields < 2:
        raise SystemExit("protocols >= 1 and fields >= 2")
    stRoot = Path(__file__).resolve().parents[1]
    stOutPath = Path(stArgs.out) if stArgs.out else stRoot / ".local" / "pb_dispatch_bench" / ("%s.cpp" % stArgs.scheme)
    stOutPath.parent.mkdir(parents=True, exist_ok=True)
    fnWrite = {"switch": WriteSwitch, "map": WriteMap, "array": WriteArray}[stArgs.scheme]
    with stOutPath.open("w", encoding="utf-8", newline="\n") as stOut:
        stOut.write("#include \"oi_bytes_stream.h\"\n")
        stOut.write("#include <array>\n")
        stOut.write("#include <chrono>\n")
        stOut.write("#include <cstdint>\n")
        stOut.write("#include <cstdio>\n")
        stOut.write("#include <map>\n\n")
        stOut.write("#if defined(_MSC_VER)\n")
        stOut.write("#define OI_NOINLINE __declspec(noinline)\n")
        stOut.write("#else\n")
        stOut.write("#define OI_NOINLINE __attribute__((noinline))\n")
        stOut.write("#endif\n\n")
        for iIndex in range(stArgs.protocols):
            stOut.write("struct P%d\n" % iIndex)
            stOut.write("{\n")
            WriteMembers(stOut, stArgs.fields)
            WriteSerialize(stOut, stArgs.fields)
            fnWrite(stOut, iIndex, stArgs.fields)
            stOut.write("};\n\n")
        WriteBench(stOut, stArgs.scheme, stArgs.protocols, stArgs.fields, stArgs.hot)
    print(stOutPath)


if __name__ == "__main__":
    Main()
