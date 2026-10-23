#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace eye {
constexpr const char* Names[]={"EyeLeftX","EyeRightX","EyeLeftY","EyeRightY","EyeLidLeft","EyeLidRight"};
struct Message { int index; float value; };
using Addresses=std::array<std::string,6>;
inline const Addresses& DefaultAddresses() {
    static const Addresses addresses={"/eyetracking/EyeLeftX","/eyetracking/EyeRightX",
        "/eyetracking/EyeLeftY","/eyetracking/EyeRightY","/eyetracking/EyeLidLeft","/eyetracking/EyeLidRight"};
    return addresses;
}
inline uint32_t Big32(const uint8_t* p) { return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3]; }
inline bool String(const uint8_t* p,size_t size,size_t& pos,std::string& out) {
    size_t end=pos;while(end<size && p[end])++end;
    if(end==size)return false;
    out.assign(reinterpret_cast<const char*>(p+pos),end-pos);
    size_t next=(end+4)&~size_t(3);
    if(next>size)return false;
    for(size_t i=end;i<next;++i)if(p[i])return false;
    pos=next;return true;
}
// Parsing is transactional: a malformed bundle never partially changes gaze.
inline bool Parse(const uint8_t* p,size_t size,std::vector<Message>& messages,const Addresses& addresses=DefaultAddresses(),int depth=0) {
    if(depth>8 || size>8192 || size<4)return false;
    if(size>=8 && !memcmp(p,"#bundle\0",8)) {
        if(size<16)return false;
        size_t pos=16;
        while(pos<size) {
            if(size-pos<4)return false;
            auto n=Big32(p+pos);pos+=4;
            if(!n || n>size-pos || !Parse(p+pos,n,messages,addresses,depth+1))return false;
            pos+=n;
        }
        return true;
    }
    size_t pos=0;std::string address,type;
    if(!String(p,size,pos,address)||!String(p,size,pos,type)||address.empty()||address[0]!='/' ||
       (type!=",f" && type!=",i") || size-pos!=4)return false;
    uint32_t bits=Big32(p+pos);float value;
    if(type==",f")memcpy(&value,&bits,4);else value=float(int32_t(bits));
    if(!std::isfinite(value)||std::fabs(value)>2)return false;
    for(int i=0;i<6;++i)if(address==addresses[i])messages.push_back({i,value});
    return true;
}
struct Config {
    bool enabled=true,flipX=false,flipY=false,showGazeCenter=false;
    int port=9100;
    float yaw=35,pitch=30,depth=2,offsetYaw=0,offsetPitch=0;
    std::array<float,4> directionGain={1,1,1,1}; // left, right, up, down
    Addresses addresses=DefaultAddresses();
};
struct Point { float x=0,y=0,z=-2;bool valid=false;uint64_t sequence=0,time=0; };
struct Tracker {
    std::array<float,6> values={0,0,0,0,1,1};
    std::array<uint64_t,6> updated{};
    unsigned seen=0;uint64_t sequence=0;
    std::array<uint64_t,6> updates{},changes{};
    float inputX=0,inputY=0,angleX=0,angleY=0;
    bool Accept(const std::vector<Message>& messages,uint64_t now,const Config& config,Point& out) {
        if(messages.empty())return false;
        for(auto m:messages){if(!(seen&(1u<<m.index))||values[m.index]!=m.value)++changes[m.index];
            values[m.index]=m.value;updated[m.index]=now;seen|=1u<<m.index;++updates[m.index];}
        return Map(now,config,out);
    }
    bool Map(uint64_t now,const Config& config,Point& out) {
        // Independent OSC updates reuse recent channels, never expired values.
        if((seen&15)!=15)return false;
        for(int i=0;i<4;++i)if(now<updated[i]||now-updated[i]>150)return false;
        float x=0,y=0;int opened=0;
        for(int side=0;side<2;++side) {
            // A stale lid sample is treated as unknown/open; fresh closures suppress gaze.
            if(updated[4+side] && now-updated[4+side]<=150 && values[4+side]<=.1f)continue;
            // Upstream calibration remains intact; manual angular trim is applied below.
            x+=values[side];y+=values[2+side];++opened;
        }
        if(opened){x/=opened;y/=opened;}
        inputX=x;inputY=y;
        if(config.flipX)x=-x;if(config.flipY)y=-y;
        // Scale around the trimmed optical center, not the raw input origin.
        float baseX=std::clamp(x,-1.f,1.f)*config.yaw+config.offsetYaw;
        float baseY=std::clamp(y,-1.f,1.f)*config.pitch+config.offsetPitch;
        angleX=std::clamp(baseX*config.directionGain[baseX<0?0:1],-89.f,89.f);
        angleY=std::clamp(baseY*config.directionGain[baseY<0?3:2],-89.f,89.f);
        constexpr float radians=3.14159265358979323846f/180;
        out={config.depth*std::tan(angleX*radians),config.depth*std::tan(angleY*radians),
             -config.depth,opened>0,++sequence,*std::min_element(updated.begin(),updated.begin()+4)};
        return true;
    }
};
}
