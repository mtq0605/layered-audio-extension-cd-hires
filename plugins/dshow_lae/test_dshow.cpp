// test of lae_source.ax without registration: graph with the filter, render, play, seek
#include <windows.h>
#include <dshow.h>
#include <initguid.h>
#include <cstdio>
DEFINE_GUID(CLSID_LaeSource, 0x5b3c6e8a, 0x2f41, 0x4c7d, 0x9e, 0x15, 0x8a, 0x0c, 0x3d, 0x6b, 0x7f, 0x21);
typedef HRESULT(STDAPICALLTYPE* GetCO)(REFCLSID, REFIID, void**);
int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED); HMODULE m = LoadLibraryW(argv[1]); if (!m) { printf("cannot load filter\n"); return 2; }
    IClassFactory* cf = nullptr; ((GetCO)GetProcAddress(m, "DllGetClassObject"))(CLSID_LaeSource, IID_IClassFactory, (void**)&cf);
    IBaseFilter* src = nullptr; HRESULT hr = cf->CreateInstance(nullptr, IID_IBaseFilter, (void**)&src); printf("create %08lx\n", hr);
    IGraphBuilder* g = nullptr; CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_IGraphBuilder, (void**)&g);
    g->AddFilter(src, L"LAE source"); IFileSourceFilter* fs = nullptr; src->QueryInterface(IID_IFileSourceFilter, (void**)&fs);
    DWORD t0 = GetTickCount(); hr = fs->Load(argv[2], nullptr); printf("load %08lx (%lu ms)\n", hr, GetTickCount() - t0); if (FAILED(hr)) return 2;
    IEnumPins* ep = nullptr; src->EnumPins(&ep); IPin* pin = nullptr; ep->Next(1, &pin, nullptr);
    AM_MEDIA_TYPE mt; pin->ConnectionMediaType(&mt); IEnumMediaTypes* et = nullptr; pin->EnumMediaTypes(&et); AM_MEDIA_TYPE* pmt = nullptr; et->Next(1, &pmt, nullptr);
    WAVEFORMATEX* w = (WAVEFORMATEX*)pmt->pbFormat; printf("format: %lu Hz, %u bit container, %u ch\n", w->nSamplesPerSec, w->wBitsPerSample, w->nChannels);
    hr = g->Render(pin); printf("render %08lx\n", hr); if (FAILED(hr)) return 3;
    IMediaControl* mc = nullptr; g->QueryInterface(IID_IMediaControl, (void**)&mc); IMediaSeeking* ms = nullptr; g->QueryInterface(IID_IMediaSeeking, (void**)&ms);
    LONGLONG dur = 0; ms->GetDuration(&dur); printf("duration %.3f s\n", dur / 1e7);
    hr = mc->Run(); printf("run %08lx\n", hr); Sleep(3000); LONGLONG cur = 0; ms->GetCurrentPosition(&cur); printf("after 3 s: %.3f s\n", cur / 1e7);
    LONGLONG to = dur * 6 / 10; hr = ms->SetPositions(&to, AM_SEEKING_AbsolutePositioning, nullptr, AM_SEEKING_NoPositioning); printf("seek to %.3f s: %08lx\n", to / 1e7, hr);
    Sleep(2000); ms->GetCurrentPosition(&cur); printf("2 s after seek: %.3f s\n", cur / 1e7);
    mc->Stop(); printf("stopped\n"); ms->Release(); mc->Release(); pin->Release(); ep->Release(); fs->Release(); g->Release(); src->Release(); cf->Release(); CoUninitialize(); return 0; }
