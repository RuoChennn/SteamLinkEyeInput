#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include "../src/gaze.h"
#include "../src/marker.h"
#include "../src/vendor/openvr_driver.h"
#pragma comment(lib,"ws2_32.lib")
void Check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
std::vector<uint8_t> Message(const std::string& name,float value) {
    std::vector<uint8_t> bytes;
    auto string=[&](const std::string& s){bytes.insert(bytes.end(),s.begin(),s.end());bytes.push_back(0);while(bytes.size()%4)bytes.push_back(0);};
    string(name.empty()||name[0]!='/'?"/eyetracking/"+name:name);string(",f");uint32_t bits;memcpy(&bits,&value,4);
    for(int i=24;i>=0;i-=8)bytes.push_back(uint8_t(bits>>i));return bytes;
}
struct Context:vr::IVRDriverContext {
    void* GetGenericInterface(const char*,vr::EVRInitError* error=nullptr)override{if(error)*error=vr::VRInitError_Init_InterfaceNotFound;return nullptr;}
    vr::DriverHandle_t GetDriverHandle()override{return 1;}
};
int wmain(int argc,wchar_t** argv) {
    try {
        eye::MarkerOverride marker;uint8_t cross=0;
        Check(marker.Set(cross,false)&&cross==0,"marker default off leaves native state untouched");
        Check(marker.Set(cross,true)&&cross==1,"marker on");
        Check(marker.Set(cross,true)&&cross==1,"repeated marker on preserves original");
        Check(marker.Set(cross,false)&&cross==0,"marker off restores original");
        cross=1;Check(marker.Set(cross,true)&&marker.Set(cross,false)&&cross==1,"native preexisting marker preserved");
        cross=7;Check(!marker.Set(cross,true)&&cross==7,"unknown native flag rejected");
        marker={};cross=0;Check(marker.Set(cross,false)&&cross==0,"reconnect resets override ownership");
        eye::Tracker tracker;eye::Config config;eye::Point point;std::vector<eye::Message> parsed;
        for(int i=0;i<6;++i){auto bytes=Message(eye::Names[i],i<4?.5f:1.f);parsed.clear();Check(eye::Parse(bytes.data(),bytes.size(),parsed),"OSC float decode");Check(parsed.size()==1&&parsed[0].index==i,"OSC parameter identity");tracker.Accept(parsed,1000,config,point);}
        Check(point.x>0&&point.y>0&&point.z==-2&&point.valid,"No-flip gaze direction");
        auto positive=point;config.flipX=config.flipY=true;
        Check(tracker.Accept({{0,.5f},{1,.5f},{2,.5f},{3,.5f}},1010,config,point),"complete frame");
        Check(fabs(point.x+positive.x)<.0001f&&fabs(point.y+positive.y)<.0001f,"axis inversion");
        config.flipX=config.flipY=false;
        tracker.Accept({{0,0},{1,0},{2,0},{3,0}},1020,config,point);
        Check(fabs(point.x)<.0001f&&fabs(point.y)<.0001f,"upstream zero maps to center");
        tracker.Accept({{0,.5f},{1,.5f},{2,.5f},{3,.5f}},1021,config,point);
        Check(fabs(point.x-positive.x)<.0001f&&fabs(point.y-positive.y)<.0001f,"source output unchanged by previous samples");
        tracker.Accept({{0,.5f},{1,.5f},{2,.5f},{3,.5f},{4,0},{5,0}},1030,config,point);Check(!point.valid,"closed eyes invalid");
        tracker.Accept({{0,.5f},{1,.5f},{2,.5f}},1040,config,point);
        Check(!tracker.Accept({{3,.5f}},1300,config,point),"stale mixed frame rejected");
        auto nan=Message("EyeLeftX",NAN);parsed.clear();Check(!eye::Parse(nan.data(),nan.size(),parsed),"NaN rejected");
        auto bytes=Message("EyeLeftX",.2f);bytes.pop_back();Check(!eye::Parse(bytes.data(),bytes.size(),parsed),"truncation rejected");
        std::vector<uint8_t> bundle={'#','b','u','n','d','l','e',0,0,0,0,0,0,0,0,1};
        bytes=Message("EyeLeftX",.2f);uint32_t count=uint32_t(bytes.size());for(int i=24;i>=0;i-=8)bundle.push_back(uint8_t(count>>i));bundle.insert(bundle.end(),bytes.begin(),bytes.end());parsed.clear();
        Check(eye::Parse(bundle.data(),bundle.size(),parsed)&&parsed.size()==1,"OSC bundle");
        auto mapping=eye::DefaultAddresses();mapping[0]="/other-device/gaze/left-horizontal";
        bytes=Message(mapping[0],.25f);parsed.clear();
        Check(eye::Parse(bytes.data(),bytes.size(),parsed,mapping)&&parsed.size()==1&&parsed[0].index==0,"custom device address mapping");
        parsed.clear();Check(eye::Parse(bytes.data(),bytes.size(),parsed)&&parsed.empty(),"unmapped device address ignored");
        config.offsetYaw=2;config.offsetPitch=-3;
        tracker.Accept({{0,0},{1,0},{2,0},{3,0}},1900,config,point);
        Check(fabs(point.x-2*std::tan(2*3.14159265/180))<.0001&&point.y<0,"manual trim shifts upstream center");
        config.flipX=config.flipY=true;
        tracker.Accept({{0,0},{1,0},{2,0},{3,0}},1910,config,point);
        Check(point.x>0&&point.y<0,"manual trim directions independent of flips");
        config.offsetYaw=config.offsetPitch=0;
        tracker.Accept({{0,0},{1,0},{2,0},{3,0}},1920,config,point);
        Check(fabs(point.x)<.0001f&&fabs(point.y)<.0001f,"reset manual trim restores upstream center");
        config.flipX=config.flipY=false;config.offsetYaw=config.offsetPitch=20;
        config.yaw=config.pitch=90;
        tracker.Accept({{0,1},{1,1},{2,1},{3,1},{4,1},{5,1}},2000,config,point);
        Check(std::isfinite(point.x)&&std::isfinite(point.y)&&point.x>0&&point.y>0,"90 degree settings remain finite without flipping sign");
        if(argc>1) {
            HMODULE dll=LoadLibraryW(argv[1]);Check(dll!=nullptr,"load provider DLL");
            auto factory=reinterpret_cast<void*(*)(const char*,int*)>(GetProcAddress(dll,"HmdDriverFactory"));Check(factory!=nullptr,"factory export");
            int error=0;Check(!factory("missing",&error)&&error==vr::VRInitError_Init_InterfaceNotFound,"interface rejection");
            auto provider=static_cast<vr::IServerTrackedDeviceProvider*>(factory(vr::IServerTrackedDeviceProvider_Version,&error));Check(provider&&error==0,"provider interface");
            Context context;
            WSADATA data;Check(!WSAStartup(MAKEWORD(2,2),&data),"test sockets");
            for(int session=0;session<2;++session) {
                Check(provider->Init(&context)==vr::VRInitError_None,"provider init");
                for(int frame=0;frame<25;++frame){provider->RunFrame();Sleep(10);}
                SOCKET socket=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);sockaddr_in address{};
                address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(19100);
                for(int i=0;i<6;++i){auto packet=Message(eye::Names[i],i<4?.25f:1.f);Check(sendto(socket,reinterpret_cast<const char*>(packet.data()),int(packet.size()),0,reinterpret_cast<sockaddr*>(&address),sizeof(address))==int(packet.size()),"UDP send");}
                closesocket(socket);Sleep(100);
                provider->Cleanup();
                auto statusPath=std::filesystem::path(argv[1]).parent_path().parent_path().parent_path()/L"status.json";
                std::ifstream status(statusPath);std::string text((std::istreambuf_iterator<char>(status)),{});
                Check(text.find("\"packets\": 6")!=std::string::npos,"real UDP datagrams received inside DLL");
                Check(text.find("\"socket_error\": 0")!=std::string::npos,"UDP receiver bound");
                Check(text.find("\"running\": false")!=std::string::npos,"worker stopped before unload");
            }
            WSACleanup();
            Check(FreeLibrary(dll)!=0,"clean unload");
        }
        std::cout<<"PASS: OSC parsing, upstream calibration and manual trim, direction/closure/freshness, real UDP reception, provider lifecycle and unload\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
