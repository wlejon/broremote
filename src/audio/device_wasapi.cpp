// The WASAPI backend (Windows): the viewer's microphone and speakers.
//
// Shared mode, event driven, with the format converted by the audio engine
// (AUTOCONVERTPCM), so the lane's rate and channel count are what the
// callback sees whatever the device runs at. Each endpoint has its own
// thread, in the MMCSS "Pro Audio" class, doing all its COM work.
//
// The microphone is the default *communications* capture device in the
// Communications category: Windows then runs its communications processing,
// which includes acoustic echo cancellation where the device's driver or
// Windows itself (Voice Clarity, Windows 11) provides it. The echo reference
// is set to the default render device, where the host's audio plays
// (IAcousticEchoCancellationControl, Windows 11 22H2+), and ducking is
// turned off so opening it does not quieten everything else.
#include "broremote/audio.h"
#include "broremote/audio_device.h"

#include <windows.h>

#include <audioclient.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace broremote::audio {

namespace {

// Defined here rather than through initguid / ksmedia.lib.
constexpr GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kEffectAec = {0x6f64adbe, 0x8211, 0x11e2, {0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa}};
constexpr PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

template <class T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T** put() {
        reset();
        return &p;
    }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

std::string hr_text(const char* what, HRESULT hr) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
    return buf;
}

std::string narrow(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string device_name(IMMDevice* dev) {
    Com<IPropertyStore> props;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, props.put()))) return {};
    PROPVARIANT v;
    PropVariantInit(&v);
    std::string name;
    if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) name = narrow(v.pwszVal);
    PropVariantClear(&v);
    return name;
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Each active device of `flow`, with its name, and whether it is the default
// (communications capture, console render).
template <class F>
void each_device(IMMDeviceEnumerator* en, EDataFlow flow, F f) {
    Com<IMMDeviceCollection> all;
    if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, all.put()))) return;
    std::wstring default_id;
    Com<IMMDevice> def;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, flow == eCapture ? eCommunications : eConsole, def.put()))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(def->GetId(&id))) {
            default_id = id;
            CoTaskMemFree(id);
        }
    }
    UINT n = 0;
    all->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        IMMDevice* dev = nullptr;
        if (FAILED(all->Item(i, &dev))) continue;
        LPWSTR id = nullptr;
        bool is_default = false;
        if (SUCCEEDED(dev->GetId(&id))) {
            is_default = default_id == id;
            CoTaskMemFree(id);
        }
        const bool keep = f(dev, device_name(dev), is_default);
        if (!keep) dev->Release();
        else return;
    }
}

// The first active device of `flow` whose name contains `part` (any case).
bool find_device(IMMDeviceEnumerator* en, EDataFlow flow, const std::string& part, Com<IMMDevice>& out) {
    const std::string want = lower(part);
    each_device(en, flow, [&](IMMDevice* dev, const std::string& name, bool) {
        if (lower(name).find(want) == std::string::npos) return false;
        out.reset();
        out.p = dev;  // the reference is ours now
        return true;
    });
    return bool(out);
}

WAVEFORMATEXTENSIBLE float_format(uint32_t rate, uint32_t channels) {
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = WORD(channels);
    f.Format.nSamplesPerSec = rate;
    f.Format.wBitsPerSample = 32;
    f.Format.nBlockAlign = WORD(4 * channels);
    f.Format.nAvgBytesPerSec = rate * 4 * channels;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = 32;
    f.dwChannelMask = channels == 1 ? SPEAKER_FRONT_CENTER
                    : channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                                    : 0;
    f.SubFormat = kSubtypeFloat;
    return f;
}

