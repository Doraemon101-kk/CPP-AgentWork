#include "pilot/process.hpp"
#include "pilot/task.hpp"
#include "pilot/build_config.hpp"
#include <chrono>
#include <thread>
#include <stdexcept>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace pilot {
namespace {
struct WinHandle {
    HANDLE value=nullptr;
    WinHandle()=default;
    explicit WinHandle(HANDLE v):value(v) {}
    ~WinHandle() { if(value && value!=INVALID_HANDLE_VALUE) CloseHandle(value); }
    WinHandle(const WinHandle&)=delete;
    void close() {if(value && value!=INVALID_HANDLE_VALUE) CloseHandle(value);value=nullptr;}
};
std::wstring quote(const std::wstring& arg) {
    std::wstring result=L"\"";std::size_t slashes=0;
    for(const auto ch:arg) {
        if(ch==L'\\') {++slashes;continue;}
        if(ch==L'"') {result.append(slashes*2+1,L'\\');result+=ch;}
        else {result.append(slashes,L'\\');result+=ch;}
        slashes=0;
    }
    result.append(slashes*2,L'\\');result+=L'"';return result;
}
}
Json run_process(const std::vector<std::string>& args,const agent_fs::path& cwd,int timeout_seconds) {
    check_cancelled();
    if(args.empty()) throw std::runtime_error("Empty process command");
    std::wstring command;
    for(const auto& arg:args) {if(!command.empty()) command+=L' ';command+=quote(agent_fs::u8path(arg).wstring());}
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
    WinHandle output_read,output_write;
    if(!CreatePipe(&output_read.value,&output_write.value,&security,0)) throw std::runtime_error("CreatePipe failed");
    if(!SetHandleInformation(output_read.value,HANDLE_FLAG_INHERIT,0)) throw std::runtime_error("Pipe configuration failed");
    WinHandle input(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr));
    WinHandle job(CreateJobObjectW(nullptr,nullptr));
    if(!job.value) throw std::runtime_error("CreateJobObject failed");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))
        throw std::runtime_error("Job configuration failed");
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdOutput=output_write.value;startup.hStdError=output_write.value;startup.hStdInput=input.value;
    PROCESS_INFORMATION process{};
    const auto began=std::chrono::steady_clock::now();
    if(!CreateProcessW(nullptr,&command[0],nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED,
        nullptr,cwd.c_str(),&startup,&process))
        throw std::runtime_error("Cannot start process; Win32 error "+std::to_string(GetLastError()));
    WinHandle child(process.hProcess),thread(process.hThread);
    if(!AssignProcessToJobObject(job.value,child.value)) {
        TerminateProcess(child.value,1);throw std::runtime_error("Cannot assign process tree to job");
    }
    if(ResumeThread(thread.value)==static_cast<DWORD>(-1)) throw std::runtime_error("Cannot resume process");
    output_write.close();input.close();thread.close();
    std::string output;bool truncated=false,timed_out=false,cancelled=false;
    constexpr std::size_t max_output=64*1024;
    auto drain=[&] {
        for(;;) {
            DWORD available=0;
            if(!PeekNamedPipe(output_read.value,nullptr,0,nullptr,&available,nullptr) || !available) break;
            char buffer[4096];DWORD count=0;
            if(!ReadFile(output_read.value,buffer,(std::min)(available,static_cast<DWORD>(sizeof(buffer))),&count,nullptr)) break;
            const auto remaining=max_output-output.size();
            output.append(buffer,(std::min)(remaining,static_cast<std::size_t>(count)));
            if(count>remaining) truncated=true;
        }
    };
    for(;;) {
        drain();
        const auto wait=WaitForSingleObject(child.value,20);
        if(wait==WAIT_OBJECT_0) break;
        if(wait==WAIT_FAILED) throw std::runtime_error("Process wait failed");
        if(cancellation_requested()) {cancelled=true;TerminateJobObject(job.value,130);WaitForSingleObject(child.value,5000);break;}
        if(std::chrono::steady_clock::now()-began>std::chrono::seconds(timeout_seconds)) {
            timed_out=true;TerminateJobObject(job.value,124);WaitForSingleObject(child.value,5000);break;
        }
    }
    // Closing the job also kills any child processes left behind by the parent.
    job.close();drain();
    DWORD code=1;GetExitCodeProcess(child.value,&code);
    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-began).count();
    return {{"command",args},{"exit_code",cancelled?130:timed_out?124:static_cast<int>(code)},
        {"cancelled",cancelled},{"timed_out",timed_out},{"duration_ms",ms},{"output",output},{"truncated",truncated}};
}
Json project_check(const agent_fs::path& root,const agent_fs::path& build,const std::string& kind,int timeout_seconds) {
    if(kind!="configure" && kind!="build" && kind!="test") throw std::runtime_error("Unknown check kind");
    if(!agent_fs::exists(checked_path(root,"CMakeLists.txt"))) throw std::runtime_error("Project has no CMakeLists.txt");
    const std::string cmake=build_config::cmake;
    const std::string ctest=build_config::ctest;
    Json runs=Json::array();
    auto run=[&](const std::vector<std::string>& command) {
        auto result=run_process(command,root,timeout_seconds);runs.push_back(result);
        return result.at("exit_code").get<int>()==0;
    };
    if(kind=="configure" || !agent_fs::exists(build/"CMakeCache.txt")) {
        std::vector<std::string> configure={cmake,"-S",root.u8string(),"-B",build.u8string(),
            "-G",build_config::generator,"-DCMAKE_BUILD_TYPE=" CPPPILOT_CONFIGURATION};
        const std::string generator=build_config::generator;
        if(generator.rfind("Visual Studio",0)==0) {
            if(std::string(build_config::platform).size()) {configure.push_back("-A");configure.push_back(build_config::platform);}
            if(std::string(build_config::toolset).size()) {configure.push_back("-T");configure.push_back(build_config::toolset);}
        } else {
            configure.push_back(std::string("-DCMAKE_CXX_COMPILER=")+build_config::compiler);
            if(std::string(build_config::make_program).size()) configure.push_back(std::string("-DCMAKE_MAKE_PROGRAM=")+build_config::make_program);
        }
        const bool passed=run(configure);
        if(!passed || kind=="configure") return {{"ok",passed},{"kind",kind},{"runs",runs}};
    }
    if(!run({cmake,"--build",build.u8string(),"--parallel","2","--config",CPPPILOT_CONFIGURATION})) return {{"ok",false},{"kind",kind},{"runs",runs}};
    if(kind=="build") return {{"ok",true},{"kind",kind},{"runs",runs}};
    // ctest returns success when no tests exist unless explicitly told otherwise.
    const bool passed=run({ctest,"--test-dir",build.u8string(),"--output-on-failure","--no-tests=error","-C",CPPPILOT_CONFIGURATION});
    return {{"ok",passed},{"kind",kind},{"runs",runs}};
}
}
