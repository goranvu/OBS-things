#include "alert-logic.hpp"
#include "adaptive-gain.hpp"
#include <cstdlib>
#include <iostream>
#include <cmath>
void ck(bool ok,const char*name){if(!ok){std::cerr<<"FAIL "<<name<<"\n";std::exit(1);}}
int main() {
    using alert::GainAction;
    // A reduction requires 8 consecutive 250ms windows of high level.
    alert::AdaptiveGain clip;
    for(int i=0;i<7;++i)ck(clip.feed(.98f,.2f,true)==GainAction::None,"protect for full 2 seconds");
    ck(clip.feed(.98f,.2f,true)==GainAction::Down3,"2 sec sustained peaks reduce gain");
    // A brief clip and several separated peaks should never trigger lowering.
    alert::AdaptiveGain transient;
    for(int i=0;i<80;++i) {
        float peak=(i==10||i==11||i==25||i==26)?.99f:.55f;
        ck(transient.feed(peak,.12f,true)!=GainAction::Down3,"isolated short peaks ignored");
    }
    // No upward adjustment before twelve 250ms windows (three seconds).
    alert::AdaptiveGain quiet;
    int first=-1,boostCount=0;
    for(int i=0;i<65;++i) {
        const bool speaking=i%4!=0; // gaps between words
        const float peak=speaking?(i%3==0?.017f:.007f):0.f;
        const float rms=speaking?.0015f:0.f;
        GainAction action=quiet.feed(peak,rms,true);
        if(i<11)ck(action==GainAction::None,"no gain rise before three seconds");
        if(action==GainAction::Up2){if(first<0)first=i;++boostCount;}
    }
    ck(first==11,"first quiet gain increase at 3s");
    ck(boostCount>=3,"sustained quiet audio keeps increasing every 3s");
    alert::AdaptiveGain whisper;
    bool boosted=false;
    for(int i=0;i<50;++i) {
        const bool talk=i%5!=0;
        if(whisper.feed(talk?.0045f:0.f,talk?.0009f:0.f,true)==GainAction::Up2)boosted=true;
    }
    ck(boosted,"whisper recognized");
    alert::AdaptiveGain silent;
    for(int i=0;i<90;++i)ck(silent.feed(0.f,0.f,true)==GainAction::None,"silence never boosted");
    alert::AdaptiveGain noise;
    for(int i=0;i<90;++i)ck(noise.feed(.011f,.003f,true)==GainAction::None,"steady low noise never boosted");
    alert::AdaptiveGain reasonable;
    for(int i=0;i<90;++i)ck(reasonable.feed(.73f,.13f,true)==GainAction::None,"normal level not raised");
    // Sustained loud sound followed by quiet must not immediately pump up.
    alert::AdaptiveGain recovery;
    for(int i=0;i<8;++i)recovery.feed(.99f,.2f,true);
    int firstRecovery=-1;
    for(int i=0;i<75;++i) {
        const bool talk=i%4!=0;
        auto action=recovery.feed(talk?.03f:0.f,talk?.007f:0.f,true);
        if(action==GainAction::Up2&&firstRecovery<0)firstRecovery=i;
    }
    ck(firstRecovery>=11,"three second hold and history after reduction");
    ck(std::fabs(alert::requestedGainDb(GainAction::Up2,-10.f,-10.f,-60.f,12.f)-(-8.f))<.01f,
       "amplify past baseline");
    ck(std::fabs(alert::requestedGainDb(GainAction::Up2,7.f,-10.f,-60.f,12.f)-8.f)<.01f,
       "maximum +18dB above startup");
    ck(std::fabs(alert::requestedGainDb(GainAction::Down3,-26.f,-10.f,-60.f,12.f)-(-28.f))<.01f,
       "maximum -18dB below startup");
    std::cout<<"ADAPTIVE_GAIN_2S_DOWN_3S_UP_TEST_PASS\n";
}
