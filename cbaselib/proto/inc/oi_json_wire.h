#pragma once
#include "json/json.h"
#include <string_view>

namespace OI
{
namespace Rpc
{
bool ValidJsonUtf8(std::string_view sText);
bool ValidJsonTree(const Json::Value& stValue);
}
}
