// AsioMtaShim.cpp
//
// ASIO pass-through driver for vMix + TASCAM US-16x08.
//
// Problem: the TASCAM driver delivers ASIO buffer callbacks on its own thread,
// which is a COM single-threaded apartment (STA) that never pumps messages.
// vMix creates a Windows Audio Resampler DSP (resampledmo.dll) inside those
// callbacks, so the object is bound to that STA. At shutdown vMix releases it
// from another thread, the cross-apartment call can never be serviced, and
// vMix deadlocks.
//
// Fix: this DLL registers as its own ASIO driver, loads the real TASCAM driver,
// and forwards every call unchanged. Only the buffer callbacks are different:
// they are handed to a dedicated MTA worker thread, which runs vMix's callback
// and returns before the driver continues. The buffers are the same, so no
// extra buffering or latency is added (just a thread handoff per buffer).
// Anything vMix creates in the callback now lives in the MTA and can be
// released from any thread.
//
// Build (x64 only), from "x64 Native Tools Command Prompt for VS":
//   cl /LD /O2 /MT /EHsc AsioMtaShim.cpp /Fe:AsioMtaShim64.dll
//
// Configuration: reads the real driver's CLSID from
//   HKLM\SOFTWARE\AsioMtaShim  TargetCLSID (REG_SZ, "{....}")

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <avrt.h>
#include <atomic>
#include <new>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(linker, "/EXPORT:DllGetClassObject,PRIVATE")
#pragma comment(linker, "/EXPORT:DllCanUnloadNow,PRIVATE")

// This shim's own CLSID. Must match the registry entries.
// {6F1D2C84-3A57-4E9B-B0C2-9D41E7A5F318}
static const CLSID CLSID_AsioMtaShim =
    { 0x6F1D2C84, 0x3A57, 0x4E9B, { 0xB0, 0xC2, 0x9D, 0x41, 0xE7, 0xA5, 0xF3, 0x18 } };

// ---------------------------------------------------------------------------
// Minimal ASIO ABI, layout-compatible with the Steinberg SDK (iasiodrv.h /
// asio.h). Types we only pass through are left opaque.
// ---------------------------------------------------------------------------
typedef long   ASIOBool;
typedef long   ASIOError;
typedef double ASIOSampleRate;
struct ASIOClockSource; struct ASIOSamples; struct ASIOTimeStamp;
struct ASIOChannelInfo; struct ASIOBufferInfo; struct ASIOTime;

static const ASIOError ASE_NotPresent = -1000;
static const ASIOError ASE_NoMemory   = -994;

struct ASIOCallbacks {
    void      (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
    void      (*sampleRateDidChange)(ASIOSampleRate sRate);
    long      (*asioMessage)(long selector, long value, void* message, double* opt);
    ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess);
};

struct IASIO : public IUnknown {
    virtual ASIOBool  init(void* sysHandle) = 0;
    virtual void      getDriverName(char* name) = 0;
    virtual long      getDriverVersion() = 0;
    virtual void      getErrorMessage(char* string) = 0;
    virtual ASIOError start() = 0;
    virtual ASIOError stop() = 0;
    virtual ASIOError getChannels(long* numIn, long* numOut) = 0;
    virtual ASIOError getLatencies(long* inLat, long* outLat) = 0;
    virtual ASIOError getBufferSize(long* minSize, long* maxSize, long* prefSize, long* granularity) = 0;
    virtual ASIOError canSampleRate(ASIOSampleRate rate) = 0;
    virtual ASIOError getSampleRate(ASIOSampleRate* rate) = 0;
    virtual ASIOError setSampleRate(ASIOSampleRate rate) = 0;
    virtual ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) = 0;
    virtual ASIOError setClockSource(long reference) = 0;
    virtual ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) = 0;
    virtual ASIOError getChannelInfo(ASIOChannelInfo* info) = 0;
    virtual ASIOError createBuffers(ASIOBufferInfo* infos, long numChannels, long bufferSize, ASIOCallbacks* cb) = 0;
    virtual ASIOError disposeBuffers() = 0;
    virtual ASIOError controlPanel() = 0;
    virtual ASIOError future(long selector, void* opt) = 0;
    virtual ASIOError outputReady() = 0;
};