class WasapiEndpoint final : public Endpoint {
public:
    WasapiEndpoint(bool capture, uint32_t rate, uint32_t channels, uint32_t period, std::string device,
                   std::string echo_reference, CaptureFn cfn, PlaybackFn pfn)
        : capture_(capture), rate_(rate), channels_(channels), period_(period ? period : rate / 100),
          cfn_(std::move(cfn)), pfn_(std::move(pfn)), device_(std::move(device)),
          echo_reference_(std::move(echo_reference)) {}
    ~WasapiEndpoint() override { stop(); }

    bool start(std::string* err) override {
        if (thread_.joinable()) return true;
        stop_ = false;
        // Shared: the thread may still answer after a start() that timed out.
        auto ready = std::make_shared<std::promise<std::string>>();
        auto result = ready->get_future();
        thread_ = std::thread([this, ready] { run(*ready); });
        std::string why;
        if (result.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
            why = "the audio device did not start within 10 s";
        } else {
            why = result.get();
        }
        if (!why.empty()) {
            stop();
            if (err) *err = why;
            return false;
        }
        return true;
    }

    void stop() override {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] EndpointInfo info() const override {
        std::lock_guard<std::mutex> lk(info_m_);
        return info_;
    }

private:
    // Everything COM happens on this thread; `ready` gets "" once running,
    // else why it could not start.
    void run(std::promise<std::string>& ready) {
        bool told = false;
        auto tell = [&](std::string why) {
            if (!told) {
                told = true;
                ready.set_value(std::move(why));
            }
        };
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DWORD task_index = 0;
        HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        {
            Com<IMMDeviceEnumerator> en;
            Com<IMMDevice> dev, render_dev;
            Com<IAudioClient2> client;
            Com<IAudioCaptureClient> cap;
            Com<IAudioRenderClient> ren;
            std::string why = open(en, dev, render_dev, client, cap, ren, event);
            if (!why.empty()) {
                tell(why);
            } else {
                tell("");
                if (capture_) capture_loop(client.p, cap.p, event);
                else render_loop(client.p, ren.p, event);
                client->Stop();
            }
        }
        if (event) CloseHandle(event);
        if (task) AvRevertMmThreadCharacteristics(task);
        if (SUCCEEDED(co)) CoUninitialize();
        tell("stopped");
    }

    std::string open(Com<IMMDeviceEnumerator>& en, Com<IMMDevice>& dev, Com<IMMDevice>& render_dev,
                     Com<IAudioClient2>& client, Com<IAudioCaptureClient>& cap, Com<IAudioRenderClient>& ren,
                     HANDLE event) {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(en.put()));
        if (FAILED(hr)) return hr_text("CoCreateInstance(MMDeviceEnumerator)", hr);
        if (!device_.empty()) {
            if (!find_device(en.p, capture_ ? eCapture : eRender, device_, dev)) {
                return std::string(capture_ ? "no microphone" : "no speakers") + " named like \"" + device_ + "\"";
            }
        } else {
            hr = en->GetDefaultAudioEndpoint(capture_ ? eCapture : eRender, capture_ ? eCommunications : eConsole,
                                             dev.put());
            if (FAILED(hr)) return capture_ ? "no microphone (" + hr_text("GetDefaultAudioEndpoint", hr) + ")"
                                            : "no speakers (" + hr_text("GetDefaultAudioEndpoint", hr) + ")";
        }
        hr = dev->Activate(__uuidof(IAudioClient2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.put()));
        if (FAILED(hr)) return hr_text("IMMDevice::Activate(IAudioClient2)", hr);
        if (capture_) {
            AudioClientProperties props{};
            props.cbSize = sizeof props;
            props.bIsOffload = FALSE;
            props.eCategory = AudioCategory_Communications;
            props.Options = AUDCLNT_STREAMOPTIONS_NONE;
            client->SetClientProperties(&props);  // best effort: the stream still works without it
        }
        WAVEFORMATEXTENSIBLE fmt = float_format(rate_, channels_);
        const REFERENCE_TIME buffer_hns = REFERENCE_TIME(10000000ll * period_ / rate_) * 2;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                buffer_hns, 0, reinterpret_cast<WAVEFORMATEX*>(&fmt), nullptr);
        if (FAILED(hr)) return hr_text("IAudioClient::Initialize", hr);
        hr = client->SetEventHandle(event);
        if (FAILED(hr)) return hr_text("IAudioClient::SetEventHandle", hr);
        if (capture_) hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(cap.put()));
        else hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(ren.put()));
        if (FAILED(hr)) return hr_text("IAudioClient::GetService", hr);

        EndpointInfo info;
        info.device = device_name(dev.p);
        UINT32 frames = 0;
        if (SUCCEEDED(client->GetBufferSize(&frames))) info.period_frames = frames;
        REFERENCE_TIME lat = 0;
        if (SUCCEEDED(client->GetStreamLatency(&lat))) {
            latency_us_ = lat / 10;
            info.latency_ms = double(lat) / 10000.0;
        }
        if (capture_) {
            // Not ducked by our own communications stream.
            Com<IAudioSessionControl> sc;
            if (SUCCEEDED(client->GetService(__uuidof(IAudioSessionControl), reinterpret_cast<void**>(sc.put())))) {
                Com<IAudioSessionControl2> sc2;
                if (SUCCEEDED(sc->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(sc2.put())))) {
                    sc2->SetDuckingPreference(TRUE);
                }
            }
