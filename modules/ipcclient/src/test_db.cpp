#include "test_db.h"
#include "oi_blocking_pool.h"
#include "oi_config.h"
#include "oi_db.h"
#include "oi_db_table.h"
#include "oi_errno.h"
#include "oi_rpcdata_dbsample.h"
#include "oi_mysql.h"
#include "oi_thread.h"
#include "oi_log.h"
#include "test_board.h"
#include <iterator>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <random>

void TestDb::Load()
{
	TryGetConfig("Test.db", m_bEnabled);
	TryGetConfig("Test.wake_case", m_sWakeCase);
}

void TestDb::Expect(int32_t& iBad, bool bOk, const char* pLabel)
{
	if (!bOk)
	{
		LOG_TRACE("mysql check failed label=%s", pLabel);
		++iBad;
	}
}

// 每个数据库字段都放了边界值：内嵌 NUL 和 0xff、整数两端、空串 key。sCache 是 [nodb]。
::OI::RpcData::DbSample TestDb::MakeFullRow()
{
	::OI::RpcData::DbSample stRow;
	stRow.iId = 1;
	stRow.sName = std::string("a\0b\xff", 4);
	stRow.iSecret = INT32_MIN;
	stRow.sCache = "skip";
	stRow.iToken = (std::numeric_limits<uint64_t>::max)();
	stRow.dScore = 1.5;
	stRow.fRatio = 0.75f;
	stRow.stPayload = ::OI::Base::Bytes("\0\xffx", 3);
	stRow.vNumbers = {(std::numeric_limits<int32_t>::min)(), 0, (std::numeric_limits<int32_t>::max)()};
	stRow.mFlags = {{"", 0}, {"on", 1}};
	stRow.stChild.iNumber = 4;
	stRow.stChild.sText = std::string("c\0d", 3);
	stRow.iFlag = (std::numeric_limits<int8_t>::min)();
	stRow.bActive = true;
	stRow.vJsonNumbers = {8, -9};
	stRow.mJsonFlags = {{"k", 3}};
	stRow.stJsonChild.iNumber = 6;
	stRow.stJsonChild.sText = "json";
	stRow.iMask = UINT8_MAX;
	stRow.iOffset = INT16_MIN;
	stRow.iPort = UINT16_MAX;
	stRow.iCount = UINT32_MAX;
	stRow.iBalance = INT64_MAX;
	stRow.sCode = "AB";
	stRow.sLabel = "text";
	stRow.sFixed = std::string("a\0b", 3);
	stRow.sDescription = "details";
	stRow.sArticle = "article";
	stRow.stData = ::OI::Base::Bytes("a\0", 2);
	stRow.stArchive = ::OI::Base::Bytes("\xff", 1);
	stRow.mGroups = {{"g", {3, -3}}};
	stRow.vJsonGroups = {{{"k", 5}}};
	return stRow;
}

// 另一头的边界：空串、空 blob、空容器、默认嵌套结构。
::OI::RpcData::DbSample TestDb::MakeEdgeRow()
{
	::OI::RpcData::DbSample stRow;
	stRow.iId = (std::numeric_limits<int64_t>::min)();
	stRow.iSecret = (std::numeric_limits<int32_t>::max)();
	stRow.iFlag = (std::numeric_limits<int8_t>::max)();
	stRow.iOffset = INT16_MAX;
	stRow.iBalance = INT64_MIN;
	stRow.sCode = "Z";
	stRow.sFixed = std::string(3, '\0');
	return stRow;
}

// 比较全部数据库字段，不比 [nodb] 的 sCache。
// 不同时逐个打出字段名，便于定位是哪一列没还原。
bool TestDb::SameRow(const ::OI::RpcData::DbSample& stFirst, const ::OI::RpcData::DbSample& stSecond)
{
	const std::pair<const char*, bool> vFields[] = {
	    {"iId", stFirst.iId == stSecond.iId},
	    {"sName", stFirst.sName == stSecond.sName},
	    {"iSecret", stFirst.iSecret == stSecond.iSecret},
	    {"iToken", stFirst.iToken == stSecond.iToken},
	    {"dScore", stFirst.dScore == stSecond.dScore},
	    {"fRatio", stFirst.fRatio == stSecond.fRatio},
	    {"stPayload", stFirst.stPayload.Size() == stSecond.stPayload.Size() && std::memcmp(stFirst.stPayload.Data(), stSecond.stPayload.Data(), stFirst.stPayload.Size()) == 0},
	    {"vNumbers", stFirst.vNumbers == stSecond.vNumbers},
	    {"mFlags", stFirst.mFlags == stSecond.mFlags},
	    {"stChild", stFirst.stChild.iNumber == stSecond.stChild.iNumber && stFirst.stChild.sText == stSecond.stChild.sText},
	    {"iFlag", stFirst.iFlag == stSecond.iFlag},
	    {"bActive", stFirst.bActive == stSecond.bActive},
	    {"vJsonNumbers", stFirst.vJsonNumbers == stSecond.vJsonNumbers},
	    {"mJsonFlags", stFirst.mJsonFlags == stSecond.mJsonFlags},
	    {"stJsonChild", stFirst.stJsonChild.iNumber == stSecond.stJsonChild.iNumber && stFirst.stJsonChild.sText == stSecond.stJsonChild.sText},
	    {"iMask", stFirst.iMask == stSecond.iMask},
	    {"iOffset", stFirst.iOffset == stSecond.iOffset},
	    {"iPort", stFirst.iPort == stSecond.iPort},
	    {"iCount", stFirst.iCount == stSecond.iCount},
	    {"iBalance", stFirst.iBalance == stSecond.iBalance},
	    {"sCode", stFirst.sCode == stSecond.sCode},
	    {"sLabel", stFirst.sLabel == stSecond.sLabel},
	    {"sFixed", stFirst.sFixed == stSecond.sFixed},
	    {"sDescription", stFirst.sDescription == stSecond.sDescription},
	    {"sArticle", stFirst.sArticle == stSecond.sArticle},
	    {"stData", stFirst.stData.Size() == stSecond.stData.Size() && std::memcmp(stFirst.stData.Data(), stSecond.stData.Data(), stFirst.stData.Size()) == 0},
	    {"stArchive", stFirst.stArchive.Size() == stSecond.stArchive.Size() && std::memcmp(stFirst.stArchive.Data(), stSecond.stArchive.Data(), stFirst.stArchive.Size()) == 0},
	    {"mGroups", stFirst.mGroups == stSecond.mGroups},
	    {"vJsonGroups", stFirst.vJsonGroups == stSecond.vJsonGroups},
	};
	static_assert(std::size(vFields) == ::OI::RpcData::DbSample::DbFieldCount);
	bool bSame = true;
	for (const auto& [pName, bEqual] : vFields)
	{
		if (!bEqual)
		{
			LOG_TRACE("mysql row field differs field=%s", pName);
			bSame = false;
		}
	}
	return bSame;
}

void TestDb::Start()
{
	if (!m_bEnabled)
	{
		return;
	}
	TestBoard::GetInstance().Arm();
	OI::Coro::StartTask([]()
	{
		return TestDb::GetInstance().Run();
	});
}

