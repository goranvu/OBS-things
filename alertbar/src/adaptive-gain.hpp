#pragma once
#include <array>
#include <algorithm>
#include <cmath>

namespace alert {
// Evaluate genuine PCM every ~250 ms. Positive control requires sustained,
// modulated low-level material: never treat silence or stable noise as speech.
enum class GainAction { None, Down3, Up2 };

inline float requestedGainDb(GainAction action,float currentDb,float initialDb,
                            float minDb,float maxDb) {
    if(action==GainAction::Down3)
        return std::max(std::max(minDb,initialDb-18.f),currentDb-3.f);
    if(action==GainAction::Up2)
        return std::min(std::min(maxDb,initialDb+18.f),currentDb+2.f);
    return currentDb;
}
class AdaptiveGain {
    static constexpr int HISTORY=16; // Four seconds, 250ms windows
    std::array<float,HISTORY> peaks{};
    std::array<bool,HISTORY> active{};
    int cursor=0,filled=0,hotWindows=0;
    int sinceAction=100;
    int afterReductionHold=0;
public:
    void reset(){*this=AdaptiveGain{};}
    GainAction feed(float peak,float rms,bool valid) {
        float safePeak=valid && std::isfinite(peak)?std::max(0.f,peak):0.f;
        float safeRms=valid && std::isfinite(rms)?std::max(0.f,rms):0.f;
        // Quiet talk often has peaks below 1.5%; previous threshold
        // prevented such talk from EVER being counted as audible.
        const bool audible=valid && safePeak>=.003f && safeRms>=.0006f;
        peaks[cursor]=safePeak;
        active[cursor]=audible;
        cursor=(cursor+1)%HISTORY;
        filled=std::min(filled+1,HISTORY);
        ++sinceAction;
        if(afterReductionHold>0)--afterReductionHold;

        // Existing clip protection stays as in the working version.
        hotWindows=(audible && safePeak>=.89125094f)?hotWindows+1:0;
        if(hotWindows>=8) {
            hotWindows=0;sinceAction=0;
            afterReductionHold=16; // wait 4s before recovering after loud section
            return GainAction::Down3;
        }
        if(filled<HISTORY || afterReductionHold>0 || sinceAction<12)
            return GainAction::None;
        int audibleWindows=0,tooHigh=0,quietPauses=0;
        float smallest=1.f,largest=0.f;
        for(int i=0;i<HISTORY;++i) {
            if(active[i]) {
                ++audibleWindows;
                if(peaks[i]>.55f)++tooHigh;
                smallest=std::min(smallest,peaks[i]);
                largest=std::max(largest,peaks[i]);
            }else ++quietPauses;
        }
        // A few-second, audible, QUIET and variable sound.
        // Require either changing envelope or word pauses to reject
        // unchanging ventilation/hum.
        const bool modulated=(smallest>.0f && largest>=smallest*1.6f)
                              || (quietPauses>=2 && audibleWindows>=8);
        if(audibleWindows>=8 && tooHigh<=1 && hotWindows==0 && modulated) {
            sinceAction=0;   // +2dB at most every 3 seconds
            return GainAction::Up2;
        }
        return GainAction::None;
    }
};
}
