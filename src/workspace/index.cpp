#include "pilot/index.hpp"
#include <regex>
#include <sstream>
#include <algorithm>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace pilot {
Json scan_project(const agent_fs::path& root) {
    Json files=Json::array(),symbols=Json::array(),todos=Json::array(),errors=Json::array();
    bool truncated=false;
    std::error_code ec;
    agent_fs::recursive_directory_iterator it(root,agent_fs::directory_options::skip_permission_denied,ec),end;
    const std::regex function(R"((?:^|\s)([A-Za-z_~][A-Za-z_0-9:~]*)\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?\{)");
    const std::regex type(R"(^\s*(?:class|struct|enum\s+class|enum)\s+([A-Za-z_][A-Za-z_0-9]*))");
    const std::regex pending(R"(TODO|FIXME|not implemented|NOT_IMPLEMENTED)");
    std::size_t visited=0;
    while(it!=end) {
        if(++visited>20000) {truncated=true;break;}
        const auto entry=*it;
        const auto attrs=GetFileAttributesW(entry.path().c_str());
        const auto name=entry.path().filename().u8string();
        if(it.depth()>10) truncated=true;
        if(attrs==INVALID_FILE_ATTRIBUTES || (attrs&FILE_ATTRIBUTE_REPARSE_POINT) || skipped(name) || it.depth()>10) {
            it.disable_recursion_pending();
        } else if(entry.is_regular_file(ec) && source_file(entry.path())) {
            if(files.size()>=1500) {truncated=true;break;}
            try {
                const auto relative=entry.path().lexically_relative(root).generic_u8string();
                const auto size=agent_fs::file_size(entry.path());
                Json info={{"path",relative},{"bytes",size},{"lines",0}};
                if(size<=256*1024) {
                    const auto data=read_bytes(entry.path(),256*1024);
                    if(data.find('\0')==std::string::npos) {
                        std::istringstream input(data);std::string line,previous;int number=0;
                        while(std::getline(input,line)) {
                            ++number;std::smatch match;
                            const auto trimmed=line.find_first_not_of(" \t\r");
                            const bool comment=trimmed!=std::string::npos && line.compare(trimmed,2,"//")==0;
                            std::string candidate=line;
                            int declaration_line=number;
                            if(trimmed!=std::string::npos && line[trimmed]=='{' && !previous.empty()) {
                                candidate=previous+" {"; declaration_line=number-1;
                            }
                            if(candidate.size()>1024) {info["skipped_long_declarations"]=true;truncated=true;}
                            if(!comment && candidate.size()<=1024 && symbols.size()<5000 && std::regex_search(candidate,match,type)) {
                                symbols.push_back({{"path",relative},{"line",declaration_line},{"name",match[1].str()},{"kind","type"}});
                            } else if(!comment && candidate.size()<=1024 && symbols.size()<5000 && std::regex_search(candidate,match,function)) {
                                const auto symbol=match[1].str();
                                if(symbol!="if" && symbol!="for" && symbol!="while" && symbol!="switch" && symbol!="catch")
                                    symbols.push_back({{"path",relative},{"line",declaration_line},{"name",symbol},{"kind","function"}});
                            }
                            if(todos.size()<1000 && std::regex_search(line,pending))
                                todos.push_back({{"path",relative},{"line",number},{"text",clip(line,300)}});
                            previous=line;
                        }
                        info["lines"]=number;
                    }
                } else {info["skipped_content"]=true;truncated=true;}
                files.push_back(info);
            } catch(const std::exception& e) {errors.push_back({{"path",entry.path().u8string()},{"error",e.what()}});}
        }
        ec.clear();it.increment(ec);
        if(ec) {errors.push_back({{"error",ec.message()}});ec.clear();}
    }
    if(ec) errors.push_back({{"error",ec.message()}});
    std::sort(files.begin(),files.end(),[](const Json& a,const Json& b){return a["path"].get<std::string>()<b["path"].get<std::string>();});
    return {{"root",root.generic_u8string()},{"generated_at",timestamp()},{"files",files},
        {"symbols",symbols},{"todos",todos},{"errors",errors},{"truncated",truncated},
        {"symbol_method","lexical heuristic; not a C++ AST or semantic call graph"}};
}
Json query_index(const Json& index,const std::string& kind,const std::string& query) {
    Json result=Json::array();
    const auto& source=index.at(kind);
    for(const auto& item:source) {
        if(query.empty() || safe_json(item).find(query)!=std::string::npos) result.push_back(item);
        if(result.size()>=100) break;
    }
    return result;
}
std::string map_text(const Json& index,std::size_t limit) {
    std::ostringstream out;
    out<<"Project files (relative paths):\n";
    for(const auto& file:index.at("files")) {
        out<<file.at("path").get<std::string>()<<" ("<<file.at("lines")<<" lines)\n";
        if(static_cast<std::size_t>(out.tellp())>limit*2/3) {out<<"[more files omitted]\n";break;}
    }
    out<<"\nSymbol candidates (lexical, verify with read_text):\n";
    for(const auto& symbol:index.at("symbols")) {
        out<<symbol.at("path").get<std::string>()<<":"<<symbol.at("line")<<" "<<symbol.at("name").get<std::string>()<<"\n";
        if(static_cast<std::size_t>(out.tellp())>limit) break;
    }
    return clip(out.str(),limit);
}
}
