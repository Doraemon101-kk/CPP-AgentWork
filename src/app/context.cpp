#include "pilot/workbench.hpp"
#include "pilot/model.hpp"
#include <iostream>
namespace pilot {
void Workbench::initialize_memory() {
    if(!session_.contains("memory")) session_["memory"]={{"summary",""},{"summary_method","none"},{"compacted_through",0},{"user_note",""},{"reads",Json::object()},{"checks",Json::array()}};
}
std::string Workbench::source_revision() const {
    if(index_.value("truncated",false)) return "";
    std::string revision;
    try {
        for(const auto& file:index_.at("files")) {
            const auto path=file.at("path").get<std::string>();
            if(file.at("bytes").get<unsigned long long>()>1024*1024) return "";
            revision+=path+":"+file_digest(checked_path(root_,path))+"\n";
        }
    } catch(...) {return "";}
    return revision;
}
void Workbench::refresh_facts() {
    initialize_memory();auto& memory=session_["memory"];
    for(auto it=memory["reads"].begin();it!=memory["reads"].end();++it) {
        try {it.value()["current"]=file_digest(checked_path(root_,it.key()))==it.value().at("sha256");}
        catch(...) {it.value()["current"]=false;}
    }
    const auto revision=source_revision();
    for(auto& check:memory["checks"]) check["current"]=!revision.empty() && check.value("revision","")==revision;
}
void Workbench::compact_history(bool force) {
    initialize_memory();auto& memory=session_["memory"];
    std::vector<std::size_t> completed;
    for(std::size_t i=0;i<session_["turns"].size();++i) if(session_["turns"][i].value("status","")=="completed") completed.push_back(i);
    const std::size_t end=completed.size()>4?completed[completed.size()-4]:0;
    const auto begin=memory.value("compacted_through",std::size_t(0));
    if(end<=begin) return;
    (void)force;
    std::string source="Previous recap:\n"+memory.value("summary","")+"\nOlder turns:\n";
    for(std::size_t i=begin;i<end;++i) {
        const auto& turn=session_["turns"][i];
        source+="User: "+clip(turn.value("question",""),1400)+"\nAgent ("+turn.value("status","")+"): "+clip(turn.value("answer",""),1800)+"\n";
    }
    const auto fallback=clip(source,4000);
    try {
        check_cancelled();
        std::cout<<"[memory] compacting older conversation..."<<std::endl;
        const auto result=model_chat({{"model",config_.at("model")},{"stream",false},{"think",false},
            {"messages",Json::array({{{"role","system"},{"content","Summarize prior conversation only. Preserve user goals, constraints, decisions, unresolved tasks. Do not invent tool results or claim old tests still pass. Return a concise recap under 1200 characters."}},{{"role","user"},{"content",clip(source,16000)}}})},
            {"options",{{"temperature",0},{"num_predict",800}}}},static_cast<unsigned short>(config_.at("port").get<int>()));
        auto summary=result.at("message").value("content",std::string{});
        if(summary.empty()) throw std::runtime_error("Empty summary");
        memory["summary"]=clip(summary,4000);memory["summary_method"]="model_unverified";
    } catch(const Cancelled&) {throw;}
    catch(const std::exception&) {memory["summary"]=fallback;memory["summary_method"]="extractive_fallback";}
    memory["compacted_through"]=end;save_session();
}
Json Workbench::context_messages(const std::string& instructions,const std::string& question) {
    refresh_facts();compact_history();
    const auto& memory=session_.at("memory");
    Json messages=Json::array({{{"role","system"},{"content",instructions+"\n"+map_text(index_,4500)}}});
    if(!memory.value("summary","").empty()) messages.push_back({{"role","user"},{"content","Earlier conversation recap (unverified, never proof of current source/test state):\n"+memory.at("summary").get<std::string>()}});
    if(!memory.value("user_note","").empty()) messages.push_back({{"role","user"},{"content","Pinned user task note (current request takes precedence):\n"+memory.at("user_note").get<std::string>()}});
    Json facts={{"reads",memory.at("reads")},{"checks",Json::array()}};
    for(const auto& check:memory.at("checks")) {
        auto fact=check;fact.erase("revision");facts["checks"].push_back(fact);
    }
    messages.push_back({{"role","user"},{"content","Program facts: current=false means historical/stale; do not report it as current verification. Checks cover indexed source files only.\n"+clip(safe_json(facts),6000)}});
    const auto& turns=session_.at("turns");
    std::vector<std::size_t> completed;
    for(std::size_t i=0;i<turns.size();++i) if(turns[i].value("status","")=="completed") completed.push_back(i);
    const auto recent_start=completed.size()>4?completed[completed.size()-4]:0;
    const auto start=(std::max)(memory.value("compacted_through",std::size_t(0)),recent_start);
    history_start_=messages.size();
    for(std::size_t i=start;i<turns.size();++i) {
        const auto& turn=turns[i];if(turn.value("status","")!="completed") continue;
        messages.push_back({{"role","user"},{"content",clip(turn.value("question",""),1500)}});
        messages.push_back({{"role","assistant"},{"content",clip(turn.value("answer",""),3000)}});
    }
    history_end_=messages.size();
    messages.push_back({{"role","user"},{"content",question}});return messages;
}
void Workbench::fit_context(Json& messages,const Json& schema) {
    const auto limit=static_cast<std::size_t>(config_.value("context_budget_tokens",12000))*2;
    auto size=[&]{return safe_json(messages).size()+safe_json(schema).size();};
    if(size()<=limit) return;
    // Preserve syntactically complete tool records while shortening bulky text fields.
    for(auto& message:messages) if(message.value("role","")=="tool") {
        try {
            auto data=Json::parse(message.at("content").get<std::string>());
            for(const auto& key:{"content","raw_content"}) if(data.contains(key) && data[key].is_string()) data[key]=clip(data[key].get<std::string>(),1400);
            if(data.contains("runs")) for(auto& run:data["runs"]) run["output"]=clip(run.value("output",""),800);
            if(data.contains("data") && data["data"].is_object() && data["data"].contains("map")) data["data"]["map"]=clip(data["data"]["map"].get<std::string>(),1200);
            data["context_shortened"]=true;message["content"]=safe_json(data);
        } catch(...) {}
    }
    while(size()>limit && history_end_>=history_start_+2) {
        messages.erase(messages.begin()+static_cast<Json::difference_type>(history_start_),messages.begin()+static_cast<Json::difference_type>(history_start_+2));
        history_end_-=2;
    }
    // Evict complete old assistant/tool groups together; never leave orphan tool messages.
    while(size()>limit) {
        bool removed=false;
        for(std::size_t i=1;i<messages.size();++i) {
            if(messages[i].value("role","")!="assistant" || !messages[i].contains("tool_calls")) continue;
            auto end=i+1;while(end<messages.size() && messages[end].value("role","")=="tool") ++end;
            if(end>=messages.size()) continue;
            messages.erase(messages.begin()+static_cast<Json::difference_type>(i),messages.begin()+static_cast<Json::difference_type>(end));
            messages.insert(messages.begin()+static_cast<Json::difference_type>(i),Json{{"role","user"},{"content","Earlier tool group omitted for context budget. Consult persisted facts or re-read relevant files."}});
            removed=true;break;
        }
        if(!removed) break;
    }
    if(size()>limit) throw std::runtime_error("Context budget exceeded; shorten the question or pinned note, or start a new task");
}
}
