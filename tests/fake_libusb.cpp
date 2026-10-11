// See fake_libusb.hpp. Only what LibusbTransport calls is here.
#include "fake_libusb.hpp"

#include <libusb.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

struct libusb_context {
    int unused = 0;
};
struct libusb_device {
    int unused = 0;
};
struct libusb_device_handle {
    libusb_device* device = nullptr;
};

namespace {

using Clock = std::chrono::steady_clock;

struct BulkItem {
    bool fails = false;
    fakeusb::Fail how = fakeusb::Fail::Error;
    fakeusb::Bytes data;
};
struct Pending {
    libusb_transfer* t;
    Clock::time_point submitted;
    bool cancelled = false;
};

std::mutex m;
std::condition_variable cv;
bool hotplug = false, present = false, interrupted = false;
libusb_device g2;
std::deque<fakeusb::Bytes> interrupts;
std::deque<BulkItem> bulks;
std::vector<Pending> pending;
libusb_hotplug_callback_fn hotplugCallback = nullptr;
void* hotplugUser = nullptr;
std::deque<libusb_hotplug_event> hotplugEvents;
fakeusb::Stats counts;

// The outcome of a pending transfer now, or false (m held).
bool ready(Pending& p, Clock::time_point now)
{
    auto& t = *p.t;
    t.actual_length = 0;
    if (p.cancelled) {
        t.status = LIBUSB_TRANSFER_CANCELLED;
        return true;
    }
    if (!present) {
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
    if (t.timeout && now - p.submitted >= std::chrono::milliseconds(t.timeout)) {
        t.status = LIBUSB_TRANSFER_TIMED_OUT;
        return true;
    }
    return false;
}

} // namespace

// ---- The script -----------------------------------------------------------------

namespace fakeusb {

void reset(bool withHotplug)
{
    const std::lock_guard lock(m);
    hotplug = withHotplug;
    present = interrupted = false;
    interrupts.clear();
    bulks.clear();
    pending.clear();
    hotplugCallback = nullptr;
    hotplugEvents.clear();
    counts = {};
}

void plug()
{
    const std::lock_guard lock(m);
    present = true;
    hotplugEvents.push_back(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED);
    cv.notify_all();
}

void unplug()
{
    const std::lock_guard lock(m);
    present = false;
    hotplugEvents.push_back(LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT);
    cv.notify_all();
}

void arrivalAgain()
{
    const std::lock_guard lock(m);
    hotplugEvents.push_back(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED);
    cv.notify_all();
}

void interrupt(Bytes packet)
{
    const std::lock_guard lock(m);
    interrupts.push_back(std::move(packet));
    cv.notify_all();
}

void bulkIn(Bytes data)
{
    const std::lock_guard lock(m);
    bulks.push_back({false, Fail::Error, std::move(data)});
    cv.notify_all();
}

void bulkInFails(Fail how)
{
    const std::lock_guard lock(m);
    bulks.push_back({true, how, {}});
    cv.notify_all();
}

Stats stats()
{
    const std::lock_guard lock(m);
    return counts;
}

} // namespace fakeusb

// ---- libusb ---------------------------------------------------------------------

int LIBUSB_CALL libusb_init_context(libusb_context** ctx, const struct libusb_init_option[], int)
{
    *ctx = new libusb_context;
    return 0;
}

void LIBUSB_CALL libusb_exit(libusb_context* ctx) { delete ctx; }

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

void LIBUSB_CALL libusb_interrupt_event_handler(libusb_context*)
{
    const std::lock_guard lock(m);
    interrupted = true;
    cv.notify_all();
}

int LIBUSB_CALL libusb_handle_events_timeout_completed(libusb_context* ctx, struct timeval* tv, int*)
{
    std::vector<libusb_transfer*> done;
    std::vector<libusb_hotplug_event> events;
    libusb_hotplug_callback_fn callback = nullptr;
    void* user = nullptr;
    {
        std::unique_lock lock(m);
        const auto until = Clock::now() + std::chrono::seconds(tv->tv_sec) + std::chrono::microseconds(tv->tv_usec);
        for (;;) {
            const auto now = Clock::now();
            for (auto it = pending.begin(); it != pending.end();) {
                if (ready(*it, now)) {
                    done.push_back(it->t);
                    it = pending.erase(it);
                } else {
                    ++it;
                }
            }
            if (hotplugCallback) {
                events.assign(hotplugEvents.begin(), hotplugEvents.end());
                callback = hotplugCallback;
                user = hotplugUser;
            }
            hotplugEvents.clear();
            if (!done.empty() || !events.empty() || interrupted || now >= until)
                break;
            auto wake = until;
            for (const auto& p : pending)
                if (p.t->timeout)
                    wake = (std::min)(wake, p.submitted + std::chrono::milliseconds(p.t->timeout)); // windows.h has a min macro
            cv.wait_until(lock, wake);
        }
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
    *dev_handle = new libusb_device_handle{dev};
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
    return static_cast<libusb_transfer*>(std::calloc(1, sizeof(libusb_transfer)));
}

void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer* transfer) { std::free(transfer); }

int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer* transfer)
{
    const std::lock_guard lock(m);
    if (!present)
        return LIBUSB_ERROR_NO_DEVICE;
    if (transfer->type == LIBUSB_TRANSFER_TYPE_BULK) {
        ++counts.bulkReads;
        counts.readTimeout = transfer->timeout;
    }
    pending.push_back({transfer, Clock::now()});
    cv.notify_all();
    return 0;
}

int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer* transfer)
{
    const std::lock_guard lock(m);
    for (auto& p : pending)
        if (p.t == transfer)
            p.cancelled = true;
    cv.notify_all();
    return 0;
}

int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle*, unsigned char, unsigned char* data, int length, int* transferred,
                                     unsigned int timeout)
{
    const std::lock_guard lock(m);
    if (!present)
        return LIBUSB_ERROR_NO_DEVICE;
    counts.sendTimeout = timeout;
    counts.sent.emplace_back(data, data + length);
    *transferred = length;
    return 0;
}
