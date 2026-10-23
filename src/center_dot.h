#pragma once
#include <windows.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace eye {
struct DotSample {
    uint64_t tick=0,nativeStamp=0;
    uint32_t valid=0;
    float uv[4]{}; // left u,v; right u,v, from ACTUAL crop rectangles
    uint32_t dimensions[4]{};
};
inline bool CropCenters(const uint16_t (&meta)[10],int square,DotSample& out) {
    if(meta[0]!=2||square<2||square>16384)return false;
    DotSample sample=out;
    for(int eye=0;eye<2;++eye){
        int at=2+eye*4,x=meta[at],y=meta[at+1],w=meta[at+2],h=meta[at+3];
        if(w<square||h<square||w>32767||h>32767||x>w-square||y>h-square)return false;
        sample.uv[eye*2]=(float(x)+square*.5f)/float(w);
        sample.uv[eye*2+1]=(float(y)+square*.5f)/float(h);
        sample.dimensions[eye*2]=uint32_t(w);sample.dimensions[eye*2+1]=uint32_t(h);
    }
    out=sample;return true;
}
inline bool FreshDot(const DotSample& sample,uint64_t now){
    if(sample.valid!=1||!sample.tick||now<sample.tick||now-sample.tick>150)return false;
    for(int i=0;i<4;++i)if(!std::isfinite(sample.uv[i])||sample.uv[i]<0||sample.uv[i]>1||sample.dimensions[i]<2||sample.dimensions[i]>32767)return false;
    return true;
}
// Invert the ACTUAL per-eye perspective matrix. Raw tangent top/bottom names
// are insufficient for asymmetric vertical frusta; preserve the optical offset.
inline bool DotQuad(const DotSample& sample,int eye,const float (&eyeToHead)[3][4],
                    const float (&projection)[4][4],float depth,float (&position)[3],float& width){
    if(eye<0||eye>1||depth<=0)return false;
    for(auto& row:projection)for(float value:row)if(!std::isfinite(value))return false;
    if(projection[0][0]<=0||projection[1][1]<=0||std::fabs(projection[0][1])>1e-5f||std::fabs(projection[1][0])>1e-5f||
       std::fabs(projection[0][3])>1e-5f||std::fabs(projection[1][3])>1e-5f||std::fabs(projection[3][0])>1e-5f||
       std::fabs(projection[3][1])>1e-5f||std::fabs(projection[3][2]+1)>1e-5f||std::fabs(projection[3][3])>1e-5f)return false;
    float ray[3]={(2*sample.uv[eye*2]-1+projection[0][2])/projection[0][0],
                  (1-2*sample.uv[eye*2+1]+projection[1][2])/projection[1][1],-1};
    float direction[3]{};
    for(int row=0;row<3;++row)for(int col=0;col<3;++col)direction[row]+=eyeToHead[row][col]*ray[col];
    if(!std::isfinite(direction[2])||direction[2]>=-.01f)return false;
    float distance=(-depth-eyeToHead[2][3])/direction[2];if(!std::isfinite(distance)||distance<=0)return false;
    for(int row=0;row<3;++row){position[row]=eyeToHead[row][3]+distance*direction[row];if(!std::isfinite(position[row]))return false;}
    width=distance*48.f/(projection[0][0]*sample.dimensions[eye*2]);
    return std::isfinite(width)&&width>0&&width<1;
}
struct alignas(8) DotShared {
    uint32_t magic=0,version=0,bytes=0,pid=0;
    volatile LONG sequence=0;uint32_t reserved=0;
    DotSample sample;
};
inline std::wstring DotMappingName(DWORD pid){return L"Local\\SteamLinkEyeInput.CropCenter."+std::to_wstring(pid);}
class DotWriter {
    HANDLE mapping=nullptr;DotShared* data=nullptr;
public:
    ~DotWriter(){Close();}
    void Open(){
        Close();auto name=DotMappingName(GetCurrentProcessId());
        mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(DotShared),name.c_str());
        if(mapping)data=static_cast<DotShared*>(MapViewOfFile(mapping,FILE_MAP_WRITE,0,0,sizeof(DotShared)));
        if(data){ZeroMemory(data,sizeof(DotShared));data->version=1;data->bytes=sizeof(DotShared);data->pid=GetCurrentProcessId();MemoryBarrier();data->magic=0x45594544;}
    }
    void Put(const DotSample& sample){if(!data)return;InterlockedIncrement(&data->sequence);MemoryBarrier();data->sample=sample;MemoryBarrier();InterlockedIncrement(&data->sequence);}
    void Close(){if(data){DotSample empty;Put(empty);UnmapViewOfFile(data);data=nullptr;}if(mapping){CloseHandle(mapping);mapping=nullptr;}}
};
class DotReader {
    HANDLE mapping=nullptr;const DotShared* data=nullptr;DWORD pid=0;
public:
    ~DotReader(){Close();}
    void Close(){if(data){UnmapViewOfFile(data);data=nullptr;}if(mapping){CloseHandle(mapping);mapping=nullptr;}pid=0;}
    bool Read(DWORD writer,DotSample& sample){
        if(writer!=pid)Close();
        if(!writer)return false;
        if(!data){auto name=DotMappingName(writer);mapping=OpenFileMappingW(FILE_MAP_READ,FALSE,name.c_str());
            if(mapping)data=static_cast<const DotShared*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,sizeof(DotShared)));
            if(!data){Close();return false;}pid=writer;}
        if(data->magic!=0x45594544||data->version!=1||data->bytes!=sizeof(DotShared)||data->pid!=writer)return false;
        for(int retry=0;retry<3;++retry){
            // Aligned 32-bit volatile loads are atomic on Windows x64. Do not
            // use InterlockedCompareExchange on a READ-ONLY mapping.
            LONG before=data->sequence;if(before&1)continue;MemoryBarrier();
            auto copy=data->sample;MemoryBarrier();LONG after=data->sequence;
            if(before==after&&!(after&1)){sample=copy;return true;}
        }return false;
    }
};
struct DotFrame {
    DotWriter& writer;DotSample sample;
    explicit DotFrame(DotWriter& value):writer(value){sample.tick=GetTickCount64();}
    ~DotFrame(){writer.Put(sample);}
};
} // namespace eye
