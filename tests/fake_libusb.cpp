// See fake_libusb.hpp. Only what LibusbTransport calls is here.
#include "fake_libusb.hpp"

#include <libusb.h>

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>

struct libusb_context {
    int unused = 0;
};
struct libusb_device {
    int unused = 0;
};
struct libusb_device_handle {
    libusb_device* device = nullptr;
    int plugged = 0; // the stay of the G2 it was opened in
};

namespace {

using Time = std::chrono::microseconds; // on the test's clock

struct BulkItem {
    bool fails = false;
    fakeusb::Fail how = fakeusb::Fail::Error;
    fakeusb::Bytes data;
};
struct Pending {
    libusb_transfer* t;
    Time submitted;
    bool gone = false; // the G2 left
    bool cancelled = false;
    Time cancelledAt{};
};

// All under m.
std::mutex m;
std::condition_variable work;  // the I/O thread waits: in handle_events, in a held cancel
std::condition_variable quiet; // the test waits for the I/O thread to be idle
Time vnow{};
bool live = false;     // a context exists, and so an I/O thread
bool idle = false;     // it waits in handle_events, with nothing to do before wakeAt
Time wakeAt{};
bool stopping = false; // libusb_interrupt_event_handler was called
bool holdCancel = false, held = false;
bool hotplug = false, present = false, interrupted = false;
int plugs = 0; // a handle opened before the last one is of a G2 that left
int failingOpens = 0;
Time cancelDelay{};
libusb_device g2;
std::deque<fakeusb::Bytes> interrupts;
std::deque<BulkItem> bulks;
std::vector<Pending> pending;
libusb_hotplug_callback_fn hotplugCallback = nullptr;
void* hotplugUser = nullptr;
std::deque<libusb_hotplug_event> hotplugEvents;
fakeusb::Stats counts;

// Waits until the I/O thread is idle (m held). The only wait in real time.
void quiesce(std::unique_lock<std::mutex>& lock)
{
    if (!quiet.wait_for(lock, fakeusb::kGuard, [] { return idle || held || !live; }))
        throw std::runtime_error("fake libusb: the transport's I/O thread is not idle after "
                                 + std::to_string(fakeusb::kGuard.count()) + " s");
}

// The script changed something: the I/O thread looks again (m held).
void changed()
{
    idle = false;
    work.notify_all();
}

// When a pending transfer completes by itself (m held).
Time due(const Pending& p)
{
    if (p.cancelled)
        return p.cancelledAt + cancelDelay;
    if (!p.t->timeout)
        return (Time::max)(); // never (windows.h has min and max macros)
    return p.submitted + std::chrono::milliseconds(p.t->timeout);
}

// The outcome of a pending transfer now, or false (m held).
bool ready(Pending& p)
{
    auto& t = *p.t;
    t.actual_length = 0;
    if (p.cancelled) {
        if (vnow < due(p))
            return false;
        t.status = LIBUSB_TRANSFER_CANCELLED;
        ++counts.cancelled;
        return true;
    }
    if (p.gone) {
        t.status = LIBUSB_TRANSFER_NO_DEVICE;
        return true;
    }
    if (t.type == LIBUSB_TRANSFER_TYPE_INTERRUPT && !interrupts.empty()) {
        const auto& d = interrupts.front();
        t.actual_length = static_cast<int>(std::min<std::size_t>(d.size(), static_cast<std::size_t>(t.length)));
        std::memcpy(t.buffer, d.data(), static_cast<std::size_t>(t.actual_length));
        interrupts.pop_front();
        t.status = LIBUSB_TRANSFER_COMPLETED;
        return true;
    }
    if (t.type == LIBUSB_TRANSFER_TYPE_BULK && !bulks.empty()) {
        const auto b = std::move(bulks.front());
        bulks.pop_front();
        if (b.fails) {
            t.status = b.how == fakeusb::Fail::Stall ? LIBUSB_TRANSFER_STALL : LIBUSB_TRANSFER_ERROR;
        } else if (b.data.size() > static_cast<std::size_t>(t.length)) {
            t.status = LIBUSB_TRANSFER_OVERFLOW;
        } else {
            t.actual_length = static_cast<int>(b.data.size());
            std::memcpy(t.buffer, b.data.data(), b.data.size());
            t.status = LIBUSB_TRANSFER_COMPLETED;
        }
        return true;
    }
    if (vnow >= due(p)) {
        t.status = LIBUSB_TRANSFER_TIMED_OUT;
        return true;
    }
    return false;
}

// m held.
bool usable(const libusb_device_handle* h) { return present && h->plugged == plugs; }

} // namespace