int32_t TestDb::CheckCellPredicates()
{
	using OI::Db::Cell;
	int32_t iBad = 0;
	const auto fnCheck = [this, pBad = &iBad](const char* pLabel, auto fnIs, auto fnAs, const std::string& sValid,
	                         const auto& stExpected, std::optional<std::string> stInvalid)
	{
		const OI::Db::Row stRow{{{"valid", sValid}, {"bad", std::move(stInvalid)}, {"null", std::nullopt}}};
		const auto stValid = stRow.at("valid");
		const auto stBad = stRow.at("bad");
		const auto stNull = stRow.at("null");
		Expect(*pBad, !stRow.find("missing") && stNull.is_null() && !(stNull.*fnIs)(), pLabel);
		Expect(*pBad, (stValid.*fnIs)() && (stValid.*fnAs)() == stExpected, pLabel);
		Expect(*pBad, !(stBad.*fnIs)(), pLabel);
		bool bThrew = false;
		try
		{
			(void)(stBad.*fnAs)();
		}
		catch (const OI::Db::RowDecodeError&)
		{
			bThrew = true;
		}
		Expect(*pBad, bThrew, pLabel);
	};
	fnCheck("string", &Cell::is_string, &Cell::as_string, std::string("a\0b", 3), std::string("a\0b", 3), std::nullopt);
	fnCheck("bool", &Cell::is_bool, &Cell::as_bool, "1", true, "2");
	fnCheck("int8", &Cell::is_int8, &Cell::as_int8, "-128", INT8_MIN, "128");
	fnCheck("uint8", &Cell::is_uint8, &Cell::as_uint8, "255", UINT8_MAX, "256");
	fnCheck("int16", &Cell::is_int16, &Cell::as_int16, "-32768", INT16_MIN, "32768");
	fnCheck("uint16", &Cell::is_uint16, &Cell::as_uint16, "65535", UINT16_MAX, "65536");
	fnCheck("int32", &Cell::is_int32, &Cell::as_int32, "-2147483648", INT32_MIN, "2147483648");
	fnCheck("uint32", &Cell::is_uint32, &Cell::as_uint32, "4294967295", UINT32_MAX, "4294967296");
	fnCheck("int64", &Cell::is_int64, &Cell::as_int64, "-9223372036854775808", INT64_MIN, "9223372036854775808");
	fnCheck("uint64", &Cell::is_uint64, &Cell::as_uint64, "18446744073709551615", UINT64_MAX, "-1");
	fnCheck("float", &Cell::is_float, &Cell::as_float, "1.25", 1.25f, "1e100");
	fnCheck("double", &Cell::is_double, &Cell::as_double, "-0.125", -0.125, "1.2x");
	LOG_TRACE("mysql cell predicates bad=%d", iBad);
	return iBad;
}

int32_t TestDb::CheckRowMapping()
{
	using ::OI::RpcData::DbSample;
	int32_t iBad = CheckCellPredicates();
	try
	{
		const DbSample stFull = MakeFullRow();
		const auto mParameters = stFull.ToDbMap();
		Expect(iBad, mParameters.at("id").is_signed() && mParameters.at("iToken").is_unsigned() && mParameters.at("dScore").is_real() && mParameters.at("sName").is_text() && mParameters.at("stPayload").is_binary() && mParameters.at("bActive").is_bool() && OI::Db::Parameter(nullptr).is_null(), "native parameter kinds");
		Expect(iBad, !mParameters.contains("sCache") && !mParameters.contains("iId"), "database field aliases and exclusions");

		// 只给部分列，只改这几个字段。
		DbSample stPartial = stFull;
		stPartial.sCache = "keep";
		Expect(iBad, stPartial.FromDbMap({{"sName", "z"}, {"notes", "{\"n\":5}"}}) && stPartial.sName == "z" && stPartial.mJsonFlags == std::map<std::string, int32_t>{{"n", 5}} && stPartial.iId == stFull.iId && stPartial.vNumbers == stFull.vNumbers && stPartial.sCache == "keep",
		    "partial cell map");

		// 任一列解不开都整行拒绝，对象保持原值。
		const std::vector<std::pair<std::string, ::OI::Db::CellMap>> vRejected = {
		    {"reject null", {{"sName", "y"}, {"id", std::nullopt}}},
		    {"reject int8 range", {{"iFlag", "128"}}},
		    {"reject uint64 sign", {{"iToken", "-1"}}},
		    {"reject integer text", {{"iSecret", "1x"}}},
		    {"reject bool", {{"bActive", "2"}}},
		    {"reject real", {{"dScore", "nan"}}},
		    {"reject pb blob", {{"vNumbers", std::string("\xff", 1)}}},
		    {"reject child blob", {{"stChild", std::string("\x01\0\0\0", 4)}}},
		    {"reject json", {{"notes", "{bad"}}},
		    {"reject json shape", {{"vJsonNumbers", "{}"}}},
		};
		for (const auto& [sLabel, mCells] : vRejected)
		{
			DbSample stKept = stFull;
			Expect(iBad, !stKept.FromDbMap(mCells) && SameRow(stKept, stFull), sLabel.c_str());
		}

		// 成员指针查列名，按传入顺序挑列；[nodb] 和重复成员拒绝，输出不变。
		const char* pId = ::OI::Db::ColumnOf(&DbSample::iId);
		const char* pNotes = ::OI::Db::ColumnOf(&DbSample::mJsonFlags);
		Expect(iBad, pId && std::string(pId) == "id" && pNotes && std::string(pNotes) == "notes", "column of member");
		Expect(iBad, ::OI::Db::ColumnOf(&DbSample::sCache) == nullptr, "nodb member has no column");
		::OI::Db::Picked stPicked;
		Expect(iBad, ::OI::Db::Pick(stFull, stPicked, &DbSample::sName, &DbSample::mJsonFlags, &DbSample::vNumbers, &DbSample::bActive) == OI_OK && stPicked.vColumns == std::vector<std::string>{"sName", "notes", "vNumbers", "bActive"} && stPicked.sColumns == "`sName`,`notes`,`vNumbers`,`bActive`" && stPicked.sMarks == "?,?,?,?" && stPicked.sAssignments == "`sName`=?,`notes`=?,`vNumbers`=?,`bActive`=?" && stPicked.sConditions == "`sName`=? AND `notes`=? AND `vNumbers`=? AND `bActive`=?" && stPicked.vParameters.size() == 4 && stPicked.vParameters[0] == mParameters.at("sName") && stPicked.vParameters[1] == mParameters.at("notes") && stPicked.vParameters[2] == mParameters.at("vNumbers") && stPicked.vParameters[3] == mParameters.at("bActive"),
		    "pick members");
		::OI::Db::Picked stKept = stPicked;
		Expect(iBad, ::OI::Db::Pick(stFull, stKept, &DbSample::iId, &DbSample::sCache) == OI_ERR_INVALID_ARGUMENT && stKept.sColumns == stPicked.sColumns && stKept.vParameters == stPicked.vParameters,
		    "pick nodb rejected");
		Expect(iBad, ::OI::Db::Pick(stFull, stKept, &DbSample::iId, &DbSample::iId) == OI_ERR_INVALID_ARGUMENT && stKept.sColumns == stPicked.sColumns,
		    "pick duplicate rejected");

		// PB 整结构往返，包括 [nodb] 和 [nojson] 字段；不认识的字段号按长度跳过。
		::OI::Base::BytesStream stStream;
		stFull.serialize(stStream);
		stStream.WritePb(99, int32_t{5});
		DbSample stPb;
		stPb.deserialize(stStream);
		Expect(iBad, SameRow(stPb, stFull) && stPb.sCache == stFull.sCache && stStream.RemainSize() == 0, "pb roundtrip skips unknown field");

		// 截断的包整条拒绝。
		::OI::Base::BytesStream stCut;
		stFull.serialize(stCut);
		::OI::Base::BytesStream stShort(stCut.Data(), stCut.Size() - 1);
		bool bCutRejected = false;
		try
		{
			DbSample stBroken;
			stBroken.deserialize(stShort);
		}
		catch (const ::OI::Base::ParserException&)
		{
			bCutRejected = true;
		}
		Expect(iBad, bCutRejected, "pb truncated rejected");

		// 线上字节固定小端：字段号、长度、定宽整数和浮点。
		::OI::RpcData::DbChild stChild;
		stChild.iNumber = 0x01020304;
		stChild.sText = "ab";
		::OI::Base::BytesStream stChildBytes;
		stChild.serialize(stChildBytes);
		const unsigned char vChildExpect[] = {0, 0, 0, 0, 4, 0, 0, 0, 4, 3, 2, 1, 1, 0, 0, 0, 2, 0, 0, 0, 'a', 'b'};
		Expect(iBad, stChildBytes.Size() == sizeof(vChildExpect) && std::memcmp(stChildBytes.Data(), vChildExpect, sizeof(vChildExpect)) == 0,
		    "pb little endian struct");
		::OI::Base::BytesStream stFixed;
		stFixed.WritePb(1, int64_t{0x0102030405060708LL});
		stFixed.WritePb(2, 1.0);
		stFixed.WritePb(3, -2.0f);
		const unsigned char vFixedExpect[] = {
		    1, 0, 0, 0, 8, 0, 0, 0, 8, 7, 6, 5, 4, 3, 2, 1,
		    2, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xf0, 0x3f,
		    3, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0xc0};
		Expect(iBad, stFixed.Size() == sizeof(vFixedExpect) && std::memcmp(stFixed.Data(), vFixedExpect, sizeof(vFixedExpect)) == 0,
		    "pb little endian fixed");

		// JSON 跳过 [nojson]，按 [json] 列名以外的字段名输出；读入不改 [nojson] 字段。
		DbSample stJsonSource = stFull;
		stJsonSource.sName = "name";
		stJsonSource.stChild.sText = "child";
		const std::string sJson = stJsonSource.ToJson();
		DbSample stJson;
		Expect(iBad, sJson.find("\"iSecret\"") == std::string::npos && sJson.find("\"notes\"") == std::string::npos && sJson.find("\"mJsonFlags\"") != std::string::npos && sJson.find("\"sCache\"") != std::string::npos && stJson.FromJson(sJson) && stJson.iSecret == 0 && stJson.sCache == stJsonSource.sCache && stJson.vNumbers == stJsonSource.vNumbers && stJson.mJsonFlags == stJsonSource.mJsonFlags && stJson.stJsonChild.iNumber == stJsonSource.stJsonChild.iNumber,
		    "json skips nojson");
		stJson.iSecret = 3;
		Expect(iBad, stJson.FromJson(std::string("{\"iSecret\":8}")) && stJson.iSecret == 3, "json keeps nojson field");
	}
	catch (const std::exception& stException)
	{
		LOG_TRACE("mysql check failed label=row mapping exception what=%s", stException.what());
		++iBad;
	}
	LOG_TRACE("mysql row mapping bad=%d", iBad);
	return iBad;
}

