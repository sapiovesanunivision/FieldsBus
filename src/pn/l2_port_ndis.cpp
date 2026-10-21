// softpn_ndis backend of L2Port (Windows): the own NDIS protocol driver softpn_ndis.sys
// (drivers/softpn_ndis), chosen by l2_port_windows.cpp. Replaces Npcap without a redistribution
// licence.
//
// - One overlapped handle per port: the driver allows one open handle per adapter.
// - kPendingReads reads stay posted in the driver. The driver completes them in the order they
//   were posted, so receive() always waits for the oldest one and reposts it after copying.
// - The driver filters (EtherTypes 0x8892, 0x88CC, the optional extra type, 802.1Q-tagged
//   0x8892), never returns our own frames and re-inserts a VLAN tag the NIC stripped.
#include "l2_port_windows.hpp"

#include "pn_defs.hpp"

#include <winioctl.h>
#include <ntddndis.h>

#include "softpn_ndis_ioctl.h"

#include <cstring>
#include <mutex>

namespace softpn {
namespace {

constexpr size_t kPendingReads = 4;
constexpr DWORD kSendTimeoutMs = 1000;

std::wstring widen(const std::string& s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

HANDLE openControl(bool overlapped)
{
    return CreateFileW(SOFTPN_NDIS_DOS_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                       overlapped ? FILE_FLAG_OVERLAPPED : 0, nullptr);
}

std::string controlError(DWORD err)
{
    if (err == ERROR_FILE_NOT_FOUND)
        return "softpn_ndis driver not running (install it, then: net start SoftPnNdis)";
    if (err == ERROR_ACCESS_DENIED)
        return "softpn_ndis: access denied (the driver allows administrators and services only)";
    return "softpn_ndis: cannot open " + win::narrow(SOFTPN_NDIS_DOS_NAME) + ", error " + std::to_string(err);
}

// DeviceIoControl on an overlapped handle, waited for synchronously.
bool ioctl(HANDLE h, DWORD code, const void* in, DWORD inLen, void* out, DWORD outLen, DWORD* got = nullptr)
{
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent)
        return false;
    DWORD n = 0;
    BOOL ok = DeviceIoControl(h, code, const_cast<void*>(in), inLen, out, outLen, &n, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = GetOverlappedResult(h, &ov, &n, TRUE);
    const DWORD err = GetLastError();
    CloseHandle(ov.hEvent);
    if (got)
        *got = n;
    SetLastError(err);
    return ok != FALSE;
}

// Opens the adapter on a new handle and sets the receive filter. INVALID_HANDLE_VALUE on failure.
HANDLE openAdapter(const AdapterInfo& adapter, uint16_t extraEtherType, std::string* error)
{
    auto fail = [&](const std::string& m) {
        if (error)
            *error = m;
        return INVALID_HANDLE_VALUE;
    };
    HANDLE h = openControl(true);
    if (h == INVALID_HANDLE_VALUE)
        return fail(controlError(GetLastError()));

    ULONG version = 0;
    if (!ioctl(h, IOCTL_SOFTPN_GET_VERSION, nullptr, 0, &version, sizeof version) || version != SOFTPN_NDIS_API_VERSION) {
        CloseHandle(h);
        return fail("softpn_ndis: driver API version " + std::to_string(version) + ", expected " +
                    std::to_string(SOFTPN_NDIS_API_VERSION) + " (driver and program do not match)");
    }

    const std::wstring name = widen(adapter.name);
    if (!ioctl(h, IOCTL_NDISPROT_OPEN_DEVICE, name.data(), DWORD(name.size() * sizeof(wchar_t)), nullptr, 0)) {
        const DWORD err = GetLastError();
        CloseHandle(h);
        if (err == ERROR_BUSY)
            return fail("softpn_ndis: " + adapter.friendlyName + " is already opened by another program "
                        "(one program per adapter)");
        return fail("softpn_ndis: cannot open " + adapter.friendlyName + ", error " + std::to_string(err));
    }

    SOFTPN_FILTER f{};
    f.Version = SOFTPN_NDIS_API_VERSION;
    f.Flags = SOFTPN_FILTER_VLAN | SOFTPN_FILTER_DROP_OWN;
    f.EtherTypes[f.EtherTypeCount++] = kEtherTypeProfinet;
    f.EtherTypes[f.EtherTypeCount++] = kEtherTypeLldp;
    if (extraEtherType != 0)
        f.EtherTypes[f.EtherTypeCount++] = extraEtherType;
    if (!ioctl(h, IOCTL_SOFTPN_SET_FILTER, &f, sizeof f, nullptr, 0)) {
        const DWORD err = GetLastError();
        CloseHandle(h);
        return fail("softpn_ndis: set filter failed, error " + std::to_string(err));
    }
    return h;
}

class NdisPort : public L2Port {
public:
    NdisPort(AdapterInfo info, HANDLE h, uint16_t extraEtherType)
        : info_(std::move(info)), h_(h), extraEtherType_(extraEtherType)
    {
        interruptEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        txEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        for (Slot& s : slots_) {
            s.ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            s.buf.resize(kMaxFrameSize + 4);
        }
        haveLuid_ = win::luidFromGuidName(info_.name, luid_);
        for (Slot& s : slots_)
            post(s);
    }

    ~NdisPort() override
    {
        drainReads();
        for (Slot& s : slots_) {
            if (s.ov.hEvent)
                CloseHandle(s.ov.hEvent);
        }
        CloseHandle(h_);
        if (interruptEvent_)
            CloseHandle(interruptEvent_);
        if (txEvent_)
            CloseHandle(txEvent_);
    }

    bool send(const uint8_t* frame, size_t len) override
    {
        uint8_t padded[kMinFrameSize] = {};
        if (len < kMinFrameSize) {
            std::memcpy(padded, frame, len);
            frame = padded;
            len = kMinFrameSize;
        }
        std::lock_guard<std::mutex> lock(txMutex_);
        OVERLAPPED ov{};
        ov.hEvent = txEvent_;
        ResetEvent(txEvent_);
        DWORD n = 0;
        BOOL ok = WriteFile(h_, frame, DWORD(len), &n, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(txEvent_, kSendTimeoutMs) != WAIT_OBJECT_0)
                CancelIoEx(h_, &ov); // NIC stuck: give up on this frame
            ok = GetOverlappedResult(h_, &ov, &n, TRUE);
        }
        return ok && n == len;
    }

    bool receive(std::vector<uint8_t>& frame, int timeoutMs) override
    {
        Slot& s = slots_[next_];
        if (!s.pending && !post(s)) {
            // Adapter gone (unbind, adapter disabled, driver restarted): the handle is dead for good.
            // Reopen the adapter once it is bound again; don't spin, the caller retries.
            Sleep(100);
            reopen();
            return false;
        }
        HANDLE events[2] = {s.ov.hEvent, interruptEvent_};
        DWORD w = WaitForMultipleObjects(2, events, FALSE, DWORD(timeoutMs < 0 ? 0 : timeoutMs));
        if (w != WAIT_OBJECT_0)
            return false; // timeout, interrupt() or wait error
        DWORD n = 0;
        const BOOL ok = GetOverlappedResult(h_, &s.ov, &n, FALSE);
        s.pending = false;
        next_ = (next_ + 1) % kPendingReads;
        if (ok)
            frame.assign(s.buf.data(), s.buf.data() + n);
        else if (GetLastError() != ERROR_OPERATION_ABORTED)
            Sleep(100); // read failed (adapter unbound): don't spin, the caller retries
        post(s);
        return ok && n > 0;
    }

    void interrupt() override { SetEvent(interruptEvent_); }

    const AdapterInfo& info() const override { return info_; }

    LinkStatus link() const override { return haveLuid_ ? win::linkStatus(luid_) : LinkStatus{}; }

private:
    struct Slot {
        OVERLAPPED ov{};
        std::vector<uint8_t> buf;
        bool pending = false;
    };

    // Cancels the posted reads and waits until each has completed.
    void drainReads()
    {
        CancelIoEx(h_, nullptr);
        for (Slot& s : slots_) {
            DWORD n = 0;
            if (s.pending)
                GetOverlappedResult(h_, &s.ov, &n, TRUE);
            s.pending = false;
        }
    }

    // Called from receive() only. At most once per second: open the adapter on a new handle and,
    // if that works, replace the dead one (send() holds txMutex_ while it uses h_).
    void reopen()
    {
        const ULONGLONG now = GetTickCount64();
        if (now - lastReopen_ < 1000)
            return;
        lastReopen_ = now;
        HANDLE h = openAdapter(info_, extraEtherType_, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return; // not bound yet
        drainReads();
        {
            std::lock_guard<std::mutex> lock(txMutex_);
            CloseHandle(h_);
            h_ = h;
        }
        next_ = 0;
        for (Slot& s : slots_)
            post(s);
    }

    // Posts one read. A read that completes at once still signals its event, so receive() treats
    // both cases alike.
    bool post(Slot& s)
    {
        HANDLE ev = s.ov.hEvent;
        s.ov = OVERLAPPED{};
        s.ov.hEvent = ev;
        ResetEvent(ev);
        DWORD n = 0;
        BOOL ok = ReadFile(h_, s.buf.data(), DWORD(s.buf.size()), &n, &s.ov);
        s.pending = ok || GetLastError() == ERROR_IO_PENDING;
        return s.pending;
    }

    AdapterInfo info_;
    HANDLE h_;
    uint16_t extraEtherType_ = 0;
    ULONGLONG lastReopen_ = 0;
    NET_LUID luid_{};
    bool haveLuid_ = false;
    Slot slots_[kPendingReads];
    size_t next_ = 0;
    HANDLE interruptEvent_ = nullptr;
    HANDLE txEvent_ = nullptr;
    std::mutex txMutex_;
};

} // namespace

bool ndisAvailable(std::string* why)
{
    HANDLE h = openControl(false);
    if (h == INVALID_HANDLE_VALUE) {
        if (why)
            *why = controlError(GetLastError());
        return false;
    }
    CloseHandle(h);
    return true;
}

std::vector<AdapterInfo> ndisListAdapters(std::string* error)
{
    std::vector<AdapterInfo> list;
    HANDLE h = openControl(true);
    if (h == INVALID_HANDLE_VALUE) {
        if (error)
            *error = controlError(GetLastError());
        return list;
    }
    // Returns once the driver has bound to all adapters (or after 5 s).
    (void)ioctl(h, IOCTL_NDISPROT_BIND_WAIT, nullptr, 0, nullptr, 0);
    for (ULONG i = 0;; ++i) {
        alignas(8) uint8_t buf[2048] = {};
        auto* q = reinterpret_cast<NDISPROT_QUERY_BINDING*>(buf);
        q->BindingIndex = i;
        DWORD got = 0;
        if (!ioctl(h, IOCTL_NDISPROT_QUERY_BINDING, q, sizeof *q, buf, sizeof buf, &got))
            break;
        if (q->DeviceNameOffset + q->DeviceNameLength > got || q->DeviceDescrOffset + q->DeviceDescrLength > got)
            continue;
        std::wstring name(reinterpret_cast<const wchar_t*>(buf + q->DeviceNameOffset), q->DeviceNameLength / sizeof(wchar_t));
        std::wstring descr(reinterpret_cast<const wchar_t*>(buf + q->DeviceDescrOffset),
                           q->DeviceDescrLength / sizeof(wchar_t));
        AdapterInfo info;
        info.name = win::narrow(name.c_str());
        info.description = win::narrow(descr.c_str());
        info.backend = "ndis";
        list.push_back(info);
    }
    CloseHandle(h);
    win::addIpHelperInfo(list);
    if (list.empty() && error && error->empty())
        *error = "softpn_ndis: no adapter bound";
    return list;
}

std::unique_ptr<L2Port> ndisOpen(const AdapterInfo& adapter, const L2Options& options, std::string* error)
{
    HANDLE h = openAdapter(adapter, options.extraEtherType, error);
    if (h == INVALID_HANDLE_VALUE)
        return nullptr;
    // options.promiscuous is not needed: the driver requests "all multicast" for every open.
    return std::make_unique<NdisPort>(adapter, h, options.extraEtherType);
}

} // namespace softpn
