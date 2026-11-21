#include "pilot/workbench.hpp"
#include <iostream>
#include <sstream>
#include <fstream>
#include <regex>
#include <iomanip>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace pilot {
namespace {
std::string key(const agent_fs::path& root) {
    auto path=root.generic_u8string();
    for(auto& ch:path) if(ch>='A' && ch<='Z') ch=static_cast<char>(ch-'A'+'a');
    unsigned long long hash=14695981039346656037ULL;
    for(const unsigned char ch:path) {hash^=ch;hash*=1099511628211ULL;}
    std::ostringstream out;out<<std::hex<<hash;return out.str();
}
std::string string_arg(const Json& args,const std::string& name,const std::string& fallback="") {
    if(!args.contains(name)) return fallback;
    if(!args.at(name).is_string()) throw std::runtime_error(name+" must be a string");
    return args.at(name).get<std::string>();
}
void display(const Json& data) {std::cout<<safe_json(data,2)<<"\n";}
void show_check(const Json& data) {
    std::cout<<"[check] "<<data.at("kind").get<std::string>()<<": "<<(data.value("ok",false)?"PASSED":"FAILED")<<"\n";
    for(const auto& run:data.at("runs")) {
        std::cout<<"  exit="<<run.at("exit_code")<<"  "<<run.at("duration_ms")<<" ms";
        if(run.value("timed_out",false)) std::cout<<"  [timeout]";
        if(run.value("truncated",false)) std::cout<<"  [output truncated]";
        std::cout<<"\n"<<run.value("output","")<<"\n";
    }
}
}
Workbench::Workbench(const agent_fs::path& root,const agent_fs::path& storage,
                     const std::string& model,unsigned short port):
    root_(agent_fs::canonical(root)),state_(agent_fs::absolute(storage)/"workspaces"/key(root_)),
    sessions_(state_/"sessions"),edits_(root_,state_/"changes") {
    checked_path(root_,".");
    agent_fs::create_directories(sessions_);
    if(agent_fs::exists(state_/"config.json")) config_=Json::parse(read_bytes(state_/"config.json"));
    else config_={{"version",2},{"model","qwen3:4b-instruct-2507-q4_K_M"},{"port",11434},{"max_rounds",12},{"max_tools",24},{"check_timeout",120}};
    if(config_.value("version",1)<2) {
        if(config_.value("model","")=="qwen3:1.7b") config_["model"]="qwen3:4b-instruct-2507-q4_K_M";
        config_["version"]=2;
    }
    if(!config_.contains("mode")) config_["mode"]="ask";
    if(!config_.contains("context_budget_tokens")) config_["context_budget_tokens"]=12000;
    mode_=config_.value("mode","ask");
    if(mode_!="ask" && mode_!="edit" && mode_!="check") throw std::runtime_error("Invalid task mode");
    if(config_.value("context_budget_tokens",0)<4000 || config_.value("context_budget_tokens",0)>14000) throw std::runtime_error("Invalid context budget");
    if(!model.empty()) config_["model"]=model;
    if(port) config_["port"]=port;
    if(config_.value("port",0)<1 || config_.value("port",0)>65535 ||
       config_.value("max_rounds",0)<1 || config_.value("max_rounds",0)>32 ||
       config_.value("max_tools",0)<1 || config_.value("max_tools",0)>128 ||
       config_.value("check_timeout",0)<1 || config_.value("check_timeout",0)>600)
        throw std::runtime_error("Invalid configuration limits");
    write_atomic(state_/"config.json",safe_json(config_,2));
    refresh_index();new_session();
}
void Workbench::refresh_index() {
    index_=scan_project(root_);write_atomic(state_/"index.json",safe_json(index_,2));
}
void Workbench::save_session() {
    session_["updated_at"]=timestamp();
    write_atomic(sessions_/agent_fs::u8path(session_.at("id").get<std::string>()+".json"),safe_json(session_,2));
}
void Workbench::new_session() {
    session_={{"version",1},{"id",unique_id()},{"root",root_.generic_u8string()},
        {"created_at",timestamp()},{"turns",Json::array()},{"events",Json::array()}};
    session_["mode"]=mode_;initialize_memory();read_files_.clear();save_session();
}
void Workbench::resume(const std::string& id) {
    if(!std::regex_match(id,std::regex("[A-Za-z0-9_-]{1,80}"))) throw std::runtime_error("Invalid session ID");
    auto loaded=Json::parse(read_bytes(sessions_/agent_fs::u8path(id+".json"),16*1024*1024));
    if(loaded.at("root")!=root_.generic_u8string()) throw std::runtime_error("Session belongs to a different root");
    session_=std::move(loaded);mode_=session_.value("mode","ask");initialize_memory();read_files_.clear();refresh_facts();
    for(auto& turn:session_["turns"]) if(turn.value("status","")=="running") turn["status"]="interrupted";
    event("resume",{{"session",id}});
}
Json Workbench::sessions() const {
    Json result=Json::array();
    for(const auto& entry:agent_fs::directory_iterator(sessions_)) {
        if(entry.path().extension()!=".json") continue;
        const auto data=Json::parse(read_bytes(entry.path(),16*1024*1024));
        if(data.at("turns").empty() && data.at("events").empty() && data.at("id")!=session_.at("id")) continue;
        result.push_back({{"id",data.at("id")},{"updated_at",data.value("updated_at","")},
            {"turns",data.at("turns").size()},{"current",data.at("id")==session_.at("id")}});
    }
    std::sort(result.begin(),result.end(),[](const Json& a,const Json& b){return a.at("id").get<std::string>()>b.at("id").get<std::string>();});
    return result;
}
void Workbench::event(const std::string& kind,const Json& data) {
    Json item={{"at",timestamp()},{"kind",kind},{"data",data}};
    session_["events"].push_back(item);
    // Bound live session metadata while retaining the append-only project event log.
    if(session_["events"].size()>150) session_["events"].erase(session_["events"].begin());
    std::ofstream stream(state_/"events.jsonl",std::ios::app|std::ios::binary);
    stream<<safe_json({{"session",session_.at("id")},{"event",item}})<<"\n";
    save_session();
}
Json Workbench::execute_tool(const std::string& name,const Json& args) {
    if(!args.is_object()) throw std::runtime_error("Tool arguments must be an object");
    check_cancelled();
    if((name=="propose_edit" || name=="propose_file") && active_mode_!="edit") throw std::runtime_error("Editing tools are disabled outside edit mode");
    if(name=="run_check" && active_mode_=="ask") throw std::runtime_error("Build/test tools are disabled in ask mode");
    const auto path=string_arg(args,"path",".");
    if(name=="list_directory" || name=="read_text" || name=="search_text") {
        if(name!="list_directory" || path!=".") checked_path(root_,path);
        ToolResult result;
        if(name=="list_directory") result=list_directory(root_,path);
        else if(name=="read_text") result=read_text(root_,path);
        else result=search_text(root_,path,string_arg(args,"keyword"));
        if(name=="read_text" && result.ok) {
            const auto normalized=checked_path(root_,path).lexically_relative(root_).generic_u8string();
            read_files_.insert(normalized);
            initialize_memory();session_["memory"]["reads"][normalized]={{"sha256",file_digest(checked_path(root_,path))},{"current",true},{"at",timestamp()}};save_session();
        }
        Json response={{"ok",result.ok},{"content",clip(result.content,20000)},
            {"error_code",result.error_code},{"truncated",result.truncated || result.content.size()>20000}};
        if(name=="read_text" && result.ok && !result.truncated)
            response["raw_content"]=clip(read_bytes(checked_path(root_,path),65536),16000);
        return response;
    }
    if(name=="project_map") {refresh_index();return ok({{"map",map_text(index_)},{"files",index_["files"].size()},{"truncated",index_["truncated"]}});}
    if(name=="find_symbols") return ok(query_index(index_,"symbols",string_arg(args,"query")));
    if(name=="find_todos") return ok(query_index(index_,"todos",string_arg(args,"query")));
    if(name=="propose_edit") {
        const auto normalized=checked_path(root_,path).lexically_relative(root_).generic_u8string();
        if(read_files_.count(normalized) && file_digest(checked_path(root_,path))!=session_["memory"]["reads"][normalized]["sha256"]) {
            session_["memory"]["reads"][normalized]["current"]=false;read_files_.erase(normalized);save_session();
            throw std::runtime_error("File changed since reading; read it again before editing");
        }
        if(!read_files_.count(normalized)) throw std::runtime_error("Read this file successfully with read_text before proposing an edit");
        try {
            auto result=edits_.propose(path,string_arg(args,"old_text"),string_arg(args,"new_text"));
            event("proposal",result);return ok(result);
        } catch(const std::exception& e) {
            Json failure=error("edit_match_error",e.what());
            Json examples=Json::array();
            std::istringstream source(read_bytes(checked_path(root_,path),256*1024));std::string line;
            while(std::getline(source,line) && examples.size()<25) {
                const auto first=line.find_first_not_of(" \t\r");
                if(first==std::string::npos) continue;
                line=line.substr(first);
                const auto comment=line.find("//");if(comment!=std::string::npos) line=line.substr(0,comment);
                const auto last=line.find_last_not_of(" \t\r");
                if(last!=std::string::npos) line.resize(last+1);
                if(line.size()>2) examples.push_back(line);
            }
            failure["valid_old_text_examples"]=examples;
            failure["next_action"]="Choose the smallest unique exact expression from these real source lines, not a reformatted whole function. Change your arguments before retrying.";
            return failure;
        }
    }
    if(name=="propose_file") {
        auto result=edits_.propose(path,"",string_arg(args,"content"),true);
        event("proposal",result);return ok(result);
    }
    if(name=="run_check") {
        const auto kind=string_arg(args,"kind","test");
        refresh_index();const auto before=source_revision();
        auto result=project_check(root_,state_/"build",kind,config_.value("check_timeout",120));
        refresh_index();const auto after=source_revision();
        initialize_memory();auto& checks=session_["memory"]["checks"];
        const auto& runs=result.at("runs");
        std::string status=result.value("ok",false)?"passed":"failed";
        for(const auto& run:runs) {if(run.value("cancelled",false)) status="interrupted";else if(run.value("timed_out",false)) status="timed_out";}
        checks.push_back({{"kind",kind},{"status",status},{"ok",result.value("ok",false)},{"at",timestamp()},
            {"exit_code",runs.empty()?-1:runs.back().value("exit_code",-1)},
            {"output",runs.empty()?"":clip(runs.back().value("output",""),3000)},
            {"revision",before},{"current",!before.empty() && before==after}});
        if(checks.size()>10) checks.erase(checks.begin());
        event("check",result);check_cancelled();return result;
    }
    throw std::runtime_error("Unknown tool: "+name);
}
std::string Workbench::report() {
    std::ostringstream out;
    out<<"# CppPilot project report\n\nRoot: `"<<root_.generic_u8string()<<"`\n\nSession: `"<<session_.at("id").get<std::string>()<<"`\n\nGenerated: "<<timestamp()<<"\n\n";
    out<<"## Project map\n\n```text\n"<<map_text(index_,16000)<<"\n```\n\n";
    out<<"## Task memory\n\n```json\n"<<safe_json(session_.value("memory",Json::object()),2)<<"\n```\n\n";
    out<<"## Conversation\n\n";
    for(const auto& turn:session_.at("turns")) {
        out<<"### User\n\n"<<turn.value("question","")<<"\n\n### Agent\n\n"<<turn.value("answer","")<<"\n\n";
        if(turn.value("status","")!="completed") out<<"Status: "<<turn.value("status","")<<"\n\n";
    }
    out<<"## Changes\n\n```json\n"<<safe_json(edits_.list(),2)<<"\n```\n\n";
    for(const auto& change:edits_.list())
        out<<"```diff\n"<<clip(edits_.review(change.at("id").get<std::string>()),12000)<<"\n```\n\n";
    out<<"## Checks\n\n";
    for(const auto& item:session_.at("events")) {
        if(item.at("kind")=="check") out<<"```json\n"<<safe_json(item.at("data"),2)<<"\n```\n\n";
    }
    const auto directory=state_/"reports";agent_fs::create_directories(directory);
    const auto file=directory/agent_fs::u8path(session_.at("id").get<std::string>()+".md");
    write_atomic(file,out.str());return file.u8string();
}
int Workbench::command(const std::string& input) {
    std::istringstream stream(input);std::string name;stream>>name;
    std::string rest;std::getline(stream,rest);const auto first=rest.find_first_not_of(" \t");rest=first==std::string::npos?"":rest.substr(first);
    if(name=="/quit" || name=="/exit") return -1;
    if(name=="/cancel") {display(cancel_active_task(state_));return 0;}
    if(name=="/mode") {
        if(rest.empty()) {std::cout<<"Mode: "<<mode_<<"\n";return 0;}
        if(rest!="ask" && rest!="edit" && rest!="check") throw std::runtime_error("Use /mode ask|edit|check");
        mode_=rest;config_["mode"]=rest;session_["mode"]=rest;write_atomic(state_/"config.json",safe_json(config_,2));save_session();return 0;
    }
    if(name=="/ask" || name=="/edit" || name=="/check") {
        if(rest.empty()) throw std::runtime_error("Provide a task after the mode command");
        return ask(rest,name.substr(1));
    }
    if(name=="/note") {
        initialize_memory();if(!rest.empty()) session_["memory"]["user_note"]=rest=="-"?"":rest;
        save_session();std::cout<<session_["memory"]["user_note"].get<std::string>()<<"\n";return 0;
    }
    if(name=="/memory") {refresh_index();refresh_facts();save_session();display(session_["memory"]);return 0;}
    if(name=="/compact") {
        TaskScope task(state_,session_["id"].get<std::string>(),"compact");
        try {compact_history(true);task.finish("completed");return 0;}
        catch(const Cancelled&) {task.finish("interrupted");std::cout<<"[cancelled] Memory compaction stopped\n";return 130;}
    }
    if(name=="/help") {
        std::cout<<
          "\n  CppPilot commands\n"
          "  /ask TASK /edit TASK /check TASK  one-turn task mode\n"
          "  /mode ask|edit|check  set default mode (default: ask)\n"
          "  /note TEXT /memory /compact  pin goal, inspect facts, compact history\n"
          "  Ctrl+C               cancel active model request or process\n"
          "  /cancel               request cancellation from a second instance\n"
          "  /map                  refresh project map\n"
          "  /symbols [query]      find function/type candidates\n"
          "  /todos [query]        find TODO/FIXME/placeholder candidates\n"
          "  /read path            read source with line numbers\n"
          "  /search keyword       search project text\n"
          "  /changes              list proposed/applied changes\n"
          "  /review ID            preview exact replacement\n"
          "  /apply ID             apply one reviewed change\n"
          "  /undo ID              restore an applied change\n"
          "  /discard ID           discard a pending proposal\n"
          "  /configure /build /test run CMake/CTest; results enter conversation\n"
          "  /sessions             list saved sessions\n"
          "  /resume ID /new       resume or start conversation\n"
          "  /history              recent questions and answers\n"
          "  /report               export Markdown report\n"
          "  /config /model NAME   inspect settings or choose model\n"
          "  /verbose              toggle full tool output\n"
          "  /quit                 save and exit\n"
          "  Any other text asks the Agent a question.\n\n";return 0;
    }
    if(name=="/map") {refresh_index();std::cout<<map_text(index_,20000)<<"\n";return 0;}
    if(name=="/symbols" || name=="/todos") {
        const auto found=query_index(index_,name=="/symbols"?"symbols":"todos",rest);
        for(const auto& item:found) std::cout<<item.at("path").get<std::string>()<<":"<<item.at("line")<<"  "
            <<(name=="/symbols"?item.at("name").get<std::string>():item.at("text").get<std::string>())<<"\n";
        if(found.empty()) std::cout<<"No matches.\n";
        return 0;
    }
    if(name=="/read" || name=="/search") {
        auto data=execute_tool(name=="/read"?"read_text":"search_text",name=="/read"?Json{{"path",rest}}:Json{{"path","."},{"keyword",rest}});
        if(data.value("ok",false)) {
            const auto content=data.value("content","");std::cout<<(content.empty()?"No content/matches.\n":content);
            if(data.value("truncated",false)) std::cout<<"[output truncated]\n";
        } else display(data);
        event("manual_tool",{{"name",name},{"result",data}});return data.value("ok",false)?0:2;
    }
    if(name=="/changes") {
        const auto changes=edits_.list();
        for(const auto& change:changes) std::cout<<change.at("id").get<std::string>()<<"  ["<<change.at("status").get<std::string>()<<"]  "<<change.at("path").get<std::string>()<<"\n";
        if(changes.empty()) std::cout<<"No changes proposed.\n";
        return 0;
    }
    if(name=="/review") {std::cout<<edits_.review(rest);return 0;}
    if(name=="/apply" || name=="/undo" || name=="/discard") {
        const auto data=name=="/apply"?edits_.apply(rest):name=="/undo"?edits_.undo(rest):edits_.discard(rest);
        event(name.substr(1),data);display(data);refresh_index();refresh_facts();save_session();return 0;
    }
    if(name=="/configure" || name=="/build" || name=="/test") {
        TaskScope task(state_,session_["id"].get<std::string>(),"check");active_mode_="check";
        try {
            const auto data=execute_tool("run_check",{{"kind",name.substr(1)}});show_check(data);
            task.finish(data.value("ok",false)?"completed":"failed");active_mode_.clear();return data.value("ok",false)?0:2;
        } catch(const Cancelled&) {task.finish("interrupted");active_mode_.clear();std::cout<<"[cancelled] Check process tree stopped\n";return 130;}
    }
    if(name=="/sessions") {display(sessions());return 0;}
    if(name=="/resume") {resume(rest);std::cout<<"Resumed "<<rest<<"\n";return 0;}
    if(name=="/new") {new_session();std::cout<<"New session "<<session_.at("id").get<std::string>()<<"\n";return 0;}
    if(name=="/history") {display(session_.at("turns"));return 0;}
    if(name=="/report") {std::cout<<"Report: "<<report()<<"\n";return 0;}
    if(name=="/config") {display(config_);std::cout<<"State: "<<state_.u8string()<<"\n";return 0;}
    if(name=="/model") {
        if(rest.empty()) throw std::runtime_error("Use /model NAME");
        config_["model"]=rest;write_atomic(state_/"config.json",safe_json(config_,2));std::cout<<"Model: "<<rest<<"\n";return 0;
    }
    if(name=="/verbose") {verbose_=!verbose_;std::cout<<"Verbose: "<<verbose_<<"\n";return 0;}
    if(!name.empty() && name[0]=='/') throw std::runtime_error("Unknown command; use /help");
    return ask(input);
}
int Workbench::repl() {
    std::cout<<"\n  +----------------------------------------------+\n"
                "  | CppPilot 0.3 / C++ project workbench        |\n"
                "  | inspect -> propose -> apply -> test -> undo |\n"
                "  +----------------------------------------------+\n"
             <<"  Project : "<<root_.u8string()<<"\n  Model   : "<<config_.at("model").get<std::string>()
             <<"\n  Session : "<<session_.at("id").get<std::string>()
             <<"\n  Mode    : "<<mode_<<"\n  Indexed : "<<index_["files"].size()<<" files / "<<index_["symbols"].size()<<" symbol candidates\n"
             <<"  /help for commands. Changes are staged until /apply ID.\n\n";
    for(std::string line;;) {
        std::cout<<"pilot> "<<std::flush;
        std::wstring wide;
        DWORD mode=0;
        if(GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),&mode)) {
            wchar_t buffer[4096];DWORD count=0;
            if(!ReadConsoleW(GetStdHandle(STD_INPUT_HANDLE),buffer,4095,&count,nullptr) || !count) break;
            wide.assign(buffer,count);
            while(!wide.empty() && (wide.back()==L'\r' || wide.back()==L'\n')) wide.pop_back();
            const int bytes=WideCharToMultiByte(CP_UTF8,0,wide.data(),static_cast<int>(wide.size()),nullptr,0,nullptr,nullptr);
            line.resize(bytes);if(bytes) WideCharToMultiByte(CP_UTF8,0,wide.data(),static_cast<int>(wide.size()),&line[0],bytes,nullptr,nullptr);
        } else if(!std::getline(std::cin,line)) break;
        if(!line.empty() && line.back()=='\r') line.pop_back();
        if(line.empty()) continue;
        try {if(command(line)<0) break;}
        catch(const std::exception& e) {std::cerr<<"[error] "<<e.what()<<"\n";}
    }
    save_session();std::cout<<"Session saved.\n";return 0;
}
int run_workbench(const agent_fs::path& root,const agent_fs::path& storage,const std::string& model,
                 unsigned short port,const std::string& session,const std::string& execute) {
    Workbench app(root,storage,model,port);
    if(!session.empty()) app.resume(session);
    if(!execute.empty()) return (std::max)(0,app.command(execute));
    return app.repl();
}
}
