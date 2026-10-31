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
// Configuration (HKLM\SOFTWARE\AsioMtaShim):
//   TargetCLSID  REG_SZ     CLSID of the real ASIO driver to wrap (set by the installer)
//   EnableLog    REG_DWORD  1 = log to C:\ProgramData\AsioMtaShim\shim.log (default 0)
//   Passthrough  REG_DWORD  troubleshooting switches, all default 0; see below
//   FreshLoad    REG_DWORD
//   OwnerThread  REG_DWORD
//
// Important: in vMix, everything that uses the interface (inputs AND outputs)
// must use the shim's device name. If anything still uses the original driver
// name, that instance holds the hardware and the shim's start() fails.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <avrt.h>
#include <atomic>
#include <new>
#include <cstdio>
#include <cstdarg>
#include <functional>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(linker, "/EXPORT:DllGetClassObject,PRIVATE")
#pragma comment(linker, "/EXPORT:DllCanUnloadNow,PRIVATE")

// This shim's own CLSID. Must match the registry entries.
// {6F1D2C84-3A57-4E9B-B0C2-9D41E7A5F318}
static const CLSID CLSID_AsioMtaShim =
    { 0x6F1D2C84, 0x3A57, 0x4E9B, { 0xB0, 0xC2, 0x9D, 0x41, 0xE7, 0xA5, 0xF3, 0x18 } };

#define SHIM_VERSION "1.1.0"

// ---------------------------------------------------------------------------
// Logging: C:\ProgramData\AsioMtaShim\shim.log (also sent to OutputDebugString)
// ---------------------------------------------------------------------------
static SRWLOCK g_logLock = SRWLOCK_INIT;
static std::atomic<bool> g_logEnabled{ false };   // set from EnableLog when the shim is created

static void Log(const char* fmt, ...)
{
    if (!g_logEnabled.load()) return;

    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[640];
    int len = snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u [%5lu] %s\r\n",
                       st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                       GetCurrentThreadId(), msg);
    if (len < 0) return;
    if (len >= (int)sizeof(line)) len = (int)sizeof(line) - 1;

    OutputDebugStringA(line);

    AcquireSRWLockExclusive(&g_logLock);
    CreateDirectoryA("C:\\ProgramData\\AsioMtaShim", nullptr);
    HANDLE h = CreateFileA("C:\\ProgramData\\AsioMtaShim\\shim.log", FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(h, line, (DWORD)len, &written, nullptr);
        CloseHandle(h);
    }
    ReleaseSRWLockExclusive(&g_logLock);
}

static const char* AptName()
{
    APTTYPE t;
    APTTYPEQUALIFIER q;
    if (FAILED(CoGetApartmentType(&t, &q))) return "no-COM";
    switch (t) {
        case APTTYPE_STA:     return "STA";
        case APTTYPE_MAINSTA: return "MainSTA";
        case APTTYPE_MTA:     return "MTA";
        case APTTYPE_NA:      return "NA";
        default:              return "?";
    }
}

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
    std::atomic<DWORD> inlineThread{ 0 };   // thread inside createBuffers/start
    std::atomic<long>  callbacks{ 0 };
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
    Log("worker thread started, apartment=%s, mmcss=%s", AptName(), mmcss ? "yes" : "no");

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

static ASIOTime* RunInline(bool timeInfo, ASIOTime* params, long index, ASIOBool direct)
{
    if (timeInfo && g_d.host.bufferSwitchTimeInfo)
        return g_d.host.bufferSwitchTimeInfo(params, index, direct);
    if (g_d.host.bufferSwitch) g_d.host.bufferSwitch(index, direct);
    return params;
}

static ASIOTime* Dispatch(bool timeInfo, ASIOTime* params, long index, ASIOBool direct)
{
    long n = ++g_d.callbacks;
    bool inlineCall = (GetCurrentThreadId() == g_d.inlineThread.load());
    if (n <= 3)
        Log("callback #%ld %s idx=%ld direct=%ld on %s thread%s", n,
            timeInfo ? "bufferSwitchTimeInfo" : "bufferSwitch", index, direct, AptName(),
            inlineCall ? " (inline: called from inside createBuffers/start)" : "");

    // A driver that fires callbacks synchronously from inside start() would
    // deadlock against the host's own lock if we handed off, so run those inline.
    if (inlineCall) return RunInline(timeInfo, params, index, direct);

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
    Log("sampleRateDidChange(%.1f)", rate);
    if (g_d.host.sampleRateDidChange) g_d.host.sampleRateDidChange(rate);
}

