#pragma once
#include <string>
#include "oi_errno.h"

// 路径只做两件事：在不在，以及把中间目录补齐。
namespace OI
{
namespace File
{
// 文件或目录当前是否存在。空路径返回 false。
bool Exists(const std::string& sPath);
// 把路径上缺的目录补齐。成功返回 OI_OK，失败保留系统错误码。
int32_t MakeDirs(const std::string& sPath);
}
}
