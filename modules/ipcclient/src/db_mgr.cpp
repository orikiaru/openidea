#include "db_mgr.h"
#include "oi_blocking_pool.h"
#include "oi_config.h"
#include "oi_db.h"
#include "oi_db_table.h"
#include "oi_errno.h"
#include "oi_rpcdata_dbsample.h"
#include "oi_mysql.h"
#include "oi_log.h"
#include "test_board.h"
#include <charconv>
#include <cstring>
#include <cstdlib>
#include <variant>
#include <limits>
#include <random>
#include <stdexcept>

void DbMgr::Load()
{
	TryGetConfig("Test.db", m_bEnabled);
}

void DbMgr::Expect(int32_t& iBad, bool bOk, const char* pLabel)
{
	if (!bOk)
	{
		LOG_TRACE("mysql check failed label=%s", pLabel);
		++iBad;
	}
}

::OI::Db::Blob DbMgr::TextBlob(const std::string& sText)
{
	return ::OI::Db::Blob(sText.begin(), sText.end());
}

std::optional<::OI::Db::Blob> DbMgr::CellOf(const ::OI::Db::Parameter& stParameter)
{
	if (std::holds_alternative<std::nullptr_t>(stParameter))
	{
		return std::nullopt;
	}
	if (const auto* pValue = std::get_if<int64_t>(&stParameter))
	{
		return TextBlob(std::to_string(*pValue));
	}
	if (const auto* pValue = std::get_if<uint64_t>(&stParameter))
	{
		return TextBlob(std::to_string(*pValue));
	}
	if (const auto* pValue = std::get_if<double>(&stParameter))
	{
		char vText[64];
		const std::to_chars_result stWritten = std::to_chars(vText, vText + sizeof(vText), *pValue);
		return ::OI::Db::Blob(vText, stWritten.ptr);
	}
	if (const auto* pValue = std::get_if<std::string>(&stParameter))
	{
		return TextBlob(*pValue);
	}
	return std::get<::OI::Db::Blob>(stParameter);
}

::OI::Db::CellMap DbMgr::CellsOf(const ::OI::Db::ParameterMap& mParameters)
{
	::OI::Db::CellMap mCells;
	for (const auto& [sColumn, stParameter] : mParameters)
	{
		mCells[sColumn] = CellOf(stParameter);
	}
	return mCells;
}

// 每个数据库字段都放了边界值：内嵌 NUL 和 0xff、整数两端、空串 key。sCache 是 [nodb]。
::OI::RpcData::DbSample DbMgr::MakeFullRow()
{
	::OI::RpcData::DbSample stRow;
	stRow.iId = 1;
	stRow.sName = std::string("a\0b\xff", 4);
	stRow.iSecret = -7;
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
	return stRow;
}

// 另一头的边界：空串、空 blob、空容器、默认嵌套结构。
::OI::RpcData::DbSample DbMgr::MakeEdgeRow()
{
	::OI::RpcData::DbSample stRow;
	stRow.iId = (std::numeric_limits<int64_t>::min)();
	stRow.iSecret = (std::numeric_limits<int32_t>::max)();
	stRow.iFlag = (std::numeric_limits<int8_t>::max)();
	return stRow;
}

