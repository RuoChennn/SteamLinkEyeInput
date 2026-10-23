#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../src/center_dot.h"
#include <iostream>
#include <thread>
#include <atomic>
#include <stdexcept>
static void Check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(){try{
    uint16_t crop[10]={2,0,1691,1080,3096,3312,892,1080,3096,3312};
    eye::DotSample sample;sample.tick=1000;sample.valid=1;
    Check(eye::CropCenters(crop,1152,sample),"actual binocular crop fixture");
    Check(std::fabs(sample.uv[0]-2267.f/3096)<1e-6&&std::fabs(sample.uv[2]-1468.f/3096)<1e-6,"independent crop centers");
    float projection[4][4]={{1,0,0,0},{0,1,0,0},{0,0,-1,-.1f},{0,0,-1,0}};
    float eyeToHead[3][4]={{1,0,0,-.032f},{0,1,0,0},{0,0,1,0}},position[3]{},width=0;
    Check(eye::DotQuad(sample,0,eyeToHead,projection,2,position,width),"head-relative stereo quad");
    Check(std::fabs(position[0]-(-.032f+2*(sample.uv[0]*2-1)))<1e-6&&std::fabs(position[1]+2*(sample.uv[1]*2-1))<1e-6&&position[2]==-2,"eye origin and downward raw vertical tangent");
    // Captured Quest 3 asymmetric vertical frustum: forward y=0 maps to
    // v=0.4034, NOT 0.5. Check round trips at multiple vertical positions.
    projection[0][0]=.902738392f;projection[0][2]=-.242512643f;
    projection[1][1]=.835478842f;projection[1][2]=-.193187386f;
    auto asymmetric=sample;
    for(float y:{-.5f,0.f,.5f}){
        asymmetric.uv[1]=(1-(projection[1][1]*y-projection[1][2]))*.5f;
        Check(eye::DotQuad(asymmetric,0,eyeToHead,projection,2,position,width),"asymmetric matrix unprojection");
        Check(std::fabs(position[1]-2*y)<1e-6,"asymmetric vertical optical offset round trip");
        auto oldWidth=width;float oldX=position[0],oldY=position[1];
        Check(eye::DotQuad(asymmetric,0,eyeToHead,projection,.35f,position,width),"foreground overlay plane");
        Check(std::fabs((position[0]-eyeToHead[0][3])/.35f-(oldX-eyeToHead[0][3])/2)<1e-6&&
              std::fabs(position[1]/.35f-oldY/2)<1e-6&&std::fabs(width/.35f-oldWidth/2)<1e-6,
              "nearer plane preserves image-space center and angular size");
    }
    eyeToHead[2][2]=-1;Check(!eye::DotQuad(sample,0,eyeToHead,projection,2,position,width),"backward ray rejected");
    Check(eye::FreshDot(sample,1150)&&!eye::FreshDot(sample,1151)&&!eye::FreshDot(sample,999),"stale/future frame hidden");
    auto before=sample;crop[2]=3000;Check(!eye::CropCenters(crop,1152,sample)&&sample.uv[0]==before.uv[0],"invalid crop rejected transactionally");
    crop[2]=0;crop[3]=0;Check(eye::CropCenters(crop,1152,sample)&&sample.uv[0]>0,"edge center follows bounded rectangle, not raw gaze");
    sample.uv[0]=NAN;Check(!eye::FreshDot(sample,1000),"NaN hidden");
    eye::DotWriter writer;writer.Open();eye::DotReader reader;
    sample={};sample.tick=GetTickCount64();sample.valid=1;crop[2]=1691;crop[3]=1080;
    Check(eye::CropCenters(crop,1152,sample),"restore fixture");writer.Put(sample);
    eye::DotSample read;Check(reader.Read(GetCurrentProcessId(),read)&&read.uv[0]==sample.uv[0]&&read.nativeStamp==sample.nativeStamp,"real read-only mapping");
    std::atomic<bool> done{false};std::atomic<unsigned> successes{0};
    std::thread producer([&]{for(unsigned i=1;i<=20000;++i){auto s=sample;s.nativeStamp=i;for(int k=0;k<4;++k)s.uv[k]=float(i);writer.Put(s);}done=true;});
    while(!done){if(reader.Read(GetCurrentProcessId(),read)&&read.nativeStamp){for(float x:read.uv)Check(x==float(read.nativeStamp),"no torn IPC snapshot");++successes;}}
    producer.join();Check(successes>0,"concurrent snapshots observed");writer.Close();
    Check(reader.Read(GetCurrentProcessId(),read)&&!eye::FreshDot(read,GetTickCount64()),"publisher shutdown clears dot");
    reader.Close();Check(!reader.Read(GetCurrentProcessId(),read),"closed mapping unavailable");
    std::cout<<"PASS: crop center, edge limits, stereo ray geometry, expiry, real read-only/concurrent IPC and shutdown\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
