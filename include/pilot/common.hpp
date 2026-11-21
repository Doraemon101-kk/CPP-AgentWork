#pragma once
#include "tools.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
using Json = nlohmann::json;
namespace pilot {
std::string timestamp();
std::string file_digest(const agent_fs::path& path);
std::string unique_id();
std::string read_bytes(const agent_fs::path& file, std::size_t max_bytes=1024*1024);
void write_atomic(const agent_fs::path& file, const std::string& content);
std::string safe_json(const Json& data, int indent=-1);
std::string clip(const std::string& text, std::size_t limit);
bool skipped(const std::string& name);
bool source_file(const agent_fs::path& path);
agent_fs::path checked_path(const agent_fs::path& root, const std::string& relative, bool allow_new=false);
Json ok(const Json& data);
Json error(const std::string& code, const std::string& detail);
}
