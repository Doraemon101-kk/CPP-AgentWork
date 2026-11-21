#include "tools.hpp"
#include <iostream>
#include "agent.hpp"
#include "pilot/workbench.hpp"
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <vector>
#endif
int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    int wide_count = 0;
    auto wide_args = CommandLineToArgvW(GetCommandLineW(), &wide_count);
    if (!wide_args) return 2;
    std::vector<std::string> utf8_args;
    for (int i=0; i<wide_count; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8,0,wide_args[i],-1,nullptr,0,nullptr,nullptr);
        std::string value(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8,0,wide_args[i],-1,&value[0],size,nullptr,nullptr);
        value.pop_back(); utf8_args.push_back(std::move(value));
    }
    LocalFree(wide_args);
    std::vector<char*> utf8_pointers;
    for (auto& value : utf8_args) utf8_pointers.push_back(&value[0]);
    argc = wide_count; argv = utf8_pointers.data();
#endif
    if (argc >= 2 && std::string(argv[1]) == "--workbench") {
        try {
            if(argc<3) throw std::runtime_error("Usage: cpp_agent --workbench ROOT [--state DIR] [--model NAME] [--port N] [--resume ID] [--exec COMMAND]");
            agent_fs::path state=agent_fs::absolute(agent_fs::u8path(argv[0])).parent_path().parent_path()/".agent_data";
            std::string model,session,execute;unsigned short port=0;
            for(int i=3;i<argc;i+=2) {
                if(i+1>=argc) throw std::runtime_error("Missing option value");
                const std::string option=argv[i],value=argv[i+1];
                if(option=="--state") state=agent_fs::u8path(value);
                else if(option=="--model") model=value;
                else if(option=="--resume") session=value;
                else if(option=="--exec") execute=value;
                else if(option=="--port") {
                    const int number=std::stoi(value);
                    if(number<1 || number>65535) throw std::runtime_error("Invalid port");
                    port=static_cast<unsigned short>(number);
                } else throw std::runtime_error("Unknown option: "+option);
            }
            return pilot::run_workbench(agent_fs::u8path(argv[2]),state,model,port,session,execute);
        } catch(const std::exception& e) {std::cerr<<"Workbench error: "<<e.what()<<'\n';return 2;}
    }
    if (argc >= 2 && std::string(argv[1]) == "--agent") {
        if (argc < 4 || argc > 6) {
            std::cerr << "Usage: cpp_agent --agent ROOT QUESTION [MODEL] [PORT]\n";
            return 2;
        }
        try {
            const int port = argc == 6 ? std::stoi(argv[5]) : 11434;
            if (port < 1 || port > 65535) throw std::runtime_error("Invalid port");
            return run_agent(agent_fs::u8path(argv[2]), argv[3], argc >= 5 ? argv[4] : "qwen3:1.7b", static_cast<unsigned short>(port));
        } catch (const std::exception& e) { std::cerr << "Agent error: " << e.what() << '\n'; return 2; }
    }
    if (argc < 4 || argc > 5) {
        std::cout << "Usage: cpp_agent ROOT TOOL RELATIVE_PATH [KEYWORD]\n"
                     "Tools: list_directory, read_text, search_text\n"
                     "Agent: cpp_agent --agent ROOT QUESTION [MODEL] [PORT]\n";
        return 2;
    }
    ToolCall call{argv[2], argv[3], argc == 5 ? argv[4] : ""};
    const auto result = dispatch_tool(agent_fs::u8path(argv[1]), call);
    std::cout << "ok=" << result.ok << " truncated=" << result.truncated
              << " error=" << result.error_code << '\n' << result.content << '\n';
    return result.ok ? 0 : 2;
}