// ---------------------------------------------------------------------------
// Callback dispatcher: runs the host's buffer callbacks on an MTA thread.
// ASIO callbacks carry no context pointer, so this is process-global
// (ASIO hosts use one driver instance at a time).
// ---------------------------------------------------------------------------
struct Dispatcher {
    ASIOCallbacks     host = {};
    HANDLE            thread = nullptr;
    HANDLE            evWork = nullptr;
    HANDLE            evDone = nullptr;
    SRWLOCK           lock = SRWLOCK_INIT;
    std::atomic<bool> quit{ false };
    // Pending call (guarded by lock + event handshake)
    bool      timeInfo = false;
    ASIOTime* params = nullptr;
    long      index = 0;
    ASIOBool  direct = 0;
    ASIOTime* result = nullptr;
};
static Dispatcher g_d;

static DWORD WINAPI WorkerProc(void*)
{
    // The whole point: this thread is in the multithreaded apartment.
    // Deliberately no CoUninitialize, so the MTA stays alive for objects
    // created here until the process exits.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    DWORD  taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    for (;;) {
        WaitForSingleObject(g_d.evWork, INFINITE);
        if (g_d.quit.load()) break;

        if (g_d.timeInfo && g_d.host.bufferSwitchTimeInfo) {
            g_d.result = g_d.host.bufferSwitchTimeInfo(g_d.params, g_d.index, g_d.direct);
        } else {
            if (g_d.host.bufferSwitch) g_d.host.bufferSwitch(g_d.index, g_d.direct);
            g_d.result = g_d.params;
        }
        SetEvent(g_d.evDone);
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    return 0;
}

static bool StartWorker()
{
    g_d.quit = false;
    g_d.evWork = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_d.evDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_d.evWork || !g_d.evDone) return false;

    HANDLE t = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
    if (!t) return false;

    AcquireSRWLockExclusive(&g_d.lock);
    g_d.thread = t;
    ReleaseSRWLockExclusive(&g_d.lock);
    return true;
}

static void StopWorker()
{
    // Taking the lock guarantees no callback is mid-handoff.
    AcquireSRWLockExclusive(&g_d.lock);
    HANDLE t = g_d.thread;
    g_d.thread = nullptr;
    ReleaseSRWLockExclusive(&g_d.lock);

    if (t) {
        g_d.quit = true;
        SetEvent(g_d.evWork);
        WaitForSingleObject(t, 5000);
        CloseHandle(t);
    }
    if (g_d.evWork) { CloseHandle(g_d.evWork); g_d.evWork = nullptr; }
    if (g_d.evDone) { CloseHandle(g_d.evDone); g_d.evDone = nullptr; }
}

static ASIOTime* Dispatch(bool timeInfo, ASIOTime* params, long index, ASIOBool direct)
{
    ASIOTime* r = nullptr;
    AcquireSRWLockExclusive(&g_d.lock);
    if (g_d.thread) {
        g_d.timeInfo = timeInfo;
        g_d.params   = params;
        g_d.index    = index;
        g_d.direct   = direct;
        g_d.result   = nullptr;
        SetEvent(g_d.evWork);
        WaitForSingleObject(g_d.evDone, INFINITE);
        r = g_d.result;
    }
    // If the worker is gone (late callback after disposeBuffers), drop the buffer.
    ReleaseSRWLockExclusive(&g_d.lock);
    return r;
}

static void ShimBufferSwitch(long index, ASIOBool direct)
{
    Dispatch(false, nullptr, index, direct);
}

static ASIOTime* ShimBufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool direct)
{
    return Dispatch(true, params, index, direct);
}

static void ShimSampleRateDidChange(ASIOSampleRate rate)
{
    if (g_d.host.sampleRateDidChange) g_d.host.sampleRateDidChange(rate);
}

static long ShimAsioMessage(long selector, long value, void* message, double* opt)
{
    return g_d.host.asioMessage ? g_d.host.asioMessage(selector, value, message, opt) : 0;
}

// The driver may keep this pointer, so it must stay valid: static.
static ASIOCallbacks g_shimCallbacks = {
    ShimBufferSwitch, ShimSampleRateDidChange, ShimAsioMessage, ShimBufferSwitchTimeInfo
};

// ---------------------------------------------------------------------------
// The shim driver object
// ---------------------------------------------------------------------------
static bool ReadTargetClsid(CLSID& out)
{
    wchar_t buf[64] = {};
    DWORD cb = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\AsioMtaShim", L"TargetCLSID",
                     RRF_RT_REG_SZ, nullptr, buf, &cb) != ERROR_SUCCESS)
        return false;
    return SUCCEEDED(CLSIDFromString(buf, &out));
}

