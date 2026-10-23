#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/bmem.h>
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <QHBoxLayout>
#include <QThread>
#include <atomic>
#include <memory>
#include <cstring>
#include <chrono>
#include <QByteArray>
#include "music-detector.hpp"
#include <QVBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPainter>
#include <QPointer>
#include <QTimer>
#include <QElapsedTimer>
#include <QSettings>
#include <QWidget>
#include <QDir>
#include <algorithm>
#include <cmath>
#include <vector>
#include <cstdint>
#include "alert-logic.hpp"
using Microsoft::WRL::ComPtr;
OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-linein-alert","en-US")
namespace {
class AlertBar:public QWidget {
    alert::Detector detector;
    bool online=false;
    bool activeMusic=false;
    int64_t now=0;
public:
    explicit AlertBar(QWidget*p):QWidget(p){
        setObjectName("obs_linein_alert_bar");
        setFixedHeight(4);
        setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    void feed(float peak,int64_t time){
        online=true;now=time;detector.update(peak,time);update();
    }
    void setMusic(bool music){if(activeMusic!=music){activeMusic=music;setFixedHeight(music?13:4);update();}}
    void offline(){ online=false;detector.reset();setMusic(false);update(); }
    void paintEvent(QPaintEvent*)override {
        QPainter p(this);
        p.fillRect(rect(),QColor(43,44,46));
        if(!online)return;
        int w=std::max(0,width()-2),h=std::max(1,height()-2);
        int v=std::clamp((int)std::lround(w*alert::fraction(detector.db())),0,w);
        int g=std::clamp((int)std::lround(w*alert::fraction(alert::kYellowDb)),0,w);
        int y=std::clamp((int)std::lround(w*alert::fraction(alert::kRedDb)),0,w);
        if(v>0)p.fillRect(1,1,std::min(v,g),h,QColor(42,176,101));
        if(v>g)p.fillRect(g+1,1,std::min(v,y)-g,h,QColor(234,176,44));
        if(v>y)p.fillRect(y+1,1,v-y,h,QColor(231,69,60));
        if(detector.blink(now)){
            p.fillRect(1,1,w,h,QColor(255,49,49,165));
            p.setPen(QPen(QColor(255,110,110),1));
            p.drawRect(0,0,width()-1,height()-1);
        }
    }
};
struct ComInit {
    HRESULT hr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    bool owned=hr==S_OK||hr==S_FALSE;
    ~ComInit(){if(owned)CoUninitialize();}
    bool ok()const{return SUCCEEDED(hr)||hr==RPC_E_CHANGED_MODE;}
};

class MusicCapture: public QThread {
    std::wstring deviceId;
    static constexpr GUID pcmGuid={0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
    static constexpr GUID floatGuid={0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
    static float sample(const BYTE*p,int kind){
        if(kind==1){float x=0;std::memcpy(&x,p,4);return std::isfinite(x)?x:0;}
        if(kind==2){int16_t v=0;std::memcpy(&v,p,2);return float(v)/32768.f;}
        if(kind==3){int32_t v=p[0]|(int32_t(p[1])<<8)|(int32_t(p[2])<<16);
                    if(v&0x800000)v|=~0xffffff;return float(v)/8388608.f;}
        if(kind==4){int32_t v=0;std::memcpy(&v,p,4);return float(double(v)/2147483648.);}
        return 0;
    }
public:
    std::atomic<bool> stopFlag{false}, isMusic{false}, isReady{false};
    explicit MusicCapture(const QString&id):deviceId(id.toStdWString()){}
protected:
    void run() override {
        HRESULT init=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        if(FAILED(init)){blog(LOG_WARNING,"[Line In Alert] Music audio: COM initialization FAILED (0x%08lx).",(unsigned long)init);return;}
        auto cleanup=[](){CoUninitialize();};
        auto fail=[&](const char*stage,HRESULT hr){
            blog(LOG_WARNING,"[Line In Alert] Music audio FAILED at %s: HRESULT 0x%08lx",stage,(unsigned long)hr);
        };
        do {
            ComPtr<IMMDeviceEnumerator> en;
            HRESULT hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&en));
            if(FAILED(hr)){fail("Enumerator",hr);break;}
            ComPtr<IMMDevice> d;
            hr=en->GetDevice(deviceId.c_str(),&d);
            if(FAILED(hr)){fail("GetDevice",hr);break;}
            ComPtr<IAudioClient> client;
            hr=d->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(client.GetAddressOf()));
            if(FAILED(hr)){fail("Activate IAudioClient",hr);break;}
            WAVEFORMATEX*wf=nullptr;
            hr=client->GetMixFormat(&wf);
            if(FAILED(hr)||!wf){fail("GetMixFormat",hr);break;}
            int channels=wf->nChannels,bits=wf->wBitsPerSample;
            int rate=(int)wf->nSamplesPerSec,block=wf->nBlockAlign;
            bool fp=wf->wFormatTag==WAVE_FORMAT_IEEE_FLOAT;
            bool pcm=wf->wFormatTag==WAVE_FORMAT_PCM;
            if(wf->wFormatTag==WAVE_FORMAT_EXTENSIBLE &&
               wf->cbSize>=sizeof(WAVEFORMATEXTENSIBLE)-sizeof(WAVEFORMATEX)) {
                auto *ext=reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wf);
                fp=IsEqualGUID(ext->SubFormat,floatGuid);
                pcm=IsEqualGUID(ext->SubFormat,pcmGuid);
            }
            int kind=fp&&bits==32?1:pcm&&bits==16?2:pcm&&bits==24?3:pcm&&bits==32?4:0;
            if(channels<1||channels>32||rate<8000||rate>192000||kind==0||
               block<channels*(bits/8)){
                blog(LOG_WARNING,"[Line In Alert] Unsupported input format for music detector.");
                CoTaskMemFree(wf);break;
            }
            blog(LOG_INFO,"[Line In Alert] Music audio requested format: %d Hz, %d ch, %d bit, encoding %d",
                rate,channels,bits,kind);
            hr=client->Initialize(AUDCLNT_SHAREMODE_SHARED,0,1000000,0,wf,nullptr);
            CoTaskMemFree(wf);
            if(FAILED(hr)){
                blog(LOG_WARNING,"[Line In Alert] WASAPI shared capture unavailable: 0x%08lx.",(unsigned long)hr);
                break;
            }
            ComPtr<IAudioCaptureClient> capture;
            hr=client->GetService(IID_PPV_ARGS(&capture));
            if(FAILED(hr)){fail("GetService Capture",hr);break;}
            hr=client->Start();
            if(FAILED(hr)){fail("Start recording",hr);break;}
            blog(LOG_INFO,"[Line In Alert] Music classifier started on Line In.");
            isReady=true;
            music::MusicDetector detector;
            double phase=0;
            auto lastPacket=std::chrono::steady_clock::now();
            auto nextReport=lastPacket+std::chrono::seconds(8);
            uint64_t totalFrames=0;
            float maxAmplitude=0;
            while(!stopFlag) {
                UINT32 packet=0;
                HRESULT step=capture->GetNextPacketSize(&packet);
                if(FAILED(step)){fail("GetNextPacketSize",step);break;}
                if(packet==0) {
                    // Input disconnected or silent: never retain music state indefinitely.
                    if(std::chrono::steady_clock::now()-lastPacket>std::chrono::milliseconds(700)){
                        isMusic=false;
                    }
                    QThread::msleep(20);
                    continue;
                }
                do {
                    BYTE*ptr=nullptr;UINT32 frames=0;DWORD flags=0;
                    if(FAILED(capture->GetBuffer(&ptr,&frames,&flags,nullptr,nullptr))){packet=0;break;}
                    const bool silent=(flags&AUDCLNT_BUFFERFLAGS_SILENT)!=0;
                    int stride=bits/8;
                    for(UINT32 f=0;f<frames;f++) {
                        float mono=0;
                        if(!silent && ptr) {
                            const BYTE*at=ptr+size_t(f)*block;
                            for(int ch=0;ch<std::min(channels,2);++ch)
                                mono+=sample(at+ch*stride,kind);
                            mono/=float(std::min(channels,2));
                        }
                        maxAmplitude=std::max(maxAmplitude,std::fabs(mono));
                        phase+=16000.0/rate;
                        if(phase>=1.0){detector.consume(mono);phase-=1.0;}
                    }
                    capture->ReleaseBuffer(frames);
                    totalFrames+=frames;
                    lastPacket=std::chrono::steady_clock::now();
                    isMusic=detector.music();
                    if(FAILED(capture->GetNextPacketSize(&packet))){packet=0;break;}
                }while(packet>0&&!stopFlag);
                auto now=std::chrono::steady_clock::now();
                if(now>=nextReport){
                    blog(LOG_INFO,"[Line In Alert] Music diagnostic: frames=%llu, peak=%.4f, tonalVotes=%d%%, classified=%s",
                        (unsigned long long)totalFrames,maxAmplitude,detector.evidencePercent(),
                        detector.music()?"MUSIC":"NOT_MUSIC");
                    nextReport=now+std::chrono::seconds(8);
                    totalFrames=0;maxAmplitude=0;
                }
            }
            isMusic=false;isReady=false;
            client->Stop();
        }while(false);
        isReady=false;isMusic=false;
        cleanup();
    }
};