// ---- The script -----------------------------------------------------------------

namespace fakeusb {

void reset(bool withHotplug)
{
    const std::lock_guard lock(m);
    vnow = std::chrono::hours(1); // any time will do
    live = idle = stopping = holdCancel = held = false;
    hotplug = withHotplug;
    present = interrupted = false;
    plugs = failingOpens = 0;
    cancelDelay = {};
    interrupts.clear();
    bulks.clear();
    pending.clear();
    hotplugCallback = nullptr;
    hotplugEvents.clear();
    counts = {};
}

std::chrono::steady_clock::time_point now()
{
    const std::lock_guard lock(m);
    return std::chrono::steady_clock::time_point(std::chrono::duration_cast<std::chrono::steady_clock::duration>(vnow));
}

void settle()
{
    std::unique_lock lock(m);
    quiesce(lock);
}

void advance(std::chrono::milliseconds by)
{
    std::unique_lock lock(m);
    quiesce(lock);
    const auto target = vnow + by;
    while (vnow < target) {
        // No further than the next moment the I/O thread has something to do:
        // it reads there the time it would have read.
        vnow = live && idle ? (std::min)(target, wakeAt) : target;
        changed();
        quiesce(lock);
    }
}

void plug()
{
    std::unique_lock lock(m);
    quiesce(lock);
    present = true;
    ++plugs;
    hotplugEvents.push_back(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED);
    changed();
}

void unplug()
{
    std::unique_lock lock(m);
    quiesce(lock);
    present = false;
    for (auto& p : pending)
        p.gone = true;
    hotplugEvents.push_back(LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT);
    changed();
}

void arrivalAgain()
{
    std::unique_lock lock(m);
    quiesce(lock);
    hotplugEvents.push_back(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED);
    changed();
}

void openFails(int count)
{
    std::unique_lock lock(m);
    quiesce(lock);
    failingOpens = count;
}

void interrupt(Bytes packet)
{
    std::unique_lock lock(m);
    quiesce(lock);
    interrupts.push_back(std::move(packet));
    changed();
}

void bulkIn(Bytes data)
{
    std::unique_lock lock(m);
    quiesce(lock);
    bulks.push_back({false, Fail::Error, std::move(data)});
    changed();
}

void bulkInFails(Fail how)
{
    std::unique_lock lock(m);
    quiesce(lock);
    bulks.push_back({true, how, {}});
    changed();
}

void cancelTakes(std::chrono::milliseconds delay)
{
    std::unique_lock lock(m);
    quiesce(lock);
    cancelDelay = delay;
    changed();
}

void holdCancelUntilStopped()
{
    std::unique_lock lock(m);
    quiesce(lock);
    holdCancel = true;
}

Stats stats()
{
    std::unique_lock lock(m);
    quiesce(lock);
    return counts;
}

} // namespace fakeusb

// ---- libusb ---------------------------------------------------------------------

int LIBUSB_CALL libusb_init_context(libusb_context** ctx, const struct libusb_init_option[], int)
{
    const std::lock_guard lock(m);
    live = true;
    idle = stopping = interrupted = false;
    *ctx = new libusb_context;
    return 0;
}

void LIBUSB_CALL libusb_exit(libusb_context* ctx)
{
    {
        const std::lock_guard lock(m);
        live = idle = false;
        quiet.notify_all();
    }
    delete ctx;
}

int LIBUSB_CALL libusb_has_capability(uint32_t capability)
{
    const std::lock_guard lock(m);
    return capability == LIBUSB_CAP_HAS_HOTPLUG && hotplug ? 1 : 0;
}