#ifdef __IAcousticEchoCancellationControl_INTERFACE_DEFINED__
            // The echo to cancel is what plays on the default render device.
            Com<IAcousticEchoCancellationControl> aec;
            if (SUCCEEDED(client->GetService(__uuidof(IAcousticEchoCancellationControl),
                                             reinterpret_cast<void**>(aec.put()))) &&
                (echo_reference_.empty() ? SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, render_dev.put()))
                                         : find_device(en.p, eRender, echo_reference_, render_dev))) {
                LPWSTR id = nullptr;
                if (SUCCEEDED(render_dev->GetId(&id))) {
                    aec->SetEchoCancellationRenderEndpoint(id);
                    CoTaskMemFree(id);
                }
            }
#endif
#ifdef __IAudioEffectsManager_INTERFACE_DEFINED__
            Com<IAudioEffectsManager> fx;
            if (SUCCEEDED(client->GetService(__uuidof(IAudioEffectsManager), reinterpret_cast<void**>(fx.put())))) {
                AUDIO_EFFECT* effects = nullptr;
                UINT32 n = 0;
                if (SUCCEEDED(fx->GetAudioEffects(&effects, &n)) && effects) {
                    for (UINT32 i = 0; i < n; ++i) {
                        if (IsEqualGUID(effects[i].id, kEffectAec) && effects[i].state == AUDIO_EFFECT_STATE_ON) {
                            info.echo_cancel = true;
                        }
                    }
                    CoTaskMemFree(effects);
                }
            }