// 比较全部数据库字段，不比 [nodb] 的 sCache。
// 不同时逐个打出字段名，便于定位是哪一列没还原。
bool DbMgr::SameRow(const ::OI::RpcData::DbSample& stFirst, const ::OI::RpcData::DbSample& stSecond)
{
	const std::pair<const char*, bool> vFields[] = {
		{"iId", stFirst.iId == stSecond.iId},
		{"sName", stFirst.sName == stSecond.sName},
		{"iSecret", stFirst.iSecret == stSecond.iSecret},
		{"iToken", stFirst.iToken == stSecond.iToken},
		{"dScore", stFirst.dScore == stSecond.dScore},
		{"fRatio", stFirst.fRatio == stSecond.fRatio},
		{"stPayload", stFirst.stPayload.Size() == stSecond.stPayload.Size()
				&& std::memcmp(stFirst.stPayload.Data(), stSecond.stPayload.Data(), stFirst.stPayload.Size()) == 0},
		{"vNumbers", stFirst.vNumbers == stSecond.vNumbers},
		{"mFlags", stFirst.mFlags == stSecond.mFlags},
		{"stChild", stFirst.stChild.iNumber == stSecond.stChild.iNumber && stFirst.stChild.sText == stSecond.stChild.sText},
		{"iFlag", stFirst.iFlag == stSecond.iFlag},
		{"bActive", stFirst.bActive == stSecond.bActive},
		{"vJsonNumbers", stFirst.vJsonNumbers == stSecond.vJsonNumbers},
		{"mJsonFlags", stFirst.mJsonFlags == stSecond.mJsonFlags},
		{"stJsonChild", stFirst.stJsonChild.iNumber == stSecond.stJsonChild.iNumber && stFirst.stJsonChild.sText == stSecond.stJsonChild.sText},
	};
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

void DbMgr::Start()
{
	if (!m_bEnabled)
	{
		return;
	}
	TestBoard::GetInstance().Arm();
	OI::Coro::StartTask([](OI::Coro::Token)
	{
		return DbMgr::GetInstance().Run();
	});
}

int32_t DbMgr::CheckRowMapping()
{
	using ::OI::RpcData::DbSample;
	int32_t iBad = 0;
	try
	{
		const DbSample stFull = MakeFullRow();
		const DbSample stEdge = MakeEdgeRow();

		Expect(iBad, std::string(DbSample::DbColumnList()) == "`id`,`sName`,`iSecret`,`iToken`,`dScore`,`fRatio`,`stPayload`,"
				"`vNumbers`,`mFlags`,`stChild`,`iFlag`,`bActive`,`vJsonNumbers`,`notes`,`stJsonChild`"
				&& std::string(DbSample::DbPlaceholders()) == "?,?,?,?,?,?,?,?,?,?,?,?,?,?,?"
				&& ::OI::Db::FieldCount<DbSample>() == 15,
			"column list");

		// 按列名取参数：标量按字段类型绑定，[dbjson] 是 JSON 文本，其余复杂字段是 PB blob，[nodb] 不出现。
		const ::OI::Db::ParameterMap mParameters = stFull.ToDbMap();
		Expect(iBad, mParameters.size() == 15 && mParameters.count("sCache") == 0 && mParameters.count("iId") == 0, "parameter map columns");
		Expect(iBad, std::get<int64_t>(mParameters.at("id")) == 1
				&& std::get<std::string>(mParameters.at("sName")) == stFull.sName
				&& std::get<int64_t>(mParameters.at("iSecret")) == -7
				&& std::get<uint64_t>(mParameters.at("iToken")) == stFull.iToken
				&& std::get<double>(mParameters.at("dScore")) == 1.5
				&& std::get<double>(mParameters.at("fRatio")) == 0.75
				&& std::get<::OI::Db::Blob>(mParameters.at("stPayload")) == ::OI::Db::Blob({0x00, 0xff, 'x'})
				&& std::get<int64_t>(mParameters.at("iFlag")) == -128
				&& std::get<int64_t>(mParameters.at("bActive")) == 1,
			"parameter map scalars");
		DbSample stDecoded;
		Expect(iBad, ::OI::Db::FromDbBlob(std::get<::OI::Db::Blob>(mParameters.at("vNumbers")), stDecoded.vNumbers)
				&& ::OI::Db::FromDbBlob(std::get<::OI::Db::Blob>(mParameters.at("mFlags")), stDecoded.mFlags)
				&& ::OI::Db::FromDbBlob(std::get<::OI::Db::Blob>(mParameters.at("stChild")), stDecoded.stChild)
				&& ::OI::Rpc::FromJsonText(std::get<std::string>(mParameters.at("vJsonNumbers")), stDecoded.vJsonNumbers)
				&& ::OI::Rpc::FromJsonText(std::get<std::string>(mParameters.at("notes")), stDecoded.mJsonFlags)
				&& ::OI::Rpc::FromJsonText(std::get<std::string>(mParameters.at("stJsonChild")), stDecoded.stJsonChild)
				&& stDecoded.vNumbers == stFull.vNumbers
				&& stDecoded.mFlags == stFull.mFlags
				&& stDecoded.stChild.sText == stFull.stChild.sText
				&& stDecoded.vJsonNumbers == stFull.vJsonNumbers
				&& stDecoded.mJsonFlags == stFull.mJsonFlags
				&& stDecoded.stJsonChild.sText == stFull.stJsonChild.sText,
			"parameter map complex values");

		// 整行参数的顺序就是 DbColumnList 的顺序。
		std::vector<std::string> vNames;
		std::string sColumn;
		for (const char iChar : std::string(DbSample::DbColumnList()))
		{
			if (iChar == ',')
			{
				vNames.push_back(sColumn);
				sColumn.clear();
			}
			else if (iChar != '`')
			{
				sColumn += iChar;
			}
		}
		vNames.push_back(sColumn);
		std::vector<::OI::Db::Parameter> vWhole;
		stFull.AppendDbParameters(vWhole);
		bool bOrdered = vWhole.size() == vNames.size();
		for (size_t iIndex = 0; bOrdered && iIndex < vNames.size(); ++iIndex)
		{
			bOrdered = vWhole[iIndex] == mParameters.at(vNames[iIndex]);
		}
		Expect(iBad, bOrdered, "row parameters follow column list");

		// 写进去的参数按库里取回的文本还原，再按列名读回，两头的边界值都不变。
		for (const DbSample* pSource : {&stFull, &stEdge})
		{
			::OI::Db::CellMap mCells = CellsOf(pSource->ToDbMap());
			mCells["extra"] = TextBlob("ignored");
			DbSample stLoaded;
			stLoaded.sCache = "keep";
			Expect(iBad, stLoaded.FromDbMap(mCells) && SameRow(stLoaded, *pSource) && stLoaded.sCache == "keep",
				pSource == &stFull ? "cell map full row" : "cell map edge row");
		}

		// 查询结果的列顺序和协议无关，按列名对上；列名重复整行拒绝。
		const ::OI::Db::CellMap mFullCells = CellsOf(mParameters);
		::OI::Db::Result stResult;
		stResult.vRows.emplace_back();
		for (auto it = mFullCells.rbegin(); it != mFullCells.rend(); ++it)
		{
			stResult.vColumns.push_back(it->first);
			stResult.vRows[0].push_back(it->second);
		}
		::OI::Db::CellMap mFromResult;
		DbSample stByMap;
		DbSample stByRow;
		Expect(iBad, ::OI::Db::RowCells(stResult, 0, mFromResult)
				&& stByMap.FromDbMap(mFromResult)
				&& stByRow.FromDbRow(stResult.vColumns, stResult.vRows[0])
				&& SameRow(stByMap, stFull)
				&& SameRow(stByRow, stFull),
			"result row by column name");
		::OI::Db::CellMap mUntouched = mFromResult;
		Expect(iBad, !::OI::Db::RowCells(stResult, 1, mUntouched) && mUntouched == mFromResult, "result row out of range");
		stResult.vColumns.push_back("id");
		stResult.vRows[0].push_back(TextBlob("2"));
		DbSample stDuplicate = stFull;
		Expect(iBad, !::OI::Db::RowCells(stResult, 0, mUntouched)
				&& mUntouched == mFromResult
				&& !stDuplicate.FromDbRow(stResult.vColumns, stResult.vRows[0])
				&& SameRow(stDuplicate, stFull),
			"result duplicate column");

		// 只给部分列，只改这几个字段。
		DbSample stPartial = stFull;
		stPartial.sCache = "keep";
		Expect(iBad, stPartial.FromDbMap({{"sName", TextBlob("z")}, {"notes", TextBlob("{\"n\":5}")}})
				&& stPartial.sName == "z"
				&& stPartial.mJsonFlags == std::map<std::string, int32_t>{{"n", 5}}
				&& stPartial.iId == stFull.iId
				&& stPartial.vNumbers == stFull.vNumbers
				&& stPartial.sCache == "keep",
			"partial cell map");

		// 任一列解不开都整行拒绝，对象保持原值。
		const std::vector<std::pair<std::string, ::OI::Db::CellMap>> vRejected = {
			{"reject null", {{"sName", TextBlob("y")}, {"id", std::nullopt}}},
			{"reject int8 range", {{"iFlag", TextBlob("128")}}},
			{"reject uint64 sign", {{"iToken", TextBlob("-1")}}},
			{"reject integer text", {{"iSecret", TextBlob("1x")}}},
			{"reject bool", {{"bActive", TextBlob("2")}}},
			{"reject real", {{"dScore", TextBlob("nan")}}},
			{"reject pb blob", {{"vNumbers", ::OI::Db::Blob({0xff})}}},
			{"reject child blob", {{"stChild", ::OI::Db::Blob({0x01, 0x00, 0x00, 0x00})}}},
			{"reject json", {{"notes", TextBlob("{bad")}}},
			{"reject json shape", {{"vJsonNumbers", TextBlob("{}")}}},
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
		Expect(iBad, ::OI::Db::Pick(stFull, stPicked, &DbSample::sName, &DbSample::mJsonFlags, &DbSample::vNumbers, &DbSample::bActive) == OI_OK
				&& stPicked.vColumns == std::vector<std::string>{"sName", "notes", "vNumbers", "bActive"}
				&& stPicked.sColumns == "`sName`,`notes`,`vNumbers`,`bActive`"
				&& stPicked.sMarks == "?,?,?,?"
				&& stPicked.sAssignments == "`sName`=?,`notes`=?,`vNumbers`=?,`bActive`=?"
				&& stPicked.sConditions == "`sName`=? AND `notes`=? AND `vNumbers`=? AND `bActive`=?"
				&& stPicked.vParameters.size() == 4
				&& stPicked.vParameters[0] == mParameters.at("sName")
				&& stPicked.vParameters[1] == mParameters.at("notes")
				&& stPicked.vParameters[2] == mParameters.at("vNumbers")
				&& stPicked.vParameters[3] == mParameters.at("bActive"),
			"pick members");
		::OI::Db::Picked stKept = stPicked;
		Expect(iBad, ::OI::Db::Pick(stFull, stKept, &DbSample::iId, &DbSample::sCache) == OI_ERR_INVALID_ARGUMENT
				&& stKept.sColumns == stPicked.sColumns
				&& stKept.vParameters == stPicked.vParameters,
			"pick nodb rejected");
		Expect(iBad, ::OI::Db::Pick(stFull, stKept, &DbSample::iId, &DbSample::iId) == OI_ERR_INVALID_ARGUMENT
				&& stKept.sColumns == stPicked.sColumns,
			"pick duplicate rejected");

		// 生成的语句逐字比对：列类型来自字段类型和 [dbtype]，主键来自 [dbkey]，参数和问号一一对应。
		const std::string sColumnList = DbSample::DbColumnList();
		const std::string sNonKeyAssignments = "`sName`=?,`iSecret`=?,`iToken`=?,`dScore`=?,`fRatio`=?,`stPayload`=?,`vNumbers`=?,"
			"`mFlags`=?,`stChild`=?,`iFlag`=?,`bActive`=?,`vJsonNumbers`=?,`notes`=?,`stJsonChild`=?";
		std::vector<::OI::Db::Parameter> vNonKey(vWhole.begin() + 1, vWhole.end());
		::OI::Db::Statement stCreate;
		Expect(iBad, ::OI::Db::BuildCreateTable<DbSample>("t_1", stCreate) == OI_OK
				&& stCreate.sSql == "CREATE TABLE IF NOT EXISTS `t_1` (`id` BIGINT NOT NULL, `sName` VARBINARY(64) NOT NULL, `iSecret` INT NOT NULL, "
					"`iToken` BIGINT UNSIGNED NOT NULL, `dScore` DOUBLE NOT NULL, `fRatio` DOUBLE NOT NULL, `stPayload` LONGBLOB NOT NULL, "
					"`vNumbers` LONGBLOB NOT NULL, `mFlags` LONGBLOB NOT NULL, `stChild` LONGBLOB NOT NULL, `iFlag` TINYINT NOT NULL, "
					"`bActive` TINYINT NOT NULL, `vJsonNumbers` LONGTEXT NOT NULL, `notes` TEXT NOT NULL, `stJsonChild` LONGTEXT NOT NULL, "
					"PRIMARY KEY (`id`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"
				&& stCreate.vParameters.empty(),
			"build create table");
		::OI::Db::Statement stInsert;
		Expect(iBad, ::OI::Db::BuildInsert("t_1", stFull, stInsert) == OI_OK
				&& stInsert.sSql == "INSERT INTO `t_1` (" + sColumnList + ") VALUES (" + DbSample::DbPlaceholders() + ")"
				&& stInsert.vParameters == vWhole,
			"build insert");
		::OI::Db::Statement stUpsert;
		std::vector<::OI::Db::Parameter> vUpsertParameters = vWhole;
		vUpsertParameters.insert(vUpsertParameters.end(), vNonKey.begin(), vNonKey.end());
		Expect(iBad, ::OI::Db::BuildUpsert("t_1", stFull, stUpsert) == OI_OK
				&& stUpsert.sSql == "INSERT INTO `t_1` (" + sColumnList + ") VALUES (" + DbSample::DbPlaceholders() + ") ON DUPLICATE KEY UPDATE " + sNonKeyAssignments
				&& stUpsert.vParameters == vUpsertParameters,
			"build upsert");
		const std::vector<::OI::Db::Parameter> vKey{mParameters.at("id")};
		::OI::Db::Statement stSelect;
		Expect(iBad, ::OI::Db::BuildSelectByKey("t_1", stFull, stSelect) == OI_OK
				&& stSelect.sSql == "SELECT " + sColumnList + " FROM `t_1` WHERE `id`=?"
				&& stSelect.vParameters == vKey,
			"build select by key");
		::OI::Db::Statement stSelectSome;
		Expect(iBad, ::OI::Db::BuildSelectByKey("t_1", stFull, stSelectSome, &DbSample::mJsonFlags, &DbSample::sName) == OI_OK
				&& stSelectSome.sSql == "SELECT `notes`,`sName` FROM `t_1` WHERE `id`=?"
				&& stSelectSome.vParameters == vKey,
			"build select picked columns");
		::OI::Db::Statement stUpdateSome;
		Expect(iBad, ::OI::Db::BuildUpdateByKey("t_1", stFull, stUpdateSome, &DbSample::sName, &DbSample::dScore) == OI_OK
				&& stUpdateSome.sSql == "UPDATE `t_1` SET `sName`=?,`dScore`=? WHERE `id`=?"
				&& stUpdateSome.vParameters == std::vector<::OI::Db::Parameter>{mParameters.at("sName"), mParameters.at("dScore"), mParameters.at("id")},
			"build update picked columns");
		::OI::Db::Statement stUpdateAll;
		std::vector<::OI::Db::Parameter> vUpdateAllParameters = vNonKey;
		vUpdateAllParameters.push_back(mParameters.at("id"));
		Expect(iBad, ::OI::Db::BuildUpdateByKey("t_1", stFull, stUpdateAll) == OI_OK
				&& stUpdateAll.sSql == "UPDATE `t_1` SET " + sNonKeyAssignments + " WHERE `id`=?"
				&& stUpdateAll.vParameters == vUpdateAllParameters,
			"build update all columns");
		::OI::Db::Statement stDelete;
		Expect(iBad, ::OI::Db::BuildDeleteByKey("t_1", stFull, stDelete) == OI_OK
				&& stDelete.sSql == "DELETE FROM `t_1` WHERE `id`=?"
				&& stDelete.vParameters == vKey,
			"build delete by key");

		// 表名只认字母、数字、下划线；主键不能改；[nodb] 和重复成员拒绝。失败时语句不变。
		const ::OI::Db::Statement stOriginal = stDelete;
		const auto fnUnchanged = [stOriginal](const ::OI::Db::Statement& stStatement)
		{
			return stStatement.sSql == stOriginal.sSql && stStatement.vParameters == stOriginal.vParameters;
		};
		for (const std::string& sBadTable : {std::string(""), std::string("t-1"), std::string("a`b"), std::string("t 1"), std::string(65, 't')})
		{
			::OI::Db::Statement stRejected = stOriginal;
			Expect(iBad, ::OI::Db::BuildCreateTable<DbSample>(sBadTable, stRejected) == OI_ERR_INVALID_ARGUMENT
					&& ::OI::Db::BuildInsert(sBadTable, stFull, stRejected) == OI_ERR_INVALID_ARGUMENT
					&& ::OI::Db::BuildUpsert(sBadTable, stFull, stRejected) == OI_ERR_INVALID_ARGUMENT
					&& ::OI::Db::BuildSelectByKey(sBadTable, stFull, stRejected) == OI_ERR_INVALID_ARGUMENT
					&& ::OI::Db::BuildUpdateByKey(sBadTable, stFull, stRejected) == OI_ERR_INVALID_ARGUMENT
					&& ::OI::Db::BuildDeleteByKey(sBadTable, stFull, stRejected) == OI_ERR_INVALID_ARGUMENT
					&& fnUnchanged(stRejected),
				"build rejects bad table name");
		}
		::OI::Db::Statement stRejected = stOriginal;
		Expect(iBad, ::OI::Db::BuildUpdateByKey("t_1", stFull, stRejected, &DbSample::iId) == OI_ERR_INVALID_ARGUMENT && fnUnchanged(stRejected),
			"build update rejects key column");
		Expect(iBad, ::OI::Db::BuildUpdateByKey("t_1", stFull, stRejected, &DbSample::sName, &DbSample::sCache) == OI_ERR_INVALID_ARGUMENT && fnUnchanged(stRejected),
			"build update rejects nodb member");
		Expect(iBad, ::OI::Db::BuildSelectByKey("t_1", stFull, stRejected, &DbSample::sName, &DbSample::sName) == OI_ERR_INVALID_ARGUMENT && fnUnchanged(stRejected),
			"build select rejects duplicate member");

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
		Expect(iBad, sJson.find("\"iSecret\"") == std::string::npos
				&& sJson.find("\"notes\"") == std::string::npos
				&& sJson.find("\"mJsonFlags\"") != std::string::npos
				&& sJson.find("\"sCache\"") != std::string::npos
				&& stJson.FromJson(sJson)
				&& stJson.iSecret == 0
				&& stJson.sCache == stJsonSource.sCache
				&& stJson.vNumbers == stJsonSource.vNumbers
				&& stJson.mJsonFlags == stJsonSource.mJsonFlags
				&& stJson.stJsonChild.iNumber == stJsonSource.stJsonChild.iNumber,
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

OI::Coro::TaskResult<int32_t> DbMgr::LoadRow(OI::Db::Client& stMysql, const std::string& sTable, ::OI::RpcData::DbSample& stRow)
{
	OI::Db::Statement stSelect;
	const int32_t iBuild = ::OI::Db::BuildSelectByKey(sTable, stRow, stSelect);
	if (iBuild != OI_OK)
	{
		co_return iBuild;
	}
	OI::Db::Result stResult;
	OI::Db::Error stError;
	const int32_t iStatus = co_await stMysql.Call(std::move(stSelect), stResult, stError);
	if (iStatus != OI_OK)
	{
		LOG_TRACE("mysql row load failed id=%lld status=%d stage=%s native=%u", static_cast<long long>(stRow.iId), iStatus, stError.sStage.c_str(), stError.iNativeCode);
		co_return iStatus;
	}
	::OI::Db::CellMap mCells;
	if (stResult.vRows.size() != 1 || !::OI::Db::RowCells(stResult, 0, mCells))
	{
		co_return OI_ERR_NOT_FOUND;
	}
	if (!stRow.FromDbMap(mCells))
	{
		co_return OI_ERR_DECODE;
	}
	co_return OI_OK;
}

OI::Coro::TaskResult<int32_t> DbMgr::CheckMysqlRows(OI::Db::Client& stMysql, const std::string& sTable)
{
	using ::OI::RpcData::DbSample;
	int32_t iBad = 0;
	OI::Db::Result stResult;
	OI::Db::Error stError;
	int32_t iStatus = OI_OK;
	bool bCreated = false;
	try
	{
		OI::Db::Statement stCreate;
		const int32_t iCreate = ::OI::Db::BuildCreateTable<DbSample>(sTable, stCreate);
		if (iCreate != OI_OK)
		{
			LOG_TRACE("mysql check failed label=build create table code=%d table=%s", iCreate, sTable.c_str());
			co_return iBad + 1;
		}
		iStatus = co_await stMysql.Call(std::move(stCreate), stResult, stError);
		if (iStatus != OI_OK)
		{
			LOG_TRACE("mysql row table create failed status=%d stage=%s native=%u", iStatus, stError.sStage.c_str(), stError.iNativeCode);
			co_return iBad + 1;
		}
		bCreated = true;

		const std::vector<DbSample> vRows{MakeFullRow(), MakeEdgeRow()};
		for (const DbSample& stRow : vRows)
		{
			OI::Db::Statement stInsert;
			const int32_t iBuild = ::OI::Db::BuildInsert(sTable, stRow, stInsert);
			Expect(iBad, iBuild == OI_OK, "build insert");
			iStatus = co_await stMysql.Call(std::move(stInsert), stResult, stError);
			if (iStatus != OI_OK || stResult.iAffected != 1)
			{
				LOG_TRACE("mysql row insert failed id=%lld status=%d stage=%s native=%u", static_cast<long long>(stRow.iId), iStatus, stError.sStage.c_str(), stError.iNativeCode);
				++iBad;
			}
		}

		// 主键重复的 INSERT 由库拒绝。
		OI::Db::Statement stDuplicate;
		const int32_t iDuplicate = ::OI::Db::BuildInsert(sTable, vRows[0], stDuplicate);
		iStatus = co_await stMysql.Call(std::move(stDuplicate), stResult, stError);
		Expect(iBad, iDuplicate == OI_OK && iStatus != OI_OK, "mysql duplicate key rejected");

		// 按主键读回，[nodb] 字段不被覆盖。
		for (const DbSample& stRow : vRows)
		{
			DbSample stLoaded;
			stLoaded.iId = stRow.iId;
			stLoaded.sCache = "keep";
			const int32_t iLoad = co_await LoadRow(stMysql, sTable, stLoaded);
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
		OI::Db::Statement stUpdate;
		const int32_t iUpdate = ::OI::Db::BuildUpdateByKey(sTable, stUnsaved, stUpdate, &DbSample::sName, &DbSample::dScore, &DbSample::mJsonFlags, &DbSample::vNumbers);
		iStatus = co_await stMysql.Call(std::move(stUpdate), stResult, stError);
		Expect(iBad, iUpdate == OI_OK && iStatus == OI_OK && stResult.iAffected == 1, "mysql pick update");
		DbSample stChangedLoaded;
		stChangedLoaded.iId = stChanged.iId;
		const int32_t iChangedLoad = co_await LoadRow(stMysql, sTable, stChangedLoaded);
		DbSample stEdgeLoaded;
		stEdgeLoaded.iId = vRows[1].iId;
		const int32_t iEdgeLoad = co_await LoadRow(stMysql, sTable, stEdgeLoaded);
		Expect(iBad, iChangedLoad == OI_OK && iEdgeLoad == OI_OK && SameRow(stChangedLoaded, stChanged) && SameRow(stEdgeLoaded, vRows[1]),
			"mysql pick update keeps other columns");

		// 按成员指针只读几列，写回只改这几个字段。
		OI::Db::Statement stPartial;
		const int32_t iPartial = ::OI::Db::BuildSelectByKey(sTable, stChanged, stPartial, &DbSample::sName, &DbSample::mJsonFlags);
		iStatus = co_await stMysql.Call(std::move(stPartial), stResult, stError);
		::OI::Db::CellMap mPartialCells;
		DbSample stMixed = vRows[1];
		Expect(iBad, iPartial == OI_OK && iStatus == OI_OK
				&& ::OI::Db::RowCells(stResult, 0, mPartialCells)
				&& mPartialCells.size() == 2
				&& stMixed.FromDbMap(mPartialCells)
				&& stMixed.sName == stChanged.sName
				&& stMixed.mJsonFlags == stChanged.mJsonFlags
				&& stMixed.iId == vRows[1].iId
				&& stMixed.vJsonNumbers == vRows[1].vJsonNumbers,
			"mysql pick select");

		// upsert：主键已存在改写其余列，不存在就插入。
		DbSample stUpserted = stChanged;
		stUpserted.iSecret = -1;
		stUpserted.stChild.sText = "upsert";
		stUpserted.bActive = false;
		OI::Db::Statement stUpsert;
		const int32_t iUpsert = ::OI::Db::BuildUpsert(sTable, stUpserted, stUpsert);
		iStatus = co_await stMysql.Call(std::move(stUpsert), stResult, stError);
		DbSample stUpsertLoaded;
		stUpsertLoaded.iId = stUpserted.iId;
		const int32_t iUpsertLoad = co_await LoadRow(stMysql, sTable, stUpsertLoaded);
		Expect(iBad, iUpsert == OI_OK && iStatus == OI_OK && iUpsertLoad == OI_OK && SameRow(stUpsertLoaded, stUpserted), "mysql upsert existing row");

		DbSample stFresh = MakeEdgeRow();
		stFresh.iId = 7;
		stFresh.sName = "fresh";
		OI::Db::Statement stUpsertFresh;
		const int32_t iUpsertFresh = ::OI::Db::BuildUpsert(sTable, stFresh, stUpsertFresh);
		iStatus = co_await stMysql.Call(std::move(stUpsertFresh), stResult, stError);
		const uint64_t iFreshAffected = stResult.iAffected;
		DbSample stFreshLoaded;
		stFreshLoaded.iId = stFresh.iId;
		const int32_t iFreshLoad = co_await LoadRow(stMysql, sTable, stFreshLoaded);
		Expect(iBad, iUpsertFresh == OI_OK && iStatus == OI_OK && iFreshAffected == 1 && iFreshLoad == OI_OK && SameRow(stFreshLoaded, stFresh),
			"mysql upsert new row");

		// 不挑成员就改全部非主键列。
		DbSample stWhole = MakeFullRow();
		stWhole.iId = vRows[1].iId;
		stWhole.sName = "whole";
		OI::Db::Statement stUpdateAll;
		const int32_t iUpdateAll = ::OI::Db::BuildUpdateByKey(sTable, stWhole, stUpdateAll);
		iStatus = co_await stMysql.Call(std::move(stUpdateAll), stResult, stError);
		const uint64_t iWholeAffected = stResult.iAffected;
		DbSample stWholeLoaded;
		stWholeLoaded.iId = stWhole.iId;
		const int32_t iWholeLoad = co_await LoadRow(stMysql, sTable, stWholeLoaded);
		Expect(iBad, iUpdateAll == OI_OK && iStatus == OI_OK && iWholeAffected == 1 && iWholeLoad == OI_OK && SameRow(stWholeLoaded, stWhole),
			"mysql update all columns");

		// 按主键删，删完读不到。
		OI::Db::Statement stDelete;
		const int32_t iDelete = ::OI::Db::BuildDeleteByKey(sTable, stFresh, stDelete);
		iStatus = co_await stMysql.Call(std::move(stDelete), stResult, stError);
		const uint64_t iDeleteAffected = stResult.iAffected;
		DbSample stDeleted;
		stDeleted.iId = stFresh.iId;
		const int32_t iDeletedLoad = co_await LoadRow(stMysql, sTable, stDeleted);
		Expect(iBad, iDelete == OI_OK && iStatus == OI_OK && iDeleteAffected == 1 && iDeletedLoad == OI_ERR_NOT_FOUND, "mysql delete by key");

		// 故障注入用手写 SQL：库里的 NULL 和坏 PB blob 读回时整行拒绝，对象保持原值。
		const std::string sQuoted = ::OI::Db::QuoteName(sTable);
		OI::Db::Statement stAllowNull{"ALTER TABLE " + sQuoted + " MODIFY `sName` VARBINARY(64) NULL", {}};
		iStatus = co_await stMysql.Call(std::move(stAllowNull), stResult, stError);
		Expect(iBad, iStatus == OI_OK, "mysql allow null");
		OI::Db::Statement stNull{"UPDATE " + sQuoted + " SET `sName`=NULL WHERE `id`=?", {stUpserted.iId}};
		iStatus = co_await stMysql.Call(std::move(stNull), stResult, stError);
		Expect(iBad, iStatus == OI_OK && stResult.iAffected == 1, "mysql set null");
		DbSample stKeptNull = stUpserted;
		const int32_t iNullLoad = co_await LoadRow(stMysql, sTable, stKeptNull);
		Expect(iBad, iNullLoad == OI_ERR_DECODE && SameRow(stKeptNull, stUpserted), "mysql null rejected");

		OI::Db::Statement stBroken{"UPDATE " + sQuoted + " SET `sName`=?, `vNumbers`=? WHERE `id`=?", {std::string("ok"), ::OI::Db::Blob({0xff}), stUpserted.iId}};
		iStatus = co_await stMysql.Call(std::move(stBroken), stResult, stError);
		Expect(iBad, iStatus == OI_OK && stResult.iAffected == 1, "mysql set bad blob");
		DbSample stKeptBroken = stUpserted;
		const int32_t iBrokenLoad = co_await LoadRow(stMysql, sTable, stKeptBroken);
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
			OI::Db::Statement stDrop{"DROP TABLE " + ::OI::Db::QuoteName(sTable), {}};
			iStatus = co_await stMysql.Call(std::move(stDrop), stResult, stError);
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

OI::Coro::Task DbMgr::Run()
{
	int32_t iBad = CheckRowMapping();
	const int32_t iConfig = OI::Thread::ConfigureBlockingPool(OI::Thread::BlockingType::Mysql, 4, 64);
	if (iConfig != OI_OK)
	{
		LOG_TRACE("mysql blocking pool configure failed code=%d", iConfig);
		TestBoard::GetInstance().Done(1);
		co_return;
	}
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
			const auto fnValue = [](const std::optional<OI::Db::Blob>& stValue, const std::string& sExpected)
			{
				return stValue && *stValue == OI::Db::Blob(sExpected.begin(), sExpected.end());
			};
			OI::Db::Statement stRepeatedId{"SELECT CONNECTION_ID()", {}};
			OI::Db::Result stRepeatedResult;
			iStatus = co_await stMysql.Call(stRepeatedId, stRepeatedResult, stError);
			if (iStatus != OI_OK || stRepeatedResult.vRows.size() != 1 || stRepeatedResult.vRows[0].size() != 1 ||
			    !stRepeatedResult.vRows[0][0] || stRepeatedResult.vRows[0][0]->empty())
			{
				LOG_TRACE("mysql repeated connection id failed status=%d", iStatus);
				++iBad;
			}
			const auto vExpectedId = stRepeatedResult.vRows;
			OI::Db::Statement stParameters;
			stParameters.sSql = "SELECT ?, ?, ?, ?, ?, ?, ?, ?";
			stParameters.vParameters = {std::string("a\0b", 3), nullptr, std::string(), OI::Db::Blob(),
			    std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max(),
			    uint64_t{0}, std::numeric_limits<uint64_t>::max()};
			for (int32_t iRound = 0; iRound < 8; ++iRound)
			{
				iStatus = co_await stMysql.Call(stParameters, stResult, stError);
				if (iStatus != OI_OK || stResult.vRows.size() != 1 || stResult.vRows[0].size() != 8 ||
				    !fnValue(stResult.vRows[0][0], std::string("a\0b", 3)) || stResult.vRows[0][1].has_value() ||
				    !fnValue(stResult.vRows[0][2], "") || !fnValue(stResult.vRows[0][3], "") ||
				    !fnValue(stResult.vRows[0][4], "-9223372036854775808") ||
				    !fnValue(stResult.vRows[0][5], "9223372036854775807") || !fnValue(stResult.vRows[0][6], "0") ||
				    !fnValue(stResult.vRows[0][7], "18446744073709551615"))
				{
					LOG_TRACE("mysql parameters round=%d failed status=%d stage=%s native=%u", iRound, iStatus, stError.sStage.c_str(), stError.iNativeCode);
					++iBad;
				}
				iStatus = co_await stMysql.Call(stRepeatedId, stRepeatedResult, stError);
				if (iStatus != OI_OK || stRepeatedResult.vRows != vExpectedId)
				{
					LOG_TRACE("mysql repeated connection reuse round=%d failed status=%d", iRound, iStatus);
					++iBad;
				}
			}
			OI::Db::Statement stConnectionId{"SELECT CONNECTION_ID()", {}};
			iStatus = co_await stMysql.Call(stConnectionId, stResult, stError);
			OI::Db::Result stFirstId = stResult;
			if (iStatus != OI_OK || stFirstId.vRows.size() != 1 || stFirstId.vRows[0].size() != 1 ||
			    !stFirstId.vRows[0][0] || stFirstId.vRows[0][0]->empty())
			{
				LOG_TRACE("mysql first connection id failed status=%d", iStatus);
				++iBad;
			}
			iStatus = co_await stMysql.Call(stConnectionId, stResult, stError);
			if (iStatus != OI_OK || stResult.vRows != stFirstId.vRows)
			{
				LOG_TRACE("mysql connection reuse failed status=%d", iStatus);
				++iBad;
			}
			OI::Db::Statement stSetSession{"SET @oi_regression_session = 42", {}};
			iStatus = co_await stMysql.Call(std::move(stSetSession), stResult, stError);
			if (iStatus != OI_OK)
			{
				LOG_TRACE("mysql session variable set failed status=%d stage=%s native=%u", iStatus, stError.sStage.c_str(), stError.iNativeCode);
				++iBad;
			}
			OI::Db::Statement stSession{"SELECT @oi_regression_session", {}};
			iStatus = co_await stMysql.Call(std::move(stSession), stResult, stError);
			if (iStatus != OI_OK || stResult.vRows.size() != 1 || stResult.vRows[0].size() != 1 ||
			    stResult.vRows[0][0].has_value())
			{
				LOG_TRACE("mysql session reset failed status=%d stage=%s native=%u", iStatus, stError.sStage.c_str(), stError.iNativeCode);
				++iBad;
			}
			OI::Db::Statement stDatabase{"SELECT DATABASE()", {}};
			iStatus = co_await stMysql.Call(std::move(stDatabase), stResult, stError);
			if (iStatus != OI_OK || stResult.vRows.size() != 1 || stResult.vRows[0].size() != 1 ||
			    !fnValue(stResult.vRows[0][0], stConfig.sDatabase))
			{
				LOG_TRACE("mysql schema reset failed status=%d stage=%s native=%u", iStatus, stError.sStage.c_str(), stError.iNativeCode);
				++iBad;
			}
			OI::Db::Statement stCreate{"CREATE TABLE " + sQuotedTable + " (id BIGINT PRIMARY KEY) ENGINE=InnoDB", {}};
			iStatus = co_await stMysql.Call(std::move(stCreate), stResult, stError);
			if (iStatus != OI_OK)
			{
				LOG_TRACE("mysql create test table failed status=%d stage=%s native=%u", iStatus, stError.sStage.c_str(), stError.iNativeCode);
				++iBad;
			}
			else
			{
				bTableCreated = true;
				std::vector<OI::Db::Statement> vStatements;
				vStatements.push_back({"INSERT INTO " + sQuotedTable + " (id) VALUES (1)", {}});
				vStatements.push_back({"INSERT INTO " + sQuotedTable + " (missing_column) VALUES (2)", {}});
				std::vector<OI::Db::Result> vResults;
				iStatus = co_await stMysql.Transaction(std::move(vStatements), vResults, stError);
				if (iStatus == OI_OK)
				{
					LOG_TRACE("mysql transaction bad SQL unexpectedly succeeded");
					++iBad;
				}
				OI::Db::Statement stCount{"SELECT COUNT(*) FROM " + sQuotedTable, {}};
				iStatus = co_await stMysql.Call(std::move(stCount), stResult, stError);
				if (iStatus != OI_OK || stResult.vRows.size() != 1 || stResult.vRows[0].size() != 1 ||
				    !fnValue(stResult.vRows[0][0], "0"))
				{
					LOG_TRACE("mysql transaction rollback or recovery failed status=%d stage=%s native=%u", iStatus, stError.sStage.c_str(), stError.iNativeCode);
					++iBad;
				}
			}
			OI::Db::Statement stBad{"SELECT missing_column", {}};
			iStatus = co_await stMysql.Call(std::move(stBad), stResult, stError);
			if (iStatus == OI_OK)
			{
				LOG_TRACE("mysql bad SQL unexpectedly succeeded");
				++iBad;
			}
			OI::Db::Statement stRecover{"SELECT 1", {}};
			iStatus = co_await stMysql.Call(std::move(stRecover), stResult, stError);
			if (iStatus != OI_OK || stResult.vRows.size() != 1 || stResult.vRows[0].size() != 1 ||
			    !fnValue(stResult.vRows[0][0], "1"))
			{
				LOG_TRACE("mysql bad SQL recovery failed status=%d stage=%s native=%u", iStatus, stError.sStage.c_str(), stError.iNativeCode);
				++iBad;
			}
			OI::Db::Result stSentinel;
			stSentinel.iAffected = 123;
			stSentinel.vColumns.push_back("sentinel");
			OI::Db::Statement stLimitedQuery{"SELECT 'too large'", {}};
			iStatus = co_await stLimited.Call(std::move(stLimitedQuery), stSentinel, stError);
			if (iStatus == OI_OK || stError.sStage != "result_limit" || stSentinel.iAffected != 123 ||
			    stSentinel.vColumns != std::vector<std::string>{"sentinel"} || !stSentinel.vRows.empty())
			{
				LOG_TRACE("mysql result limit failed status=%d stage=%s", iStatus, stError.sStage.c_str());
				++iBad;
			}
			iBad += co_await CheckMysqlRows(stMysql, sTable + "_row");
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
			OI::Db::Statement stDrop{"DROP TABLE " + sQuotedTable, {}};
			try
			{
				iStatus = co_await stMysql.Call(std::move(stDrop), stResult, stError);
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
