// OBS Tools-menu automatic Windows Recording gain (no UI bar).
// GPL-2.0-or-later. Windows Core Audio (WASAPI) + Qt.
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/bmem.h>
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <QAction>
#include <QMainWindow>
#include <QPointer>
#include <QTimer>
#include <QElapsedTimer>
#include <QSettings>
#include <QThread>
#include <QByteArray>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "adaptive-gain.hpp"

using Microsoft::WRL::ComPtr;
OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-linein-alert","en-US")

namespace {
struct ComInit {
    HRESULT hr;
    bool owned;
    explicit ComInit(DWORD mode) : hr(CoInitializeEx(nullptr,mode)),
        owned(hr==S_OK||hr==S_FALSE) {}
    ~ComInit() { if(owned) CoUninitialize(); }
    bool ok()const { return SUCCEEDED(hr)||hr==RPC_E_CHANGED_MODE; }
};

// No audio output is touched. This captures the SAME Windows Recording Line In
// independently in shared mode; it never selects Desktop Audio or microphone.
class AutoGainThread final:public QThread {
    const std::wstring endpointId;
    static constexpr GUID pcmGuid={0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
    static constexpr GUID floatGuid={0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
    static float absoluteSample(const BYTE*p,int kind) {
        if(kind==1) { float v=0;std::memcpy(&v,p,4);return std::isfinite(v)?std::fabs(v):0; }
        if(kind==2) { int16_t v=0;std::memcpy(&v,p,2);return (float)std::fabs((double)v/32768.0); }
        if(kind==3) { int32_t v=(int32_t)p[0]|((int32_t)p[1]<<8)|((int32_t)p[2]<<16);
            if(v&0x800000)v|=~0xffffff;return (float)std::fabs((double)v/8388608.0); }
        if(kind==4) { int32_t v=0;std::memcpy(&v,p,4);return (float)std::fabs((double)v/2147483648.0); }
        return 0;
    }
    void failed(const char*where,HRESULT hr) {
        blog(LOG_WARNING,"[Line In Alert] Auto unavailable: %s (HRESULT 0x%08lx).",where,(unsigned long)hr);
    }
public:
    std::atomic<bool> stopFlag{false};
    std::atomic<bool> autoEnabled{true}; // Always ON when OBS is started.
    std::atomic<bool> ready{false};       // true only if real PCM and gain API work.
    std::atomic<int> reductionDb{0};
    explicit AutoGainThread(const QString&id):endpointId(id.toStdWString()) {}
protected:
    void run()override {
        ComInit com(COINIT_MULTITHREADED);
        if(!com.ok()) { failed("COM initialization",com.hr);return; }
        do {
            ComPtr<IMMDeviceEnumerator> enumerator;
            HRESULT hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator));
            if(FAILED(hr)) {failed("device enumerator",hr);break;}
            ComPtr<IMMDevice> device;
            hr=enumerator->GetDevice(endpointId.c_str(),&device);
            if(FAILED(hr)) {failed("Line In device",hr);break;}
            ComPtr<IAudioEndpointVolume> volume;
            hr=device->Activate(__uuidof(IAudioEndpointVolume),CLSCTX_ALL,nullptr,
                                reinterpret_cast<void**>(volume.GetAddressOf()));
            if(FAILED(hr)) {failed("Windows Recording volume",hr);break;}
            float low=0,high=0,step=0;
            hr=volume->GetVolumeRange(&low,&high,&step);
            if(FAILED(hr)||!std::isfinite(low)||!std::isfinite(high)||high-low<1.0f) {
                failed("Windows Recording volume range",hr);break;
            }
            DWORD hardwareFlags=0;
            if(SUCCEEDED(volume->QueryHardwareSupport(&hardwareFlags))) {
                blog(LOG_INFO,"[Line In Alert] Windows Line In gain: %s; range %.1f to %.1f dB.",
                    (hardwareFlags&ENDPOINT_HARDWARE_SUPPORT_VOLUME)?"hardware":"software",
                    low,high);
            }
            ComPtr<IAudioClient> client;
            hr=device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,
                                reinterpret_cast<void**>(client.GetAddressOf()));
            if(FAILED(hr)) {failed("WASAPI capture activation",hr);break;}
            WAVEFORMATEX*wf=nullptr;
            hr=client->GetMixFormat(&wf);
            if(FAILED(hr)||!wf) {failed("WASAPI mix format",hr);break;}
            const int channels=wf->nChannels;
            const int bits=wf->wBitsPerSample;
            const int block=wf->nBlockAlign;
            bool floating=wf->wFormatTag==WAVE_FORMAT_IEEE_FLOAT;
            bool pcm=wf->wFormatTag==WAVE_FORMAT_PCM;
            if(wf->wFormatTag==WAVE_FORMAT_EXTENSIBLE &&
               wf->cbSize>=sizeof(WAVEFORMATEXTENSIBLE)-sizeof(WAVEFORMATEX)) {
                auto*ext=reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wf);
                floating=IsEqualGUID(ext->SubFormat,floatGuid);
                pcm=IsEqualGUID(ext->SubFormat,pcmGuid);
            }
            const int kind=floating&&bits==32?1:pcm&&bits==16?2:pcm&&bits==24?3:pcm&&bits==32?4:0;
            if(channels<1||channels>32||kind==0||block<channels*(bits/8)) {
                CoTaskMemFree(wf);
                blog(LOG_WARNING,"[Line In Alert] Auto unavailable: unsupported Line In sample format.");
                break;
            }
            hr=client->Initialize(AUDCLNT_SHAREMODE_SHARED,0,1000000,0,wf,nullptr);
            CoTaskMemFree(wf);
            if(FAILED(hr)) {failed("WASAPI shared input",hr);break;}
            ComPtr<IAudioCaptureClient> capture;
            hr=client->GetService(IID_PPV_ARGS(&capture));
            if(FAILED(hr)) {failed("WASAPI capture service",hr);break;}
            hr=client->Start();
            if(FAILED(hr)) {failed("WASAPI capture start",hr);break;}
            ready=true;
            blog(LOG_INFO,"[Line In Alert] AUTO gain v3.1 ready: DOWN -3 dB after 2s sustained high level; UP +2 dB after 3s sustained quiet audio; +/-18 dB safety window.");
            alert::AdaptiveGain logic;
            using Clock=std::chrono::steady_clock;
            auto slotStart=Clock::now();
            bool lastEnabled=false, baselineKnown=false;
            float baselineDb=0;
            double sumSquares=0;
            uint64_t samples=0;
            float slotPeak=0;
            auto lastReport=Clock::now();
            while(!stopFlag.load()) {
                const bool enabled=autoEnabled.load();
                if(!enabled || !lastEnabled)logic.reset();
                if(enabled && !baselineKnown) {
                    HRESULT getHr=volume->GetMasterVolumeLevel(&baselineDb);
                    if(SUCCEEDED(getHr)) {
                        baselineKnown=true;
                        blog(LOG_INFO,"[Line In Alert] AUTO initial Windows Line In: %.2f dB; adaptive gain can raise up to 18 dB above this (and Windows maximum).",baselineDb);
                    } else {
                        failed("Read initial Windows Line In gain",getHr);
                    }
                }
                lastEnabled=enabled;
                UINT32 packet=0;
                hr=capture->GetNextPacketSize(&packet);
                if(FAILED(hr)) {failed("WASAPI next packet",hr);break;}
                while(packet>0&&!stopFlag.load()) {
                    BYTE*buffer=nullptr;
                    UINT32 frameCount=0;
                    DWORD flags=0;
                    hr=capture->GetBuffer(&buffer,&frameCount,&flags,nullptr,nullptr);
                    if(FAILED(hr)) {failed("WASAPI read",hr);break;}
                    if(buffer && !(flags&AUDCLNT_BUFFERFLAGS_SILENT)) {
                        const int bytesPerSample=bits/8;
                        for(UINT32 f=0;f<frameCount;++f) {
                            const BYTE*frame=buffer+(size_t)f*block;
                            for(int ch=0;ch<channels;++ch) {
                                const float amplitude=absoluteSample(frame+ch*bytesPerSample,kind);
                                slotPeak=std::max(slotPeak,amplitude);
                                sumSquares+=double(amplitude)*amplitude;
                                ++samples;
                            }
                        }
                    } else {
                        samples+=(uint64_t)frameCount*channels; // silent PCM frames
                    }
                    capture->ReleaseBuffer(frameCount);
                    hr=capture->GetNextPacketSize(&packet);
                    if(FAILED(hr)) {failed("WASAPI next packet",hr);break;}
                }
                if(FAILED(hr))break;
                const auto now=Clock::now();
                if(now-slotStart>=std::chrono::milliseconds(250)) {
                    const float rms=samples?static_cast<float>(std::sqrt(sumSquares/double(samples))):0.f;
                    // Read independent 250 ms intervals. Never raise when silent.
                    alert::GainAction action=enabled&&baselineKnown
                        ?logic.feed(slotPeak,rms,samples!=0):alert::GainAction::None;
                    if(action!=alert::GainAction::None) {
                        float currentDb=0;
                        hr=volume->GetMasterVolumeLevel(&currentDb);
                        if(SUCCEEDED(hr)) {
                            const float requestedDb=alert::requestedGainDb(action,currentDb,baselineDb,low,high);
                            // Honor unchecked Tools menu before any actual gain change.
                            if(autoEnabled.load(std::memory_order_acquire) &&
                               std::fabs(requestedDb-currentDb)>.05f) {
                                hr=volume->SetMasterVolumeLevel(requestedDb,nullptr);
                                if(SUCCEEDED(hr)) {
                                    const int remaining=(int)std::lround(baselineDb-requestedDb);
                                    reductionDb.store(remaining);
                                    blog(LOG_INFO,"[Line In Alert] AUTO %s Line In: %.1f -> %.1f dB; peak %.3f, rms %.3f; difference from startup %+d dB.",
                                        action==alert::GainAction::Down3?"DOWN":"UP",
                                        currentDb,requestedDb,slotPeak,rms,remaining);
                                }else failed("Set Windows Recording gain",hr);
                            }
                        } else failed("Get Windows Recording gain",hr);
                    }
                    if(enabled && now-lastReport>=std::chrono::seconds(8)) {
                        float actualDb=0;
                        if(SUCCEEDED(volume->GetMasterVolumeLevel(&actualDb))) {
                            blog(LOG_INFO,"[Line In Alert] AUTO diagnostic: peak %.4f RMS %.5f inputLevel %.1f dB, start %.1f dB, UP ceiling %.1f dB.",
                                slotPeak,rms,actualDb,baselineDb,std::min(high,baselineDb+18.f));
                        }
                        lastReport=now;
                    }
                    slotPeak=0;
                    sumSquares=0;
                    samples=0;
                    slotStart=now;
                }
                QThread::msleep(20);
            }
            ready=false;
            client->Stop();
        }while(false);
        ready=false;
    }
};

