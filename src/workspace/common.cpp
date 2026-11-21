#include "pilot/common.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <atomic>
#include <set>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
namespace pilot {
std::string file_digest(const agent_fs::path& path) {
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) throw std::runtime_error("SHA256 initialization failed");
    std::vector<unsigned char> object,digest(32);
    try {
        DWORD length=0,received=0;
        if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&length),sizeof(length),&received,0)<0) throw std::runtime_error("SHA256 size failed");
        object.resize(length);
        if(BCryptCreateHash(algorithm,&hash,object.data(),length,nullptr,0,0)<0) throw std::runtime_error("SHA256 create failed");
        std::ifstream input(path,std::ios::binary);if(!input) throw std::runtime_error("Cannot hash file");
        unsigned char buffer[8192];
        while(input) {
            input.read(reinterpret_cast<char*>(buffer),sizeof(buffer));
            if(input.gcount() && BCryptHashData(hash,buffer,static_cast<ULONG>(input.gcount()),0)<0) throw std::runtime_error("SHA256 update failed");
        }
        if(input.bad() || BCryptFinishHash(hash,digest.data(),32,0)<0) throw std::runtime_error("SHA256 read/finish failed");
        BCryptDestroyHash(hash);hash=nullptr;BCryptCloseAlgorithmProvider(algorithm,0);algorithm=nullptr;
        std::ostringstream result;result<<std::hex<<std::setfill('0');for(auto byte:digest) result<<std::setw(2)<<static_cast<unsigned>(byte);return result.str();
    } catch(...) {if(hash) BCryptDestroyHash(hash);if(algorithm) BCryptCloseAlgorithmProvider(algorithm,0);throw;}
}
std::string timestamp() {
    const auto t=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm value{}; gmtime_s(&value,&t);
    std::ostringstream out; out<<std::put_time(&value,"%Y-%m-%dT%H:%M:%SZ"); return out.str();
}
std::string unique_id() {
    static std::atomic<unsigned> counter{0};
    const auto tick=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return std::to_string(tick)+"-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(counter++);
}
std::string safe_json(const Json& data,int indent) {
    return data.dump(indent,' ',false,Json::error_handler_t::replace);
}
std::string clip(const std::string& text,std::size_t limit) {
    if(text.size()<=limit) return text;
    std::size_t end=limit;
    while(end && (static_cast<unsigned char>(text[end])&0xc0)==0x80) --end;
    return text.substr(0,end)+"\n[output truncated]";
}
std::string read_bytes(const agent_fs::path& file,std::size_t max_bytes) {
    if(agent_fs::file_size(file)>max_bytes) throw std::runtime_error("File exceeds byte limit");
    std::ifstream stream(file,std::ios::binary);
    if(!stream) throw std::runtime_error("Cannot open file: "+file.u8string());
    std::string data((std::istreambuf_iterator<char>(stream)),{});
    if(stream.bad()) throw std::runtime_error("File read failed");
    if(data.size()>max_bytes) throw std::runtime_error("File grew beyond byte limit");
    return data;
}
void write_atomic(const agent_fs::path& file,const std::string& content) {
    const auto temp=agent_fs::path(file.wstring()+L".pilot-"+agent_fs::u8path(unique_id()).wstring()+L".tmp");
    try {
        std::ofstream out(temp,std::ios::binary|std::ios::trunc);
        if(!out) throw std::runtime_error("Cannot create temporary file");
        out.write(content.data(),static_cast<std::streamsize>(content.size()));out.close();
        if(!out) throw std::runtime_error("File write failed");
        if(!MoveFileExW(temp.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Atomic replace failed: "+std::to_string(GetLastError()));
    } catch(...) { std::error_code ec;agent_fs::remove(temp,ec);throw; }
}
bool skipped(const std::string& name) {
    return name==".git" || name==".runtime" || name==".agent_data" || name=="third_party" ||
        name=="node_modules" || name==".venv" || name=="venv" || name=="history" ||
        name=="__pycache__" || name.rfind("build",0)==0 || name==".cpppilot-build";
}
bool source_file(const agent_fs::path& path) {
    static const std::set<std::string> extensions={".cpp",".cc",".cxx",".c",".h",".hpp",".hxx",
        ".md",".txt",".cmake",".json",".py",".ps1",".toml",".yaml",".yml"};
    return extensions.count(path.extension().u8string())!=0;
}
agent_fs::path checked_path(const agent_fs::path& root,const std::string& relative,bool allow_new) {
    if(relative.empty() || relative.find('\0')!=std::string::npos || relative.find(':')!=std::string::npos)
        throw std::runtime_error("Invalid relative path");
    const auto path=agent_fs::u8path(relative);
    if(path.has_root_path()) throw std::runtime_error("Absolute paths are not allowed");
    for(const auto& part:path) if(part=="..") throw std::runtime_error("Parent traversal is not allowed");
    const auto full=(agent_fs::absolute(root)/path).lexically_normal();
    auto current=full.root_path();
    for(const auto& part:full.relative_path()) {
        current/=part;
        const auto attributes=GetFileAttributesW(current.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES) {
            const auto code=GetLastError();
            if(allow_new && current==full && (code==ERROR_FILE_NOT_FOUND || code==ERROR_PATH_NOT_FOUND) &&
               agent_fs::is_directory(full.parent_path())) continue;
            throw std::runtime_error("Path missing or inaccessible: "+current.u8string());
        }
        if(attributes&FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("Links/reparse points are not allowed");
    }
    return full;
}
Json ok(const Json& data) { return {{"ok",true},{"data",data}}; }
Json error(const std::string& code,const std::string& detail) {
    return {{"ok",false},{"error_code",code},{"detail",detail}};
}
}
