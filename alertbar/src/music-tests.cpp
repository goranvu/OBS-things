#include "music-detector.hpp"
#include "alert-logic.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
#include <random>
void tone(music::MusicDetector&d,double sec){
 const double pi=3.14159265358979323846;
 for(int i=0;i<int(sec*16000);++i) {
  double t=i/16000.;
  float f=float(.12*std::sin(2*pi*220*t)+.09*std::sin(2*pi*330*t)+.08*std::sin(2*pi*440*t));
  d.consume(f);
 }
}
void silence(music::MusicDetector&d,double sec){
 for(int i=0;i<int(sec*16000);++i)d.consume(0);
}
int main(){
 music::MusicDetector d;
 silence(d,2);assert(!d.music());
 tone(d,3);assert(d.music());
 silence(d,1);assert(!d.music());
 music::MusicDetector noise;
 std::mt19937 mt(17);
 std::uniform_real_distribution<float> dist(-.15,.15);
 for(int i=0;i<16000*3;++i)noise.consume(dist(mt));
 assert(!noise.music());
 std::cout<<"Music detector: sine chord -> music, silence/noise -> normal PASS\n";
}
