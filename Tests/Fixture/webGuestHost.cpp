#include "Presence/webGuestRuntime.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>

// Integration-only host. Links the real guest runtime and response pipeline while
// replacing the desktop lifecycle with explicit stdin owner-busy events.
int main(int argc,char** argv) {
    try {
        if(argc!=3)return 2;
        const char* token=std::getenv("REVIA_WEB_LOCAL_TOKEN");if(!token)return 2;
        llmSettings model;model.port=std::stoi(argv[1]);model.modelName="local-model";
        if(const char* name=std::getenv("REVIA_WEB_MODEL_NAME"))model.modelName=name;
        if(const char* key=std::getenv("REVIA_WEB_MODEL_TOKEN"))model.apiKey=key;
        std::atomic<bool> busy=false;
        revia::presence::WebGuestRuntime runtime(model,[&]{return busy.load();});
        if(!runtime.Start(std::stoi(argv[2]),token,true))return 3;
        std::cout<<"{\"port\":"<<runtime.Port()<<"}"<<std::endl;
        std::string command;
        while(std::getline(std::cin,command)&&command!="quit"){
            if(command=="busy"){busy=true;runtime.Preempt();}
            else if(command=="ready")busy=false;
        }
        runtime.Stop();return 0;
    }catch(...){std::cerr<<"Guest test host failed.\n";return 1;}
}
