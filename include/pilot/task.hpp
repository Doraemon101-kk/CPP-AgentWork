#pragma once
#include "pilot/common.hpp"
#include <stdexcept>
namespace pilot {
struct Cancelled : std::runtime_error { Cancelled():std::runtime_error("Task cancelled") {} };
bool cancellation_requested();
void check_cancelled();
class TaskScope {
    agent_fs::path state_, signal_;
    Json record_;
    bool finished_=false;
public:
    TaskScope(const agent_fs::path& state,const std::string& session,const std::string& mode);
    ~TaskScope();
    void finish(const std::string& status);
};
Json cancel_active_task(const agent_fs::path& state);
}