// 用相反记录的值初始化输出。任何漏解的字段都会与预期不同。
OI::Coro::TaskResult<int32_t> TestDb::CheckBatchRead(OI::Db::Client& stMysql, const std::string& sTable, const std::vector<OI::RpcData::DbSample>& vExpected)
{
	OI::Db::Error stError;
	std::vector<OI::RpcData::DbSample> vRead{vExpected[0], vExpected[1]};
	std::swap(vRead[0].iId, vRead[1].iId);
	const int32_t iCode = co_await stMysql.Select(sTable, vRead, stError);
	if (iCode != OI_OK || !SameRow(vRead[0], vExpected[1]) || !SameRow(vRead[1], vExpected[0]))
	{
		LOG_TRACE("mysql batch read failed status=%d stage=%s", iCode, stError.sStage.c_str());
		co_return 1;
	}
	co_return 0;
}

OI::Coro::TaskResult<int32_t> TestDb::CheckBatchCrud(OI::Db::Client& stMysql, const std::string& sTable)
{
	using OI::RpcData::DbSample;
	int32_t iBad = 0;
	OI::Db::Error stError;
	std::vector<DbSample> vRows{MakeFullRow(), MakeEdgeRow()};
	vRows[0].iId = 81;
	vRows[1].iId = 1; // 已存在，第二次 INSERT 必须回滚第一次。
	int32_t iCode = co_await stMysql.Insert(sTable, vRows, stError);
	Expect(iBad, iCode == OI_ERR_EXIST && stError.iNativeCode == 1062 && stError.sSqlState == "23000", "batch duplicate key");
	iCode = co_await stMysql.Select(sTable, vRows[0], stError);
	Expect(iBad, iCode == OI_ERR_NOT_FOUND, "batch insert rollback");

	vRows[1].iId = 82;
	iCode = co_await stMysql.Insert(sTable, vRows, stError);
	Expect(iBad, iCode == OI_OK, "batch insert all types");
	iBad += co_await CheckBatchRead(stMysql, sTable, vRows);

	std::swap(vRows[0].iId, vRows[1].iId);
	iCode = co_await stMysql.Update(sTable, vRows, stError);
	Expect(iBad, iCode == OI_OK, "batch update all types");
	iBad += co_await CheckBatchRead(stMysql, sTable, vRows);

	iCode = co_await stMysql.Delete(sTable, vRows[0], stError);
	Expect(iBad, iCode == OI_OK, "delete before batch upsert");
	iCode = co_await stMysql.Select(sTable, vRows[0], stError);
	Expect(iBad, iCode == OI_ERR_NOT_FOUND, "deleted key before upsert");
	std::swap(vRows[0].iId, vRows[1].iId);
	iCode = co_await stMysql.Upsert(sTable, vRows, stError);
	Expect(iBad, iCode == OI_OK, "batch upsert all types for existing and missing keys");
	iBad += co_await CheckBatchRead(stMysql, sTable, vRows);

	iCode = co_await stMysql.Delete(sTable, vRows, stError);
	Expect(iBad, iCode == OI_OK, "batch delete all types");
	for (auto& stRow : vRows)
	{
		iCode = co_await stMysql.Select(sTable, stRow, stError);
		Expect(iBad, iCode == OI_ERR_NOT_FOUND, "each batch key deleted");
	}
	vRows[0].iId = 1;
	vRows[0].sName = "untouched";
	iCode = co_await stMysql.Select(sTable, vRows, stError);
	Expect(iBad, iCode == OI_ERR_NOT_FOUND && vRows[0].sName == "untouched", "batch missing key preserves output");
	LOG_TRACE("mysql all field types batch bad=%d", iBad);
	co_return iBad;
}

OI::Coro::TaskResult<int32_t> TestDb::CheckDefaultTable(OI::Db::Client& stMysql, const std::string& sExplicitTable)
{
	using OI::RpcData::DbSample;
	int32_t iBad = 0;
	OI::Db::Error stError;
	OI::Db::Result stResult;
	int32_t iCode = co_await stMysql.CreateTable<DbSample>(stError);
	if (iCode != OI_OK)
	{
		LOG_TRACE("mysql default table create failed status=%d stage=%s", iCode, stError.sStage.c_str());
		co_return 1;
	}
	std::string sIndexColumns;
	iCode = co_await stMysql.Scalar("SELECT GROUP_CONCAT(COLUMN_NAME ORDER BY SEQ_IN_INDEX) FROM information_schema.STATISTICS "
	                                "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='dbsample' AND INDEX_NAME='idx_code_id'",
	    {}, sIndexColumns, stError);
	Expect(iBad, iCode == OI_OK && sIndexColumns == "sCode,id", "runtime table index uses ordered physical column names");

	DbSample stRow = MakeFullRow();
	stRow.sName = "default";
	iCode = co_await stMysql.Insert(stRow, stResult, stError);
	Expect(iBad, iCode == OI_OK && stResult.iAffected == 1, "default insert");
	iCode = co_await stMysql.Insert(stRow, stError);
	Expect(iBad, iCode == OI_ERR_EXIST && stError.iNativeCode == 1062, "default duplicate insert");

	std::string sStored;
	iCode = co_await stMysql.Scalar("SELECT sName FROM `dbsample` WHERE id=1", {}, sStored, stError);
	Expect(iBad, iCode == OI_OK && sStored == "default", "lowercase physical table");
	DbSample stOther = stRow;
	iCode = co_await stMysql.Select(sExplicitTable, stOther, stError);
	Expect(iBad, iCode == OI_OK && SameRow(stOther, MakeFullRow()), "same key in explicit table");

	stRow.sName = "updated";
	stRow.iCount = 42;
	iCode = co_await stMysql.Update(stRow, stResult, stError, &DbSample::sName);
	Expect(iBad, iCode == OI_OK && stResult.iAffected == 1, "default update result");
	iCode = co_await stMysql.Select(stRow, stError, &DbSample::iCount);
	Expect(iBad, iCode == OI_OK && stRow.iCount == UINT32_MAX && stRow.sName == "updated", "default projection");
	stRow.iCount = 42;
	iCode = co_await stMysql.Update(stRow, stError);
	Expect(iBad, iCode == OI_OK, "default full update");
	stRow.iCount = 43;
	iCode = co_await stMysql.Upsert(stRow, stResult, stError);
	Expect(iBad, iCode == OI_OK && stResult.iAffected == 2, "default upsert result");
	iCode = co_await stMysql.Delete(stRow, stResult, stError);
	Expect(iBad, iCode == OI_OK && stResult.iAffected == 1, "default delete result");

	std::vector<DbSample> vRows{stRow, MakeEdgeRow()};
	iCode = co_await stMysql.Insert(vRows, stError);
	Expect(iBad, iCode == OI_OK, "default batch insert");
	vRows[0].sName = "first";
	vRows[1].sName = "second";
	iCode = co_await stMysql.Update(vRows, stError, &DbSample::sName);
	Expect(iBad, iCode == OI_OK, "default batch update");
	std::swap(vRows[0].iId, vRows[1].iId);
	iCode = co_await stMysql.Select(vRows, stError, &DbSample::sName);
	Expect(iBad, iCode == OI_OK && vRows[0].sName == "second" && vRows[1].sName == "first", "default batch projection");
	iCode = co_await stMysql.Delete(vRows, stError);
	Expect(iBad, iCode == OI_OK, "default batch delete");
	std::vector<DbSample> vRead;
	iCode = co_await stMysql.SelectAll(vRead, stError);
	Expect(iBad, iCode == OI_OK && vRead.empty(), "default table empty");
	iCode = co_await stMysql.Upsert(vRows, stError);
	Expect(iBad, iCode == OI_OK, "default batch upsert");
	iCode = co_await stMysql.Delete(stRow, stError);
	Expect(iBad, iCode == OI_OK, "default delete");
	iCode = co_await stMysql.Upsert(stRow, stError);
	Expect(iBad, iCode == OI_OK, "default upsert");
	iCode = co_await stMysql.SelectAll(vRead, stError);
	Expect(iBad, iCode == OI_OK && vRead.size() == 2, "default select all");
	stRow.iId = 0;
	iCode = co_await stMysql.Insert(stRow, stResult, stError);
	Expect(iBad, iCode == OI_OK && stResult.iAffected == 1 && stResult.iInsertId > 1, "auto increment insert returns id");
	stRow.iId = static_cast<int64_t>(stResult.iInsertId);
	stRow.sName.clear();
	iCode = co_await stMysql.Select(stRow, stError);
	Expect(iBad, iCode == OI_OK && stRow.sName == "updated", "read generated primary key");
	const int64_t iGeneratedId = stRow.iId;
	stRow.iId = 0;
	iCode = co_await stMysql.Upsert(stRow, stResult, stError);
	Expect(iBad, iCode == OI_OK && stResult.iInsertId > static_cast<uint64_t>(iGeneratedId), "upsert allocates a new key");
	stRow.iId = INT64_MAX;
	iCode = co_await stMysql.Insert(stRow, stResult, stError);
	Expect(iBad, iCode == OI_OK && stResult.iInsertId == static_cast<uint64_t>(INT64_MAX), "preserve explicit 64-bit id");
	LOG_TRACE("mysql default_table bad=%d", iBad);
	co_return iBad;
}

