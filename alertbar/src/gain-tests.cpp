#include "alert-logic.hpp"
#include "adaptive-gain.hpp"
#include <cstdlib>
#include <iostream>
void check(bool ok,const char*which){if(!ok){std::cerr<<"FAIL: "<<which<<"\n";std::exit(1);}}
int main() {
    alert::AdaptiveGain a;
    for(int i=0;i<7;i++)check(a.feed(.98f,.20f,true)==alert::GainAction::None,"needs two full seconds");
    check(a.feed(.98f,.20f,true)==alert::GainAction::Down3,"loud two seconds => -3dB");
    alert::AdaptiveGain transient;
    for(int i=0;i<70;++i) {
        float pk=i==10?.99f:.35f;
        check(transient.feed(pk,.08f,true)!=alert::GainAction::Down3,"single peak ignored");
    }
    alert::AdaptiveGain quiet;
    bool raised=false;
    for(int i=0;i<70;++i) {
        bool speaking=i%4!=0; // natural speech pauses
        auto action=quiet.feed(speaking?.22f:0.f,speaking?.04f:0.f,true);
        if(action==alert::GainAction::Up1)raised=true;
        check(action!=alert::GainAction::Down3,"quiet never lowers");
    }
    check(raised,"quiet speech sustained => +1dB");
    alert::AdaptiveGain silence;
    for(int i=0;i<90;++i)
        check(silence.feed(0,0,true)==alert::GainAction::None,"silence never raises");
    alert::AdaptiveGain roomNoise;
    for(int i=0;i<90;++i)
        check(roomNoise.feed(.011f,.003f,true)==alert::GainAction::None,"low noise never raises");
    alert::AdaptiveGain medium;
    for(int i=0;i<90;++i)
        check(medium.feed(.76f,.20f,true)==alert::GainAction::None,"adequate level unchanged");
    alert::AdaptiveGain recovery;
    for(int i=0;i<8;++i)recovery.feed(.98f,.2f,true);
    int firstUp=-1;
    for(int i=0;i<70;++i) {
        if(recovery.feed(.21f,.035f,true)==alert::GainAction::Up1 && firstUp<0)firstUp=i;
    }
    check(firstUp>=23,"no rapid gain rebound");
    std::cout<<"ADAPTIVE_GAIN_TEST_PASS\n";
}