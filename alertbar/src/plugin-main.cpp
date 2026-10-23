#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/bmem.h>
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <QHBoxLayout>
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
    int64_t now=0;
public:
    explicit AlertBar(QWidget*p):QWidget(p){
        setObjectName("obs_linein_alert_bar");
        setFixedHeight(13);
        setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    void feed(float peak,int64_t time){
        online=true;now=time;detector.update(peak,time);update();
    }
    void offline(){ online=false;detector.reset();update(); }
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
class Controller:public QObject {
    QMainWindow *main;
    ComInit com;
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> dev;
    ComPtr<IAudioMeterInformation> meter;
    QPointer<AlertBar> bar;
    std::vector<float> samples;
    QTimer timer;
    QElapsedTimer clock;
    qint64 lastScan=-10000;
    QString settingsPath;
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
            if(!warned){blog(LOG_WARNING,"[Line In Alert] Need exactly one Windows Recording Line In.");warned=true;}
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
        blog(LOG_INFO,"[Line In Alert] Capture endpoint monitored (%u channels).",count);
    }
    void tick(){
        if(!bar)attach();
        if(!meter&&clock.elapsed()-lastScan>=3000)searchDevice();
        if(!meter)return;
        if(FAILED(meter->GetChannelsPeakValues((UINT)samples.size(),samples.data()))){
            meter.Reset();dev.Reset();samples.clear();
            if(bar)bar->offline();
            lastScan=clock.elapsed();return;
        }
        float peak=0;
        for(float s:samples)if(std::isfinite(s))peak=std::max(peak,s);
        if(bar)bar->feed(peak,clock.elapsed());
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
    blog(LOG_INFO,"[Line In Alert] Native plugin v1.0 loaded.");return true;
}
extern "C" void obs_module_unload(void){
    obs_frontend_remove_event_callback(eventCallback,nullptr);
    delete controller.data();controller.clear();
}