OI::Coro::TaskResult<int32_t> TestDb::CheckColumnTypes(OI::Db::Client& stMysql, const std::string& sTable)
{
	OI::Db::Result stResult;
	OI::Db::Error stError;
	std::vector<OI::Db::Parameter> vParameters{sTable};
	const int32_t iCode = co_await stMysql.Call("SELECT COLUMN_NAME AS col, DATA_TYPE AS kind FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME=?", std::move(vParameters), stResult, stError);
	if (iCode != OI_OK)
	{
		LOG_TRACE("mysql column metadata failed status=%d stage=%s", iCode, stError.sStage.c_str());
		co_return 1;
	}
	const std::map<std::string, std::string> mExpected{
	    {"id", "bigint"}, {"sName", "varbinary"}, {"iSecret", "int"}, {"iToken", "bigint"}, {"dScore", "double"}, {"fRatio", "double"},
	    {"stPayload", "longblob"}, {"vNumbers", "longblob"}, {"mFlags", "longblob"}, {"stChild", "longblob"}, {"iFlag", "tinyint"},
	    {"bActive", "tinyint"}, {"vJsonNumbers", "longtext"}, {"notes", "text"}, {"stJsonChild", "longtext"},
	    {"iMask", "tinyint"}, {"iOffset", "smallint"}, {"iPort", "smallint"}, {"iCount", "int"}, {"iBalance", "bigint"},
	    {"sCode", "char"}, {"sLabel", "varchar"}, {"sFixed", "binary"}, {"sDescription", "mediumtext"}, {"sArticle", "longtext"},
	    {"stData", "blob"}, {"stArchive", "mediumblob"}, {"mGroups", "longblob"}, {"vJsonGroups", "longtext"}};
	std::map<std::string, std::string> mActual;
	for (const auto& stRow : stResult.vRows)
	{
		mActual.emplace(stRow.at("col").as_string(), stRow.at("kind").as_string());
	}
	int32_t iBad = 0;
	Expect(iBad, mActual == mExpected, "actual column names and SQL types");

	const std::string sSql = "SELECT * FROM " + OI::Db::QuoteName(sTable) + " WHERE id=1";
	const int32_t iRead = co_await stMysql.Call(sSql, {}, stResult, stError);
	if (iRead != OI_OK || stResult.vRows.size() != 1)
	{
		co_return iBad + 1;
	}
	const auto& stRow = stResult.vRows.front();
	Expect(iBad, stRow.at("iFlag").as_int8() == INT8_MIN && stRow.at("iMask").as_uint8() == UINT8_MAX, "named 8-bit integers");
	Expect(iBad, stRow.at("iOffset").as_int16() == INT16_MIN && stRow.at("iPort").as_uint16() == UINT16_MAX, "named 16-bit integers");
	const auto stCount = stRow.find("iCount");
	Expect(iBad, stCount && stCount->is_uint32() && stCount->as_uint32() == UINT32_MAX && stRow.at("iSecret").as_int32() == INT32_MIN, "named 32-bit integers");
	Expect(iBad, stRow.at("iBalance").as_int64() == INT64_MAX && stRow.at("iToken").as_uint64() == UINT64_MAX, "named 64-bit integers");
	Expect(iBad, stRow.at("bActive").as_bool() && stRow.at("fRatio").as_float() == 0.75f && stRow.at("dScore").as_double() == 1.5, "named bool and reals");
	Expect(iBad, stRow.at("sName").as_string() == std::string("a\0b\xff", 4) && stRow.at("stPayload").as_string() == std::string("\0\xffx", 3), "named text and bytes");
	co_return iBad;
}

OI::Coro::TaskResult<int32_t> TestDb::ReadRequiredId(OI::Db::Client& stMysql, std::string sSql, uint32_t& iId){
    OI_DB_TRY{
        OI::Db::Error stError;
OI::Db::Result stResult;
const int32_t iCode = co_await stMysql.Call(std::move(sSql), {}, stResult, stError);
stError.ThrowIfFailed(iCode);
if (stResult.vRows.empty())
{
	co_return OI_ERR_NOT_FOUND;
}
iId = stResult.vRows.front().at("required_id").as_uint32();
co_return OI_OK;
}
OI_DB_CATCH
}

#ifdef OI_ENABLE_MYSQL
OI::Coro::TaskResult<int32_t> TestDb::CheckDbErrors(OI::Db::Client& stMysql, const OI::Mysql::Config& stConfig)
{
	int32_t iBad = 0;
	uint32_t iId = 0;
	int32_t iCode = co_await ReadRequiredId(stMysql, "SELECT 77 AS required_id", iId);
	Expect(iBad, iCode == OI_OK && iId == 77, "default database handler success");
	iCode = co_await ReadRequiredId(stMysql, "SELECT 1 AS other", iId);
	Expect(iBad, iCode == OI_ERR_DECODE && iId == 77, "default database handler missing column");
	iCode = co_await ReadRequiredId(stMysql, "SELECT missing_db_macro_column", iId);
	Expect(iBad, iCode == OI_ERR_DB && iId == 77, "default database handler SQL error");

	OI::Mysql::Config stInvalid = stConfig;
	stInvalid.sUser = "oi_missing_regression_user";
	stInvalid.sPassword = "oi_test_credential";
	OI::Db::Client stDenied(OI::Mysql::MakeDriver(stInvalid), 1);
	iCode = co_await ReadRequiredId(stDenied, "SELECT 1 AS required_id", iId);
	Expect(iBad, iCode == OI_ERR_CONNECT && iId == 77, "default database handler login rejected");
	iCode = co_await stDenied.Close();
	Expect(iBad, iCode == OI_OK, "denied client closes");

	const char* pRefusedPort = std::getenv("OI_TEST_DB_REFUSED_PORT");
	if (pRefusedPort)
	{
		OI::Mysql::Config stRefusedConfig = stConfig;
		stRefusedConfig.sHost = "127.0.0.1";
		stRefusedConfig.iPort = static_cast<uint32_t>(std::stoul(pRefusedPort));
		OI::Db::Client stRefused(OI::Mysql::MakeDriver(stRefusedConfig), 1);
		iCode = co_await ReadRequiredId(stRefused, "SELECT 1 AS required_id", iId);
		Expect(iBad, iCode == OI_ERR_CONNECT && iId == 77, "default database handler TCP refused");
		iCode = co_await stRefused.Close();
		Expect(iBad, iCode == OI_OK, "refused client closes");
	}
	const auto fnUnexpected = []() -> OI::Coro::TaskResult<int32_t>
	{
		OI_DB_TRY
		{
			throw std::runtime_error("oi_private_exception_probe");
		}
		OI_DB_CATCH
	};
	iCode = co_await fnUnexpected();
	Expect(iBad, iCode == OI_ERR_FAIL, "default database handler unexpected exception");
	LOG_TRACE("mysql error handling bad=%d", iBad);
	co_return iBad;
}
#endif

