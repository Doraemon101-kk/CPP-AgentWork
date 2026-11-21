#include "pilot/model.hpp"
#include "pilot/task.hpp"
#include <atomic>
#include <map>
#include <memory>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
using Json=nlohmann::json;
namespace {
struct Handle {
    HINTERNET value;
    explicit Handle(HINTERNET h):value(h) {if(!value) throw std::runtime_error("HTTP initialization failed");}
    ~Handle(){if(value) WinHttpCloseHandle(value);}
    Handle(const Handle&)=delete;
};
struct RequestState {
    HANDLE ready=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    HANDLE closing=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::atomic<DWORD> status{0},count{0},error{0};
    char buffer[8192];
    ~RequestState(){if(ready)CloseHandle(ready);if(closing)CloseHandle(closing);}
};
void CALLBACK status_callback(HINTERNET,DWORD_PTR context,DWORD status,void* data,DWORD bytes) {
    auto* state=reinterpret_cast<RequestState*>(context);
    if(!state) return;
    if(status==WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING){SetEvent(state->closing);return;}
    if(status==WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) state->error=static_cast<WINHTTP_ASYNC_RESULT*>(data)->dwError;
    else if(status==WINHTTP_CALLBACK_STATUS_READ_COMPLETE) state->count=bytes;
    else if(status==WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE) state->count=*static_cast<DWORD*>(data);
    else if(status!=WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE && status!=WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE) return;
    state->status=status;SetEvent(state->ready);
}
struct Request {
    HINTERNET value;
    std::unique_ptr<RequestState> state=std::make_unique<RequestState>();
    explicit Request(HINTERNET handle):value(handle) {
        if(!value || !state->ready || !state->closing) throw std::runtime_error("HTTP request initialization failed");
    }
    ~Request(){
        if(value && WinHttpCloseHandle(value)) {
            // Buffers and callback context must outlive HANDLE_CLOSING, including cancellation.
            if(WaitForSingleObject(state->closing,10000)!=WAIT_OBJECT_0) state.release();
        }
    }
    void wait(BOOL started,DWORD expected) {
        if(!started && GetLastError()!=ERROR_IO_PENDING) throw std::runtime_error("HTTP operation failed: "+std::to_string(GetLastError()));
        for(;;) {
            pilot::check_cancelled();
            const auto result=WaitForSingleObject(state->ready,25);
            if(result==WAIT_FAILED) throw std::runtime_error("HTTP wait failed");
            if(result==WAIT_OBJECT_0) break;
        }
        if(state->status==WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) throw std::runtime_error("HTTP request failed: "+std::to_string(state->error.load()));
        if(state->status!=expected) throw std::runtime_error("Unexpected HTTP completion");
    }
};
class Stream {
    std::string pending_,content_,thinking_;
    std::map<int,Json> calls_;
    Json metadata_=Json::object();
    ModelDelta callback_;
    bool done_=false;
    std::size_t bytes_=0;
    void record(const Json& chunk) {
        if(chunk.contains("error")) throw std::runtime_error("Ollama: "+chunk.at("error").get<std::string>());
        if(done_) throw std::runtime_error("Data received after stream completion");
        if(chunk.contains("message")) {
            const auto& message=chunk.at("message");
            const auto text=message.value("content",std::string{});content_+=text;
            thinking_+=message.value("thinking",std::string{});
            if(callback_ && !text.empty()) callback_(text);
            if(message.contains("tool_calls")) {
                if(!message.at("tool_calls").is_array()) throw std::runtime_error("Streamed tool_calls must be an array");
                int position=0;
                for(const auto& call:message.at("tool_calls")) {
                    const auto& function=call.at("function");
                    const int index=function.value("index",call.value("index",position++));
                    auto& destination=calls_[index];
                    if(destination.is_null()) destination={{"function",Json::object()}};
                    if(call.contains("id")) destination["id"]=call["id"];
                    const auto name=function.value("name",std::string{});
                    if(!name.empty()) {
                        auto& current=destination["function"]["name"];
                        if(current.is_null()) current=name;
                        else if(current!=name) current=current.get<std::string>()+name;
                    }
                    if(function.contains("arguments")) {
                        const auto& args=function.at("arguments");auto& current=destination["function"]["arguments"];
                        if(args.is_string()) {
                            if(current.is_null()) current="";
                            if(!current.is_string()) throw std::runtime_error("Mixed streamed argument formats");
                            current=current.get<std::string>()+args.get<std::string>();
                        } else if(args.is_object()) {
                            if(current.is_null()) current=Json::object();
                            if(!current.is_object()) throw std::runtime_error("Mixed streamed argument formats");
                            current.update(args);
                        } else throw std::runtime_error("Invalid streamed arguments");
                    }
                }
            }
        }
        for(const auto& key:{"prompt_eval_count","eval_count","total_duration"}) if(chunk.contains(key)) metadata_[key]=chunk[key];
        done_=chunk.value("done",false);
    }
public:
    explicit Stream(ModelDelta callback):callback_(std::move(callback)){}
    void feed(const char* data,std::size_t count) {
        bytes_+=count;if(bytes_>4*1024*1024) throw std::runtime_error("Model stream exceeds limit");
        pending_.append(data,count);
        for(;;) {
            const auto end=pending_.find('\n');if(end==std::string::npos) break;
            auto line=pending_.substr(0,end);pending_.erase(0,end+1);
            if(!line.empty() && line.back()=='\r') line.pop_back();
            if(!line.empty()) record(Json::parse(line));
        }
    }
    Json finish(bool strict) {
        if(!pending_.empty()) record(Json::parse(pending_));
        if(strict && !done_) throw std::runtime_error("Model stream ended without done=true; partial tools were not executed");
        Json calls=Json::array();
        for(auto& item:calls_) {
            auto& function=item.second.at("function");
            if(!function.contains("name")) throw std::runtime_error("Incomplete streamed tool name");
            if(!function.contains("arguments")) function["arguments"]=Json::object();
            if(function["arguments"].is_string()) function["arguments"]=Json::parse(function["arguments"].get<std::string>());
            calls.push_back(item.second);
        }
        Json message={{"role","assistant"},{"content",content_}};
        if(!thinking_.empty()) message["thinking"]=thinking_;
        if(!calls.empty()) message["tool_calls"]=calls;
        metadata_["message"]=message;return metadata_;
    }
};
}
Json model_chat(const Json& request,unsigned short port,ModelDelta on_text) {
    pilot::check_cancelled();
    if(port==0) throw std::runtime_error("Invalid model server port");
    const auto body=request.dump();
    Handle session(WinHttpOpen(L"CppPilot/0.2",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,WINHTTP_FLAG_ASYNC));
    WinHttpSetTimeouts(session.value,5000,5000,300000,300000);
    Handle connection(WinHttpConnect(session.value,L"127.0.0.1",port,0));
    Request call(WinHttpOpenRequest(connection.value,L"POST",L"/api/chat",nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,0));
    DWORD_PTR context=reinterpret_cast<DWORD_PTR>(call.state.get());
    if(!WinHttpSetOption(call.value,WINHTTP_OPTION_CONTEXT_VALUE,&context,sizeof(context)) ||
       WinHttpSetStatusCallback(call.value,status_callback,WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS|WINHTTP_CALLBACK_FLAG_HANDLES,0)==WINHTTP_INVALID_STATUS_CALLBACK)
        throw std::runtime_error("Cannot configure asynchronous HTTP callbacks");
    call.wait(WinHttpSendRequest(call.value,L"Content-Type: application/json\r\n",-1L,
        const_cast<char*>(body.data()),static_cast<DWORD>(body.size()),static_cast<DWORD>(body.size()),context),WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE);
    call.wait(WinHttpReceiveResponse(call.value,nullptr),WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE);
    DWORD status=0,length=sizeof(status);
    if(!WinHttpQueryHeaders(call.value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&length,WINHTTP_NO_HEADER_INDEX))
        throw std::runtime_error("Cannot read HTTP status");
    wchar_t content_type[256]{};length=sizeof(content_type);
    WinHttpQueryHeaders(call.value,WINHTTP_QUERY_CONTENT_TYPE,WINHTTP_HEADER_NAME_BY_INDEX,content_type,&length,WINHTTP_NO_HEADER_INDEX);
    const bool streaming=request.value("stream",false);
    Stream stream(std::move(on_text));std::string response;
    for(;;) {
        call.wait(WinHttpQueryDataAvailable(call.value,nullptr),WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE);
        const auto available=call.state->count.load();if(!available) break;
        call.wait(WinHttpReadData(call.value,call.state->buffer,(std::min)(available,static_cast<DWORD>(sizeof(call.state->buffer))),nullptr),WINHTTP_CALLBACK_STATUS_READ_COMPLETE);
        const auto count=call.state->count.load();if(!count) break;
        if(streaming && status==200) stream.feed(call.state->buffer,count);
        else {response.append(call.state->buffer,count);if(response.size()>4*1024*1024) throw std::runtime_error("HTTP response too large");}
    }
    pilot::check_cancelled();
    if(status!=200) throw std::runtime_error("Ollama HTTP "+std::to_string(status)+": "+response.substr(0,800));
    return streaming?stream.finish(std::wstring(content_type).find(L"ndjson")!=std::wstring::npos):Json::parse(response);
}
