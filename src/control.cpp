#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <filesystem>
#include <string>
#include <sstream>
#include <regex>
#include <map>
#include <cmath>
#include <iomanip>
#include <fstream>
#include <vector>
#include <algorithm>
#include <d3d11.h>
#include <dxgi.h>
#include "vendor/openvr.h"
#include "center_dot.h"
#pragma comment(lib,"comctl32.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"dxgi.lib")
namespace fs=std::filesystem;
static fs::path root;
static HWND window,statusLabel,detailLabel,yawSlider,pitchSlider,yawLabel,pitchLabel,enabledBox,flipXBox,flipYBox,markerBox,offsetLabel,stepButton,offsetXEdit,offsetYEdit,diagnosticLabel;
static double offsetYaw=0,offsetPitch=0,trimStep=.1;
static constexpr int PanelHeight=790;
static bool directionView=false,buildingDirectionControls=false;
static std::vector<HWND> normalControls,directionControls;
static HWND directionLabels[4]{},directionStepButton;
static std::array<double,4> directionGains={1,1,1,1};
static int directionStep=1;
static constexpr const char* GainKeys[]={"gain_left","gain_right","gain_up","gain_down"};
static constexpr const wchar_t* GainNames[]={L"左侧",L"右侧",L"上方",L"下方"};
static HFONT font,titleFont;static bool loading=false,pending=false;
static std::map<std::string,std::string> changes;
static NOTIFYICONDATAW tray{};static bool hotkeyRegistered=false;
static vr::IVROverlay* overlay=nullptr;static vr::VROverlayHandle_t overlayHandle=0,thumbnailHandle=0;
static bool vrInitialized=false;
static vr::VROverlayHandle_t dotHandles[2]{};
static bool dotVisible[2]{},dotActive=false,nativeDotAllowed=false;
static int dotError=0;static DWORD dotPid=0;
static eye::DotReader dotReader;
static ID3D11Texture2D* dotTextures[2]{};
static ID3D11Device* gpu=nullptr;static ID3D11DeviceContext* gpuContext=nullptr;static ID3D11Texture2D* gpuTexture=nullptr;
static void RenderVR();
static void VRStatus(int init,int create,int render=-1) {try{std::ofstream out(root/L"dashboard-status.json");out<<"{\"pid\":"<<GetCurrentProcessId()<<",\"init_error\":"<<init<<",\"overlay_error\":"<<create<<",\"render_error\":"<<render<<",\"hotkey_registered\":"<<(hotkeyRegistered?"true":"false")<<",\"renderer\":\"d3d11\",\"main_handle\":"<<overlayHandle<<",\"thumbnail_handle\":"<<thumbnailHandle<<",\"center_dot_renderer\":\"head_relative_stereo_quad\",\"center_dot_active\":"<<(dotActive?"true":"false")<<",\"center_dot_error\":"<<dotError<<"}";}catch(...){} }
static bool StartVR(bool show=false) {
    if(overlay){if(show)overlay->ShowDashboard("steamlinkeyeinput.control");return true;}
    vr::EVRInitError error=vr::VRInitError_None;vr::VR_Init(&error,vr::VRApplication_Overlay);
    if(error){VRStatus(error,-1);return false;}vrInitialized=true;overlay=vr::VROverlay();
    if(!overlay){VRStatus(0,-2);vr::VR_Shutdown();vrInitialized=false;return false;}
    auto manifest=(root/L"steamlinkeyeinput.vrmanifest").u8string();
    bool installed=vr::VRApplications()->IsApplicationInstalled("steamlinkeyeinput.control");
    auto registered=vr::VRApplications()->AddApplicationManifest(manifest.c_str(),false);
    if(registered==vr::VRApplicationError_None){if(!installed)vr::VRApplications()->SetApplicationAutoLaunch("steamlinkeyeinput.control",true);std::ofstream marker(root/L"dashboard-registered");marker<<"steamlinkeyeinput.control";}
    auto created=overlay->CreateDashboardOverlay("steamlinkeyeinput.control","Eye Input",&overlayHandle,&thumbnailHandle);
    if(created){VRStatus(0,created);overlay=nullptr;vr::VR_Shutdown();vrInitialized=false;return false;}
    vr::HmdVector2_t scale{{560,float(PanelHeight)}};overlay->SetOverlayMouseScale(overlayHandle,&scale);
    overlay->SetOverlayInputMethod(overlayHandle,vr::VROverlayInputMethod_Mouse);
    overlay->SetOverlayWidthInMeters(overlayHandle,1.25f);RenderVR();
    SetTimer(window,3,16,nullptr);
    if(show)overlay->ShowDashboard("steamlinkeyeinput.control");return true;
}
static double initialYaw=35,initialPitch=30;
static uint64_t previousPackets=0,previousPoints=0,previousTime=0;static DWORD previousPid=0;
static std::string Read(const fs::path& path) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot read file");
    DWORD size=GetFileSize(file,nullptr),count=0;
    if(size>65536){CloseHandle(file);throw std::runtime_error("File too large");}
    std::string text(size,0);bool ok=ReadFile(file,text.data(),size,&count,nullptr)!=0;CloseHandle(file);
    if(!ok||count!=size)throw std::runtime_error("Read failed");return text;
}
static std::string Number(double value){std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(12)<<value;return out.str();}
static std::string Patch(std::string text,const std::map<std::string,std::string>& fields) {
    for(const auto& field:fields) {
        std::regex pattern("(^|\\n)("+field.first+"=)[^\\r\\n]*");
        if(!std::regex_search(text,pattern)){
            if(field.first!="show_gaze_center"&&field.first!="offset_yaw"&&field.first!="offset_pitch"&&field.first!="gain_left"&&field.first!="gain_right"&&field.first!="gain_up"&&field.first!="gain_down")throw std::runtime_error("Missing config field");
            std::regex section("(\\[steamlinkeyeinput\\]\\r?\\n)");
            if(!std::regex_search(text,section))throw std::runtime_error("Missing config section");
            text=std::regex_replace(text,section,"$1"+field.first+"="+field.second+"\r\n",std::regex_constants::format_first_only);
            continue;
        }
        text=std::regex_replace(text,pattern,"$1"+field.first+"="+field.second);
    }
    return text;
}
static void Write(const fs::path& path,const std::map<std::string,std::string>& fields) {
    auto text=Patch(Read(path),fields);auto temporary=path;temporary+=L".control-"+std::to_wstring(GetCurrentProcessId())+L".tmp";
    HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,CREATE_ALWAYS,0,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot write config");
    DWORD count=0;bool ok=WriteFile(file,text.data(),DWORD(text.size()),&count,nullptr)!=0;
    if(ok)ok=FlushFileBuffers(file)!=0;CloseHandle(file);
    if(!ok||count!=text.size()){DeleteFileW(temporary.c_str());throw std::runtime_error("Config write failed");}
    for(int i=0;i<20;++i){if(MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return;Sleep(5);}
    DeleteFileW(temporary.c_str());throw std::runtime_error("Config temporarily locked");
}
static double Setting(const std::string& text,const char* key,double fallback) {
    std::smatch match;if(!std::regex_search(text,match,std::regex(std::string("(?:^|\\n)")+key+"=([^\\r\\n]+)")))return fallback;
    try{double value=std::stod(match[1]);return std::isfinite(value)?value:fallback;}catch(...){return fallback;}
}
static double Json(const std::string& text,const char* key,double fallback=0) {
    std::smatch match;if(!std::regex_search(text,match,std::regex(std::string("\"")+key+"\"\\s*:\\s*(true|false|-?[0-9]+(?:\\.[0-9]+)?)")))return fallback;
    std::string value=match[1];return value=="true"?1:value=="false"?0:std::stod(value);
}
static void Label(HWND control,const std::wstring& text){SetWindowTextW(control,text.c_str());}
static void ShowPanel(){ShowWindow(window,SW_RESTORE);SetForegroundWindow(window);}
static void RefreshNumbers(){
    auto number=[](HWND slider){std::wostringstream out;out<<std::fixed<<std::setprecision(1)<<SendMessageW(slider,TBM_GETPOS,0,0)/10.;return out.str();};
    Label(yawLabel,number(yawSlider));Label(pitchLabel,number(pitchSlider));
}
static void RefreshOffsets(){std::wostringstream out;out<<std::fixed<<std::setprecision(2)<<L"中心微调（度）  右 "<<offsetYaw<<L"   上 "<<offsetPitch;Label(offsetLabel,out.str());
    Label(offsetXEdit,std::to_wstring(offsetYaw));Label(offsetYEdit,std::to_wstring(offsetPitch));
    std::wostringstream step;step<<L"步长 "<<trimStep<<L"°";Label(stepButton,step.str());
    const wchar_t* directions[]={L"左",L"右",L"上",L"下"};
    for(int i=0;i<4;++i){std::wostringstream name;name<<directions[i]<<L" "<<trimStep<<L"°";Label(GetDlgItem(window,50+i),name.str());}
}
static void RefreshDirections(){
    for(int i=0;i<4;++i){std::wostringstream label;label<<GainNames[i]<<L"  "<<std::fixed<<std::setprecision(0)<<directionGains[i]*100<<L"%";Label(directionLabels[i],label.str());}
    Label(directionStepButton,L"幅度步长 "+std::to_wstring(directionStep)+L"%");
}
static void Reload(bool remember=false) {
    auto text=Read(root/L"steamlinkeyeinput"/L"steamlinkeyeinput.ini");loading=true;
    double yaw=Setting(text,"yaw",35),pitch=Setting(text,"pitch",30);
    if(remember){initialYaw=yaw;initialPitch=pitch;}
    SendMessageW(yawSlider,TBM_SETPOS,TRUE,LPARAM(lround(yaw*10)));
    SendMessageW(pitchSlider,TBM_SETPOS,TRUE,LPARAM(lround(pitch*10)));
    SendMessageW(enabledBox,BM_SETCHECK,Setting(text,"enabled",1)?BST_CHECKED:BST_UNCHECKED,0);
    SendMessageW(flipXBox,BM_SETCHECK,Setting(text,"flip_x",0)?BST_CHECKED:BST_UNCHECKED,0);
    SendMessageW(flipYBox,BM_SETCHECK,Setting(text,"flip_y",0)?BST_CHECKED:BST_UNCHECKED,0);
    SendMessageW(markerBox,BM_SETCHECK,Setting(text,"show_gaze_center",0)?BST_CHECKED:BST_UNCHECKED,0);
    offsetYaw=std::clamp(Setting(text,"offset_yaw",0),-20.,20.);offsetPitch=std::clamp(Setting(text,"offset_pitch",0),-20.,20.);RefreshOffsets();
    for(int i=0;i<4;++i)directionGains[i]=std::clamp(Setting(text,GainKeys[i],1),.1,4.);
    RefreshDirections();
    RefreshNumbers();loading=false;
}
static void Queue(const char* key,const std::string& value){changes[key]=value;pending=true;SetTimer(window,2,180,nullptr);}
static bool Save() {
    KillTimer(window,2);if(!pending)return true;
    try{Write(root/L"steamlinkeyeinput"/L"steamlinkeyeinput.ini",changes);pending=false;changes.clear();return true;}
    catch(...){Label(statusLabel,L"设置未保存：文件暂时被占用或无写入权限");return false;}
}
static void DirectionView(bool show){
    if(!Save())return;
    directionView=show;
    for(auto control:normalControls)ShowWindow(control,!show||control==statusLabel||control==detailLabel?SW_SHOW:SW_HIDE);
    for(auto control:directionControls)ShowWindow(control,show?SW_SHOW:SW_HIDE);
}
static void AdjustDirection(int id){
    int side=(id-70)/2;double delta=(id%2?1:-1)*directionStep/100.;
    directionGains[side]=std::clamp(std::round((directionGains[side]+delta)*100)/100.,.1,4.);
    Queue(GainKeys[side],Number(directionGains[side]));RefreshDirections();Save();
}
static void Trim(int id) {
    offsetYaw=std::clamp(offsetYaw+(id==50?-trimStep:id==51?trimStep:0),-20.,20.);
    offsetPitch=std::clamp(offsetPitch+(id==52?trimStep:id==53?-trimStep:0),-20.,20.);
    if(id==54)offsetYaw=offsetPitch=0;
    Queue("offset_yaw",Number(offsetYaw));Queue("offset_pitch",Number(offsetPitch));RefreshOffsets();Save();
}
static void ApplyExactOffsets(){
    try{double values[2]{};int i=0;
        for(HWND edit:{offsetXEdit,offsetYEdit}){wchar_t text[64]{};GetWindowTextW(edit,text,64);std::wstring value=text;size_t used=0;
            double number=std::stod(value,&used);if(used!=value.size()||!std::isfinite(number)||number< -20||number>20)throw std::runtime_error("Invalid trim");values[i++]=number;}
        offsetYaw=values[0];offsetPitch=values[1];Trim(58);
    }catch(...){Label(statusLabel,L"中心偏移请输入 -20 到 20 之间的度数");}
}
static std::vector<double> JsonArray(const std::string& text,const char* key){
    std::smatch match;std::vector<double> values;
    if(!std::regex_search(text,match,std::regex(std::string("\"")+key+"\"\\s*:\\s*\\[([^\\]]*)\\]")))return values;
    std::istringstream input(match[1]);std::string part;
    while(std::getline(input,part,',')){double value=std::stod(part);if(!std::isfinite(value))return {};values.push_back(value);}return values;
}
static void UpdateStatus() {
    try {
        auto path=root/L"steamlinkeyeinput"/L"status.json";auto text=Read(path);auto pid=DWORD(Json(text,"pid"));
        WIN32_FILE_ATTRIBUTE_DATA info{};GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&info);
        ULARGE_INTEGER written{};written.LowPart=info.ftLastWriteTime.dwLowDateTime;written.HighPart=info.ftLastWriteTime.dwHighDateTime;
        FILETIME time;GetSystemTimeAsFileTime(&time);ULARGE_INTEGER nowFile{};nowFile.LowPart=time.dwLowDateTime;nowFile.HighPart=time.dwHighDateTime;
        HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,pid);bool alive=process&&WaitForSingleObject(process,0)==WAIT_TIMEOUT;if(process)CloseHandle(process);
        bool running=Json(text,"running")&&alive&&nowFile.QuadPart-written.QuadPart<30000000;
        dotPid=running?pid:0;nativeDotAllowed=running&&Json(text,"enabled")&&Json(text,"show_gaze_center");
        if(nativeDotAllowed&&!overlay)StartVR();
        std::wstring state=!running?L"插件未运行或状态已过期":Json(text,"fault")?L"插件异常":!Json(text,"headset_connected")?L"插件运行中 · 等待 Steam Link 连接":!Json(text,"enabled")?L"眼追已暂停":Json(text,"data_age_ms",-1)<0?L"等待眼动数据":Json(text,"data_age_ms")>150?L"眼动数据已中断":L"眼追运行中";
        Label(statusLabel,state);
        if(running&&!Json(text,"compatible_driver"))Label(statusLabel,L"原生接口：等待布局校验或当前版本不匹配");
        uint64_t packets=uint64_t(Json(text,"packets")),points=uint64_t(Json(text,"accepted_points")),now=GetTickCount64();
        double packetRate=0,pointRate=0;
        if(previousTime&&previousPid==pid&&now>previousTime&&packets>=previousPackets&&points>=previousPoints){double seconds=(now-previousTime)/1000.;packetRate=(packets-previousPackets)/seconds;pointRate=(points-previousPoints)/seconds;}
        previousTime=now;previousPid=pid;previousPackets=packets;previousPoints=points;
        std::wostringstream detail;detail<<L"收包 "<<int(packetRate)<<L" 包/秒　提交 "<<int(pointRate)<<L" 次/秒\r\n"
            <<L"数据新鲜度 "<<int(Json(text,"data_age_ms",-1))<<L" ms　UDP "<<int(Json(text,"port"))
            <<L"　错误 "<<int(Json(text,"socket_error"));
        Label(detailLabel,detail.str());
        auto raw=JsonArray(text,"raw"),angles=JsonArray(text,"angles"),age=JsonArray(text,"channel_age_ms");
        if(running&&raw.size()>=4&&angles.size()==2&&age.size()>=4){std::wostringstream diagnosis;diagnosis<<std::fixed<<std::setprecision(4)
            <<L"原始左眼 "<<raw[0]<<L", "<<raw[2]<<L"　右眼 "<<raw[1]<<L", "<<raw[3]
            <<L"\r\n映射角度 "<<std::setprecision(2)<<angles[0]<<L"°, "<<angles[1]<<L"°　最旧通道 "<<int(*std::max_element(age.begin(),age.begin()+4))<<L" ms";
            Label(diagnosticLabel,diagnosis.str());}
        else Label(diagnosticLabel,L"原始通道诊断：重启 SteamVR 加载新版驱动后可用");
    } catch(...){dotPid=0;nativeDotAllowed=false;Label(statusLabel,L"尚未读取到插件状态");Label(detailLabel,L"请确认插件已安装，并启动 SteamVR。");Label(diagnosticLabel,L"原始通道诊断：等待插件状态");}
}
static void HideDots(){
    if(overlay)for(int i=0;i<2;++i)if(dotHandles[i]&&dotVisible[i]){overlay->HideOverlay(dotHandles[i]);dotVisible[i]=false;}
    dotActive=false;
}
static void UpdateDots(){
    eye::DotSample sample;
    if(!overlay||!nativeDotAllowed||!dotReader.Read(dotPid,sample)||!eye::FreshDot(sample,GetTickCount64())){HideDots();return;}
    if(!gpu){dotError=int(vr::VROverlayError_RequestFailed);HideDots();return;}
    for(int eye=0;eye<2;++eye)if(!dotTextures[eye]){
        uint8_t pixels[32*16*4]{};
        for(int y=0;y<16;++y)for(int x=0;x<16;++x){float d=(x-7.5f)*(x-7.5f)+(y-7.5f)*(y-7.5f);auto p=pixels+(y*32+x+eye*16)*4;
            if(d<=36){p[3]=255;if(d<=16){p[0]=80;p[1]=255;p[2]=100;}}}
        D3D11_TEXTURE2D_DESC desc{};desc.Width=32;desc.Height=16;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{pixels,32*4,0};
        if(FAILED(gpu->CreateTexture2D(&desc,&initial,&dotTextures[eye]))){dotError=int(vr::VROverlayError_RequestFailed);HideDots();return;}
    }
    vr::TrackedDevicePose_t pose{};vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseRawAndUncalibrated,0,&pose,1);
    if(!pose.bPoseIsValid||!pose.bDeviceIsConnected){HideDots();return;}
    for(int i=0;i<2;++i){
        auto eye=vr::EVREye(i);
        if(!dotHandles[i]){
            auto error=overlay->CreateOverlay(i?"steamlinkeyeinput.cropdot.right":"steamlinkeyeinput.cropdot.left","Crop center",&dotHandles[i]);
            if(error){dotError=int(error);HideDots();return;}
            overlay->SetOverlayInputMethod(dotHandles[i],vr::VROverlayInputMethod_None);
            overlay->SetOverlayFlag(dotHandles[i],vr::VROverlayFlags_VisibleInDashboard,true);
            overlay->SetOverlayFlag(dotHandles[i],vr::VROverlayFlags_SideBySide_Parallel,true);
            overlay->SetOverlayFlag(dotHandles[i],vr::VROverlayFlags_SortWithNonSceneOverlays,true);
            overlay->SetOverlaySortOrder(dotHandles[i],0xffffffffu);
            vr::Texture_t texture{dotTextures[i],vr::TextureType_DirectX,vr::ColorSpace_Gamma};
            error=overlay->SetOverlayTexture(dotHandles[i],&texture);
            if(error){overlay->DestroyOverlay(dotHandles[i]);dotHandles[i]=0;dotError=int(error);HideDots();return;}
        }
        auto projection=vr::VRSystem()->GetProjectionMatrix(eye,.1f,100.f);
        auto eyeToHead=vr::VRSystem()->GetEyeToHeadTransform(eye);
        float position[3]{},width=0;
        // Keep diagnostic dots in front of dashboard panels. Recompute the ray
        // intersection and physical size, preserving their image-space center.
        if(!eye::DotQuad(sample,i,eyeToHead.m,projection.m,.35f,position,width)){HideDots();return;}
        vr::HmdMatrix34_t transform{};for(int row=0;row<3;++row){transform.m[row][row]=1;transform.m[row][3]=position[row];}
        auto error=overlay->SetOverlayWidthInMeters(dotHandles[i],width);
        if(!error)error=overlay->SetOverlayTransformTrackedDeviceRelative(dotHandles[i],vr::k_unTrackedDeviceIndex_Hmd,&transform);
        if(!error&&!dotVisible[i]){error=overlay->ShowOverlay(dotHandles[i]);if(!error)dotVisible[i]=true;}
        if(error){dotError=int(error);HideDots();return;}
    }
    dotError=0;dotActive=true;
}
static void RenderVR() {
    if(!overlay)return;
    HDC dc=CreateCompatibleDC(nullptr);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=560;info.bmiHeader.biHeight=-PanelHeight;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    void* pixels=nullptr;HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);auto old=SelectObject(dc,bitmap);
    RECT full{0,0,560,PanelHeight};HBRUSH background=CreateSolidBrush(RGB(24,28,24));FillRect(dc,&full,background);DeleteObject(background);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(245,245,245));SelectObject(dc,font);
    auto text=[&](int x,int y,const std::wstring& value,int width=520){RECT rect{x,y,x+width,y+48};DrawTextW(dc,value.c_str(),-1,&rect,DT_LEFT|DT_WORDBREAK);};
    auto label=[](HWND control){wchar_t buffer[512]{};GetWindowTextW(control,buffer,512);return std::wstring(buffer);};
    auto button=[&](RECT rect,const std::wstring& value){HBRUSH brush=CreateSolidBrush(RGB(56,80,56));FillRect(dc,&rect,brush);DeleteObject(brush);DrawTextW(dc,value.c_str(),-1,&rect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);};
    SelectObject(dc,titleFont);text(24,20,L"SteamLinkEyeInput");SelectObject(dc,font);text(24,67,label(statusLabel));text(24,100,label(detailLabel));
    if(directionView){
        text(24,160,L"分方向校准：保持已对准的中央位置");
        for(int i=0;i<4;++i){int y=225+i*85;button({24,y,140,y+44},L"− "+std::to_wstring(directionStep)+L"%");
            text(190,y+9,label(directionLabels[i]),220);button({420,y,536,y+44},L"+ "+std::to_wstring(directionStep)+L"%");}
        text(24,550,L"绿点不到目标就放大，越过目标就缩小。\n先调中央，再调四向，避开画面最边缘。");
        button({24,610,268,650},label(directionStepButton));text(292,619,L"切换 1 / 5 / 10%");
        button({24,670,536,714},L"四向恢复 100%");button({24,733,536,775},L"返回中心与整体调节");
    }else{
    button({24,160,268,205},SendMessageW(enabledBox,BM_GETCHECK,0,0)==BST_CHECKED?L"暂停眼追":L"开启眼追");
    std::wostringstream minus,plus;minus<<L"− "<<trimStep;plus<<L"+ "<<trimStep;
    text(24,219,L"水平幅度 / Horizontal");button({24,250,116,294},minus.str());button({444,250,536,294},plus.str());text(245,259,label(yawLabel));
    text(24,309,L"垂直幅度 / Vertical");button({24,340,116,384},minus.str());button({444,340,536,384},plus.str());text(245,349,label(pitchLabel));
    button({24,413,268,452},SendMessageW(flipXBox,BM_GETCHECK,0,0)==BST_CHECKED?L"左右反转：开":L"左右反转：关");
    button({292,413,536,452},SendMessageW(flipYBox,BM_GETCHECK,0,0)==BST_CHECKED?L"上下反转：开":L"上下反转：关");
    button({24,464,536,503},SendMessageW(markerBox,BM_GETCHECK,0,0)==BST_CHECKED?L"画框 + 中心点：开 / Marker ON":L"画框 + 中心点：关 / Marker OFF");
    text(24,520,label(offsetLabel));
    for(int i=0;i<5;++i)button({24+i*104,560,120+i*104,604},label(GetDlgItem(window,50+i)));
    button({24,616,220,654},label(stepButton));text(240,621,L"点击切换 0.1 / 0.5 / 1°");
    text(24,672,label(diagnosticLabel));button({24,733,536,775},L"分方向校准：左 / 右 / 上 / 下");
    }
    GdiFlush();auto bytes=static_cast<uint8_t*>(pixels);for(int i=0;i<560*PanelHeight;++i)bytes[i*4+3]=255;
    vr::EVROverlayError error=vr::VROverlayError_RequestFailed;
    if(!gpu) {
        IDXGIFactory* factory=nullptr;IDXGIAdapter* adapter=nullptr;int32_t index=0;vr::VRSystem()->GetDXGIOutputInfo(&index);
        if(SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory),reinterpret_cast<void**>(&factory)))){factory->EnumAdapters(UINT(std::max(0,index)),&adapter);factory->Release();}
        D3D11CreateDevice(adapter,adapter?D3D_DRIVER_TYPE_UNKNOWN:D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&gpu,nullptr,&gpuContext);
        if(adapter)adapter->Release();
        if(gpu){D3D11_TEXTURE2D_DESC texture{};texture.Width=560;texture.Height=PanelHeight;texture.MipLevels=texture.ArraySize=1;texture.Format=DXGI_FORMAT_B8G8R8A8_UNORM;texture.SampleDesc.Count=1;texture.Usage=D3D11_USAGE_DEFAULT;texture.BindFlags=D3D11_BIND_SHADER_RESOURCE;gpu->CreateTexture2D(&texture,nullptr,&gpuTexture);}
    }
    // Keep the same GPU resource alive. SetOverlayRaw recreates image resources
    // asynchronously and caused visible flashes on repeated status refreshes.
    if(gpuTexture){gpuContext->UpdateSubresource(gpuTexture,0,nullptr,pixels,560*4,0);gpuContext->Flush();vr::Texture_t texture{gpuTexture,vr::TextureType_DirectX,vr::ColorSpace_Gamma};error=overlay->SetOverlayTexture(overlayHandle,&texture);if(thumbnailHandle)overlay->SetOverlayTexture(thumbnailHandle,&texture);}
    VRStatus(0,0,error);SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
}
static void PollVR() {
    if(!overlay)return;vr::VREvent_t event{};
    UpdateDots();
    while(overlay->PollNextOverlayEvent(overlayHandle,&event,sizeof(event))) {
        if(event.eventType==vr::VREvent_Quit){vr::VRSystem()->AcknowledgeQuit_Exiting();DestroyWindow(window);return;}
        if(event.eventType!=vr::VREvent_MouseButtonDown)continue;
        float x=event.data.mouse.x,y=PanelHeight-event.data.mouse.y;
        if(directionView){
            for(int i=0;i<4;++i)if(y>=225+i*85&&y<=269+i*85){if(x>=24&&x<=140)AdjustDirection(70+i*2);else if(x>=420&&x<=536)AdjustDirection(71+i*2);}
            if(y>=610&&y<=650&&x>=24&&x<=268)SendMessageW(window,WM_COMMAND,63,0);
            else if(y>=670&&y<=714&&x>=24&&x<=536)SendMessageW(window,WM_COMMAND,62,0);
            else if(y>=733&&y<=775&&x>=24&&x<=536)DirectionView(false);
            RenderVR();continue;
        }
        if(y>=160&&y<=205&&x<268){SendMessageW(enabledBox,BM_SETCHECK,SendMessageW(enabledBox,BM_GETCHECK,0,0)==BST_CHECKED?BST_UNCHECKED:BST_CHECKED,0);SendMessageW(window,WM_COMMAND,10,0);}
        else if((y>=250&&y<=294)||(y>=340&&y<=384)){
            if(x>116&&x<444)continue;auto slider=y<300?yawSlider:pitchSlider;
            auto pos=SendMessageW(slider,TBM_GETPOS,0,0);auto step=LRESULT(std::lround(trimStep*10));SendMessageW(slider,TBM_SETPOS,TRUE,std::clamp<LRESULT>(pos+(x<116?-step:step),50,900));
            SendMessageW(window,WM_HSCROLL,0,reinterpret_cast<LPARAM>(slider));Save();
        }else if(y>=413&&y<=452){int id=x<280?11:12;HWND box=id==11?flipXBox:flipYBox;SendMessageW(box,BM_SETCHECK,SendMessageW(box,BM_GETCHECK,0,0)==BST_CHECKED?BST_UNCHECKED:BST_CHECKED,0);SendMessageW(window,WM_COMMAND,id,0);}
        else if(y>=464&&y<=503){SendMessageW(markerBox,BM_SETCHECK,SendMessageW(markerBox,BM_GETCHECK,0,0)==BST_CHECKED?BST_UNCHECKED:BST_CHECKED,0);SendMessageW(window,WM_COMMAND,13,0);}
        else if(y>=560&&y<=604&&x>=24&&x<536){int i=int(x-24)/104;if(x<120+i*104)Trim(50+i);}
        else if(y>=616&&y<=654&&x>=24&&x<=220)SendMessageW(window,WM_COMMAND,55,0);
        else if(y>=733&&y<=775&&x>=24&&x<=536)DirectionView(true);
        RenderVR();
    }
    vr::VREvent_t systemEvent{};while(vr::VRSystem()->PollNextEvent(&systemEvent,sizeof(systemEvent)))if(systemEvent.eventType==vr::VREvent_Quit){vr::VRSystem()->AcknowledgeQuit_Exiting();DestroyWindow(window);return;}
    static ULONGLONG last=0;if(GetTickCount64()-last>500){if(overlay->IsOverlayVisible(overlayHandle))RenderVR();else VRStatus(0,0);last=GetTickCount64();}
}
static HWND Add(const wchar_t* type,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id=0) {
    HWND control=CreateWindowExW(0,type,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,window,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
    (buildingDirectionControls?directionControls:normalControls).push_back(control);
    SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);return control;
}
static LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
    switch(message) {
    case WM_CREATE:{window=hwnd;font=CreateFontW(-17,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");titleFont=CreateFontW(-23,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
        auto title=Add(L"STATIC",L"SteamLinkEyeInput 快速调节",0,24,18,510,32);SendMessageW(title,WM_SETFONT,reinterpret_cast<WPARAM>(titleFont),TRUE);
        Add(L"STATIC",L"Ctrl + Alt + E 呼出面板 · 关闭窗口收起到托盘",0,24,58,510,26);
        statusLabel=Add(L"STATIC",L"正在读取状态…",0,24,100,510,26);
        detailLabel=Add(L"STATIC",L"",0,24,131,510,48);
        Add(L"STATIC",L"新鲜度仅从插件收到数据计时，并非端到端延迟。",0,24,182,510,24);
        enabledBox=Add(L"BUTTON",L"启用持续眼追",BS_AUTOCHECKBOX|WS_TABSTOP,24,219,200,28,10);
        Add(L"STATIC",L"接口：原生",0,294,221,200,26);
        Add(L"STATIC",L"水平幅度",0,24,264,90,26);yawSlider=Add(TRACKBAR_CLASSW,L"",TBS_HORZ|WS_TABSTOP,110,255,350,44,20);yawLabel=Add(L"STATIC",L"",0,468,264,66,26);
        Add(L"STATIC",L"垂直幅度",0,24,316,90,26);pitchSlider=Add(TRACKBAR_CLASSW,L"",TBS_HORZ|WS_TABSTOP,110,307,350,44,21);pitchLabel=Add(L"STATIC",L"",0,468,316,66,26);
        for(auto slider:{yawSlider,pitchSlider}){SendMessageW(slider,TBM_SETRANGEMIN,FALSE,50);SendMessageW(slider,TBM_SETRANGEMAX,TRUE,900);SendMessageW(slider,TBM_SETPAGESIZE,0,10);}
        flipXBox=Add(L"BUTTON",L"左右反转",BS_AUTOCHECKBOX|WS_TABSTOP,24,363,150,26,11);flipYBox=Add(L"BUTTON",L"上下反转",BS_AUTOCHECKBOX|WS_TABSTOP,194,363,150,26,12);
        Add(L"BUTTON",L"撤销本次幅度调整",BS_PUSHBUTTON|WS_TABSTOP,24,410,215,35,30);Add(L"BUTTON",L"重新读取设置",BS_PUSHBUTTON|WS_TABSTOP,260,410,170,35,31);
        markerBox=Add(L"BUTTON",L"高清区画框 + 中心点",BS_AUTOCHECKBOX|WS_TABSTOP,24,461,510,28,13);
        offsetLabel=Add(L"STATIC",L"",0,24,515,510,26);
        const wchar_t* trimNames[]={L"左 0.5°",L"右 0.5°",L"上 0.5°",L"下 0.5°",L"偏移归零"};
        for(int i=0;i<5;++i)Add(L"BUTTON",trimNames[i],BS_PUSHBUTTON|WS_TABSTOP,24+i*104,553,96,38,50+i);
        stepButton=Add(L"BUTTON",L"步长 0.1°",BS_PUSHBUTTON|WS_TABSTOP,24,607,110,34,55);
        Add(L"STATIC",L"右",0,145,614,24,24);offsetXEdit=Add(L"EDIT",L"",WS_BORDER|WS_TABSTOP|ES_AUTOHSCROLL,172,607,93,32,56);
        Add(L"STATIC",L"上",0,278,614,24,24);offsetYEdit=Add(L"EDIT",L"",WS_BORDER|WS_TABSTOP|ES_AUTOHSCROLL,305,607,93,32,57);
        Add(L"BUTTON",L"应用度数",BS_PUSHBUTTON|WS_TABSTOP,410,607,126,34,58);
        diagnosticLabel=Add(L"STATIC",L"",0,24,660,512,62);
        Add(L"BUTTON",L"分方向校准：左 / 右 / 上 / 下",BS_PUSHBUTTON|WS_TABSTOP,24,733,512,42,60);
        buildingDirectionControls=true;
        Add(L"STATIC",L"分方向校准",0,24,18,512,32);
        Add(L"STATIC",L"先校中央，再分方向调整；中央目标保持不变。",0,24,58,512,26);
        for(int i=0;i<4;++i){int y=225+i*85;
            Add(L"BUTTON",L"缩小",BS_PUSHBUTTON|WS_TABSTOP,24,y,116,44,70+i*2);
            directionLabels[i]=Add(L"STATIC",L"",0,190,y+9,220,30);
            Add(L"BUTTON",L"放大",BS_PUSHBUTTON|WS_TABSTOP,420,y,116,44,71+i*2);}
        Add(L"STATIC",L"绿点不到目标就放大，越过目标就缩小。\r\n先调中央，再调四向，避开画面最边缘。",0,24,550,512,50);
        directionStepButton=Add(L"BUTTON",L"幅度步长 1%",BS_PUSHBUTTON|WS_TABSTOP,24,610,244,40,63);
        Add(L"STATIC",L"切换 1 / 5 / 10%",0,292,619,244,30);
        Add(L"BUTTON",L"四向恢复 100%",BS_PUSHBUTTON|WS_TABSTOP,24,670,512,44,62);
        Add(L"BUTTON",L"返回中心与整体调节",BS_PUSHBUTTON|WS_TABSTOP,24,733,512,42,61);
        buildingDirectionControls=false;DirectionView(false);
        try{Reload(true);}catch(...){Label(statusLabel,L"未找到插件配置文件");}
        tray.cbSize=sizeof(tray);tray.hWnd=hwnd;tray.uID=1;tray.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;tray.uCallbackMessage=WM_APP+1;tray.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
        wcscpy_s(tray.szTip,L"SteamLinkEyeInput · Ctrl+Alt+E");Shell_NotifyIconW(NIM_ADD,&tray);
        hotkeyRegistered=RegisterHotKey(hwnd,1,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,'E')!=FALSE;
        if(!hotkeyRegistered)SetWindowTextW(hwnd,L"SteamLinkEyeInput 快速调节（快捷键被占用，请用托盘）");
        SetTimer(hwnd,1,500,nullptr);UpdateStatus();return 0;}
    case WM_HSCROLL:if(!loading){auto slider=reinterpret_cast<HWND>(lp);if(slider==yawSlider||slider==pitchSlider){Queue(slider==yawSlider?"yaw":"pitch",Number(SendMessageW(slider,TBM_GETPOS,0,0)/10.));RefreshNumbers();}}return 0;
    case WM_COMMAND:if(HIWORD(wp)==BN_CLICKED){int id=LOWORD(wp);if(id>=50&&id<=54){Trim(id);return 0;}
        if(id==60||id==61){DirectionView(id==60);return 0;}
        if(id>=70&&id<=77){AdjustDirection(id);return 0;}
        if(id==63){directionStep=directionStep==1?5:directionStep==5?10:1;RefreshDirections();return 0;}
        if(id==62){for(int i=0;i<4;++i){directionGains[i]=1;Queue(GainKeys[i],"1");}RefreshDirections();Save();return 0;}
        if(id==55){trimStep=trimStep<.2?.5:trimStep<.75?1.:.1;RefreshOffsets();return 0;}
        if(id==58){ApplyExactOffsets();return 0;}
        if(id==40){ShowPanel();return 0;}if(id==41){if(Save())DestroyWindow(hwnd);return 0;}if(id==13){Queue("show_gaze_center",SendMessageW(markerBox,BM_GETCHECK,0,0)==BST_CHECKED?"1":"0");Save();}else if(id>=10&&id<=12){HWND checkbox=id==10?enabledBox:id==11?flipXBox:flipYBox;Queue(id==10?"enabled":id==11?"flip_x":"flip_y",SendMessageW(checkbox,BM_GETCHECK,0,0)==BST_CHECKED?"1":"0");Save();}else if(id==30){Queue("yaw",Number(initialYaw));Queue("pitch",Number(initialPitch));if(Save())Reload();}else if(id==31){if(Save())try{Reload();}catch(...){Label(statusLabel,L"重新读取失败");}}}return 0;
    case WM_HOTKEY:if(wp==1)ShowPanel();return 0;
    case WM_APP+1:if(lp==WM_LBUTTONUP||lp==WM_LBUTTONDBLCLK)ShowPanel();else if(lp==WM_RBUTTONUP){HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,40,L"打开调节面板　Ctrl+Alt+E");AppendMenuW(menu,MF_STRING,41,L"退出面板（插件继续工作）");POINT point;GetCursorPos(&point);SetForegroundWindow(hwnd);TrackPopupMenu(menu,TPM_RIGHTBUTTON,point.x,point.y,0,hwnd,nullptr);DestroyMenu(menu);PostMessageW(hwnd,WM_NULL,0,0);}return 0;
    case WM_APP+2:StartVR(true);return 0;
    case WM_APP+4:if(vrInitialized){vr::VRApplications()->SetApplicationAutoLaunch("steamlinkeyeinput.control",false);vr::VRApplications()->RemoveApplicationManifest((root/L"steamlinkeyeinput.vrmanifest").u8string().c_str());DeleteFileW((root/L"dashboard-registered").c_str());}if(Save())DestroyWindow(hwnd);return 0;
    case WM_TIMER:if(wp==1)UpdateStatus();else if(wp==2)Save();else if(wp==3)PollVR();return 0;
    case WM_CLOSE:if(Save())ShowWindow(hwnd,SW_HIDE);return 0;
    case WM_DESTROY:HideDots();dotReader.Close();for(auto& texture:dotTextures)if(texture){texture->Release();texture=nullptr;}if(overlay){for(auto handle:dotHandles)if(handle)overlay->DestroyOverlay(handle);overlay->DestroyOverlay(overlayHandle);overlay->DestroyOverlay(thumbnailHandle);overlay=nullptr;}if(gpuTexture){gpuTexture->Release();gpuTexture=nullptr;}if(gpuContext){gpuContext->Release();gpuContext=nullptr;}if(gpu){gpu->Release();gpu=nullptr;}if(vrInitialized){vr::VR_Shutdown();vrInitialized=false;}if(hotkeyRegistered)UnregisterHotKey(hwnd,1);Shell_NotifyIconW(NIM_DELETE,&tray);KillTimer(hwnd,1);KillTimer(hwnd,2);KillTimer(hwnd,3);DeleteObject(font);DeleteObject(titleFont);PostQuitMessage(0);return 0;
    }return DefWindowProcW(hwnd,message,wp,lp);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show) {
    int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);root=fs::path(path).parent_path();
    try{if(fs::exists(root/L"install-location.txt")){auto text=Read(root/L"install-location.txt");root=fs::path(std::wstring(text.begin(),text.end()));}}catch(...){}
    bool test=false,uiTest=false,startHidden=false,startVR=false,unregisterVR=false;for(int i=1;i<argc;++i){if(!wcscmp(argv[i],L"--root")&&i+1<argc)root=argv[++i];else if(!wcscmp(argv[i],L"--self-test"))test=true;else if(!wcscmp(argv[i],L"--ui-self-test"))uiTest=true;else if(!wcscmp(argv[i],L"--tray"))startHidden=true;else if(!wcscmp(argv[i],L"--vr"))startVR=true;else if(!wcscmp(argv[i],L"--unregister-vr"))unregisterVR=true;}LocalFree(argv);
    if(unregisterVR){if(auto existing=FindWindowW(L"SteamLinkEyeInputControl",nullptr)){SendMessageW(existing,WM_APP+4,0,0);return 0;}vr::EVRInitError error;vr::VR_Init(&error,vr::VRApplication_Utility);if(error)return 3;vr::VRApplications()->SetApplicationAutoLaunch("steamlinkeyeinput.control",false);auto result=vr::VRApplications()->RemoveApplicationManifest((root/L"steamlinkeyeinput.vrmanifest").u8string().c_str());vr::VR_Shutdown();if(result==vr::VRApplicationError_None)DeleteFileW((root/L"dashboard-registered").c_str());return result==vr::VRApplicationError_None?0:4;}
    if(test){try{auto text=Patch("[steamlinkeyeinput]\r\nyaw=32\r\npitch=28\r\naddress_EyeLeftX=/papertracker/EyeLeftX\r\n",{{"yaw","40"}});text=Patch(text,{{"offset_yaw","-1.5"},{"offset_pitch","2"}});if(Setting(text,"offset_yaw",0)!=-1.5||Setting(text,"offset_pitch",0)!=2)return 5;text=Patch(text,{{"offset_yaw","0"}});if(Setting(text,"offset_yaw",-1)!=0||Setting(text,"offset_pitch",0)!=2)return 6;text=Patch(text,{{"show_gaze_center","1"}});text=Patch(text,{{"show_gaze_center","0"}});if(Setting(text,"show_gaze_center",-1)!=0)return 3;if(Setting(text,"yaw",0)!=40||Setting(text,"pitch",0)!=28||text.find("address_EyeLeftX=/papertracker/EyeLeftX")==std::string::npos)return 2;return 0;}catch(...){return 1;}}
    if(uiTest){root/=L"control-fixture-"+std::to_wstring(GetCurrentProcessId());if(fs::exists(root))return 10;
        fs::create_directories(root/L"steamlinkeyeinput");std::ofstream fixture(root/L"steamlinkeyeinput"/L"steamlinkeyeinput.ini");
        fixture<<"[steamlinkeyeinput]\nyaw=60\npitch=54.5\noffset_yaw=0\noffset_pitch=-5\naddress_EyeLeftX=/papertracker/EyeLeftX\n";}
    HANDLE singleton=uiTest?nullptr:CreateMutexW(nullptr,FALSE,L"Local\\SteamLinkEyeInputControl");
    if(singleton&&GetLastError()==ERROR_ALREADY_EXISTS){HWND existing=FindWindowW(L"SteamLinkEyeInputControl",nullptr);if(existing){if(startVR)PostMessageW(existing,WM_APP+2,0,0);else{ShowWindow(existing,SW_RESTORE);SetForegroundWindow(existing);}}CloseHandle(singleton);return 0;}
    SetProcessDPIAware();INITCOMMONCONTROLSEX common{sizeof(common),ICC_BAR_CLASSES};InitCommonControlsEx(&common);
    WNDCLASSW klass{};klass.hInstance=instance;klass.lpfnWndProc=Proc;klass.lpszClassName=L"SteamLinkEyeInputControl";klass.hCursor=LoadCursorW(nullptr,IDC_ARROW);klass.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);RegisterClassW(&klass);
    RECT rect{0,0,560,PanelHeight};DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;AdjustWindowRect(&rect,style,FALSE);
    HWND hwnd=CreateWindowW(klass.lpszClassName,L"SteamLinkEyeInput 快速调节",style,CW_USEDEFAULT,CW_USEDEFAULT,rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,instance,nullptr);
    if(!hwnd)return 1;
    if(uiTest){int result=0;try{
        SendMessageW(hwnd,WM_COMMAND,51,0);
        auto configPath=root/L"steamlinkeyeinput"/L"steamlinkeyeinput.ini";
        if(std::fabs(Setting(Read(configPath),"offset_yaw",0)-.1)>1e-6)result=11;
        SendMessageW(hwnd,WM_COMMAND,55,0);if(trimStep!=.5)result=12;
        SendMessageW(hwnd,WM_COMMAND,55,0);if(trimStep!=1)result=13;
        SendMessageW(hwnd,WM_COMMAND,55,0);if(trimStep!=.1)result=14;
        Label(offsetXEdit,L"0.05");Label(offsetYEdit,L"-4.95");SendMessageW(hwnd,WM_COMMAND,58,0);
        auto text=Read(configPath);
        if(std::fabs(Setting(text,"offset_yaw",0)-.05)>1e-6||std::fabs(Setting(text,"offset_pitch",0)+4.95)>1e-6||Setting(text,"yaw",0)!=60||Setting(text,"pitch",0)!=54.5)result=15;
        Label(offsetXEdit,L"20.1");SendMessageW(hwnd,WM_COMMAND,58,0);if(Read(configPath)!=text)result=16;
        Label(offsetXEdit,L"0.1junk");SendMessageW(hwnd,WM_COMMAND,58,0);if(Read(configPath)!=text)result=17;
        auto numbers=JsonArray("{\"raw\":[0.1,-0.2,0.3,0.4]}","raw");if(numbers.size()!=4||numbers[1]!=-.2)result=18;
        SendMessageW(hwnd,WM_COMMAND,60,0);if(!directionView)result=20;
        SendMessageW(hwnd,WM_COMMAND,70,0);SendMessageW(hwnd,WM_COMMAND,73,0);
        auto directional=Read(configPath);
        if(std::fabs(Setting(directional,"gain_left",1)-.99)>1e-6||std::fabs(Setting(directional,"gain_right",1)-1.01)>1e-6||Setting(directional,"gain_up",1)!=1||Setting(directional,"offset_yaw",0)!=.05||Setting(directional,"pitch",0)!=54.5)result=21;
        SendMessageW(hwnd,WM_COMMAND,63,0);if(directionStep!=5)result=22;
        SendMessageW(hwnd,WM_COMMAND,63,0);if(directionStep!=10)result=23;
        SendMessageW(hwnd,WM_COMMAND,63,0);if(directionStep!=1)result=24;
        SendMessageW(hwnd,WM_COMMAND,62,0);directional=Read(configPath);
        for(auto key:GainKeys)if(Setting(directional,key,0)!=1)result=25;
        SendMessageW(hwnd,WM_COMMAND,61,0);if(directionView)result=26;
    }catch(...){result=19;}DestroyWindow(hwnd);return result;}
    ShowWindow(hwnd,startHidden?SW_HIDE:show==SW_HIDE?SW_SHOWNORMAL:show);if(startVR)StartVR();MSG message;while(GetMessageW(&message,nullptr,0,0)>0){if(!IsDialogMessageW(hwnd,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}if(singleton)CloseHandle(singleton);return 0;
}
