#include "alert-logic.hpp"
#include "adaptive-gain.hpp"
#include <cstdlib>
#include <iostream>
#include <cmath>
void ck(bool ok,const char*name){if(!ok){std::cerr<<"FAIL "<<name<<"\n";std::exit(1);}}
int main() {
    alert::AdaptiveGain hot;
    for(int i=0;i<7;++i) ck(hot.feed(.98f,.2f,true)==alert::GainAction::None,"2 sec hot guard");
    ck(hot.feed(.98f,.2f,true)==alert::GainAction::Down3,"hot triggers down3");
    alert::AdaptiveGain quiet;
    int first=-1,count=0;
    for(int i=0;i<60;i++) {
        bool voice=i%4!=0;
        float peak=voice ? ((i%3==0)? .017f : .007f) : .0f;
        float rms=voice?.0015f:0;
        auto act=quiet.feed(peak,rms,true);
        if(act==alert::GainAction::Up2) {if(first<0)first=i; ++count;}
    }
    ck(first>=15 && first<=18,"quiet voice raises after four seconds");
    ck(count>=2,"quiet voice can raise repeatedly");
    alert::AdaptiveGain subtle;
    bool rises=false;
    for(int i=0;i<56;++i){
        bool talk=i%5!=0;
        auto act=subtle.feed(talk?.0045f:.0f,talk?.0009f:.0f,true);
        if(act==alert::GainAction::Up2)rises=true;
    }
    ck(rises,"very quiet speech detectable");
    alert::AdaptiveGain silence;
    for(int i=0;i<90;++i)
        ck(silence.feed(0,0,true)==alert::GainAction::None,"never boost silence");
    alert::AdaptiveGain noise;
    for(int i=0;i<90;++i)
        ck(noise.feed(.011f,.003f,true)==alert::GainAction::None,"never boost steady noise");
    alert::AdaptiveGain adequate;
    for(int i=0;i<90;++i)
        ck(adequate.feed(.73f,.13f,true)==alert::GainAction::None,"no increase near strong level");
    alert::AdaptiveGain transients;
    for(int i=0;i<90;++i)
        ck(transients.feed(i==9?.98f:.72f,.13f,true)!=alert::GainAction::Down3,"single peak not clipped");
    alert::AdaptiveGain rebound;
    for(int i=0;i<8;++i)rebound.feed(.99f,.2f,true);
    int firstRecovery=-1;
    for(int i=0;i<90;++i){
        auto act=rebound.feed(i%4?.03f:0.f,i%4?.007f:0.f,true);
        if(act==alert::GainAction::Up2 && firstRecovery<0)firstRecovery=i;
    }
    ck(firstRecovery>=15,"no immediate rebound after music");
    using alert::GainAction;
    ck(std::abs(alert::requestedGainDb(GainAction::Up2,-10.f,-10.f,-60.f,12.f)-(-8.f))<.01f,
       "increase above startup gain, regression fix");
    ck(std::abs(alert::requestedGainDb(GainAction::Up2,7.f,-10.f,-60.f,12.f)-8.f)<.01f,
       "+18 dB ceiling");
    ck(std::abs(alert::requestedGainDb(GainAction::Up2,0.f,-10.f,-60.f,0.f))<.01f,
       "respect Windows hardware maximum");
    ck(std::abs(alert::requestedGainDb(GainAction::Down3,-26.f,-10.f,-60.f,12.f)-(-28.f))<.01f,
       "-18dB floor");
    std::cout<<"ADAPTIVE_GAIN_UP_DOWN_TEST_PASS\n";
}
