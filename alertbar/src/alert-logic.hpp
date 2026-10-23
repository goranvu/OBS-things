#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace alert {
constexpr float kYellowDb=-12.f;
constexpr float kRedDb=-3.f;
inline float dbfs(float x){return !std::isfinite(x)||x<=.000001f?-60.f:std::clamp(20.f*std::log10(x),-60.f,0.f);}
inline float fraction(float d){return std::clamp((d+60.f)/60.f,0.f,1.f);}
class Detector{
    float last=-60;
    int64_t next=0,end=0;
public:
    void reset(){last=-60;next=0;end=0;}
    void update(float p,int64_t now){
        float measured=dbfs(p);
        last=std::max(measured,last-1.2f);
        if(measured>=kRedDb&&now>=next){end=now+360;next=now+440;}
    }
    float db()const{return last;}
    bool blink(int64_t now)const{return now<end&&((now/105)%2==0);}
};
}
