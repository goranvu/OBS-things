#pragma once
#include <array>
#include <algorithm>
#include <cmath>
namespace alert {
// Decisions are made once per 250ms window, using real PCM from Line In.
// A full window of silence does not count as quiet "speech": this avoids
// raising gain while nothing is happening.
enum class GainAction { None, Down3, Up1 };
class AdaptiveGain {
    static constexpr int HISTORY=24; // 6 seconds
    std::array<float,HISTORY> peaks{};
    std::array<bool,HISTORY> active{};
    int cursor=0,filled=0;
    int hotWindows=0;
    int windowAfterAdjustment=100;
    int reduceHold=0;
public:
    void reset() {*this=AdaptiveGain{};}
    GainAction feed(float peak,float rms,bool valid) {
        const float safePeak=valid&&std::isfinite(peak)?std::max(0.f,peak):0.f;
        const float safeRms=valid&&std::isfinite(rms)?std::max(0.f,rms):0.f;
        const bool audible=valid && safePeak>=.015f && safeRms>=.0035f;
        peaks[cursor]=safePeak;
        active[cursor]=audible;
        cursor=(cursor+1)%HISTORY;
        filled=std::min(HISTORY,filled+1);
        ++windowAfterAdjustment;
        if(reduceHold>0)--reduceHold;
        if(audible && safePeak>=.89125094f)++hotWindows;
        else hotWindows=0;
        if(hotWindows>=8) {
            hotWindows=0;
            windowAfterAdjustment=0;
            reduceHold=24;  // do not pump gain upward immediately after music
            return GainAction::Down3;
        }
        if(filled<HISTORY || reduceHold>0 || windowAfterAdjustment<12)
            return GainAction::None;
        int audibleWindows=0,loudWindows=0;
        for(int i=0;i<HISTORY;++i) {
            if(active[i]) {
                ++audibleWindows;
                if(peaks[i]>.65f)++loudWindows;
            }
        }
        // Sustained quiet material, allowing typical pauses between words.
        // Require ~2.5 seconds of real sound within the last six seconds.
        if(audibleWindows>=10 && loudWindows<=1 && hotWindows==0) {
            windowAfterAdjustment=0; // at most +1 dB per 3 seconds
            return GainAction::Up1;
        }
        return GainAction::None;
    }
};
}