class AsioMtaShim final : public IASIO {
    std::atomic<long> m_ref{ 1 };
    IASIO*            m_inner = nullptr;

public:
    ~AsioMtaShim()
    {
        StopWorker();
        if (m_inner) m_inner->Release();
    }

    HRESULT LoadInner()
    {
        CLSID target;
        if (!ReadTargetClsid(target)) return REGDB_E_CLASSNOTREG;
        if (target == CLSID_AsioMtaShim) return E_INVALIDARG;   // don't load ourselves
        // ASIO convention: the CLSID doubles as the interface ID.
        return CoCreateInstance(target, nullptr, CLSCTX_INPROC_SERVER, target,
                                reinterpret_cast<void**>(&m_inner));
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == CLSID_AsioMtaShim) {
            *ppv = static_cast<IASIO*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++m_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        long r = --m_ref;
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    // IASIO: straight pass-through except createBuffers/disposeBuffers
    ASIOBool  init(void* h) override                          { return m_inner->init(h); }
    void      getDriverName(char* n) override                 { m_inner->getDriverName(n); }
    long      getDriverVersion() override                     { return m_inner->getDriverVersion(); }
    void      getErrorMessage(char* s) override               { m_inner->getErrorMessage(s); }
    ASIOError start() override                                { return m_inner->start(); }
    ASIOError stop() override                                 { return m_inner->stop(); }
    ASIOError getChannels(long* i, long* o) override          { return m_inner->getChannels(i, o); }
    ASIOError getLatencies(long* i, long* o) override         { return m_inner->getLatencies(i, o); }
    ASIOError getBufferSize(long* a, long* b, long* c, long* d) override { return m_inner->getBufferSize(a, b, c, d); }
    ASIOError canSampleRate(ASIOSampleRate r) override        { return m_inner->canSampleRate(r); }
    ASIOError getSampleRate(ASIOSampleRate* r) override       { return m_inner->getSampleRate(r); }
    ASIOError setSampleRate(ASIOSampleRate r) override        { return m_inner->setSampleRate(r); }
    ASIOError getClockSources(ASIOClockSource* c, long* n) override { return m_inner->getClockSources(c, n); }
    ASIOError setClockSource(long r) override                 { return m_inner->setClockSource(r); }
    ASIOError getSamplePosition(ASIOSamples* s, ASIOTimeStamp* t) override { return m_inner->getSamplePosition(s, t); }
    ASIOError getChannelInfo(ASIOChannelInfo* i) override     { return m_inner->getChannelInfo(i); }
    ASIOError controlPanel() override                         { return m_inner->controlPanel(); }
    ASIOError future(long s, void* o) override                { return m_inner->future(s, o); }
    ASIOError outputReady() override                          { return m_inner->outputReady(); }

    ASIOError createBuffers(ASIOBufferInfo* infos, long numChannels, long bufferSize, ASIOCallbacks* cb) override
    {
        if (!cb) return m_inner->createBuffers(infos, numChannels, bufferSize, cb);

        StopWorker();               // in case the host calls createBuffers twice
        g_d.host = *cb;
        if (!StartWorker()) { StopWorker(); return ASE_NoMemory; }

        ASIOError e = m_inner->createBuffers(infos, numChannels, bufferSize, &g_shimCallbacks);
        if (e != 0) StopWorker();
        return e;
    }

    ASIOError disposeBuffers() override
    {
        ASIOError e = m_inner->disposeBuffers();   // driver stops calling us first
        StopWorker();
        return e;
    }
};

// ---------------------------------------------------------------------------
// COM plumbing
// ---------------------------------------------------------------------------
class ShimFactory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) { *ppv = this; return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override  { return 2; }   // static object
    STDMETHODIMP_(ULONG) Release() override { return 1; }

    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;

        AsioMtaShim* s = new (std::nothrow) AsioMtaShim();
        if (!s) return E_OUTOFMEMORY;
        HRESULT hr = s->LoadInner();
        if (SUCCEEDED(hr)) hr = s->QueryInterface(riid, ppv);
        s->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL) override { return S_OK; }
};
static ShimFactory g_factory;

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* ppv)
{
    if (clsid != CLSID_AsioMtaShim) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

// Never unload: the worker thread's code lives in this DLL.
STDAPI DllCanUnloadNow() { return S_FALSE; }
