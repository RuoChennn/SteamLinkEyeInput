// Source-independent OSC eye input provider; no external bridge or Valve file patch.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <fstream>
#include <sstream>
#include <filesystem>
#include "vendor/openvr_driver.h"
#include "gaze.h"
#include "marker.h"
#pragma comment(lib,"ws2_32.lib")
#pragma comment(lib,"bcrypt.lib")

namespace {
HMODULE selfModule;
constexpr uintptr_t ProviderRva=0xd24ad0,HmdVtableRva=0xc38790;
const char* ExpectedHash="8e7100b6b1e88d3ad6dd05f30f030dc9a69b092b64a60d35347cebb5c516b55d";
std::filesystem::path Root() {
    wchar_t path[32768]{};GetModuleFileNameW(selfModule,path,32768);
    return std::filesystem::path(path).parent_path().parent_path().parent_path();
}
bool Verify(HMODULE module) {
    wchar_t path[32768]{};if(!GetModuleFileNameW(module,path,32768))return false;
    std::ifstream file(std::filesystem::path(path),std::ios::binary);if(!file)return false;
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    std::vector<uint8_t> object;bool ok=false;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return false;
    DWORD length=0,n=0;
    if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&length),4,&n,0)>=0) {
        object.resize(length);
        if(BCryptCreateHash(algorithm,&hash,object.data(),length,nullptr,0,0)>=0) {
            std::array<char,65536> buffer;bool readOk=true;
            while(file) {
                file.read(buffer.data(),buffer.size());auto count=file.gcount();
                if(count && BCryptHashData(hash,reinterpret_cast<PUCHAR>(buffer.data()),ULONG(count),0)<0){readOk=false;break;}
            }
            std::array<uint8_t,32> digest{};
            if(readOk && file.eof() && BCryptFinishHash(hash,digest.data(),32,0)>=0) {
                std::string hex;const char* digits="0123456789abcdef";
                for(auto b:digest){hex+=digits[b>>4];hex+=digits[b&15];}
                ok=hex==ExpectedHash;
            }
        }
    }
    if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);
    return ok;
}
// Only called on the server's RunFrame thread. The current HMD is retrieved every
// call from Valve's provider, never retained over disconnects or reconnects.
void* CurrentHmd(uint8_t* base) {
    __try {
        auto provider=base+ProviderRva;
        auto hmd=*reinterpret_cast<uint8_t**>(provider+0x390);
        if(!hmd || *reinterpret_cast<uintptr_t*>(hmd)!=uintptr_t(base)+HmdVtableRva ||
           *reinterpret_cast<uint32_t*>(hmd+0x68)!=4 ||
           *reinterpret_cast<void**>(hmd+0x950)!=provider ||
           !*reinterpret_cast<uint64_t*>(hmd+0x958))return nullptr;
        return hmd;
    } __except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
}
int ShowOptions(uint8_t* base) {
    __try {
        if(base[0x9efb10]!=0x4c || base[0xb90f20]!=0x48)return -1;
        // Defer until Valve's provider has been initialized by vrserver.
        if(!*reinterpret_cast<void**>(base+0xd247d0))return -2;
        auto paths=reinterpret_cast<void*(*)()>(base+0xb90f20)();
        if(!paths)return -3;
        auto path=reinterpret_cast<uint64_t(*)(void*)>(base+0x9e46b0)(base+0xd16a10);
        return reinterpret_cast<int(*)(void*,uint64_t,uint64_t,bool,void*)>(base+0x9efb10)
            (paths,0x600000000ULL,path,true,nullptr);
    } __except(EXCEPTION_EXECUTE_HANDLER){return -4;}
}
bool Apply(uint8_t* base,void* hmd,const eye::Point& point) {
    __try {
        alignas(8) uint8_t packet[0x140]{};
        *reinterpret_cast<uint32_t*>(packet)=0x1103;
        *reinterpret_cast<uint16_t*>(packet+4)=0x11;
        memcpy(packet+8,&point.x,4);memcpy(packet+12,&point.y,4);memcpy(packet+16,&point.z,4);
        packet[0x15]=point.valid?1:0;
        reinterpret_cast<void(*)(void*,const void*)>(base+0x9e9880)(hmd,packet);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
// Eye Cross is encoded by Valve as sideband flag 0x100, alongside crop metadata.
// Only used after the existing exact-binary hash and current HMD checks.
bool SetCross(void* hmd,eye::MarkerOverride& marker,bool show) {
    __try {return marker.Set(*(static_cast<uint8_t*>(hmd)+0x103),show);}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
uint64_t Session(void* hmd) {
    __try {return hmd?*reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(hmd)+0x958):0;}
    __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
class Provider:public vr::IServerTrackedDeviceProvider {
    vr::IVRDriverLog* log=nullptr;vr::IVRServerDriverHost* host=nullptr;
    std::atomic<bool> stop{false};std::thread worker;
    std::atomic<uint8_t*> verified{nullptr};HMODULE pinned=nullptr;
    std::atomic<bool> shown{false},connected{false},enabled{true},fault{false};
    std::atomic<uint64_t> accepted{0},packets{0},bad{0};
    std::atomic<bool> markerDesired{false},markerActive{false};
    eye::MarkerOverride marker;uint64_t lastSession=0;
    std::atomic<int> socketError{0};std::mutex lock;eye::Point latest;
    uint64_t lastSequence=0,lastShow=0;void* lastHmd=nullptr;bool active=false;
    std::filesystem::path root;
    uint64_t started=0;bool dashboardRequested=false;
    void Log(const char* text){if(log)log->Log(text);}
    eye::Config ReadConfig(const eye::Config& previous) {
        auto result=previous;auto path=root/L"steamlinkeyeinput.ini";
        // A denied/temporarily locked file preserves the last valid calibration.
        std::ifstream stream(path);if(!stream)return previous;
        std::string line;bool section=false;
        try {
            while(std::getline(stream,line)) {
                if(!line.empty()&&line.back()=='\r')line.pop_back();
                if(line.empty()||line[0]==';'||line[0]=='#')continue;
                if(line[0]=='['){section=line=="[steamlinkeyeinput]";continue;}
                if(!section)continue;
                auto equal=line.find('=');if(equal==std::string::npos)continue;
                auto key=line.substr(0,equal),value=line.substr(equal+1);
                // Obsolete neutral offsets must not affect upstream calibration.
                bool legacyCenter=false;
                for(int i=0;i<4;++i)if(key==eye::Names[i])legacyCenter=true;
                if(legacyCenter)continue;
                if(key.rfind("address_",0)==0) {
                    if(value.empty()||value[0]!='/'||value.size()>256||value.find_first_of(" \t\r\n")!=std::string::npos)return previous;
                    for(int i=0;i<6;++i)if(key==std::string("address_")+eye::Names[i])result.addresses[i]=value;
                    continue;
                }
                float number=std::stof(value);if(!std::isfinite(number))return previous;
                if(key=="enabled"){if(number!=0&&number!=1)return previous;result.enabled=number!=0;}
                else if(key=="show_gaze_center"){if(number!=0&&number!=1)return previous;result.showGazeCenter=number!=0;}
                else if(key=="flip_x"){if(number!=0&&number!=1)return previous;result.flipX=number!=0;}
                else if(key=="flip_y"){if(number!=0&&number!=1)return previous;result.flipY=number!=0;}
                else if(key=="port"){if(number<1024||number>65535||floorf(number)!=number)return previous;result.port=int(number);}
                else if(key=="yaw"){if(number<=0||number>90)return previous;result.yaw=number;}
                else if(key=="pitch"){if(number<=0||number>90)return previous;result.pitch=number;}
                else if(key=="depth"){if(number<.1f||number>5)return previous;result.depth=number;}
            }
        } catch(...){return previous;}
        if(stream.bad())return previous;
        for(int i=0;i<6;++i)for(int j=i+1;j<6;++j)if(result.addresses[i]==result.addresses[j])return previous;
        return result;
    }
    void Status(const eye::Config& config,const eye::Point& point,bool done=false) {
        auto path=root/L"status.json",temp=root/L"status.tmp";
        std::ofstream out(temp,std::ios::trunc);if(!out)return;
        out<<"{\n  \"running\": "<<(!done?"true":"false")<<",\n  \"pid\": "<<GetCurrentProcessId()
           <<",\n  \"port\": "<<config.port<<",\n  \"socket_error\": "<<socketError.load()
           <<",\n  \"compatible_driver\": "<<(verified.load()?"true":"false")
           <<",\n  \"osc_options_shown\": "<<(shown.load()?"true":"false")
           <<",\n  \"headset_connected\": "<<(connected.load()?"true":"false")
           <<",\n  \"enabled\": "<<(config.enabled?"true":"false")
           <<",\n  \"show_gaze_center\": "<<(markerActive.load()?"true":"false")
           <<",\n  \"fault\": "<<(fault.load()?"true":"false")
           <<",\n  \"packets\": "<<packets.load()<<",\n  \"rejected_packets\": "<<bad.load()
           <<",\n  \"accepted_points\": "<<accepted.load()
           <<",\n  \"data_age_ms\": "<<(point.time?int64_t(GetTickCount64()-point.time):-1)
           <<",\n  \"point\": ["<<point.x<<","<<point.y<<","<<point.z<<"],\n  \"flip_x\": "<<(config.flipX?"true":"false")
           <<",\n  \"flip_y\": "<<(config.flipY?"true":"false")<<"\n}\n";
        out.close();if(!out)return;
        // Telemetry failure must not stall eye tracking or the SteamVR frame loop.
        MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING);
    }
    void Work() {
        WSADATA data{};int startup=WSAStartup(MAKEWORD(2,2),&data);
        if(startup){socketError=startup;return;}
        SOCKET socket=INVALID_SOCKET;int boundPort=0;uint64_t lastConfig=0,lastVerify=0,lastStatus=0,lastBind=0;
        eye::Config config;eye::Tracker tracker;eye::Point point;
        bool checked=false;
        while(!stop) {
            auto now=GetTickCount64();
            if(now-lastConfig>=500 || !lastConfig){config=ReadConfig(config);enabled=config.enabled;markerDesired=config.showGazeCenter;lastConfig=now;}
            if(!checked && now-lastVerify>=1000) {
                HMODULE module=nullptr;
                if(GetModuleHandleExW(0,L"driver_vrlink.dll",&module)) {
                    checked=true;
                    if(Verify(module)){pinned=module;verified=reinterpret_cast<uint8_t*>(module);}
                    else {FreeLibrary(module);Log("SteamLinkEyeInput: unsupported Steam Link binary; private calls disabled.");}
                }
                lastVerify=now;
            }
            if(socket!=INVALID_SOCKET && boundPort!=config.port){closesocket(socket);socket=INVALID_SOCKET;tracker={};point={};}
            if(socket==INVALID_SOCKET && now-lastBind>=1000) {
                lastBind=now;socket=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
                int exclusive=1;setsockopt(socket,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<char*>(&exclusive),4);
                sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(u_short(config.port));
                u_long nonblocking=1;
                if(socket==INVALID_SOCKET || bind(socket,reinterpret_cast<sockaddr*>(&address),sizeof(address)) || ioctlsocket(socket,FIONBIO,&nonblocking)) {
                    socketError=WSAGetLastError();if(socket!=INVALID_SOCKET)closesocket(socket);socket=INVALID_SOCKET;
                } else {socketError=0;boundPort=config.port;Log("SteamLinkEyeInput: OSC receiver bound to 127.0.0.1.");}
            }
            bool changed=false;
            if(socket!=INVALID_SOCKET)for(int i=0;i<256;++i) {
                uint8_t bytes[8192];int size=recv(socket,reinterpret_cast<char*>(bytes),sizeof(bytes),0);
                if(size==SOCKET_ERROR){int error=WSAGetLastError();if(error!=WSAEWOULDBLOCK){socketError=error;if(error==WSAEMSGSIZE)++bad;}break;}
                ++packets;std::vector<eye::Message> messages;
                if(!eye::Parse(bytes,size,messages,config.addresses)){++bad;continue;}
                if(tracker.Accept(messages,GetTickCount64(),config,point))changed=true;
            }
            if(changed){std::lock_guard<std::mutex> guard(lock);latest=point;}
            if(now-lastStatus>=500){Status(config,point);lastStatus=now;}
            Sleep(2);
        }
        if(socket!=INVALID_SOCKET)closesocket(socket);WSACleanup();Status(config,point,true);
    }
public:
    vr::EVRInitError Init(vr::IVRDriverContext* context) override {
        if(!context)return vr::VRInitError_Init_InvalidInterface;
        log=static_cast<vr::IVRDriverLog*>(context->GetGenericInterface(vr::IVRDriverLog_Version));
        host=static_cast<vr::IVRServerDriverHost*>(context->GetGenericInterface(vr::IVRServerDriverHost_Version));
        root=Root();stop=false;fault=false;shown=false;connected=false;verified=nullptr;socketError=0;enabled=true;started=GetTickCount64();dashboardRequested=false;
        marker={};lastSession=0;markerDesired=false;markerActive=false;
        lastSequence=lastShow=0;lastHmd=nullptr;active=false;accepted=0;packets=0;bad=0;latest={};
        try{worker=std::thread([this]{try{Work();}catch(...){fault=true;Log("SteamLinkEyeInput: worker stopped after exception.");}});}
        catch(...){return vr::VRInitError_Driver_Failed;}
        Log("SteamLinkEyeInput 1.3.0: provider loaded; upstream calibration only.");
        return vr::VRInitError_None;
    }
    void Cleanup() override {
        stop=true;if(worker.joinable())worker.join();
        // No native call on teardown: Valve may already have destroyed its HMD.
        // The native encoder's own 250 ms timestamp timeout expires old gaze.
        verified=nullptr;if(pinned){FreeLibrary(pinned);pinned=nullptr;}
        connected=false;shown=false;Log("SteamLinkEyeInput: provider stopped.");log=nullptr;host=nullptr;
    }
    const char* const* GetInterfaceVersions() override{return vr::k_InterfaceVersions;}
    void RunFrame() override {
        if(!dashboardRequested && GetTickCount64()-started>3000){
            dashboardRequested=true;
            auto appRoot=root.parent_path();auto executable=appRoot/L"SteamLinkEyeInputControl.exe";
            if(GetFileAttributesW((appRoot/L"dashboard-registered").c_str())==INVALID_FILE_ATTRIBUTES && GetFileAttributesW(executable.c_str())!=INVALID_FILE_ATTRIBUTES){
                std::wstring command=L"\""+executable.wstring()+L"\" --vr --tray";STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION child{};
                if(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,appRoot.c_str(),&startup,&child)){CloseHandle(child.hThread);CloseHandle(child.hProcess);}
            }
        }
        auto base=verified.load();if(!base||fault)return;
        auto now=GetTickCount64();
        if(!shown && now-lastShow>=1000){lastShow=now;if(ShowOptions(base)==0){shown=true;Log("SteamLinkEyeInput: OSC options exposed.");}}
        void* hmd=CurrentHmd(base);
        if(host) {vr::TrackedDevicePose_t pose{};host->GetRawTrackedDevicePoses(0,&pose,1);if(!pose.bDeviceIsConnected)hmd=nullptr;}
        connected=hmd!=nullptr;
        auto session=Session(hmd);
        if(hmd!=lastHmd || session!=lastSession){lastHmd=hmd;lastSession=session;lastSequence=0;active=false;marker={};markerActive=false;}
        if(!hmd)return;
        if(!SetCross(hmd,marker,markerDesired && enabled)){fault=true;return;}
        markerActive=markerDesired && enabled;
        eye::Point point;
        {std::unique_lock<std::mutex> guard(lock,std::try_to_lock);if(!guard.owns_lock())return;point=latest;}
        now=GetTickCount64();
        bool fresh=enabled && point.time && now-point.time<=150;
        if(!fresh){if(active){point.valid=false;if(!Apply(base,hmd,point))fault=true;active=false;}return;}
        if(point.sequence==lastSequence)return;
        lastSequence=point.sequence;
        if(Apply(base,hmd,point)){++accepted;active=point.valid;}
        else {fault=true;Log("SteamLinkEyeInput: native eye handler fault; stopped for this session.");}
    }
    bool ShouldBlockStandbyMode() override{return false;}
    void EnterStandby() override{}
    void LeaveStandby() override{}
} provider;
}
extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* name,int* error) {
    if(name && !strcmp(name,vr::IServerTrackedDeviceProvider_Version)){if(error)*error=0;return &provider;}
    if(error)*error=vr::VRInitError_Init_InterfaceNotFound;return nullptr;
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH){selfModule=module;DisableThreadLibraryCalls(module);}return TRUE;}
