#pragma once
#include <string>

// 路径只做两件事：在不在，以及把中间目录补齐。
namespace OI { namespace File {
bool Exists(const std::string& sPathParam);
bool MakeDirs(const std::string& sPathParam);
}}
