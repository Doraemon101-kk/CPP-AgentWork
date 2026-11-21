#pragma once
#include "pilot/common.hpp"
namespace pilot {
class EditStore {
    agent_fs::path root_, directory_;
    Json load(const std::string& id) const;
    void save(const Json& change) const;
public:
    EditStore(agent_fs::path root,agent_fs::path directory);
    Json propose(const std::string& path,const std::string& old_text,const std::string& new_text,bool create=false);
    Json list() const;
    std::string review(const std::string& id) const;
    Json apply(const std::string& id);
    Json undo(const std::string& id);
    Json discard(const std::string& id);
};
}
