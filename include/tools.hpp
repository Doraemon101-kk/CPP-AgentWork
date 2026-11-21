#pragma once
#include "filesystem_compat.hpp"
#include <string>

// Shared tool contracts used by the CLI and model tool-calling loop.
struct ToolCall {
    std::string name;
    std::string path;
    std::string keyword;  // search_text only
};

struct ToolResult {
    bool ok = false;
    std::string content;
    std::string error_code;
    bool truncated = false;
};

// All returned data owns its storage. The root outlives each call.
// Inputs are read-only borrows; returned results own their text.
ToolResult list_directory(const agent_fs::path& root,
                          const std::string& relative_path);
ToolResult read_text(const agent_fs::path& root,
                     const std::string& relative_path);
ToolResult search_text(const agent_fs::path& root,
                       const std::string& relative_path,
                       const std::string& keyword);
// JSON adapter validates required fields/types before constructing ToolCall.
// Dispatch validates names and arguments; file tools enforce path/output limits.
ToolResult dispatch_tool(const agent_fs::path& root,
                         const ToolCall& call);
