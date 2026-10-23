#include "../src/gaze.h"
#include <iostream>
#include <stdexcept>
static void Check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(){try{
    eye::Tracker tracker;eye::Config config;eye::Point point;
    Check(tracker.Accept({{0,0},{1,0},{2,0},{3,0}},1000,config,point),"initial complete sample");
    auto first=point;
    Check(tracker.Accept({{0,.001f},{1,.001f}},1010,config,point),"small horizontal-only update must not wait for unchanged vertical channels");
    Check(point.valid&&point.x>first.x&&point.y==first.y,"sub-degree movement preserved");
    Check(point.time==1000,"partial update must not refresh age of unchanged axes");
    Check(tracker.changes[0]==2&&tracker.updates[2]==1,"source change counters survive sparse updates");
    Check(!tracker.Accept({{0,.002f},{1,.002f}},1200,config,point),"expired vertical channels must not be reused");
    Check(tracker.Accept({{2,0},{3,0}},1201,config,point),"fresh values recover independently");
    Check(tracker.Accept({{4,0},{5,0}},1202,config,point)&&!point.valid,"lid-only closure invalidates gaze immediately");
    Check(tracker.Accept({{4,1},{5,1}},1203,config,point)&&point.valid,"lid-only reopening recovers gaze");
    auto time=point.time;config.offsetYaw=.05f;
    Check(tracker.Map(1204,config,point)&&point.time==time&&std::fabs(tracker.angleX-(.002f*35+.05f))<1e-5,"fine remapping uses held sample without faking freshness");
    Check(!tracker.Map(1400,config,point),"config changes cannot revive expired input");
    eye::Tracker uninitialized;
    Check(!uninitialized.Accept({{0,.1f},{1,.1f},{2,.1f}},1500,config,point),"missing initial channel never treated as zero");
    eye::Tracker asymmetric;eye::Config calibrated;
    calibrated.yaw=40;calibrated.pitch=30;calibrated.offsetYaw=4;calibrated.offsetPitch=-3;
    calibrated.directionGain={1.5f,.75f,1.2f,.8f};
    auto sample=[&](float x,float y){return asymmetric.Accept({{0,x},{1,x},{2,y},{3,y}},2000,calibrated,point);};
    Check(sample(-.5f,.5f)&&std::fabs(asymmetric.angleX+24)<1e-4&&std::fabs(asymmetric.angleY-14.4f)<1e-4,"asymmetric left/up target fit");
    Check(sample(.5f,-.5f)&&std::fabs(asymmetric.angleX-18)<1e-4&&std::fabs(asymmetric.angleY+14.4f)<1e-4,"asymmetric right/down target fit");
    auto right=point;calibrated.directionGain[0]=2;
    Check(sample(.5f,-.5f)&&point.x==right.x&&point.y==right.y,"left adjustment cannot move right or vertical target");
    Check(sample(-.1f,.1f)&&std::fabs(asymmetric.angleX)<1e-5&&std::fabs(asymmetric.angleY)<1e-5,"all gains pivot around trimmed center, not raw zero");
    for(int axis=0;axis<2;++axis){float previous=-100;
        for(int i=-100;i<=100;++i){float x=-.1f+(axis==0?i*1e-6f:0),y=.1f+(axis==1?i*1e-6f:0);sample(x,y);
            float angle=axis==0?asymmetric.angleX:asymmetric.angleY;Check(angle>=previous,"micro movement stays monotonic across center");previous=angle;}}
    calibrated.flipX=true;sample(.5f,0);Check(std::fabs(asymmetric.angleX+32)<1e-4,"left gain follows physical direction after flip");
    calibrated.directionGain={4,4,4,4};sample(1,1);Check(std::isfinite(point.x)&&std::isfinite(point.y)&&std::fabs(asymmetric.angleX)<=89&&std::fabs(asymmetric.angleY)<=89,"direction gains respect finite angle limit");
    std::cout<<"PASS: sparse updates, micro movement, expiry, lids and asymmetric calibration\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
