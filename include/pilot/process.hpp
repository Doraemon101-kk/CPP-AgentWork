#pragma once
#include "pilot/common.hpp"
namespace pilot {
Json run_process(const std::vector<std::string>& args,const agent_fs::path& cwd,int timeout_seconds=120);
Json project_check(const agent_fs::path& root,const agent_fs::path& build,const std::string& kind,int timeout_seconds=120);
}
