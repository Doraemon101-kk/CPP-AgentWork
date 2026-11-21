#pragma once
#include "pilot/index.hpp"
#include "pilot/edits.hpp"
#include "pilot/process.hpp"
#include "pilot/task.hpp"
#include <set>
namespace pilot {
class Workbench {
    agent_fs::path root_, state_, sessions_;
    Json config_, session_, index_;
    EditStore edits_;
    bool verbose_=false;
    std::string mode_="ask",active_mode_;
    std::size_t history_start_=0,history_end_=0;
    void initialize_memory();
    void refresh_facts();
    std::string source_revision() const;
    void compact_history(bool force=false);
    Json context_messages(const std::string& instructions,const std::string& question);
    void fit_context(Json& messages,const Json& schema);
    std::set<std::string> read_files_;
    void save_session();
    void refresh_index();
    void event(const std::string& kind,const Json& data);
    std::string report();
public:
    Workbench(const agent_fs::path& root,const agent_fs::path& state,
              const std::string& model,unsigned short port);
    void new_session();
    void resume(const std::string& id);
    Json sessions() const;
    Json execute_tool(const std::string& name,const Json& args);
    Json tool_schema(const std::string& mode) const;
    int ask(const std::string& question,const std::string& mode="");
    int command(const std::string& input);
    int repl();
};
int run_workbench(const agent_fs::path& root,const agent_fs::path& storage,
                  const std::string& model,unsigned short port,const std::string& session,
                  const std::string& execute);
}