#endif
        }
        {
            std::lock_guard<std::mutex> lk(info_m_);
            info_ = info;
        }
        if (!capture_) {
            // Start on a buffer of silence, so the first period does not underrun.
            BYTE* p = nullptr;
            if (frames && SUCCEEDED(ren->GetBuffer(frames, &p))) ren->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
        }
        hr = client->Start();
        if (FAILED(hr)) return hr_text("IAudioClient::Start", hr);
        return {};
    }

    void capture_loop(IAudioClient2* client, IAudioCaptureClient* cap, HANDLE event) {
        (void)client;
        std::vector<float> silence;
        while (!stop_) {
            if (WaitForSingleObject(event, 200) != WAIT_OBJECT_0) continue;
            UINT32 next = 0;
            while (!stop_ && SUCCEEDED(cap->GetNextPacketSize(&next)) && next > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                UINT64 dev_pos = 0, qpc = 0;
                const HRESULT hr = cap->GetBuffer(&data, &frames, &flags, &dev_pos, &qpc);
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED) return lost();
                if (FAILED(hr) || hr == AUDCLNT_S_BUFFER_EMPTY) break;
                // The QPC time the first frame was captured, in 100 ns units:
                // the steady clock's epoch (MSVC's steady_clock is QPC).
                const int64_t t = (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) || !qpc
                                      ? now_us() - int64_t(frames) * 1000000 / rate_
                                      : int64_t(qpc / 10);
                const float* f = reinterpret_cast<const float*>(data);
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    silence.assign(size_t(frames) * channels_, 0.0f);
                    f = silence.data();
                }
                if (frames) cfn_(f, frames, t);
                cap->ReleaseBuffer(frames);
            }
        }
    }

    void render_loop(IAudioClient2* client, IAudioRenderClient* ren, HANDLE event) {
        UINT32 size = 0;
        client->GetBufferSize(&size);
        while (!stop_) {
            if (WaitForSingleObject(event, 200) != WAIT_OBJECT_0) continue;
            UINT32 padding = 0;
            HRESULT hr = client->GetCurrentPadding(&padding);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) return lost();
            if (FAILED(hr) || padding >= size) continue;
            const UINT32 n = size - padding;
            BYTE* p = nullptr;
            hr = ren->GetBuffer(n, &p);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) return lost();
            if (FAILED(hr)) continue;
            // These frames play after what is already queued, and the device's own latency.
            const int64_t t = now_us() + int64_t(padding) * 1000000 / rate_ + latency_us_;
            pfn_(reinterpret_cast<float*>(p), n, t);
            ren->ReleaseBuffer(n, 0);
        }
    }

    void lost() {
        std::lock_guard<std::mutex> lk(info_m_);
        info_.device += " (lost)";
    }

    bool capture_;
    uint32_t rate_, channels_, period_;
    CaptureFn cfn_;
    PlaybackFn pfn_;
    std::string device_, echo_reference_;
    int64_t latency_us_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    mutable std::mutex info_m_;
    EndpointInfo info_;
};

class WasapiBackend final : public Backend {
public:
    [[nodiscard]] const char* name() const override { return "wasapi"; }
    std::unique_ptr<Endpoint> open_capture(const CaptureSpec& spec, CaptureFn fn, std::string* err) override {
        if (spec.source != Source::Microphone) {
            if (err) *err = "WASAPI here captures only the microphone";
            return nullptr;
        }
        return std::make_unique<WasapiEndpoint>(true, spec.rate, spec.channels, spec.period_frames, spec.device,
                                                spec.echo_reference, std::move(fn), PlaybackFn{});
    }
    std::unique_ptr<Endpoint> open_playback(const PlaybackSpec& spec, PlaybackFn fn, std::string* err) override {
        if (spec.sink != Sink::Speakers) {
            if (err) *err = "a virtual microphone is the host's (PipeWire), not Windows'";
            return nullptr;
        }
        return std::make_unique<WasapiEndpoint>(false, spec.rate, spec.channels, spec.period_frames, spec.device,
                                                std::string(), CaptureFn{}, std::move(fn));
    }
    [[nodiscard]] std::vector<Device> devices() override {
        std::vector<Device> out;
        // COM on a thread of its own, so the caller's apartment does not matter.
        std::thread([&out] {
            const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            {
                Com<IMMDeviceEnumerator> en;
                if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                               __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(en.put())))) {
                    for (EDataFlow flow : {eCapture, eRender}) {
                        each_device(en.p, flow, [&](IMMDevice*, const std::string& name, bool is_default) {
                            out.push_back({name, flow == eCapture, is_default});
                            return false;
                        });
                    }
                }
            }
            if (SUCCEEDED(co)) CoUninitialize();
        }).join();
        return out;
    }
};

}  // namespace

std::shared_ptr<Backend> platform_backend(std::string*) { return std::make_shared<WasapiBackend>(); }

}  // namespace broremote::audio