static long ShimAsioMessage(long selector, long value, void* message, double* opt)
{
    long r = g_d.host.asioMessage ? g_d.host.asioMessage(selector, value, message, opt) : 0;
    Log("asioMessage(selector=%ld, value=%ld) -> %ld", selector, value, r);
    return r;
}

// The driver may keep this pointer, so it must stay valid: static.
static ASIOCallbacks g_shimCallbacks = {
    ShimBufferSwitch, ShimSampleRateDidChange, ShimAsioMessage, ShimBufferSwitchTimeInfo
};

// ---------------------------------------------------------------------------
// Switches (REG_DWORD under HKLM\SOFTWARE\AsioMtaShim, read when the shim is
// created). All default to 0; the defaults are the working configuration.
//   EnableLog   = 1  write C:\ProgramData\AsioMtaShim\shim.log
//   Passthrough = 1  give the driver vMix's own callbacks (no MTA worker);
//                    turns the shim into a pure proxy, so it does NOT fix the hang
//   FreshLoad   = 1  load the TASCAM DLL with LoadLibrary and FreeLibrary it
//                    when the shim is released, so every open gets a freshly
//                    loaded driver DLL
//   OwnerThread = 1  run control calls on the thread that created the driver
// ---------------------------------------------------------------------------
static DWORD ReadConfigDword(const wchar_t* name, DWORD def)
{
    DWORD v = 0, cb = sizeof(v);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\AsioMtaShim", name, RRF_RT_REG_DWORD,
                     nullptr, &v, &cb) == ERROR_SUCCESS)
        return v;
    return def;
}

static bool GetInprocPath(const CLSID& clsid, wchar_t* path, DWORD pathBytes)
{
    wchar_t clsidStr[64] = {};
    StringFromGUID2(clsid, clsidStr, 64);
    wchar_t key[160];
    swprintf(key, 160, L"SOFTWARE\\Classes\\CLSID\\%ls\\InprocServer32", clsidStr);
    DWORD cb = pathBytes;
    return RegGetValueW(HKEY_LOCAL_MACHINE, key, nullptr, RRF_RT_REG_SZ, nullptr, path, &cb) == ERROR_SUCCESS;
}

static const wchar_t* FileNameOf(const wchar_t* path)
{
    const wchar_t* s = wcsrchr(path, L'\\');
    return s ? s + 1 : path;
}

// ---------------------------------------------------------------------------
// Owner-thread marshaling. The TASCAM driver is created on whatever thread the
// host uses to create the shim. Some drivers only work when every control
// call (init, createBuffers, start, ...) comes from that same thread, so the
// shim creates a message-only window there and sends control calls to it.
// getSamplePosition/outputReady are exempt: ASIO allows them from the
// callback thread, and they run every buffer.
// ---------------------------------------------------------------------------
static const UINT WM_SHIM_CALL    = WM_APP + 0x51;
static const UINT WM_SHIM_DESTROY = WM_APP + 0x52;
static const wchar_t kOwnerClass[] = L"AsioMtaShimOwner";

static LRESULT CALLBACK OwnerWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_SHIM_CALL) {
        (*reinterpret_cast<const std::function<void()>*>(l))();
        return 1;
    }
    if (m == WM_SHIM_DESTROY) {
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static HWND CreateOwnerWindow()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&OwnerWndProc), &self);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OwnerWndProc;
    wc.hInstance = self;
    wc.lpszClassName = kOwnerClass;
    RegisterClassExW(&wc);   // fails harmlessly if already registered
    return CreateWindowExW(0, kOwnerClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, self, nullptr);
}

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
    long              m_posFailures = 0;
    DWORD             m_ownerTid = 0;
    HWND              m_ownerWnd = nullptr;
    bool              m_passthrough = false;
    bool              m_freshLoad = false;
    HMODULE           m_innerDll = nullptr;
    wchar_t           m_innerPath[MAX_PATH] = {};

    // Run fn on the thread that created the TASCAM driver.
    void OnOwner(const char* what, const std::function<void()>& fn)
    {
        if (!m_ownerWnd || GetCurrentThreadId() == m_ownerTid) { fn(); return; }
        if (!IsWindow(m_ownerWnd)) {
            Log("  %s: owner thread %lu is gone, calling directly", what, m_ownerTid);
            fn();
            return;
        }
        bool ran = false;
        std::function<void()> wrapped = [&] { ran = true; fn(); };
        SendMessageW(m_ownerWnd, WM_SHIM_CALL, 0, reinterpret_cast<LPARAM>(&wrapped));
        if (!ran) {
            Log("  %s: SendMessage to owner thread %lu failed (error %lu), calling directly",
                what, m_ownerTid, GetLastError());
            fn();
        }
    }

    void LogDriverError(const char* where)
    {
        char err[256] = {};
        OnOwner("getErrorMessage", [&] { m_inner->getErrorMessage(err); });
        Log("  %s driver error message: '%s'", where, err);
    }

