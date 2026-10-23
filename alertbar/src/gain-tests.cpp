#include "alert-logic.hpp"
#include <cstdlib>
#include <iostream>
#include <cmath>
void check(bool ok,const char*which){if(!ok){std::cerr<<"FAIL: "<<which<<"\n";std::exit(1);}}
int main(){
    // Continual high peaks: fires no earlier than two full seconds.
    alert::ClipGuard a;
    double fireTime=-1;
    for(int i=0;i<150;++i){
        double t=i*.02;
        if(a.feed(t,.99f,true)&&fireTime<0)fireTime=t;
    }
    check(fireTime>=2.0&&fireTime<2.3,"2-second threshold");
    // Isolated transient cannot trigger the gain change.
    alert::ClipGuard b;bool triggered=false;
    for(int i=0;i<300;i++){
        float p=(i>=10&&i<12)?1.f:.2f;
        triggered|=b.feed(i*.02,p,true);
    }
    check(!triggered,"isolated clip");
    // Missing packets (silence) must break sequence.
    alert::ClipGuard c;triggered=false;
    for(int i=0;i<300;i++){
        bool packet=(i<40||i>55);
        triggered|=c.feed(i*.02,.99f,packet);
        if(i==100)break;
    }
    check(!triggered,"silence interrupts clip streak");
    // A fresh event must re-arm when reset is called after reduction.
    alert::ClipGuard d;int fires=0;
    for(int i=0;i<300;i++){
        if(d.feed(i*.02,.99f,true)){++fires;d.reset();}
    }
    check(fires==2,"3dB steps every 2 seconds");
    std::cout<<"AUTO_GAIN_TEST_PASS\n";
}
