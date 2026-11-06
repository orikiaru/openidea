#pragma once
#include <string>

// 路径只做两件事：在不在，以及把中间目录补齐。
namespace OI
{
namespace File
{
// 文件或目录当前是否存在。空路径返回 false。
bool Exists(const std::string& sPath);
// 把路径上缺的目录补齐。已经是目录返回 true，中间某层建不出来返回 false。
bool MakeDirs(const std::string& sPath);
}
}
