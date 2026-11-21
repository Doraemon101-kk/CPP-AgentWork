#pragma once
#include "pilot/common.hpp"
namespace pilot {
Json scan_project(const agent_fs::path& root);
Json query_index(const Json& index,const std::string& kind,const std::string& query);
std::string map_text(const Json& index,std::size_t limit=9000);
}
