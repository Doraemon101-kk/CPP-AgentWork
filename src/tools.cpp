#include "tools.hpp"
#include "pilot/common.hpp"
#include <map>
#include <fstream>
#include <sstream>
#include <vector>
#include <iterator>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
// Reject links and Windows junctions before traversing them.
namespace {
bool is_link(const agent_fs::path& path) {
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        throw agent_fs::filesystem_error("Cannot inspect path", path,
            std::error_code(GetLastError(), std::system_category()));
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return agent_fs::is_symlink(agent_fs::symlink_status(path));
#endif
}
}
ToolResult list_directory(const agent_fs::path& root, const std::string& relative_path) {
    try {
        // 1. Only relative paths without parent traversal are accepted.
        const auto relative = agent_fs::u8path(relative_path);
        if (relative.empty() || relative.has_root_path()) {
            return {false, "Use a non-empty relative path", "invalid_arguments", false};
        }
        for (const auto& part : relative) {
            if (part == "..") {
                return {false, "Parent traversal is not allowed", "path_outside_root", false};
            }
        }

        // 2. Inspect every component, including ancestors of the configured root.
        const auto base = agent_fs::absolute(root).lexically_normal();
        const auto target = (base / relative).lexically_normal();
        auto current = target.root_path();
        for (const auto& part : target.relative_path()) {
            current /= part;
            if (!agent_fs::exists(current)) {
                return {false, "Path does not exist", "path_not_found", false};
            }
            if (is_link(current)) {
                return {false, "Links and reparse points are not allowed", "link_not_allowed", false};
            }
        }
        if (!agent_fs::is_directory(target)) {
            return {false, "Target is not a directory", "not_a_directory", false};
        }

        // 3. A map keeps names sorted. Retain only the first 200 names.
        constexpr std::size_t limit = 200;
        std::map<std::string, std::string> entries;
        bool truncated = false;
        for (const auto& entry : agent_fs::directory_iterator(target)) {
            const auto name = entry.path().filename().u8string();
            if (pilot::skipped(name)) {
                continue;
            }
            // Report links as links; never follow them to determine their type.
            const std::string type = is_link(entry.path()) ? "[link] " :
                entry.is_directory() ? "[dir] " :
                entry.is_regular_file() ? "[file] " : "[other] ";
            entries.emplace(name, type);
            if (entries.size() > limit) {
                entries.erase(std::prev(entries.end()));
                truncated = true;
            }
        }

        // 4. Return owned text. An empty directory is a successful empty result.
        std::string content;
        for (const auto& entry : entries) {
            content += entry.second + entry.first + "\n";
        }
        return {true, content, "", truncated};
    } catch (const agent_fs::filesystem_error& error) {
        return {false, error.what(), "filesystem_error", false};
    }
}
ToolResult read_text(const agent_fs::path& root, const std::string& relative_path) {
    try {
        const auto relative = agent_fs::u8path(relative_path);
        if (relative.empty() || relative.has_root_path())
            return {false, "Expected relative file path", "invalid_arguments", false};
        for (const auto& part : relative)
            if (part == "..") return {false, "Parent traversal denied", "path_outside_root", false};
        const auto parent = relative.parent_path().empty() ? agent_fs::path(".") : relative.parent_path();
        const auto checked = list_directory(root, parent.u8string());
        if (!checked.ok) return checked;
        const auto file = agent_fs::absolute(root / relative).lexically_normal();
        if (!agent_fs::exists(file)) return {false, "File not found", "path_not_found", false};
        if (is_link(file)) return {false, "Link denied", "link_not_allowed", false};
        if (!agent_fs::is_regular_file(file)) return {false, "Not a regular file", "not_a_file", false};
        std::ifstream input(file, std::ios::binary);
        if (!input) return {false, "Cannot open file", "read_error", false};
        std::string data(65537, '\0');
        input.read(&data[0], static_cast<std::streamsize>(data.size()));
        data.resize(static_cast<std::size_t>(input.gcount()));
        if (input.bad()) return {false, "Read failed", "read_error", false};
        const bool truncated = data.size() > 65536;
        if (truncated) data.resize(65536);
        if (data.find('\0') != std::string::npos)
            return {false, "Binary file is not supported", "unsupported_encoding", false};
        // Avoid ending in the middle of an UTF-8 code point after truncation.
        if (truncated) {
            auto end = data.size();
            while (end && (static_cast<unsigned char>(data[end-1]) & 0xc0) == 0x80) --end;
            if (end && static_cast<unsigned char>(data[end-1]) >= 0xc0) data.resize(end-1);
        }
#ifdef _WIN32
        if (!data.empty() && MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,data.data(),
                static_cast<int>(data.size()),nullptr,0)==0)
            return {false,"File is not valid UTF-8","unsupported_encoding",false};
#endif
        std::istringstream lines(data);
        std::string line, content;
        std::size_t number = 0;
        while (std::getline(lines, line)) content += std::to_string(++number) + ": " + line + "\n";
        return {true, content, "", truncated};
    } catch (const agent_fs::filesystem_error& e) {
        return {false, e.what(), "filesystem_error", false};
    }
}
ToolResult search_text(const agent_fs::path& root, const std::string& relative_path,
                       const std::string& keyword) {
    if (keyword.empty()) return {false, "Empty keyword", "invalid_arguments", false};
    // Accept either a file or a directory; read_text performs the same path checks.
    const auto single_file = read_text(root, relative_path);
    if (single_file.ok) {
        std::istringstream lines(single_file.content);
        std::string line, content;std::size_t matches=0;
        while (std::getline(lines,line)) {
            const auto start=line.find(": ");
            if(start!=std::string::npos && line.find(keyword,start+2)!=std::string::npos) {
                if(++matches>100) return {true,content,"",true};
                content+=relative_path+":"+line+"\n";
            }
        }
        return {true,content,"",single_file.truncated};
    }
    if(single_file.error_code!="not_a_file") return single_file;
    auto checked = list_directory(root, relative_path);
    if (!checked.ok) return checked;
    try {
        std::vector<std::pair<agent_fs::path,int>> pending{{agent_fs::u8path(relative_path),0}};
        std::string content;
        std::size_t files = 0, matches = 0;
        bool truncated = false;
        while (!pending.empty()) {
            const auto item = pending.back(); pending.pop_back();
            for (const auto& entry : agent_fs::directory_iterator(root / item.first)) {
                const auto name = entry.path().filename().u8string();
                if (is_link(entry.path()) || pilot::skipped(name)) continue;
                const auto relative = item.first / entry.path().filename();
                if (entry.is_directory()) {
                    if (item.second < 4) pending.push_back({relative,item.second+1});
                    else truncated = true;
                } else if (entry.is_regular_file()) {
                    const auto ext = entry.path().extension().u8string();
                    if (ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".c" &&
                        ext != ".txt" && ext != ".md" && ext != ".json" && ext != ".cmake") continue;
                    if (++files > 500) return {true,content,"",true};
                    const auto result = read_text(root, relative.u8string());
                    if (!result.ok) { truncated = true; continue; }
                    truncated = truncated || result.truncated;
                    std::istringstream lines(result.content); std::string line;
                    while (std::getline(lines,line)) {
                        const auto start = line.find(": ");
                        if (start != std::string::npos && line.find(keyword,start+2) != std::string::npos) {
                            if (++matches > 100) return {true,content,"",true};
                            content += relative.generic_u8string() + ":" + line + "\n";
                        }
                    }
                }
            }
        }
        return {true, content, "", truncated};
    } catch (const agent_fs::filesystem_error& e) {
        return {false,e.what(),"filesystem_error",false};
    }
}
// Route only known tool names. Each tool validates its own filesystem path.
ToolResult dispatch_tool(const agent_fs::path& root, const ToolCall& call) {
    if (call.name != "list_directory" &&
        call.name != "read_text" &&
        call.name != "search_text") {
        return {false, "Unknown tool: " + call.name, "unknown_tool", false};
    }

    if (call.path.empty()) {
        return {false, "path must not be empty; use . for the root",
                "invalid_arguments", false};
    }

    if (call.name == "search_text") {
        if (call.keyword.empty()) {
            return {false, "search_text requires a non-empty keyword",
                    "invalid_arguments", false};
        }
        return search_text(root, call.path, call.keyword);
    }

    if (!call.keyword.empty()) {
        return {false, "This tool does not accept a keyword",
                "invalid_arguments", false};
    }

    if (call.name == "list_directory") {
        return list_directory(root, call.path);
    }
    return read_text(root, call.path);
}
