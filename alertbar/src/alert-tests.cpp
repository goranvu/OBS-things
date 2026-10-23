#include "alert-logic.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
int main(){
using namespace alert;
assert(dbfs(0)==-60);assert(std::fabs(dbfs(.5f)+6.0206f)<.002f);
assert(fraction(-60)==0&&fraction(0)==1);
Detector d;d.update(.8f,100);assert(d.db()>-3);
bool flash=false;for(int i=100;i<400;i+=10)flash|=d.blink(i);assert(flash);
for(int i=0;i<80;i++)d.update(0,600+i*40);assert(d.db()<-50);
std::cout<<"ALERT_TEST_PASS\n";
}
