#include "Presence/webGuestRuntime.h"
#include "Runtime/conversationRuntime.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <stop_token>
#include <thread>

namespace revia::presence {
namespace {
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
long long Now() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
bool Exact(const json& value, std::initializer_list<const char*> keys) {
    if(!value.is_object() || value.size()!=keys.size()) return false;
    return std::all_of(keys.begin(),keys.end(),[&](auto key){return value.contains(key);});
}
bool Id(const json& value) {
    if(!value.is_string()) return false;
    const auto& s=value.get_ref<const std::string&>();
    if(s.size()!=36) return false;
    for(std::size_t i=0;i<s.size();++i) {
        if(i==8||i==13||i==18||i==23) {if(s[i]!='-')return false;}
        else if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f'))) return false;
    }
    return s[14]=='4' && (s[19]=='8'||s[19]=='9'||s[19]=='a'||s[19]=='b');
}
bool Identity(const json& b, bool request) {
    return b.contains("version")&&b["version"].is_number_integer()&&b["version"]==1 &&
        b.contains("epoch")&&Id(b["epoch"])&&b.contains("sessionId")&&Id(b["sessionId"])&&
        (!request||(b.contains("requestId")&&Id(b["requestId"])));
}
bool Text(const json& value) {
    if(!value.is_string())return false;
    const auto& s=value.get_ref<const std::string&>();
    if(s.empty()||s.size()>8192||s.find('\0')!=std::string::npos)return false;
    // The JSON parser rejects malformed UTF-8. Count leading code-unit bytes only.
    std::size_t count=0;bool visible=false;
    for(unsigned char c:s){if((c&0xc0)!=0x80)++count;if(c>32)visible=true;}
    return count<=2000&&visible;
}
void Respond(httplib::Response& r,const json& b,int code=200){r.status=code;r.set_header("Cache-Control","no-store");r.set_content(b.dump(),"application/json");}
void Error(httplib::Response& r,const char* code,int status){Respond(r,{{"error",code}},status);}
}

struct WebGuestRuntime::Impl {
    struct Session {Clock::time_point created=Clock::now(),last=created; std::vector<conversationMessage> history; bool ended=false;};
    struct Work {std::string key,id;long long deadline;std::stop_source stop;};
    llmSettings model;
    std::function<bool()> ownerBusy;
    messageRouter router;
    httplib::Server server;
    std::jthread listener,maintenance,healthWorker;
    std::mutex mutex;
    std::map<std::string,Session> sessions;
    std::map<std::string,long long> retired;
    std::map<std::string,long long> seen;
    std::shared_ptr<Work> active;
    std::atomic<bool> enabled{false},paused{false},ready{false},checked{false};
    int port=0;std::string token;
    Impl(const llmSettings& settings,std::function<bool()> busy):model(settings),ownerBusy(std::move(busy)) {
        // Copy only model transport/inference configuration; never an owner profile.
        model.backend="LLamaCpp";model.bAutoStartServer=false;model.bVisionEnabled=false;
        model.bAutoMaxTokens=false;model.maxTokens=384;model.parallelRequests=1;
        model.contextSize=std::min(8192,model.contextSize);
        router.ApplyLLMSettings(model,{},runtime::ConversationRuntime::PublicGuestProfile());
    }
    bool Busy() const {return ownerBusy&&ownerBusy();}
    std::string State() {
        if(!enabled)return "offline";
        if(paused)return "paused";
        if(!checked)return "starting";
        if(!ready)return "unavailable";
        std::lock_guard lock(mutex);return Busy()||active?"busy":"online";
    }
    void Preempt() {std::lock_guard lock(mutex);if(active)active->stop.request_stop();}
    bool Authenticate(const httplib::Request& req,httplib::Response& res) {
        const auto auth=req.get_header_value("Authorization"),expected="Bearer "+token;
        unsigned int difference=static_cast<unsigned int>(auth.size()^expected.size());
        for(std::size_t i=0;i<expected.size();++i)difference|=static_cast<unsigned char>(expected[i])^(i<auth.size()?static_cast<unsigned char>(auth[i]):0);
        if(difference){Error(res,"unauthorized",401);return false;}
        // Browser origins are never legitimate on the native interface, including
        // same-machine websites; only the connector and owner's local controls use it.
        if(req.has_header("Origin")){Error(res,"forbidden",403);return false;}
        return true;
    }
    void Prune() {
        const auto now=Clock::now();
        for(auto it=sessions.begin();it!=sessions.end();) {
            if(now-it->second.created>30min || now-it->second.last>15min){
                if(active&&active->key==it->first)active->stop.request_stop();
                retired[it->first]=Now()+1800000;
                it=sessions.erase(it);
            } else ++it;
        }
        std::erase_if(seen,[](const auto& p){return p.second<Now();});
        std::erase_if(retired,[](const auto& p){return p.second<Now();});
    }
    void Turn(const json& b,httplib::Response& res) {
        if(!Exact(b,{"version","epoch","sessionId","requestId","text","deadline"})||!Identity(b,true)||!Text(b["text"])||!b["deadline"].is_number_integer()) {Error(res,"invalid_request",400);return;}
        const long long requestedDeadline=b["deadline"].get<long long>();
        if(requestedDeadline<=Now()||requestedDeadline>Now()+125000){Error(res,"invalid_deadline",400);return;}
        const long long deadline=std::min(requestedDeadline,Now()+120000);
        const std::string key=b["epoch"].get<std::string>()+":"+b["sessionId"].get<std::string>(),id=b["requestId"];
        std::shared_ptr<Work> work;std::vector<conversationMessage> history;
        {
            std::lock_guard lock(mutex);Prune();
            if(!enabled||paused||!ready||Busy()||active){Error(res,"busy",409);return;}
            if(seen.contains(id)){Error(res,"replayed",409);return;}
            if(seen.size()>=512){Error(res,"capacity",429);return;}
            auto found=sessions.find(key);
            if(retired.contains(key)){Error(res,"session_ended",410);return;}
            if(found==sessions.end()&&(sessions.size()>=10||sessions.size()+retired.size()>=512)){Error(res,"capacity",429);return;}
            auto& session=sessions[key];session.last=Clock::now();history=session.history;
            work=std::make_shared<Work>();work->key=key;work->id=id;work->deadline=deadline;active=work;seen[id]=requestedDeadline;
        }
        runtime::SessionResult reply;reply.succeeded=false;
        try {reply=runtime::ConversationRuntime::ReplyPublic(router,b["text"],history,work->stop.get_token());}
        catch(...) { /* Diagnostics never cross this boundary or contain transcripts. */ }
        std::lock_guard lock(mutex);
        const bool expired=Now()>=deadline;
        auto found=sessions.find(key);
        const bool cancelled=work->stop.stop_requested()||!enabled||paused||Busy()||found==sessions.end()||found->second.ended;
        if(active==work)active.reset();
        if(expired||cancelled){Respond(res,{{"requestId",id},{"state",expired?"expired":"cancelled"}});return;}
        if(!reply.succeeded){Respond(res,{{"requestId",id},{"state","failed"}});return;}
        auto& session=found->second;session.last=Clock::now();
        session.history.push_back({"user",b["text"]});session.history.push_back({"assistant",reply.text});
        // <= 8 messages AND 32 KiB per guest, <= 320 KiB total retained history.
        const auto bytes=[&]{std::size_t n=0;for(auto& m:session.history)n+=m.content.size();return n;};
        while(session.history.size()>8||bytes()>32768)session.history.erase(session.history.begin(),session.history.begin()+2);
        Respond(res,{{"requestId",id},{"state","completed"},{"text",reply.text}});
    }
    void Routes() {
        server.set_payload_max_length(16384);server.set_read_timeout(2);server.set_write_timeout(2);
        server.set_keep_alive_max_count(4);
        server.new_task_queue=[] {return new httplib::ThreadPool(4,16);};
        server.Get("/web/v1/status",[this](const auto& req,auto& res){if(Authenticate(req,res))Respond(res,{{"state",State()}});});
        auto post=[this](const std::string& path,auto handler){server.Post(path,[this,handler](const auto& req,auto& res){
            if(!Authenticate(req,res))return;
            if(req.get_header_value("Content-Type").find("application/json")!=0){Error(res,"invalid_request",400);return;}
            try {const auto b=json::parse(req.body);handler(b,res);}catch(...){Error(res,"invalid_request",400);}
        });};
        post("/web/v1/turn",[this](const json& b,auto& res){Turn(b,res);});
        post("/web/v1/cancel",[this](const json& b,auto& res){
            if(!Exact(b,{"version","epoch","sessionId","requestId"})||!Identity(b,true)){Error(res,"invalid_request",400);return;}
            const std::string key=b["epoch"].get<std::string>()+":"+b["sessionId"].get<std::string>();
            std::lock_guard lock(mutex);
            if(active&&active->key==key&&active->id==b["requestId"].get<std::string>())active->stop.request_stop();
            if(seen.size()<512||seen.contains(b["requestId"].get<std::string>()))seen[b["requestId"].get<std::string>()]=Now()+125000;
            else {Error(res,"capacity",429);return;}
            Respond(res,{{"requestId",b["requestId"]},{"state","cancelled"}});
        });
        post("/web/v1/end",[this](const json& b,auto& res){
            if(!Exact(b,{"version","epoch","sessionId"})||!Identity(b,false)){Error(res,"invalid_request",400);return;}
            const std::string key=b["epoch"].get<std::string>()+":"+b["sessionId"].get<std::string>();
            std::lock_guard lock(mutex);Prune();
            if(active&&active->key==key)active->stop.request_stop();
            if(!sessions.contains(key)&&!retired.contains(key)&&sessions.size()+retired.size()>=512){Error(res,"capacity",429);return;}
            sessions.erase(key);retired[key]=Now()+1800000;
            Respond(res,{{"state","ended"}});
        });
        post("/web/v1/control",[this](const json& b,auto& res){
            if(!Exact(b,{"enabled","paused"})||!b["enabled"].is_boolean()||!b["paused"].is_boolean()){Error(res,"invalid_request",400);return;}
            enabled=b["enabled"].get<bool>();paused=b["paused"].get<bool>();
            if(!enabled||paused){std::lock_guard lock(mutex);if(active)active->stop.request_stop();for(auto& [key,s]:sessions)retired[key]=Now()+1800000;sessions.clear();}
            Respond(res,{{"state",State()}});
        });
        server.set_exception_handler([](const auto&,auto& res,std::exception_ptr){Error(res,"unavailable",503);});
    }
};

WebGuestRuntime::WebGuestRuntime(const llmSettings& m,std::function<bool()> busy):impl(std::make_unique<Impl>(m,std::move(busy))){}
WebGuestRuntime::~WebGuestRuntime(){Stop();}
bool WebGuestRuntime::Start(int requestedPort,const std::string& localToken,bool enabled){
    if(impl->listener.joinable()||requestedPort<0||requestedPort>65535||localToken.size()<32||localToken.size()>256||
        std::any_of(localToken.begin(),localToken.end(),[](unsigned char c){return c<33||c>126;})||
        (impl->model.host!="127.0.0.1"&&impl->model.host!="localhost"&&impl->model.host!="::1"))return false;
    impl->token=localToken;impl->enabled=enabled;impl->Routes();
    impl->port=requestedPort? (impl->server.bind_to_port("127.0.0.1",requestedPort)?requestedPort:0):impl->server.bind_to_any_port("127.0.0.1");
    if(impl->port<=0)return false;
    impl->listener=std::jthread([this]{impl->server.listen_after_bind();});
    impl->maintenance=std::jthread([this](std::stop_token stop){
        while(!stop.stop_requested()){
            {std::lock_guard lock(impl->mutex);impl->Prune();if(impl->active&&(Now()>=impl->active->deadline||impl->Busy()))impl->active->stop.request_stop();}
            std::this_thread::sleep_for(20ms);
        }
    });
    // Model discovery can wait on an unhealthy server. It must never delay the
    // independent deadline sweep or owner-priority cancellation.
    impl->healthWorker=std::jthread([this](std::stop_token stop){
        auto nextHealth=Clock::now();
        while(!stop.stop_requested()){
            if(Clock::now()>=nextHealth){
                impl->ready=impl->router.CheckLLMHealth(stop).bIsAvailable;impl->checked=true;
                nextHealth=Clock::now()+2s;
            }
            std::this_thread::sleep_for(20ms);
        }
    });
    return true;
}
void WebGuestRuntime::Preempt(){impl->Preempt();}
int WebGuestRuntime::Port()const{return impl->port;}
void WebGuestRuntime::Stop(){
    impl->enabled=false;impl->Preempt();
    if(impl->healthWorker.joinable()){impl->healthWorker.request_stop();impl->healthWorker.join();}
    if(impl->maintenance.joinable()){impl->maintenance.request_stop();impl->maintenance.join();}
    impl->server.stop();if(impl->listener.joinable())impl->listener.join();
    std::lock_guard lock(impl->mutex);impl->sessions.clear();impl->seen.clear();impl->retired.clear();impl->token.clear();
}
}
