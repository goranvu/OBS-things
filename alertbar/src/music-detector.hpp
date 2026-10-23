#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numeric>
namespace music {
// A small, offline MUSIC-LIKE acoustic estimator, not a trained model.
// Uses tonal concentration, adjacent-spectrum coherence and sound continuity.
// Operates on mono samples resampled to 16 kHz, off the OBS/UI thread.
class MusicDetector {
    static constexpr int N=1024, H=512, History=64;
    std::array<float,N> pcm{};
    int filled=0;
    std::array<float,N/2> previous{};
    bool hasPrevious=false;
    std::array<int,History> votes{};
    std::array<int,History> active{};
    int index=0, collected=0, silent=0;
    bool state=false;
    static void fft(std::array<std::complex<float>,N>& a) {
        for(int i=1,j=0;i<N;++i) {
            int bit=N>>1;
            for(;j&bit;bit>>=1)j^=bit;
            j^=bit;
            if(i<j)std::swap(a[i],a[j]);
        }
        constexpr float pi=3.14159265358979323846f;
        for(int len=2;len<=N;len<<=1) {
            float angle=-2*pi/len;
            std::complex<float> step(std::cos(angle),std::sin(angle));
            for(int i=0;i<N;i+=len) {
                std::complex<float> w(1,0);
                for(int j=0;j<len/2;++j) {
                    auto u=a[i+j],v=a[i+j+len/2]*w;
                    a[i+j]=u+v;a[i+j+len/2]=u-v;
                    w*=step;
                }
            }
        }
    }
    void frame() {
        double energy=0;
        for(float f:pcm)energy+=double(f)*f;
        const float rms=std::sqrt(energy/N);
        if(rms<0.006f) {
            hasPrevious=false;
            ++silent;
            push(0,0);
            if(silent>=14)state=false; // ~0.45 seconds silence
            return;
        }
        silent=0;
        std::array<std::complex<float>,N> fftData{};
        constexpr float pi=3.14159265358979323846f;
        for(int i=0;i<N;i++) {
            float w=.5f-.5f*std::cos(2*pi*i/(N-1));
            fftData[i]={pcm[i]*w,0};
        }
        fft(fftData);
        std::array<float,N/2> amps{};
        double total=0, maxMag=0, simNum=0, simA=0, simB=0;
        for(int k=5;k<255;++k) {
            float a=std::abs(fftData[k]);
            amps[k]=a;
            total+=double(a)*a;
            maxMag=std::max(maxMag,double(a)*a);
            if(hasPrevious) {
                simNum+=double(a)*previous[k];
                simA+=double(a)*a;
                simB+=double(previous[k])*previous[k];
            }
        }
        // Stationary instruments retain a similar spectrum between windows.
        // Formants and pitch in speech normally change faster.
        double similarity=hasPrevious && simA>0 && simB>0
            ? simNum/std::sqrt(simA*simB):0;
        // A narrowband tone/chord has energy in a few narrow FFT bins.
        double peakFraction=total>0?maxMag/total:0;
        // Preserve normalized amplitude spectrum between frames.
        previous=amps;hasPrevious=true;
        // These thresholds are deliberately conservative: default to narrow.
        bool tonal=peakFraction>.095;
        bool steady=similarity>.87;
        push((tonal&&steady)?1:0,1);
    }
    void push(int musicVote,int nonSilent) {
        votes[index]=musicVote;
        active[index]=nonSilent;
        index=(index+1)%History;
        collected=std::min(collected+1,History);
        int heard=0,good=0;
        for(int j=0;j<collected;++j){heard+=active[j];good+=votes[j];}
        // At least ~1.15 seconds of recent audio and stable music-like features.
        if(!state && collected>=38 && heard>=35 && good>=28 && good*100>=heard*66)
            state=true;
        if(state && (silent>=14 || (collected>=38 && good*100<heard*34)))
            state=false;
    }
public:
    void consume(float f) {
        if(!std::isfinite(f))f=0;
        pcm[filled++]=std::clamp(f,-1.f,1.f);
        if(filled==N) {
            frame();
            std::move(pcm.begin()+H,pcm.end(),pcm.begin());
            filled=H;
        }
    }
    bool music()const{return state;}
    void reset(){*this=MusicDetector{};}
};
}
