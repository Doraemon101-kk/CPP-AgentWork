#include "pilot/edits.hpp"
#include <regex>
#include <sstream>
namespace pilot {
namespace {
void text_only(const std::string& value) {
    if(value.find('\0')!=std::string::npos) throw std::runtime_error("Binary content is not editable");
    if(value.size()>256*1024) throw std::runtime_error("Edit exceeds 256 KiB limit");
    // Parsing through JSON dump validates UTF-8 without altering file bytes.
    Json(value).dump();
}
std::string preview(const Json& change) {
    std::ostringstream out;
    out<<"Change "<<change.at("id").get<std::string>()<<" ["<<change.at("status").get<std::string>()<<"]\n";
    const auto path=change.at("path").get<std::string>();
    out<<"--- "<<(change.at("created").get<bool>()?"/dev/null":"a/"+path)<<"\n+++ b/"<<path<<"\n";
    out<<"@@ exact replacement preview @@\n";
    std::istringstream before(change.at("old_text").get<std::string>()),after(change.at("new_text").get<std::string>());
    std::string line;
    while(std::getline(before,line)) out<<"- "<<line<<"\n";
    while(std::getline(after,line)) out<<"+ "<<line<<"\n";
    return out.str();
}
}
EditStore::EditStore(agent_fs::path root,agent_fs::path directory):root_(std::move(root)),directory_(std::move(directory)) {
    agent_fs::create_directories(directory_);
}
Json EditStore::load(const std::string& id) const {
    if(!std::regex_match(id,std::regex("[A-Za-z0-9_-]{1,80}"))) throw std::runtime_error("Invalid change ID");
    return Json::parse(read_bytes(directory_/agent_fs::u8path(id+".json"),2*1024*1024));
}
void EditStore::save(const Json& change) const {
    write_atomic(directory_/agent_fs::u8path(change.at("id").get<std::string>()+".json"),safe_json(change,2));
}
Json EditStore::propose(const std::string& path,const std::string& old_text,const std::string& new_text,bool create) {
    const auto target=checked_path(root_,path,create);
    if(skipped(target.filename().u8string())) throw std::runtime_error("Cannot edit excluded path");
    for(const auto& component:target.lexically_relative(root_))
        if(skipped(component.u8string())) throw std::runtime_error("Cannot edit excluded directories");
    const auto normalized=target.lexically_relative(root_).generic_u8string();
    std::string before;
    if(create) {
        if(agent_fs::exists(target)) throw std::runtime_error("New file already exists");
    } else {
        if(!agent_fs::is_regular_file(target)) throw std::runtime_error("Target is not a file");
        before=read_bytes(target,256*1024);text_only(before);
        if(old_text.empty()) throw std::runtime_error("old_text must not be empty for existing files");
    }
    text_only(old_text);text_only(new_text);
    std::string after=new_text;
    if(!create) {
        const auto position=before.find(old_text);
        if(position==std::string::npos) throw std::runtime_error("old_text does not exactly match; read the file, omit line-number prefixes, preserve whitespace");
        if(before.find(old_text,position+1)!=std::string::npos) throw std::runtime_error("old_text is ambiguous; include more surrounding text");
        after=before;after.replace(position,old_text.size(),new_text);
        if(after==before) throw std::runtime_error("Edit makes no change");
    }
    text_only(after);
    for(const auto& item:agent_fs::directory_iterator(directory_)) {
        if(item.path().extension()!=".json") continue;
        auto previous=Json::parse(read_bytes(item.path(),2*1024*1024));
        if(previous.at("status")=="pending" && previous.at("path")==normalized && previous.at("after")==after && previous.at("before")==before)
            return {{"id",previous.at("id")},{"status","pending"},{"preview",preview(previous)},{"applied",false}};
    }
    Json change={{"id",unique_id()},{"path",normalized},{"before",before},{"after",after},
        {"old_text",old_text},{"new_text",new_text},{"created",create},{"status","pending"},{"proposed_at",timestamp()}};
    save(change);
    return {{"id",change.at("id")},{"status","pending"},{"preview",preview(change)},
        {"applied",false},{"next","Review and apply with /review ID then /apply ID"}};
}
Json EditStore::list() const {
    Json result=Json::array();
    for(const auto& item:agent_fs::directory_iterator(directory_)) {
        if(item.path().extension()!=".json") continue;
        auto change=Json::parse(read_bytes(item.path(),2*1024*1024));
        result.push_back({{"id",change.at("id")},{"path",change.at("path")},{"status",change.at("status")}});
    }
    std::sort(result.begin(),result.end(),[](const Json& a,const Json& b){return a.at("id").get<std::string>()<b.at("id").get<std::string>();});
    return result;
}
std::string EditStore::review(const std::string& id) const {return preview(load(id));}
Json EditStore::apply(const std::string& id) {
    auto change=load(id);
    if(change.at("status")!="pending") throw std::runtime_error("Only pending changes can be applied");
    const bool created=change.at("created").get<bool>();
    const auto path=checked_path(root_,change.at("path").get<std::string>(),created);
    const auto before=change.at("before").get<std::string>(),after=change.at("after").get<std::string>();
    if(created ? agent_fs::exists(path) : read_bytes(path,256*1024)!=before)
        throw std::runtime_error("Conflict: file changed since proposal; no content was written");
    write_atomic(path,after);
    change["status"]="applied";change["applied_at"]=timestamp();
    try {save(change);} catch(...) {
        if(created) agent_fs::remove(path);else write_atomic(path,before);
        throw;
    }
    return {{"id",id},{"path",change.at("path")},{"status","applied"}};
}
Json EditStore::undo(const std::string& id) {
    auto change=load(id);
    if(change.at("status")!="applied") throw std::runtime_error("Only applied changes can be undone");
    const auto path=checked_path(root_,change.at("path").get<std::string>());
    if(read_bytes(path,256*1024)!=change.at("after").get<std::string>())
        throw std::runtime_error("Conflict: file changed after application; refusing to overwrite newer work");
    const bool created=change.at("created").get<bool>();
    if(created) agent_fs::remove(path);else write_atomic(path,change.at("before").get<std::string>());
    change["status"]="undone";change["undone_at"]=timestamp();
    try {save(change);} catch(...) {write_atomic(path,change.at("after").get<std::string>());throw;}
    return {{"id",id},{"path",change.at("path")},{"status","undone"}};
}
Json EditStore::discard(const std::string& id) {
    auto change=load(id);
    if(change.at("status")!="pending") throw std::runtime_error("Only pending changes can be discarded");
    change["status"]="discarded";save(change);return {{"id",id},{"status","discarded"}};
}
}
