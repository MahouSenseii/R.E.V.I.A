#include "testSupport.h"
#include "Presence/webGuestRuntime.h"
#include "Core/messageRouter.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;
using revia::tests::Check;
using json = nlohmann::json;
namespace {
const std::string epoch = "10000000-0000-4000-8000-000000000001";
const std::string guest = "20000000-0000-4000-8000-000000000001";
const std::string other = "20000000-0000-4000-8000-000000000002";
const std::string token(48, 'z');
long long Now() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
json Turn(int id, const std::string& session = guest, const std::string& text = "Describe a maple leaf.") {
    char request[40]; std::snprintf(request, sizeof(request), "30000000-0000-4000-8000-%012d", id);
    return {{"version",1},{"epoch",epoch},{"sessionId",session},{"requestId",request},{"text",text},{"deadline",Now()+10000}};
}
json Post(int port, const std::string& path, const json& body, int status = 200) {
    httplib::Client client("127.0.0.1",port); client.set_read_timeout(15);
    auto result=client.Post(path,{{"Authorization","Bearer "+token}},body.dump(),"application/json");
    Check(result && result->status==status,"Unexpected native HTTP status for "+path);
    return json::parse(result->body);
}
}
int main() {
 try {
    revia::tests::ScopedTestDirectory temp;
    const auto previous=std::filesystem::current_path(); std::filesystem::current_path(temp.root);
    httplib::Server model; std::mutex mutex; std::vector<json> requests;
    std::atomic<bool> hold=false,entered=false,ownerBusy=false,holdModels=false;
    model.Get("/health",[](const auto&,auto& res){res.set_content(R"({"status":"ok","slots_idle":1})","application/json");});
    model.Get("/v1/models",[&](const auto&,auto& res){while(holdModels)std::this_thread::sleep_for(5ms);res.set_content(R"({"data":[{"id":"guest-fixture"}]})","application/json");});
    model.Post("/v1/chat/completions",[&](const auto& req,auto& res){
      auto body=json::parse(req.body); {std::lock_guard lock(mutex);requests.push_back(body);} entered=true;
      while(hold) std::this_thread::sleep_for(5ms);
      std::string reply="A maple leaf has a striking branching pattern.";
      if(req.body.find("PRIVACY_FILTER_PROBE")!=std::string::npos) reply="The file is C:/Users/secret/private.txt";
      const json chunk={{"choices",json::array({{{"delta",{{"content",reply}}},{"finish_reason","stop"}}})}};
      res.set_content("data: "+chunk.dump()+"\n\ndata: [DONE]\n\n","text/event-stream");
    });
    model.new_task_queue=[] {return new httplib::ThreadPool(4);};
    int modelPort=model.bind_to_any_port("127.0.0.1");std::jthread modelThread([&]{model.listen_after_bind();});
    struct ModelCleanup {httplib::Server& model;std::atomic<bool>& hold;std::jthread& thread;~ModelCleanup(){hold=false;model.stop();if(thread.joinable())thread.join();}} cleanup{model,hold,modelThread};
    while(!model.is_running()) std::this_thread::sleep_for(5ms);
    llmSettings settings;settings.port=modelPort;settings.modelName="guest-fixture";settings.bAutoMaxTokens=false;settings.maxTokens=384;
    // Real desktop-side model request on the same backend first. Its private
    // prompt/history/state are deliberately recognizable at the serialization boundary.
    messageRouter ownerRouter;aiProfile privateProfile;privateProfile.bMemoryEnabled=false;
    privateProfile.systemPrompt="OWNER_PROFILE_CANARY";
    ownerRouter.ApplyLLMSettings(settings,{},privateProfile);
    ownerRouter.SetPosture("MOOD_CAUSE_CANARY PREFERENCE_CANARY INTEREST_CANARY THOUGHT_CANARY SCREEN_CANARY CAMERA_CANARY CLIPBOARD_CANARY ARCHIVE_CANARY RELATIONSHIP_CANARY");
    auto ownerReply=ownerRouter.RouteMessage("OWNER_CHAT_CANARY",{{"user","OWNER_CHAT_CANARY"}}, {},{}, {},revia::llm::PrivateMemoryAccess::Denied);
    Check(ownerReply.bSuccess,"Owner canary request reached same model backend");
    {std::lock_guard lock(mutex);Check(requests.back().dump().find("MOOD_CAUSE_CANARY")!=std::string::npos,"Canary fixture is present in actual owner request");requests.clear();}
    revia::presence::WebGuestRuntime runtime(settings,[&]{return ownerBusy.load();});
    Check(runtime.Start(0,token,false),"Native listener must start");int port=runtime.Port();
    auto state=Post(port,"/web/v1/control",{{"enabled",true},{"paused",false}});
    Check(state["state"]=="online" || state["state"]=="starting","Explicit enable starts model readiness");
    httplib::Client client("127.0.0.1",port);
    for(int i=0;i<200;++i){auto r=client.Get("/web/v1/status",{{"Authorization","Bearer "+token}});if(r&&json::parse(r->body)["state"]=="online")break;std::this_thread::sleep_for(10ms);}
    Check(client.Get("/web/v1/status")->status==401,"Unauthenticated loopback access rejected");
    auto forged=Turn(1);forged["role"]="owner";Post(port,"/web/v1/turn",forged,400);
    Post(port,"/web/v1/turn",Turn(2,guest,std::string(2001,'x')),400);
    auto invalid=Turn(3);invalid["sessionId"]="../private";Post(port,"/web/v1/turn",invalid,400);
    Check(requests.empty(),"Invalid requests must not reach model");
    auto first=Post(port,"/web/v1/turn",Turn(4,guest,"GUEST_A_CANARY: Describe a maple leaf."));
    Check(first["state"]=="completed"&&!first.value("text","").empty(),"Guest receives final reply");
    auto second=Post(port,"/web/v1/turn",Turn(5,other));Check(second["state"]=="completed","Second guest replies");
    {std::lock_guard lock(mutex);auto serialized=requests.back().dump();Check(serialized.find("CANARY")==std::string::npos,"Private owner sources and other guest history isolated");Check(serialized.find("Revia")!=std::string::npos,"Public persona preserved");Check(!requests.back().contains("tools"),"No privileged tool schema");Check(requests.back().value("max_tokens",0)<=384,"Native generation budget bounded");}
    Post(port,"/web/v1/turn",Turn(5,other),409);
    auto filtered=Post(port,"/web/v1/turn",Turn(6,guest,"PRIVACY_FILTER_PROBE"));
    Check(filtered.value("text","").find("C:/Users/")==std::string::npos,"Final public privacy filter applied");
    holdModels=true;std::atomic<bool> discoverySettled=false;json discoveryResult;
    std::jthread healthThread([&]{discoveryResult=Post(port,"/web/v1/turn",Turn(16));discoverySettled=true;});
    std::this_thread::sleep_for(100ms);runtime.Preempt();
    for(int i=0;i<100&&!discoverySettled;++i)std::this_thread::sleep_for(10ms);
    const bool cancelledDiscovery=discoverySettled;
    holdModels=false;healthThread.join();
    Check(cancelledDiscovery&&discoveryResult["state"]=="cancelled","Cancellation must interrupt model discovery");
    hold=true;entered=false;auto pending=Turn(7,guest,"CANCELLED_GUEST_CANARY");json cancelled;
    std::jthread requestThread([&]{cancelled=Post(port,"/web/v1/turn",pending);});
    while(!entered)std::this_thread::sleep_for(5ms);
    auto cancel=pending;cancel.erase("text");cancel.erase("deadline");
    auto ack=Post(port,"/web/v1/cancel",cancel);Check(ack["state"]=="cancelled","Native acknowledges cancellation");
    hold=false;requestThread.join();Check(cancelled["state"]=="cancelled"&&!cancelled.contains("text"),"Cancelled output suppressed");
    Post(port,"/web/v1/turn",Turn(8));
    {std::lock_guard lock(mutex);Check(requests.back().dump().find("CANCELLED_GUEST_CANARY")==std::string::npos,"Cancelled input never retained");}
    ownerBusy=true;Post(port,"/web/v1/turn",Turn(9),409);ownerBusy=false;
    hold=true;entered=false;auto preemptedTurn=Turn(12);json preempted;
    std::jthread preemptedThread([&]{preempted=Post(port,"/web/v1/turn",preemptedTurn);});
    while(!entered)std::this_thread::sleep_for(5ms);
    const auto preemptStart=std::chrono::steady_clock::now();runtime.Preempt();
    preemptedThread.join();Check(std::chrono::steady_clock::now()-preemptStart<2s,"Owner preemption interrupts in-flight HTTP generation");
    hold=false;Check(preempted["state"]=="cancelled","Owner preemption suppresses reply");
    // A cancelled request arriving before its turn is accepted cannot be revived.
    auto early=Turn(13);auto earlyCancel=early;earlyCancel.erase("text");earlyCancel.erase("deadline");
    Post(port,"/web/v1/cancel",earlyCancel);Post(port,"/web/v1/turn",early,409);
    auto expired=Turn(14);expired["deadline"]=Now()-1;Post(port,"/web/v1/turn",expired,400);
    auto end=json{{"version",1},{"epoch",epoch},{"sessionId",guest}};Post(port,"/web/v1/end",end);
    Post(port,"/web/v1/turn",Turn(10),410);
    Post(port,"/web/v1/control",{{"enabled",true},{"paused",true}});
    Post(port,"/web/v1/turn",Turn(11,other),409);
    Post(port,"/web/v1/control",{{"enabled",true},{"paused",false}});
    for(int i=20;i<32;++i){
        char session[40];std::snprintf(session,sizeof(session),"20000000-0000-4000-8000-%012d",i);
        Check(Post(port,"/web/v1/turn",Turn(i,session))["state"]=="completed","Ended guests release live capacity");
        Post(port,"/web/v1/end",{{"version",1},{"epoch",epoch},{"sessionId",session}});
    }
    Check(Post(port,"/web/v1/control",{{"enabled",false},{"paused",false}})["state"]=="offline","Owner disable is distinct from pause and stops connector reconnect");
    runtime.Stop();model.stop();modelThread.join();
    std::filesystem::current_path(previous);
    Check(std::filesystem::is_empty(temp.root),"Guest path must not write transcripts or private memory to disk");
    std::cout<<"web guest isolation, auth, validation, filtering, cancellation, ownership and retention checks passed\n";
    return 0;
 } catch(const std::exception& ex){std::cerr<<ex.what()<<'\n';return 1;}
}
