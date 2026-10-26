#include <unistd.h>
#include <string.h>
#include <vector>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include "file_helper.h"
#include "logger.h"

namespace FileHelper
{
    bool FileExsit(const char* file_name)
    {
        return access(file_name, F_OK) == 0;
    }

    bool MakeDir(const std::string& dir_str)
    {
        //暂时没有检查\\\/,不允许建立有\/字符的目录
        size_t offset = 0;
        size_t next = 0;
        for (;;)
        {
            next = dir_str.find_first_of("/", offset);
            std::string dir = dir_str.substr(0, next);
            LOG_TRACE("dir=%s,next_dir=%s\n", dir_str.c_str(), dir.c_str());
            if (next == offset)
            {
                //根目录或者复数//情况
                offset = next + 1;
                continue;
            }
            offset = next + 1;
            bool end = false;
            if (next == std::string::npos)
            {
                end = true;
            }
            if (access(dir.c_str(), F_OK) == 0)
            {
                if (end)
                {
                    break;
                }
                continue;
            }
            int ret = mkdir(dir.c_str(), 0755);
            if (ret != 0)
            {
                perror("mkdir");
                return false;
            }
            if (end)
            {
                break;
            }

        }
        return true;
    }
}
