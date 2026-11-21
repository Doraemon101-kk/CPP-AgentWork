#include "pilot/task.hpp"
#include <atomic>
#include <mutex>
#include <regex>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace pilot {
namespace {
std::atomic<bool> active{false},cancelled{false};
agent_fs::path signal_path;
BOOL WINAPI console_control(DWORD type) {
    if((type==CTRL_C_EVENT || type==CTRL_BREAK_EVENT) && active.load()) {
        cancelled.store(true);return TRUE;
    }
    return FALSE;
}
}
bool cancellation_requested() {
    if(cancelled.load()) return true;
    std::error_code error;
    return active.load() && !signal_path.empty() && agent_fs::exists(signal_path,error);
}
void check_cancelled() {if(cancellation_requested()) throw Cancelled();}
TaskScope::TaskScope(const agent_fs::path& state,const std::string& session,const std::string& mode):state_(state) {
    static std::once_flag installed;
    std::call_once(installed,[]{if(!SetConsoleCtrlHandler(console_control,TRUE)) throw std::runtime_error("Cannot install cancellation handler");});
    if(agent_fs::exists(state_/"active-task.json")) {
        const auto prior=Json::parse(read_bytes(state_/"active-task.json"));
        if(prior.value("status","")=="running") {
            HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,prior.value("pid",0UL));
            if(process) {
                const bool running=WaitForSingleObject(process,0)==WAIT_TIMEOUT;CloseHandle(process);
                if(running) throw std::runtime_error("This project already has an active task; cancel it or wait for completion");
            }
        }
    }
    const auto id=unique_id();signal_=state_/agent_fs::u8path("cancel-"+id+".signal");
    record_={{"id",id},{"session",session},{"mode",mode},{"status","running"},
        {"pid",GetCurrentProcessId()},{"started_at",timestamp()}};
    write_atomic(state_/"active-task.json",safe_json(record_,2));
    signal_path=signal_;cancelled=false;active=true;
}
TaskScope::~TaskScope() {if(!finished_) {try {finish(cancellation_requested()?"interrupted":"failed");} catch(...) {active=false;}}}
void TaskScope::finish(const std::string& status) {
    if(finished_) return;
    record_["status"]=status;record_["finished_at"]=timestamp();
    write_atomic(state_/"active-task.json",safe_json(record_,2));
    active=false;signal_path.clear();cancelled=false;finished_=true;
    std::error_code error;agent_fs::remove(signal_,error);
}
Json cancel_active_task(const agent_fs::path& state) {
    if(!agent_fs::exists(state/"active-task.json")) return {{"requested",false},{"reason","No active task"}};
    const auto task=Json::parse(read_bytes(state/"active-task.json"));
    if(task.value("status","")!="running") return {{"requested",false},{"reason","Task is idle"}};
    const auto id=task.at("id").get<std::string>();
    if(!std::regex_match(id,std::regex("[A-Za-z0-9_-]{1,80}"))) throw std::runtime_error("Invalid active task ID");
    write_atomic(state/agent_fs::u8path("cancel-"+id+".signal"),"cancel\n");
    return {{"requested",true},{"task",id}};
}
}
