#pragma once
#include <stdio.h>
#include <string>

namespace FileHelper
{
	//检查文件是否存在
	bool FileExsit(const char* file_name);
	static bool FileExsit(const std::string& file_name)	{	return FileExsit(file_name.c_str());}
	//创建目录,支持多级
	bool MakeDir(const std::string& dir_name);
};