class Controller final:public QObject {
    ComInit com{COINIT_APARTMENTTHREADED};
    ComPtr<IMMDeviceEnumerator> enumerator;
    std::unique_ptr<AutoGainThread> worker;
    QPointer<QAction> autoAction;
    QTimer timer;
    QElapsedTimer clock;
    QString settingsPath, monitoredId;
    qint64 lastScan=-10000, lastRetry=-10000;
    bool warned=false;
    bool requestedAuto=true; // On at every OBS startup, not persisted.
    void stopWorker() {
        if(worker){worker->stopFlag=true;worker->wait();worker.reset();}
    }
    void chooseDevice(){
        lastScan=clock.elapsed();
        stopWorker();
        monitoredId.clear();
        if(!enumerator)return;
        ComPtr<IMMDeviceCollection> collection;
        HRESULT hr=enumerator->EnumAudioEndpoints(eCapture,DEVICE_STATE_ACTIVE,&collection);
        if(FAILED(hr)){
            blog(LOG_WARNING,"[Line In Alert] Cannot enumerate Windows Recording endpoints: 0x%08lx.",(unsigned long)hr);
            return;
        }
        UINT count=0;
        collection->GetCount(&count);
        std::vector<QString> allIds,lineIds;
        for(UINT i=0;i<count;++i){
            ComPtr<IMMDevice>d;
            if(FAILED(collection->Item(i,&d)))continue;
            LPWSTR wideId=nullptr;
            if(FAILED(d->GetId(&wideId))||!wideId)continue;
            QString id=QString::fromWCharArray(wideId);
            CoTaskMemFree(wideId);
            allIds.push_back(id);
            QString name;
            ComPtr<IPropertyStore>store;
            if(SUCCEEDED(d->OpenPropertyStore(STGM_READ,&store))){
                PROPVARIANT value;PropVariantInit(&value);
                if(SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName,&value))&&
                   value.vt==VT_LPWSTR&&value.pwszVal)
                    name=QString::fromWCharArray(value.pwszVal);
                PropVariantClear(&value);
            }
            const QString lower=name.toLower();
            if(lower.contains("line in")||lower.contains("line-in")||
               lower.contains("line input")||lower.contains("linijski ulaz"))
                lineIds.push_back(id);
        }
        QString chosen;
        if(!settingsPath.isEmpty()){
            QSettings settings(settingsPath,QSettings::IniFormat);
            const QString preferred=settings.value("device_id").toString();
            for(const auto&id:allIds)if(id==preferred)chosen=id;
        }
        if(chosen.isEmpty()&&lineIds.size()==1)chosen=lineIds.front();
        if(chosen.isEmpty()){
            if(!warned)blog(LOG_WARNING,"[Line In Alert] No unique Line In input available. Tools control remains checked; no gain changes performed.");
            warned=true;
            return;
        }
        warned=false;
        monitoredId=chosen;
        lastRetry=clock.elapsed();
        worker=std::make_unique<AutoGainThread>(chosen);
        worker->autoEnabled.store(requestedAuto,std::memory_order_release);
        worker->start();
        blog(LOG_INFO,"[Line In Alert] Automatic Line In control started; Tools menu state: %s.",requestedAuto?"ON":"OFF");
    }
    void tick(){
        if(!worker){
            if(clock.elapsed()-lastScan>=3000)chooseDevice();
            return;
        }
        if(!worker->isRunning() && clock.elapsed()-lastRetry>=12000 && !monitoredId.isEmpty()){
            blog(LOG_INFO,"[Line In Alert] Reconnecting shared Line In PCM capture.");
            stopWorker();
            worker=std::make_unique<AutoGainThread>(monitoredId);
            worker->autoEnabled.store(requestedAuto,std::memory_order_release);
            lastRetry=clock.elapsed();
            worker->start();
        }
    }