public:
    ~AsioMtaShim()
    {
        Log("shim released");
        StopWorker();
        if (m_inner) {
            IASIO* inner = m_inner;
            OnOwner("Release", [inner] { inner->Release(); });
            m_inner = nullptr;
        }
        if (m_ownerWnd) {
            if (GetCurrentThreadId() == m_ownerTid) DestroyWindow(m_ownerWnd);
            else PostMessageW(m_ownerWnd, WM_SHIM_DESTROY, 0, 0);
        }
        if (m_innerDll) {
            FreeLibrary(m_innerDll);
            m_innerDll = nullptr;
        }
        if (m_innerPath[0])
            Log("  after release: %ls still loaded = %s", FileNameOf(m_innerPath),
                GetModuleHandleW(FileNameOf(m_innerPath)) ? "YES" : "no");
    }

    HRESULT LoadInner()
    {
        m_passthrough = ReadConfigDword(L"Passthrough", 0) != 0;
        m_freshLoad   = ReadConfigDword(L"FreshLoad", 0) != 0;
        bool useOwner = ReadConfigDword(L"OwnerThread", 0) != 0;
        Log("config: Passthrough=%d FreshLoad=%d OwnerThread=%d",
            (int)m_passthrough, (int)m_freshLoad, (int)useOwner);

        CLSID target;
        if (!ReadTargetClsid(target)) {
            Log("LoadInner: could not read HKLM\\SOFTWARE\\AsioMtaShim\\TargetCLSID");
            return REGDB_E_CLASSNOTREG;
        }
        if (target == CLSID_AsioMtaShim) {
            Log("LoadInner: TargetCLSID points at the shim itself");
            return E_INVALIDARG;
        }

        if (GetInprocPath(target, m_innerPath, sizeof(m_innerPath)))
            Log("LoadInner: driver DLL %ls, already loaded = %s", m_innerPath,
                GetModuleHandleW(FileNameOf(m_innerPath)) ? "YES" : "no");
        else
            Log("LoadInner: could not read the driver's InprocServer32 path");

        HRESULT hr;
        if (m_freshLoad && m_innerPath[0]) {
            m_innerDll = LoadLibraryW(m_innerPath);
            if (!m_innerDll) {
                Log("LoadInner: LoadLibrary failed, error %lu", GetLastError());
                return HRESULT_FROM_WIN32(GetLastError());
            }
            typedef HRESULT (STDAPICALLTYPE *GetClassObjectFn)(REFCLSID, REFIID, LPVOID*);
            GetClassObjectFn getClassObject =
                reinterpret_cast<GetClassObjectFn>(GetProcAddress(m_innerDll, "DllGetClassObject"));
            if (!getClassObject) {
                Log("LoadInner: driver has no DllGetClassObject export");
                return E_FAIL;
            }
            IClassFactory* factory = nullptr;
            hr = getClassObject(target, IID_IClassFactory, reinterpret_cast<void**>(&factory));
            if (SUCCEEDED(hr)) {
                hr = factory->CreateInstance(nullptr, target, reinterpret_cast<void**>(&m_inner));
                factory->Release();
            }
            Log("LoadInner: fresh load via LoadLibrary/DllGetClassObject -> 0x%08lX on %s thread",
                (unsigned long)hr, AptName());
        } else {
            // ASIO convention: the CLSID doubles as the interface ID.
            hr = CoCreateInstance(target, nullptr, CLSCTX_INPROC_SERVER, target,
                                  reinterpret_cast<void**>(&m_inner));
            Log("LoadInner: CoCreateInstance(TASCAM) -> 0x%08lX on %s thread", (unsigned long)hr, AptName());
        }

        if (SUCCEEDED(hr) && useOwner) {
            m_ownerTid = GetCurrentThreadId();
            m_ownerWnd = CreateOwnerWindow();
            Log("LoadInner: owner thread %lu, owner window %p", m_ownerTid, (void*)m_ownerWnd);
        }
        return hr;
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

    // IASIO. Control calls run on the owner thread; the log line shows the
    // caller's thread id, and "->owner" marks calls that were marshaled.
    const char* Via() const { return (m_ownerWnd && GetCurrentThreadId() != m_ownerTid) ? " ->owner" : ""; }

    ASIOBool init(void* h) override
    {
        Log("init(sysHandle=%p) on %s thread%s", h, AptName(), Via());
        ASIOBool r = 0;
        OnOwner("init", [&] { r = m_inner->init(h); });
        Log("init -> %ld", r);
        if (!r) LogDriverError("init");
        return r;
    }
    void getDriverName(char* n) override
    {
        OnOwner("getDriverName", [&] { m_inner->getDriverName(n); });
        Log("getDriverName -> '%s'%s", n, Via());
    }
    long getDriverVersion() override
    {
        long v = 0;
        OnOwner("getDriverVersion", [&] { v = m_inner->getDriverVersion(); });
        Log("getDriverVersion -> %ld%s", v, Via());
        return v;
    }
    void getErrorMessage(char* s) override
    {
        OnOwner("getErrorMessage", [&] { m_inner->getErrorMessage(s); });
        Log("getErrorMessage -> '%s'%s", s, Via());
    }

    ASIOError start() override
    {
        Log("start() on %s thread%s", AptName(), Via());
        ASIOError e = 0;
        OnOwner("start", [&] {
            Log("  start running on owner thread (%s)", AptName());
            g_d.inlineThread = GetCurrentThreadId();
            e = m_inner->start();
            g_d.inlineThread = 0;
        });
        Log("start -> %ld", e);
        if (e != 0) LogDriverError("start");
        return e;
    }
    ASIOError stop() override
    {
        ASIOError e = 0;
        OnOwner("stop", [&] { e = m_inner->stop(); });
        Log("stop -> %ld%s", e, Via());
        return e;
    }

    ASIOError getChannels(long* i, long* o) override
    {
        ASIOError e = 0;
        OnOwner("getChannels", [&] { e = m_inner->getChannels(i, o); });
        Log("getChannels -> %ld (in=%ld out=%ld)%s", e, i ? *i : -1, o ? *o : -1, Via());
        return e;
    }
    ASIOError getLatencies(long* i, long* o) override
    {
        ASIOError e = 0;
        OnOwner("getLatencies", [&] { e = m_inner->getLatencies(i, o); });
        Log("getLatencies -> %ld (in=%ld out=%ld)%s", e, i ? *i : -1, o ? *o : -1, Via());
        return e;
    }
    ASIOError getBufferSize(long* a, long* b, long* c, long* d) override
    {
        ASIOError e = 0;
        OnOwner("getBufferSize", [&] { e = m_inner->getBufferSize(a, b, c, d); });
        Log("getBufferSize -> %ld (min=%ld max=%ld pref=%ld gran=%ld)%s", e,
            a ? *a : -1, b ? *b : -1, c ? *c : -1, d ? *d : -1, Via());
        return e;
    }
    ASIOError canSampleRate(ASIOSampleRate r) override
    {
        ASIOError e = 0;
        OnOwner("canSampleRate", [&] { e = m_inner->canSampleRate(r); });
        Log("canSampleRate(%.1f) -> %ld%s", r, e, Via());
        return e;
    }
    ASIOError getSampleRate(ASIOSampleRate* r) override
    {
        ASIOError e = 0;
        OnOwner("getSampleRate", [&] { e = m_inner->getSampleRate(r); });
        Log("getSampleRate -> %ld (%.1f)%s", e, r ? *r : 0.0, Via());
        return e;
    }
    ASIOError setSampleRate(ASIOSampleRate r) override
    {
        ASIOError e = 0;
        OnOwner("setSampleRate", [&] { e = m_inner->setSampleRate(r); });
        Log("setSampleRate(%.1f) -> %ld%s", r, e, Via());
        return e;
    }
    ASIOError getClockSources(ASIOClockSource* c, long* n) override
    {
        ASIOError e = 0;
        OnOwner("getClockSources", [&] { e = m_inner->getClockSources(c, n); });
        Log("getClockSources -> %ld (n=%ld)%s", e, n ? *n : -1, Via());
        return e;
    }
    ASIOError setClockSource(long r) override
    {
        ASIOError e = 0;
        OnOwner("setClockSource", [&] { e = m_inner->setClockSource(r); });
        Log("setClockSource(%ld) -> %ld%s", r, e, Via());
        return e;
    }
    ASIOError getSamplePosition(ASIOSamples* s, ASIOTimeStamp* t) override
    {
        // Called every buffer, possibly from the callback: never marshaled.
        ASIOError e = m_inner->getSamplePosition(s, t);
        if (e != 0 && ++m_posFailures <= 5) Log("getSamplePosition -> %ld", e);
        return e;
    }
    ASIOError getChannelInfo(ASIOChannelInfo* i) override
    {
        ASIOError e = 0;
        OnOwner("getChannelInfo", [&] { e = m_inner->getChannelInfo(i); });
        if (e != 0) Log("getChannelInfo -> %ld", e);
        return e;
    }
    ASIOError controlPanel() override
    {
        ASIOError e = 0;
        OnOwner("controlPanel", [&] { e = m_inner->controlPanel(); });
        Log("controlPanel -> %ld", e);
        return e;
    }
    ASIOError future(long s, void* o) override
    {
        ASIOError e = 0;
        OnOwner("future", [&] { e = m_inner->future(s, o); });
        Log("future(selector=%ld) -> %ld%s", s, e, Via());
        return e;
    }
    ASIOError outputReady() override { return m_inner->outputReady(); }   // called every buffer: never marshaled

    ASIOError createBuffers(ASIOBufferInfo* infos, long numChannels, long bufferSize, ASIOCallbacks* cb) override
    {
        Log("createBuffers(numChannels=%ld, bufferSize=%ld, callbacks=%p) on %s thread%s",
            numChannels, bufferSize, (void*)cb, AptName(), Via());
        if (!cb) {
            ASIOError e0 = 0;
            OnOwner("createBuffers", [&] { e0 = m_inner->createBuffers(infos, numChannels, bufferSize, cb); });
            return e0;
        }

        if (m_passthrough) {
            ASIOError ep = 0;
            OnOwner("createBuffers", [&] { ep = m_inner->createBuffers(infos, numChannels, bufferSize, cb); });
            Log("createBuffers (PASSTHROUGH, host callbacks given to driver) -> %ld", ep);
            if (ep != 0) LogDriverError("createBuffers");
            return ep;
        }

        StopWorker();               // in case the host calls createBuffers twice
        g_d.host = *cb;
        g_d.callbacks = 0;
        Log("  host callbacks: bufferSwitch=%p sampleRateDidChange=%p asioMessage=%p bufferSwitchTimeInfo=%p",
            (void*)cb->bufferSwitch, (void*)cb->sampleRateDidChange,
            (void*)cb->asioMessage, (void*)cb->bufferSwitchTimeInfo);
        if (!StartWorker()) {
            Log("  StartWorker failed, error %lu", GetLastError());
            StopWorker();
            return ASE_NoMemory;
        }

        ASIOError e = 0;
        OnOwner("createBuffers", [&] {
            g_d.inlineThread = GetCurrentThreadId();
            e = m_inner->createBuffers(infos, numChannels, bufferSize, &g_shimCallbacks);
            g_d.inlineThread = 0;
        });
        Log("createBuffers -> %ld", e);
        if (e != 0) {
            LogDriverError("createBuffers");
            StopWorker();
        }
        return e;
    }

    ASIOError disposeBuffers() override
    {
        ASIOError e = 0;
        OnOwner("disposeBuffers", [&] { e = m_inner->disposeBuffers(); });   // driver stops calling us first
        Log("disposeBuffers -> %ld (callbacks delivered: %ld)", e, g_d.callbacks.load());
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

        g_logEnabled = ReadConfigDword(L"EnableLog", 0) != 0;
        Log("---- AsioMtaShim " SHIM_VERSION " CreateInstance (process %lu) ----", GetCurrentProcessId());
        AsioMtaShim* s = new (std::nothrow) AsioMtaShim();
        if (!s) return E_OUTOFMEMORY;
        HRESULT hr = s->LoadInner();
        if (SUCCEEDED(hr)) hr = s->QueryInterface(riid, ppv);
        Log("CreateInstance -> 0x%08lX", (unsigned long)hr);
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