class Controller:public QObject {
    QMainWindow *main;
    ComInit com;
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> dev;
    ComPtr<IAudioMeterInformation> meter;
    std::unique_ptr<MusicCapture> musicCapture;
    QPointer<AlertBar> bar;
    std::vector<float> samples;
    QTimer timer;
    QElapsedTimer clock;
    qint64 lastScan=-10000;
    QString settingsPath;
    QString monitoredDeviceId;
    qint64 lastCaptureAttempt=0;
    bool warned=false;
public:
    explicit Controller(QMainWindow*w):QObject(w),main(w) {
        char*path=obs_module_config_path("settings.ini");
        if(path){settingsPath=QString::fromUtf8(path);bfree(path);}
        if(com.ok())CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&en));
        clock.start();
        connect(&timer,&QTimer::timeout,this,[this](){tick();});
        timer.start(40);
        QTimer::singleShot(0,this,[this](){attach();});
    }
    ~Controller()override{
        timer.stop();
        if(bar){delete bar.data();bar.clear();}
        if(musicCapture){musicCapture->stopFlag=true;musicCapture->wait();musicCapture.reset();}
        meter.Reset();dev.Reset();en.Reset();
    }
    void attach(){
        if(bar)return;
        auto*layout=main->findChild<QHBoxLayout*>("previewLayout");
        if(!layout)return;
        for(int i=0;i<layout->count();i++){
            QWidget *container=layout->itemAt(i)->widget();
            if(!container)continue;
            auto*box=qobject_cast<QVBoxLayout*>(container->layout());
            if(!box||box->count()<2)continue;
            auto*label=qobject_cast<QLabel*>(box->itemAt(0)->widget());
            auto*display=box->itemAt(1)->widget();
            if(!label||!display)continue;
            if(label->property("class").toString()!="label-preview-title")continue;
            if(QString::fromLatin1(display->metaObject()->className())!="OBSQTDisplay")continue;
            bar=new AlertBar(container);
            box->insertWidget(2,bar);
            blog(LOG_INFO,"[Line In Alert] Attached to Program display.");
            return;
        }
    }
    void searchDevice(){
        lastScan=clock.elapsed();
        if(musicCapture){musicCapture->stopFlag=true;musicCapture->wait();musicCapture.reset();}
        if(!en)return;
        ComPtr<IMMDeviceCollection> list;
        if(FAILED(en->EnumAudioEndpoints(eCapture,DEVICE_STATE_ACTIVE,&list)))return;
        UINT n=0;list->GetCount(&n);
        std::vector<QString> line;
        std::vector<QString> all;
        for(UINT i=0;i<n;i++){
            ComPtr<IMMDevice>d;if(FAILED(list->Item(i,&d)))continue;
            LPWSTR uid=nullptr;
            if(FAILED(d->GetId(&uid))||!uid)continue;
            QString id=QString::fromWCharArray(uid);
            CoTaskMemFree(uid);
            all.push_back(id);
            ComPtr<IPropertyStore>ps;
            QString name;
            if(SUCCEEDED(d->OpenPropertyStore(STGM_READ,&ps))){
                PROPVARIANT pv;PropVariantInit(&pv);
                if(SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName,&pv)) &&
                   pv.vt==VT_LPWSTR&&pv.pwszVal)name=QString::fromWCharArray(pv.pwszVal);
                PropVariantClear(&pv);
            }
            QString lowered=name.toLower();
            QByteArray labelUtf8=name.toUtf8();
            blog(LOG_INFO,"[Line In Alert] Windows recording endpoint: %s",labelUtf8.constData());
            if(lowered.contains("line in")||lowered.contains("line-in")||
               lowered.contains("line input")||lowered.contains("linijski ulaz"))
                line.push_back(id);
        }
        QString chosen;
        if(!settingsPath.isEmpty()){
            QSettings s(settingsPath,QSettings::IniFormat);
            QString overrideId=s.value("device_id").toString();
            for(const auto&id:all)if(id==overrideId)chosen=id;
        }
        if(chosen.isEmpty()&&line.size()==1)chosen=line[0];
        if(chosen.isEmpty()){
            if(!warned){blog(LOG_WARNING,"[Line In Alert] Need exactly one Windows Recording Line In; none selected. Read endpoint names above.");warned=true;}
            return;
        }
        ComPtr<IMMDevice> newDev;
        if(FAILED(en->GetDevice(reinterpret_cast<LPCWSTR>(chosen.utf16()),&newDev)))return;
        ComPtr<IAudioMeterInformation> newMeter;
        if(FAILED(newDev->Activate(__uuidof(IAudioMeterInformation),CLSCTX_ALL,nullptr,
            reinterpret_cast<void**>(newMeter.GetAddressOf()))))return;
        UINT count=0;
        if(FAILED(newMeter->GetMeteringChannelCount(&count))||count<1||count>32)return;
        dev=newDev;meter=newMeter;samples.assign(count,0.0f);
        blog(LOG_INFO,"[Line In Alert] Starting PCM music classifier for selected Line In.");
        monitoredDeviceId=chosen;
        lastCaptureAttempt=clock.elapsed();
        musicCapture=std::make_unique<MusicCapture>(chosen);
        musicCapture->start();
        blog(LOG_INFO,"[Line In Alert] Capture endpoint monitored (%u channels).",count);
    }
    void tick(){
        if(!bar)attach();
        if(!meter&&clock.elapsed()-lastScan>=3000)searchDevice();
        if(!meter)return;
        if(FAILED(meter->GetChannelsPeakValues((UINT)samples.size(),samples.data()))){
            if(musicCapture){musicCapture->stopFlag=true;musicCapture->wait();musicCapture.reset();}
            meter.Reset();dev.Reset();samples.clear();
            if(bar)bar->offline();
            lastScan=clock.elapsed();return;
        }
        float peak=0;
        for(float s:samples)if(std::isfinite(s))peak=std::max(peak,s);
        if(musicCapture && !musicCapture->isRunning() &&
           clock.elapsed()-lastCaptureAttempt>12000 && !monitoredDeviceId.isEmpty()) {
            blog(LOG_INFO,"[Line In Alert] Retrying WASAPI music capture after startup failure.");
            musicCapture->wait();
            musicCapture=std::make_unique<MusicCapture>(monitoredDeviceId);
            lastCaptureAttempt=clock.elapsed();
            musicCapture->start();
        }
        if(bar){bar->feed(peak,clock.elapsed());
            bar->setMusic(musicCapture && musicCapture->isReady && musicCapture->isMusic);
        }
    }
};
QPointer<Controller> controller;
bool exiting=false;
void begin(){
    if(controller||exiting)return;
    if(auto*mw=static_cast<QMainWindow*>(obs_frontend_get_main_window()))controller=new Controller(mw);
}
void eventCallback(enum obs_frontend_event event,void*){
    if(event==OBS_FRONTEND_EVENT_FINISHED_LOADING){
        auto*mw=static_cast<QMainWindow*>(obs_frontend_get_main_window());
        if(mw)QTimer::singleShot(0,mw,[](){begin();});
    }else if(event==OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED){
        if(controller)QTimer::singleShot(0,controller.data(),[](){if(controller)controller->attach();});
    }else if(event==OBS_FRONTEND_EVENT_EXIT){
        exiting=true;delete controller.data();controller.clear();
    }
}
}
extern "C" const char*obs_module_description(void){
    return "Minimal flashing Line In peak alert bar beneath Program display.";
}
extern "C" bool obs_module_load(void){
    obs_frontend_add_event_callback(eventCallback,nullptr);
    blog(LOG_INFO,"[Line In Alert] Native plugin v1.2 loaded (music detection diagnostics).");return true;
}
extern "C" void obs_module_unload(void){
    obs_frontend_remove_event_callback(eventCallback,nullptr);
    delete controller.data();controller.clear();
}
