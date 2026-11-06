// test helper: records what the default playback device plays (WASAPI loopback) for N seconds and prints, per second, the level and
// the zero-crossing rate of the left channel (music: hundreds to a few thousand crossings per second; white noise: tens of thousands)
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cmath>
#include <cstdio>
#include <vector>
int wmain(int argc, wchar_t** argv) {
    const int secs = argc > 1 ? _wtoi(argv[1]) : 6; CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr; CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
    IMMDevice* dev = nullptr; en->GetDefaultAudioEndpoint(eRender, eConsole, &dev); IAudioClient* ac = nullptr; dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
    WAVEFORMATEX* wf = nullptr; ac->GetMixFormat(&wf); printf("device mix format: %lu Hz, %u ch, %u bit\n", wf->nSamplesPerSec, wf->nChannels, wf->wBitsPerSample);
    HRESULT hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, wf, nullptr); if (FAILED(hr)) { printf("init %08lx\n", hr); return 2; }
    IAudioCaptureClient* cc = nullptr; ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cc); ac->Start();
    const unsigned rate = wf->nSamplesPerSec, ch = wf->nChannels; std::vector<float> left; DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < (DWORD)secs * 1000) { Sleep(10); UINT32 n = 0; while (SUCCEEDED(cc->GetNextPacketSize(&n)) && n) { BYTE* d; UINT32 fr; DWORD fl;
            cc->GetBuffer(&d, &fr, &fl, nullptr, nullptr); const float* f = (const float*)d; for (UINT32 i = 0; i < fr; i++) left.push_back((fl & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.f : f[i * ch]); cc->ReleaseBuffer(fr); } }
    ac->Stop();
    for (size_t s = 0; s + rate <= left.size(); s += rate) { double e = 0; unsigned zc = 0; for (size_t i = s; i < s + rate; i++) { e += (double)left[i] * left[i]; if (i > s && ((left[i] >= 0) != (left[i - 1] >= 0))) zc++; }
        printf("second %zu: level %6.1f dBFS, zero crossings %u\n", s / rate + 1, e > 0 ? 10 * log10(e / rate) : -200.0, zc); }
    return 0; }