const char* LIBUSB_CALL libusb_strerror(int) { return "fake error"; }

int LIBUSB_CALL libusb_hotplug_register_callback(libusb_context*, int, int, int, int, int, libusb_hotplug_callback_fn cb_fn,
                                                 void* user_data, libusb_hotplug_callback_handle* callback_handle)
{
    const std::lock_guard lock(m);
    hotplugCallback = cb_fn;
    hotplugUser = user_data;
    hotplugEvents.clear();
    *callback_handle = 1;
    return LIBUSB_SUCCESS;
}

void LIBUSB_CALL libusb_hotplug_deregister_callback(libusb_context*, libusb_hotplug_callback_handle)
{
    const std::lock_guard lock(m);
    hotplugCallback = nullptr;
}

// The transport calls it only when it is destroyed.
void LIBUSB_CALL libusb_interrupt_event_handler(libusb_context*)
{
    const std::lock_guard lock(m);
    interrupted = stopping = true;
    changed();
}

int LIBUSB_CALL libusb_handle_events_timeout_completed(libusb_context* ctx, struct timeval* tv, int*)
{
    std::vector<libusb_transfer*> done;
    std::vector<libusb_hotplug_event> events;
    libusb_hotplug_callback_fn callback = nullptr;
    void* user = nullptr;
    {
        std::unique_lock lock(m);
        const auto until = vnow + std::chrono::seconds(tv->tv_sec) + std::chrono::microseconds(tv->tv_usec);
        for (;;) {
            // Hotplug events first and alone: the transport hears of a
            // departure while its transfers are still pending, and has to
            // cancel them.
            if (hotplugCallback) {
                events.assign(hotplugEvents.begin(), hotplugEvents.end());
                callback = hotplugCallback;
                user = hotplugUser;
            }
            hotplugEvents.clear();
            auto wake = until;
            for (auto it = pending.begin(); events.empty() && it != pending.end();) {
                if (ready(*it)) {
                    done.push_back(it->t);
                    it = pending.erase(it);
                } else {
                    wake = (std::min)(wake, due(*it));
                    ++it;
                }
            }
            if (!done.empty() || !events.empty() || interrupted || vnow >= until)
                break;
            if (stopping) {
                vnow = wake;
                ++counts.waitsWhileStopping;
                continue;
            }
            // Looking for work above and waiting here are one step under m:
            // what the script does in between is not missed.
            wakeAt = wake;
            idle = true;
            quiet.notify_all();
            work.wait(lock);
        }
        idle = false;
        interrupted = false;
    }
    for (const auto e : events)
        callback(ctx, &g2, e, user);
    for (auto* t : done)
        t->callback(t);
    return 0;
}

ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context*, libusb_device*** list)
{
    const std::lock_guard lock(m);
    ++counts.scans;
    *list = new libusb_device*[2]{present ? &g2 : nullptr, nullptr};
    return present ? 1 : 0;
}

void LIBUSB_CALL libusb_free_device_list(libusb_device** list, int) { delete[] list; }

int LIBUSB_CALL libusb_get_device_descriptor(libusb_device*, struct libusb_device_descriptor* desc)
{
    *desc = {};
    desc->idVendor = 0x0FFC;
    desc->idProduct = 0x0002;
    desc->bNumConfigurations = 1;
    return 0;
}

int LIBUSB_CALL libusb_open(libusb_device* dev, libusb_device_handle** dev_handle)
{
    const std::lock_guard lock(m);
    counts.calls.push_back("open");
    if (failingOpens > 0) {
        --failingOpens;
        return LIBUSB_ERROR_ACCESS;
    }
    *dev_handle = new libusb_device_handle{dev, plugs};
    return 0;
}

void LIBUSB_CALL libusb_close(libusb_device_handle* dev_handle)
{
    {
        const std::lock_guard lock(m);
        counts.calls.push_back("close");
    }
    delete dev_handle;
}

libusb_device* LIBUSB_CALL libusb_get_device(libusb_device_handle* dev_handle) { return dev_handle->device; }