public:
    explicit Controller(QMainWindow*window):QObject(window){
        char*path=obs_module_config_path("settings.ini");
        if(path){settingsPath=QString::fromUtf8(path);bfree(path);}
        if(com.ok()){
            const HRESULT hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator));
            if(FAILED(hr))blog(LOG_WARNING,"[Line In Alert] Failed initializing Windows Core Audio: 0x%08lx.",(unsigned long)hr);
        }
        // Official OBS frontend API. OBS owns the Tools menu itself.
        QAction*action=static_cast<QAction*>(
            obs_frontend_add_tools_menu_qaction("Automatska kontrola nivoa zvuka"));
        if(action){
            autoAction=action;
            action->setCheckable(true);
            action->setChecked(true);
            QObject::connect(action,&QAction::toggled,this,[this](bool enabled){
                requestedAuto=enabled;
                if(worker)worker->autoEnabled.store(enabled,std::memory_order_release);
                blog(LOG_INFO,"[Line In Alert] Tools > Automatic Sound Level Control: %s.",
                     enabled?"ENABLED":"DISABLED");
            });
        }else{
            blog(LOG_WARNING,"[Line In Alert] Unable to add Tools menu action.");
        }
        clock.start();
        QObject::connect(&timer,&QTimer::timeout,this,[this](){tick();});
        timer.start(250);
        QTimer::singleShot(0,this,[this](){tick();});
    }
    ~Controller()override {
        timer.stop();
        stopWorker();
        if(autoAction){delete autoAction.data();autoAction.clear();}
        enumerator.Reset();
    }
};

QPointer<Controller> controller;
bool exiting=false;
void start() {
    if(controller||exiting)return;
    if(auto*main=static_cast<QMainWindow*>(obs_frontend_get_main_window()))
        controller=new Controller(main);
}
void frontendEvent(enum obs_frontend_event e,void*) {
    if(e==OBS_FRONTEND_EVENT_FINISHED_LOADING) {
        auto*main=static_cast<QMainWindow*>(obs_frontend_get_main_window());
        if(main)QTimer::singleShot(0,main,[](){start();});
    } else if(e==OBS_FRONTEND_EVENT_EXIT) {
        exiting=true;delete controller.data();controller.clear();
    }
}
}
extern "C" const char*obs_module_description(void) {
    return "Automatically regulate Windows Recording Line In via checkable OBS Tools menu action.";
}
extern "C" bool obs_module_load(void) {
    obs_frontend_add_event_callback(frontendEvent,nullptr);
    blog(LOG_INFO,"[Line In Alert] v3.1 headless automatic Line In gain control. Tools menu enabled by default.");
    return true;
}
extern "C" void obs_module_unload(void) {
    obs_frontend_remove_event_callback(frontendEvent,nullptr);
    delete controller.data();controller.clear();
}