OI::Coro::TaskResult<int32_t> TestDb::CheckMysqlRows(OI::Db::Client& stMysql, const std::string& sBaseTable, const std::string& sDatabase)
{
	using ::OI::RpcData::DbSample;
	int32_t iBad = 0;
	const std::string sTable = sBaseTable + "_row";
	OI::Db::Result stResult;
	OI::Db::Error stError;
	int32_t iStatus = OI_OK;
	bool bCreated = false;
	try
	{
		iStatus = co_await stMysql.CreateTable<DbSample>(sTable, stError);
		if (iStatus != OI_OK)
		{
			LOG_TRACE("mysql row table create failed status=%d stage=%s native=%u", iStatus, stError.sStage.c_str(), stError.iNativeCode);
			co_return iBad + 1;
		}
		bCreated = true;
		const std::string sQuoted = ::OI::Db::QuoteName(sTable);
		iStatus = co_await stMysql.Call("ALTER TABLE " + sQuoted + " MODIFY `sName` VARBINARY(64) NOT NULL FIRST", {}, stResult, stError);
		Expect(iBad, iStatus == OI_OK, "mysql physical column order differs from schema");

		DbSample stInvalid = MakeFullRow();
		iStatus = co_await stMysql.Update(sTable, stInvalid, stError, &DbSample::iId);
		Expect(iBad, iStatus == OI_ERR_INVALID_ARGUMENT, "reject key update");
		iStatus = co_await stMysql.Update(sTable, stInvalid, stError, &DbSample::sCache);
		Expect(iBad, iStatus == OI_ERR_INVALID_ARGUMENT, "reject non-database member");
		iStatus = co_await stMysql.Select(sTable, stInvalid, stError, &DbSample::sName, &DbSample::sName);
		Expect(iBad, iStatus == OI_ERR_INVALID_ARGUMENT, "reject duplicate selected member");
		const std::vector<std::string> vInvalidTables{"", "bad`name", std::string(65, 'x')};
		for (const std::string& sInvalid : vInvalidTables)
		{
			iStatus = co_await stMysql.Insert(sInvalid, stInvalid, stError);
			Expect(iBad, iStatus == OI_ERR_INVALID_ARGUMENT && stError.sStage == "build_statement", "reject invalid table name");
		}
		const std::vector<DbSample> vRows{MakeFullRow(), MakeEdgeRow()};
		for (const DbSample& stRow : vRows)
		{
			iStatus = co_await stMysql.Insert(sTable, stRow, stResult, stError);
			if (iStatus != OI_OK || stResult.iAffected != 1)
			{
				LOG_TRACE("mysql row insert failed id=%lld status=%d stage=%s native=%u", static_cast<long long>(stRow.iId), iStatus, stError.sStage.c_str(), stError.iNativeCode);
				++iBad;
			}
		}

		const char* pIsolated = std::getenv("OI_TEST_MYSQL_ISOLATED");
		if (pIsolated && std::strcmp(pIsolated, "1") == 0)
		{
			iBad += co_await CheckDefaultTable(stMysql, sTable);
		}
		iBad += co_await CheckColumnTypes(stMysql, sTable);

		// 主键重复的 INSERT 由库拒绝，原生错误必须是 ER_DUP_ENTRY（1062），连接错误等别的失败不算。
		iStatus = co_await stMysql.Insert(sTable, vRows[0], stError);
		if (iStatus == OI_OK || stError.iNativeCode != 1062)
		{
			LOG_TRACE("mysql duplicate key status=%d stage=%s native=%u", iStatus, stError.sStage.c_str(), stError.iNativeCode);
		}
		Expect(iBad, iStatus == OI_ERR_EXIST && stError.iNativeCode == 1062 && stError.sSqlState == "23000", "mysql duplicate key rejected");

		// 同一客户端先后执行两条查询。稍后才解码联表结果，错误仍须指向列的原表。
		const std::string sBaseQuoted = OI::Db::QuoteName(sBaseTable);
		OI::Db::Result stJoined;
		const std::string sJoin = "SELECT a.sName AS business_id, b.v AS metric FROM " + sQuoted + " AS a CROSS JOIN " + sBaseQuoted + " AS b WHERE a.id=1 LIMIT 1";
		const int32_t iJoin = co_await stMysql.Call(sJoin, {}, stJoined, stError);
		const int32_t iLater = co_await stMysql.Call("SELECT v FROM " + sBaseQuoted + " LIMIT 1", {}, stResult, stError);
		const auto fnBusinessId = [](const OI::Db::Row& stRow)
		{
			return stRow.at("business_id").as_uint32();
		};
		const auto fnMissingId = [](const OI::Db::Row& stRow)
		{
			return stRow.at("missing_id").as_uint32();
		};
		std::vector<uint32_t> vIds{77};
		const std::string sDatabaseLabel = "database=" + sDatabase + " table=";
		Expect(iBad, iJoin == OI_OK && iLater == OI_OK && stJoined.DecodeRows(vIds, stError, fnBusinessId) == OI_ERR_DECODE && stError.sMessage == sDatabaseLabel + sTable + " column=business_id invalid uint32" && vIds == std::vector<uint32_t>{77}, "deferred decode uses original table");
		Expect(iBad, stResult.DecodeRows(vIds, stError, fnMissingId) == OI_ERR_DECODE && stError.sMessage == sDatabaseLabel + sBaseTable + " column=missing_id missing", "missing column from single table");
		Expect(iBad, stJoined.DecodeRows(vIds, stError, fnMissingId) == OI_ERR_DECODE && stError.sMessage == sDatabaseLabel + sBaseTable + "," + sTable + " column=missing_id missing", "missing column from join");

		// 按主键读回，[nodb] 字段不被覆盖。
		for (const DbSample& stRow : vRows)
		{
			DbSample stLoaded = vRows[0];
			if (stRow.iId == vRows[0].iId)
			{
				stLoaded = vRows[1];
			}
			stLoaded.iId = stRow.iId;
			stLoaded.sCache = "keep";
			const int32_t iLoad = co_await stMysql.Select(sTable, stLoaded, stError);
			Expect(iBad, iLoad == OI_OK && SameRow(stLoaded, stRow) && stLoaded.sCache == "keep",
			    stRow.iId == 1 ? "mysql full row roundtrip" : "mysql edge row roundtrip");
		}

		// 按成员指针只改几列：没挑的字段即使对象里改了也不写，另一行不变。
		DbSample stChanged = vRows[0];
		stChanged.sName = std::string("\x01\0z", 3);
		stChanged.dScore = -0.25;
		stChanged.mJsonFlags = {{"x", -1}};
		stChanged.vNumbers.clear();
		DbSample stUnsaved = stChanged;
		stUnsaved.iSecret = 99;
		iStatus = co_await stMysql.Update(sTable, stUnsaved, stResult, stError, &DbSample::sName, &DbSample::dScore, &DbSample::mJsonFlags, &DbSample::vNumbers);
		Expect(iBad, iStatus == OI_OK && stResult.iAffected == 1, "mysql pick update");
		DbSample stChangedLoaded;
		stChangedLoaded.iId = stChanged.iId;
		const int32_t iChangedLoad = co_await stMysql.Select(sTable, stChangedLoaded, stError);
		DbSample stEdgeLoaded;
		stEdgeLoaded.iId = vRows[1].iId;
		const int32_t iEdgeLoad = co_await stMysql.Select(sTable, stEdgeLoaded, stError);
		Expect(iBad, iChangedLoad == OI_OK && iEdgeLoad == OI_OK && SameRow(stChangedLoaded, stChanged) && SameRow(stEdgeLoaded, vRows[1]),
		    "mysql pick update keeps other columns");

		// 非法 JSON 不得生成写语句；未选中该列时，其他列仍可正常写入。
		DbSample stInvalidJson = stChanged;
		stInvalidJson.mJsonFlags = {{std::string("\xFF", 1), 1}};
		stInvalidJson.sName = "valid-picked-update";
		const int32_t iInvalidAll = co_await stMysql.Update(sTable, stInvalidJson, stError);
		const int32_t iInvalidPicked = co_await stMysql.Update(sTable, stInvalidJson, stError, &DbSample::mJsonFlags);
		const int32_t iInvalidInsert = co_await stMysql.Insert(sTable, stInvalidJson, stError);
		const int32_t iInvalidUpsert = co_await stMysql.Upsert(sTable, stInvalidJson, stError);
		DbSample stAfterRejected;
		stAfterRejected.iId = stChanged.iId;
		const int32_t iRejectedLoad = co_await stMysql.Select(sTable, stAfterRejected, stError);
		Expect(iBad, iInvalidAll == OI_ERR_INVALID_ARGUMENT && iInvalidPicked == OI_ERR_INVALID_ARGUMENT && iInvalidInsert == OI_ERR_INVALID_ARGUMENT && iInvalidUpsert == OI_ERR_INVALID_ARGUMENT && iRejectedLoad == OI_OK && SameRow(stAfterRejected, stChanged), "mysql invalid json rejected keeps row");
		iStatus = co_await stMysql.Update(sTable, stInvalidJson, stError, &DbSample::sName);
		stChanged.sName = stInvalidJson.sName;
		DbSample stAfterPicked;
		stAfterPicked.iId = stChanged.iId;
		const int32_t iPickedLoad = co_await stMysql.Select(sTable, stAfterPicked, stError);
		Expect(iBad, iStatus == OI_OK && iPickedLoad == OI_OK && SameRow(stAfterPicked, stChanged), "mysql unpicked invalid json update roundtrip");

		// 按成员指针只读几列，写回只改这几个字段。
		DbSample stMixed = vRows[1];
		stMixed.iId = stChanged.iId;
		iStatus = co_await stMysql.Select(sTable, stMixed, stError, &DbSample::mJsonFlags, &DbSample::sName);
		Expect(iBad, iStatus == OI_OK && stMixed.sName == stChanged.sName && stMixed.mJsonFlags == stChanged.mJsonFlags && stMixed.iId == stChanged.iId && stMixed.vJsonNumbers == vRows[1].vJsonNumbers,
		    "mysql pick select");

		// 写入另一组字段值，然后逐字段读回。用相反的初值防止漏读默认值。
		DbSample stUpserted = MakeEdgeRow();
		stUpserted.iId = stChanged.iId;
		iStatus = co_await stMysql.Upsert(sTable, stUpserted, stError);
		DbSample stUpsertLoaded = MakeFullRow();
		stUpsertLoaded.iId = stUpserted.iId;
		const int32_t iUpsertLoad = co_await stMysql.Select(sTable, stUpsertLoaded, stError);
		Expect(iBad, iStatus == OI_OK && iUpsertLoad == OI_OK && SameRow(stUpsertLoaded, stUpserted), "mysql upsert existing row");

		DbSample stFresh = MakeEdgeRow();
		stFresh.iId = 7;
		stFresh.sName = "fresh";
		iStatus = co_await stMysql.Upsert(sTable, stFresh, stResult, stError);
		const uint64_t iFreshAffected = stResult.iAffected;
		DbSample stFreshLoaded;
		stFreshLoaded.iId = stFresh.iId;
		const int32_t iFreshLoad = co_await stMysql.Select(sTable, stFreshLoaded, stError);
		Expect(iBad, iStatus == OI_OK && iFreshAffected == 1 && iFreshLoad == OI_OK && SameRow(stFreshLoaded, stFresh),
		    "mysql upsert new row");

		// 不挑成员就改全部非主键列。
		DbSample stWhole = MakeFullRow();
		stWhole.iId = vRows[1].iId;
		stWhole.sName = "whole";
		iStatus = co_await stMysql.Update(sTable, stWhole, stResult, stError);
		const uint64_t iWholeAffected = stResult.iAffected;
		DbSample stWholeLoaded;
		stWholeLoaded.iId = stWhole.iId;
		const int32_t iWholeLoad = co_await stMysql.Select(sTable, stWholeLoaded, stError);
		Expect(iBad, iStatus == OI_OK && iWholeAffected == 1 && iWholeLoad == OI_OK && SameRow(stWholeLoaded, stWhole),
		    "mysql update all columns");

		// 按主键删，删完读不到。
		iStatus = co_await stMysql.Delete(sTable, stFresh, stResult, stError);
		const uint64_t iDeleteAffected = stResult.iAffected;
		DbSample stDeleted;
		stDeleted.iId = stFresh.iId;
		const int32_t iDeletedLoad = co_await stMysql.Select(sTable, stDeleted, stError);
		Expect(iBad, iStatus == OI_OK && iDeleteAffected == 1 && iDeletedLoad == OI_ERR_NOT_FOUND, "mysql delete by key");

		iBad += co_await CheckBatchCrud(stMysql, sTable);

		// 故障注入用手写 SQL：库里的 NULL 和坏 PB blob 读回时整行拒绝，对象保持原值。
		iStatus = co_await stMysql.Call("ALTER TABLE " + sQuoted + " MODIFY `sName` VARBINARY(64) NULL", {}, stResult, stError);
		Expect(iBad, iStatus == OI_OK, "mysql allow null");
		std::vector<OI::Db::Parameter> vKey{stUpserted.iId};
		iStatus = co_await stMysql.Call("UPDATE " + sQuoted + " SET `sName`=NULL WHERE `id`=?", std::move(vKey), stResult, stError);
		Expect(iBad, iStatus == OI_OK && stResult.iAffected == 1, "mysql set null");
		DbSample stKeptNull = stUpserted;
		const int32_t iNullLoad = co_await stMysql.Select(sTable, stKeptNull, stError);
		Expect(iBad, iNullLoad == OI_ERR_DECODE && SameRow(stKeptNull, stUpserted), "mysql null rejected");

		std::vector<OI::Db::Parameter> vBroken{"ok", OI::Db::Parameter::Binary(std::string(1, static_cast<char>(0xff))), stUpserted.iId};
		iStatus = co_await stMysql.Call("UPDATE " + sQuoted + " SET `sName`=?, `vNumbers`=? WHERE `id`=?", std::move(vBroken), stResult, stError);
		Expect(iBad, iStatus == OI_OK && stResult.iAffected == 1, "mysql set bad blob");
		DbSample stKeptBroken = stUpserted;
		const int32_t iBrokenLoad = co_await stMysql.Select(sTable, stKeptBroken, stError);
		Expect(iBad, iBrokenLoad == OI_ERR_DECODE && SameRow(stKeptBroken, stUpserted), "mysql bad blob rejected");
	}
	catch (const std::exception& stException)
	{
		LOG_TRACE("mysql check failed label=row table exception what=%s", stException.what());
		++iBad;
	}
	if (bCreated)
	{
		try
		{
			iStatus = co_await stMysql.Call("DROP TABLE " + ::OI::Db::QuoteName(sTable), {}, stResult, stError);
			if (iStatus != OI_OK)
			{
				LOG_TRACE("mysql row table drop failed status=%d stage=%s native=%u table=%s", iStatus, stError.sStage.c_str(), stError.iNativeCode, sTable.c_str());
				++iBad;
			}
		}
		catch (const std::exception&)
		{
			LOG_TRACE("mysql row table drop raised exception table=%s", sTable.c_str());
			++iBad;
		}
	}
	LOG_TRACE("mysql rows bad=%d", iBad);
	co_return iBad;
}