int LIBUSB_CALL libusb_reset_device(libusb_device_handle*)
{
    const std::lock_guard lock(m);
    counts.calls.push_back("reset");
    return 0;
}

int LIBUSB_CALL libusb_set_auto_detach_kernel_driver(libusb_device_handle*, int) { return LIBUSB_ERROR_NOT_SUPPORTED; }

int LIBUSB_CALL libusb_get_config_descriptor(libusb_device*, uint8_t, struct libusb_config_descriptor** config)
{
    // The G2's (re/notes/usb-crosscheck.md §1): one vendor interface, 0x81
    // interrupt-IN 16 bytes, 0x82 bulk-IN and 0x03 bulk-OUT 64 bytes.
    static libusb_endpoint_descriptor endpoints[3] = {};
    endpoints[0].bEndpointAddress = 0x81;
    endpoints[0].bmAttributes = LIBUSB_TRANSFER_TYPE_INTERRUPT;
    endpoints[0].wMaxPacketSize = 16;
    endpoints[1].bEndpointAddress = 0x82;
    endpoints[1].bmAttributes = LIBUSB_TRANSFER_TYPE_BULK;
    endpoints[1].wMaxPacketSize = 64;
    endpoints[2].bEndpointAddress = 0x03;
    endpoints[2].bmAttributes = LIBUSB_TRANSFER_TYPE_BULK;
    endpoints[2].wMaxPacketSize = 64;
    static libusb_interface_descriptor alt = {};
    alt.bNumEndpoints = 3;
    alt.endpoint = endpoints;
    static libusb_interface interface0 = {};
    interface0.altsetting = &alt;
    interface0.num_altsetting = 1;
    static libusb_config_descriptor c = {};
    c.bConfigurationValue = 1;
    c.bNumInterfaces = 1;
    c.interface = &interface0;
    *config = &c;
    return 0;
}

void LIBUSB_CALL libusb_free_config_descriptor(struct libusb_config_descriptor*) {}

int LIBUSB_CALL libusb_get_configuration(libusb_device_handle*, int* config)
{
    *config = 1;
    return 0;
}

int LIBUSB_CALL libusb_set_configuration(libusb_device_handle*, int) { return 0; }

int LIBUSB_CALL libusb_claim_interface(libusb_device_handle*, int)
{
    const std::lock_guard lock(m);
    counts.calls.push_back("claim");
    return 0;
}

int LIBUSB_CALL libusb_release_interface(libusb_device_handle*, int) { return 0; }

struct libusb_transfer* LIBUSB_CALL libusb_alloc_transfer(int)
{
    const std::lock_guard lock(m);
    ++counts.transfers;
    return static_cast<libusb_transfer*>(std::calloc(1, sizeof(libusb_transfer)));
}

void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer* transfer)
{
    const std::lock_guard lock(m);
    --counts.transfers;
    std::free(transfer);
}

int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer* transfer)
{
    const std::lock_guard lock(m);
    if (!usable(transfer->dev_handle))
        return LIBUSB_ERROR_NO_DEVICE;
    if (transfer->type == LIBUSB_TRANSFER_TYPE_BULK) {
        ++counts.bulkReads;
        counts.readTimeout = transfer->timeout;
    }
    pending.push_back({transfer, vnow});
    return 0;
}

int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer* transfer)
{
    std::unique_lock lock(m);
    if (holdCancel) {
        holdCancel = false;
        held = true;
        quiet.notify_all();
        work.wait(lock, [] { return stopping; });
        held = false;
    }
    for (auto& p : pending) {
        if (p.t == transfer && !p.cancelled) {
            p.cancelled = true;
            p.cancelledAt = vnow;
        }
    }
    return 0;
}

int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle* dev_handle, unsigned char, unsigned char* data, int length,
                                     int* transferred, unsigned int timeout)
{
    const std::lock_guard lock(m);
    if (!usable(dev_handle))
        return LIBUSB_ERROR_NO_DEVICE;
    counts.sendTimeout = timeout;
    counts.sent.emplace_back(data, data + length);
    *transferred = length;
    return 0;
}