OI::Coro::Task TestDb::Run()
{
	const int32_t iConfig = OI::Thread::ConfigureBlockingPool(OI::Thread::BlockingType::Mysql, 4, 64);
	if (iConfig != OI_OK)
	{
		LOG_TRACE("mysql blocking pool configure failed code=%d", iConfig);
		TestBoard::GetInstance().Done(1);
		co_return;
	}
	if (!m_sWakeCase.empty())
	{
		const int32_t iBenchmark = co_await RunWakeBenchmark(m_sWakeCase == "blocking");
		TestBoard::GetInstance().Done(iBenchmark);
		co_return;
	}
	int32_t iBad = CheckRowMapping();
#ifdef OI_ENABLE_MYSQL
	OI::Db::Result stResult;
	OI::Db::Error stError;
	int32_t iStatus = OI_OK;
	const char* pHost = std::getenv("OI_TEST_MYSQL_HOST");
	const char* pPort = std::getenv("OI_TEST_MYSQL_PORT");
	const char* pUser = std::getenv("OI_TEST_MYSQL_USER");
	const char* pPassword = std::getenv("OI_TEST_MYSQL_PASSWORD");
	const char* pDatabase = std::getenv("OI_TEST_MYSQL_DATABASE");
	if (pHost && pPort && pUser && pPassword && pDatabase)
	{
		OI::Mysql::Config stConfig;
		stConfig.sHost = pHost;
		stConfig.iPort = static_cast<uint32_t>(std::strtoul(pPort, nullptr, 10));
		stConfig.sUser = pUser;
		stConfig.sPassword = pPassword;
		stConfig.sDatabase = pDatabase;
		std::random_device stRandom;
		const std::string sTable = "oi_db_reg_" + std::to_string(stRandom()) + "_" + std::to_string(stRandom());
		const std::string sQuotedTable = "`" + sTable + "`";
		bool bTableCreated = false;
		OI::Db::Client stMysql(OI::Mysql::MakeDriver(stConfig), 1);
		OI::Mysql::Config stLimitedConfig = stConfig;
		stLimitedConfig.iMaxResultBytes = 1;
		OI::Db::Client stLimited(OI::Mysql::MakeDriver(stLimitedConfig), 1);
		try
		{
			const std::string sRepeatedId = "SELECT CONNECTION_ID() AS id";
			OI::Db::Result stRepeatedResult;
			iStatus = co_await stMysql.Call(sRepeatedId, {}, stRepeatedResult, stError);
			if (iStatus != OI_OK || stRepeatedResult.vRows.size() != 1 || stRepeatedResult.vRows[0].mCells.size() != 1 ||
			    stRepeatedResult.vRows[0].at("id").as_uint64() == 0)
			{
				LOG_TRACE("mysql repeated connection id failed status=%d", iStatus);
				++iBad;
			}
			const auto vExpectedId = stRepeatedResult.vRows;
			const std::string sParameters = "SELECT ? AS binary_value, ? AS null_value, ? AS empty_text, ? AS empty_blob, ? AS min_signed, ? AS max_signed, ? AS min_unsigned, ? AS max_unsigned";
			const std::vector<OI::Db::Parameter> vParameters = {std::string("a\0b", 3), nullptr, std::string(), OI::Db::Blob(),
			    std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max(),
			    uint64_t{0}, std::numeric_limits<uint64_t>::max()};
			for (int32_t iRound = 0; iRound < 8; ++iRound)
			{
				iStatus = co_await stMysql.Call(sParameters, vParameters, stResult, stError);
				bool bValuesMatch = false;
				if (iStatus == OI_OK && stResult.vRows.size() == 1)
				{
					const auto& stRow = stResult.vRows.front();
					bValuesMatch = stRow.at("binary_value").as_string() == std::string("a\0b", 3) && stRow.at("null_value").is_null() &&
					               stRow.at("empty_text").as_string().empty() && stRow.at("empty_blob").as_string().empty() &&
					               stRow.at("min_signed").as_int64() == std::numeric_limits<int64_t>::min() && stRow.at("max_signed").as_int64() == std::numeric_limits<int64_t>::max() &&
					               stRow.at("min_unsigned").as_uint64() == 0 && stRow.at("max_unsigned").as_uint64() == std::numeric_limits<uint64_t>::max();
				}
				if (!bValuesMatch)
				{
					LOG_TRACE("mysql parameters round=%d failed status=%d stage=%s native=%u", iRound, iStatus, stError.sStage.c_str(), stError.iNativeCode);
					++iBad;
				}
				iStatus = co_await stMysql.Call(sRepeatedId, {}, stRepeatedResult, stError);
				if (iStatus != OI_OK || stRepeatedResult.vRows != vExpectedId)
				{
					LOG_TRACE("mysql repeated connection reuse round=%d failed status=%d", iRound, iStatus);
					++iBad;
				}
			}
			const auto fnReadId = [](const OI::Db::Row& stRow)
			{
				return stRow.at("id").as_uint32();
			};
			iStatus = co_await stMysql.Call("SELECT '4294967295' AS id", {}, stResult, stError);
			std::vector<uint32_t> vIds;
			Expect(iBad, iStatus == OI_OK && stResult.DecodeRows(vIds, stError, fnReadId) == OI_OK && vIds == std::vector<uint32_t>{UINT32_MAX}, "named uint32 max");
			const std::vector<std::pair<std::string, std::string>> vBadProjections = {
			    {"SELECT 1 AS other", "id missing"},
			    {"SELECT NULL AS id", "id is NULL"},
			    {"SELECT '1' AS id UNION ALL SELECT '4294967296'", "id invalid uint32"},
			    {"SELECT '-1' AS id", "id invalid uint32"},
			    {"SELECT '1x' AS id", "id invalid uint32"}};
			for (const auto& [sSql, sReason] : vBadProjections)
			{
				iStatus = co_await stMysql.Call(sSql, {}, stResult, stError);
				Expect(iBad, iStatus == OI_OK && stResult.DecodeRows(vIds, stError, fnReadId) == OI_ERR_DECODE && stError.sStage == "decode_row" && stError.sMessage == "database=" + stConfig.sDatabase + " table=<unknown> column=" + sReason && vIds == std::vector<uint32_t>{UINT32_MAX}, "named projection failure preserves output");
			}
			const std::string sConnectionId = "SELECT CONNECTION_ID() AS id";
			iStatus = co_await stMysql.Call(sConnectionId, {}, stResult, stError);
			OI::Db::Result stFirstId = stResult;
			if (iStatus != OI_OK || stFirstId.vRows.size() != 1 || stFirstId.vRows[0].mCells.size() != 1 ||
			    stFirstId.vRows[0].at("id").as_uint64() == 0)
			{
				LOG_TRACE("mysql first connection id failed status=%d", iStatus);
				++iBad;
			}
			iStatus = co_await stMysql.Call(sConnectionId, {}, stResult, stError);
			if (iStatus != OI_OK || stResult.vRows != stFirstId.vRows)
			{
				LOG_TRACE("mysql connection reuse failed status=%d", iStatus);
				++iBad;
			}
			iStatus = co_await stMysql.Call("SET @oi_regression_session = 42", {}, stResult, stError);
			if (iStatus != OI_OK)
			{
				LOG_TRACE("mysql session variable set failed status=%d stage=%s native=%u", iStatus, stError.sStage.c_str(), stError.iNativeCode);
				++iBad;
			}
			iStatus = co_await stMysql.Call("SELECT @oi_regression_session AS session_value", {}, stResult, stError);
			if (iStatus != OI_OK || stResult.vRows.size() != 1 || stResult.vRows[0].mCells.size() != 1 ||
			    !stResult.vRows[0].at("session_value").is_null())
			{
				LOG_TRACE("mysql session reset failed status=%d stage=%s native=%u", iStatus, stError.sStage.c_str(), stError.iNativeCode);
				++iBad;
			}
			iStatus = co_await stMysql.Call("SELECT DATABASE() AS database_name", {}, stResult, stError);
			if (iStatus != OI_OK || stResult.vRows.size() != 1 || stResult.vRows[0].mCells.size() != 1 ||
			    stResult.vRows[0].at("database_name").as_string() != stConfig.sDatabase)
			{
				LOG_TRACE("mysql schema reset failed status=%d stage=%s native=%u", iStatus, stError.sStage.c_str(), stError.iNativeCode);
				++iBad;
			}
			// 自增主键：回滚不退还已分配的自增值，回滚后再插入拿到的编号能证明事务里第一条 INSERT 真的执行过。
			iStatus = co_await stMysql.Call("CREATE TABLE " + sQuotedTable + " (id BIGINT AUTO_INCREMENT PRIMARY KEY, v INT NOT NULL) ENGINE=InnoDB", {}, stResult, stError);
			if (iStatus != OI_OK)
			{
				LOG_TRACE("mysql create test table failed status=%d stage=%s native=%u", iStatus, stError.sStage.c_str(), stError.iNativeCode);
				++iBad;
			}
			else
			{
				bTableCreated = true;
				OI::Db::Batch stBatch;
				stBatch.Add("INSERT INTO " + sQuotedTable + " (v) VALUES (1)");
				stBatch.Add("INSERT INTO " + sQuotedTable + " (missing_column) VALUES (2)");
				// 失败时不写输出，哨兵必须原样保留。
				std::vector<OI::Db::Result> vResults(1);
				vResults[0].iAffected = 321;
				iStatus = co_await stMysql.Transaction(std::move(stBatch), vResults, stError);
				// 失败必须来自第二条的未知列（ER_BAD_FIELD_ERROR 1054），BEGIN 或连接失败不算。
				if (iStatus == OI_OK || stError.iNativeCode != 1054 || vResults.size() != 1 || vResults[0].iAffected != 321)
				{
					LOG_TRACE("mysql transaction bad SQL status=%d stage=%s native=%u results=%zu", iStatus, stError.sStage.c_str(),
					    stError.iNativeCode, vResults.size());
					++iBad;
				}
				int64_t iCount = -1;
				iStatus = co_await stMysql.Scalar("SELECT COUNT(*) FROM " + sQuotedTable, {}, iCount, stError);
				if (iStatus != OI_OK || iCount != 0)
				{
					LOG_TRACE("mysql transaction rollback or recovery failed status=%d stage=%s native=%u", iStatus, stError.sStage.c_str(), stError.iNativeCode);
					++iBad;
				}
				// 第一条已执行并被回滚，占掉了自增值 1；第一条没执行的话这里会拿到 1。
				iStatus = co_await stMysql.Call("INSERT INTO " + sQuotedTable + " (v) VALUES (3)", {}, stResult, stError);
				if (iStatus != OI_OK || stResult.iAffected != 1 || stResult.iInsertId != 2)
				{
					LOG_TRACE("mysql transaction first insert evidence status=%d affected=%llu insert_id=%llu native=%u", iStatus,
					    static_cast<unsigned long long>(stResult.iAffected), static_cast<unsigned long long>(stResult.iInsertId), stError.iNativeCode);
					++iBad;
				}
				OI::Db::Batch stCommitBatch;
				stCommitBatch.Add("INSERT INTO " + sQuotedTable + " (v) VALUES (10)");
				stCommitBatch.Add("INSERT INTO " + sQuotedTable + " (v) VALUES (11)");
				std::vector<OI::Db::Result> vCommitResults(1);
				vCommitResults[0].iAffected = 321;
				iStatus = co_await stMysql.Transaction(std::move(stCommitBatch), vCommitResults, stError);
				if (iStatus != OI_OK || vCommitResults.size() != 2 || vCommitResults[0].iAffected != 1 ||
				    vCommitResults[1].iAffected != 1 || vCommitResults[0].iInsertId != 3 || vCommitResults[1].iInsertId != 4)
				{
					LOG_TRACE("mysql transaction commit results failed status=%d stage=%s native=%u results=%zu", iStatus,
					    stError.sStage.c_str(), stError.iNativeCode, vCommitResults.size());
					++iBad;
				}
				OI::Db::Client stObserver(OI::Mysql::MakeDriver(stConfig), 1);
				int64_t iCommitted = -1;
				iStatus = co_await stObserver.Scalar("SELECT COUNT(*) FROM " + sQuotedTable + " WHERE v IN (10, 11)", {}, iCommitted, stError);
				if (iStatus != OI_OK || iCommitted != 2)
				{
					LOG_TRACE("mysql transaction commit persistence failed status=%d stage=%s native=%u", iStatus,
					    stError.sStage.c_str(), stError.iNativeCode);
					++iBad;
				}
			}
			iStatus = co_await stMysql.Call("SELECT missing_column", {}, stResult, stError);
			if (iStatus == OI_OK || stError.iNativeCode != 1054)
			{
				LOG_TRACE("mysql bad SQL status=%d stage=%s native=%u", iStatus, stError.sStage.c_str(), stError.iNativeCode);
				++iBad;
			}
			int64_t iRecovered = -1;
			iStatus = co_await stMysql.Scalar("SELECT 1", {}, iRecovered, stError);
			if (iStatus != OI_OK || iRecovered != 1)
			{
				LOG_TRACE("mysql bad SQL recovery failed status=%d stage=%s native=%u", iStatus, stError.sStage.c_str(), stError.iNativeCode);
				++iBad;
			}
			int64_t iScalar = 77;
			static constexpr const char* vInvalidSql[] = {"SELECT 1 WHERE FALSE", "SELECT 1 UNION ALL SELECT 2", "SELECT 1 AS a, 2 AS b", "SELECT NULL", "SELECT 'bad'"};
			for (const char* pSql : vInvalidSql)
			{
				iStatus = co_await stMysql.Scalar(pSql, {}, iScalar, stError);
				Expect(iBad, iStatus == OI_ERR_DECODE && iScalar == 77, "bad scalar preserves output");
			}
			iStatus = co_await stMysql.Call("SELECT 1 AS dup, 2 AS dup WHERE FALSE", {}, stResult, stError);
			Expect(iBad, iStatus == OI_ERR_DECODE && stError.sStage == "duplicate_column", "duplicate names rejected on empty result");
			OI::Db::Result stSentinel;
			stSentinel.iAffected = 123;
			stSentinel.vRows.emplace_back();
			stSentinel.vRows.front().mCells["sentinel"] = std::nullopt;
			iStatus = co_await stLimited.Call("SELECT 'too large'", {}, stSentinel, stError);
			if (iStatus == OI_OK || stError.sStage != "result_limit" || stSentinel.iAffected != 123 ||
			    stSentinel.vRows.size() != 1 || !stSentinel.vRows.front().mCells.contains("sentinel"))
			{
				LOG_TRACE("mysql result limit failed status=%d stage=%s", iStatus, stError.sStage.c_str());
				++iBad;
			}
			iBad += co_await CheckMysqlRows(stMysql, sTable, stConfig.sDatabase);
			iBad += co_await CheckDbErrors(stMysql, stConfig);
		}
		catch (const std::exception&)
		{
			LOG_TRACE("mysql regression raised exception");
			++iBad;
		}
		try
		{
			iStatus = co_await stLimited.Close();
			if (iStatus != OI_OK)
			{
				LOG_TRACE("mysql limited client close failed status=%d", iStatus);
				++iBad;
			}
		}
		catch (const std::exception&)
		{
			LOG_TRACE("mysql limited client close raised exception");
			++iBad;
		}
		if (bTableCreated)
		{
			try
			{
				iStatus = co_await stMysql.Call("DROP TABLE " + sQuotedTable, {}, stResult, stError);
				if (iStatus != OI_OK)
				{
					LOG_TRACE("mysql drop test table failed status=%d stage=%s native=%u table=%s", iStatus, stError.sStage.c_str(), stError.iNativeCode, sTable.c_str());
					++iBad;
				}
			}
			catch (const std::exception&)
			{
				LOG_TRACE("mysql drop test table raised exception table=%s", sTable.c_str());
				++iBad;
			}
		}
		try
		{
			iStatus = co_await stMysql.Close();
			if (iStatus != OI_OK)
			{
				LOG_TRACE("mysql client close failed status=%d", iStatus);
				++iBad;
			}
		}
		catch (const std::exception&)
		{
			LOG_TRACE("mysql client close raised exception");
			++iBad;
		}
	}
	else
	{
		LOG_TRACE("mysql environment blocked: missing connection environment variables");
		++iBad;
	}
#else
	LOG_TRACE("mysql environment blocked: driver disabled at build time");
	++iBad;
#endif
	LOG_TRACE("mysql done bad=%d", iBad);
	TestBoard::GetInstance().Done(iBad ? 1 : 0);
}
