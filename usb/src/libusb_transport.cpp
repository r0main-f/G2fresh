#include "g2/usb/libusb_transport.hpp"

#include "g2/proto/frame.hpp"

#include <libusb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace g2::usb {

namespace {

constexpr unsigned kSendTimeoutMs = 2000;
constexpr auto kScanInterval = std::chrono::seconds(1);

} // namespace

struct LibusbTransport::Impl {
    enum class Kind { Arrived, Removed, Interrupt, Bulk };
    struct Event {
        Kind kind;
        std::vector<std::uint8_t> data;
    };

    libusb_context* ctx = nullptr;
    std::thread io;
    std::atomic<bool> running{false};

    // The open device. The I/O thread opens and closes it; send() uses it from
    // the client's thread, under deviceMutex.
    std::mutex deviceMutex;
    libusb_device_handle* handle = nullptr;
    std::uint8_t interruptIn = 0x81, bulkIn = 0x82, bulkOut = 0x03;
    int interruptSize = 16;

    // I/O thread only.
    libusb_transfer* interrupt = nullptr;
    libusb_transfer* bulk = nullptr;
    std::vector<std::uint8_t> interruptBuffer, bulkBuffer;
    bool interruptActive = false, bulkActive = false;
    bool announced = false; // Arrived was queued for the open device
    bool closing = false;   // no resubmissions while cancelling
    bool hotplug = false;
    libusb_hotplug_callback_handle hotplugHandle{};
    std::atomic<bool> arrivedHint{false}, goneHint{false};
    std::chrono::steady_clock::time_point nextScan{};

    std::mutex queueMutex;
    std::deque<Event> queue;
    mutable std::mutex errorMutex;
    std::string error;

    void push(Kind kind, std::vector<std::uint8_t> data = {})
    {
        const std::lock_guard lock(queueMutex);
        queue.push_back({kind, std::move(data)});
    }
    void setError(std::string e)
    {
        const std::lock_guard lock(errorMutex);
        error = std::move(e);
    }

    void run();
    void tryOpen();
    void closeDevice();
    void submitInterrupt();
    void submitBulk(std::size_t length);

    static void LIBUSB_CALL onInterrupt(libusb_transfer* t);
    static void LIBUSB_CALL onBulk(libusb_transfer* t);
    static int LIBUSB_CALL onHotplug(libusb_context*, libusb_device*, libusb_hotplug_event event, void* user);
};

// ---- I/O thread ----------------------------------------------------------------

void LibusbTransport::Impl::run()
{
    while (running) {
        if (handle && goneHint.exchange(false))
            closeDevice();
        if (!handle) {
            // With hotplug, arrivals wake us; a slow scan still catches a
            // device that could not be opened at its arrival (busy, no access).
            const auto now = std::chrono::steady_clock::now();
            if (arrivedHint.exchange(false) || now >= nextScan) {
                nextScan = now + (hotplug ? 5 * kScanInterval : kScanInterval);
                tryOpen();
            }
        }
        timeval tv{0, 100'000};
        libusb_handle_events_timeout_completed(ctx, &tv, nullptr);
    }
    closeDevice();
}

void LibusbTransport::Impl::tryOpen()
{
    libusb_device** list = nullptr;
    const auto count = libusb_get_device_list(ctx, &list);
    if (count < 0)
        return;
    libusb_device* found = nullptr;
    for (std::ptrdiff_t i = 0; i < static_cast<std::ptrdiff_t>(count) && !found; ++i) {
        libusb_device_descriptor d{};
        if (libusb_get_device_descriptor(list[i], &d) == 0 && d.idVendor == kVendorId && d.idProduct == kProductId)
            found = list[i];
    }
    libusb_device_handle* h = nullptr;
    int r = found ? libusb_open(found, &h) : LIBUSB_ERROR_NO_DEVICE;
    if (r != 0) {
        libusb_free_device_list(list, 1);
        setError(found ? std::string("cannot open the G2: ") + libusb_strerror(r) : std::string());
        return;
    }

    // The first configuration and interface 0, endpoints by type and direction.
    libusb_set_auto_detach_kernel_driver(h, 1); // unsupported on macOS and Windows: ignored
    libusb_config_descriptor* config = nullptr;
    r = libusb_get_config_descriptor(found, 0, &config);
    libusb_free_device_list(list, 1);
    if (r == 0) {
        int current = 0;
        if (libusb_get_configuration(h, &current) == 0 && current != config->bConfigurationValue)
            libusb_set_configuration(h, config->bConfigurationValue); // Windows: not supported, already set
        if (config->bNumInterfaces > 0 && config->interface[0].num_altsetting > 0) {
            const auto& alt = config->interface[0].altsetting[0];
            for (int e = 0; e < alt.bNumEndpoints; ++e) {
                const auto& ep = alt.endpoint[e];
                const auto type = ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
                const bool in = (ep.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN;
                if (type == LIBUSB_TRANSFER_TYPE_BULK && in)
                    bulkIn = ep.bEndpointAddress;
                else if (type == LIBUSB_TRANSFER_TYPE_BULK)
                    bulkOut = ep.bEndpointAddress;
                else if (type == LIBUSB_TRANSFER_TYPE_INTERRUPT && in) {
                    interruptIn = ep.bEndpointAddress;
                    interruptSize = std::max<int>(16, ep.wMaxPacketSize);
                }
            }
        }
        libusb_free_config_descriptor(config);
    }
    r = libusb_claim_interface(h, 0);
    if (r != 0) {
        libusb_close(h);
        setError(std::string("cannot claim the G2 (in use, or no driver): ") + libusb_strerror(r));
        return;
    }
    {
        const std::lock_guard lock(deviceMutex);
        handle = h;
    }
    setError({});
    interrupt = libusb_alloc_transfer(0);
    bulk = libusb_alloc_transfer(0);
    goneHint = false;
    announced = true;
    push(Kind::Arrived);
    submitInterrupt();
}

void LibusbTransport::Impl::closeDevice()
{
    if (!handle)
        return;
    // Cancel what is pending and let the callbacks run.
    closing = true;
    if (interruptActive)
        libusb_cancel_transfer(interrupt);
    if (bulkActive)
        libusb_cancel_transfer(bulk);
    for (int i = 0; i < 100 && (interruptActive || bulkActive); ++i) {
        timeval tv{0, 10'000};
        libusb_handle_events_timeout_completed(ctx, &tv, nullptr);
    }
    libusb_release_interface(handle, 0);
    {
        const std::lock_guard lock(deviceMutex);
        libusb_close(handle);
        handle = nullptr;
    }
    if (!interruptActive)
        libusb_free_transfer(interrupt);
    if (!bulkActive)
        libusb_free_transfer(bulk);
    interrupt = bulk = nullptr;
    interruptActive = bulkActive = false;
    closing = false;
    if (announced)
        push(Kind::Removed);
    announced = false;
}

void LibusbTransport::Impl::submitInterrupt()
{
    if (!handle || !interrupt || closing)
        return;
    interruptBuffer.assign(static_cast<std::size_t>(interruptSize), 0);
    libusb_fill_interrupt_transfer(interrupt, handle, interruptIn, interruptBuffer.data(), interruptSize, &Impl::onInterrupt,
                                   this, 0);
    if (libusb_submit_transfer(interrupt) == 0)
        interruptActive = true;
    else
        goneHint = true;
}

void LibusbTransport::Impl::submitBulk(std::size_t length)
{
    if (!handle || !bulk || closing)
        return;
    bulkBuffer.assign(std::max<std::size_t>(length, 1), 0);
    libusb_fill_bulk_transfer(bulk, handle, bulkIn, bulkBuffer.data(), static_cast<int>(bulkBuffer.size()), &Impl::onBulk,
                              this, 0);
    if (libusb_submit_transfer(bulk) == 0)
        bulkActive = true;
    else
        goneHint = true;
}

void LIBUSB_CALL LibusbTransport::Impl::onInterrupt(libusb_transfer* t)
{
    auto& self = *static_cast<Impl*>(t->user_data);
    self.interruptActive = false;
    switch (t->status) {
    case LIBUSB_TRANSFER_COMPLETED: {
        std::vector<std::uint8_t> packet(t->buffer, t->buffer + t->actual_length);
        packet.resize(std::max<std::size_t>(packet.size(), proto::kInterruptPacketSize), 0);
        const auto i = proto::parseInterrupt(packet);
        self.push(Kind::Interrupt, std::move(packet));
        // An extended message: its bulk-IN transfer comes before the next packet.
        if (i.kind == proto::Interrupt::Kind::Extended && i.length > 0)
            self.submitBulk(i.length);
        else
            self.submitInterrupt();
        return;
    }
    case LIBUSB_TRANSFER_CANCELLED:
        return; // closing
    case LIBUSB_TRANSFER_TIMED_OUT:
        self.submitInterrupt();
        return;
    default: // NO_DEVICE, ERROR, STALL, OVERFLOW
        self.goneHint = true;
        return;
    }
}

void LIBUSB_CALL LibusbTransport::Impl::onBulk(libusb_transfer* t)
{
    auto& self = *static_cast<Impl*>(t->user_data);
    self.bulkActive = false;
    switch (t->status) {
    case LIBUSB_TRANSFER_COMPLETED:
        self.push(Kind::Bulk, std::vector<std::uint8_t>(t->buffer, t->buffer + t->actual_length));
        self.submitInterrupt();
        return;
    case LIBUSB_TRANSFER_CANCELLED:
        return;
    default:
        self.goneHint = true;
        return;
    }
}

int LIBUSB_CALL LibusbTransport::Impl::onHotplug(libusb_context*, libusb_device* device, libusb_hotplug_event event, void* user)
{
    auto& self = *static_cast<Impl*>(user);
    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) {
        self.arrivedHint = true;
    } else if (self.handle && libusb_get_device(self.handle) == device) {
        self.goneHint = true;
    }
    return 0; // keep the callback
}

// ---- Transport -------------------------------------------------------------------

LibusbTransport::LibusbTransport() : impl_(std::make_unique<Impl>())
{
    if (libusb_init_context(&impl_->ctx, nullptr, 0) != 0) {
        impl_->ctx = nullptr;
        impl_->setError("USB is not available");
        return;
    }
    if (libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
        impl_->hotplug =
            libusb_hotplug_register_callback(impl_->ctx,
                                             static_cast<libusb_hotplug_event>(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED
                                                                               | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
                                             LIBUSB_HOTPLUG_NO_FLAGS, kVendorId, kProductId, LIBUSB_HOTPLUG_MATCH_ANY,
                                             &Impl::onHotplug, impl_.get(), &impl_->hotplugHandle)
            == LIBUSB_SUCCESS;
    }
    impl_->running = true;
    impl_->io = std::thread([impl = impl_.get()] { impl->run(); });
}

LibusbTransport::~LibusbTransport()
{
    if (!impl_->ctx)
        return;
    impl_->running = false;
    libusb_interrupt_event_handler(impl_->ctx);
    if (impl_->io.joinable())
        impl_->io.join();
    if (impl_->hotplug)
        libusb_hotplug_deregister_callback(impl_->ctx, impl_->hotplugHandle);
    libusb_exit(impl_->ctx);
}

bool LibusbTransport::available() const { return impl_->ctx != nullptr; }

std::string LibusbTransport::lastError() const
{
    const std::lock_guard lock(impl_->errorMutex);
    return impl_->error;
}

bool LibusbTransport::send(std::span<const std::uint8_t> frame)
{
    const std::lock_guard lock(impl_->deviceMutex);
    if (!impl_->handle)
        return false;
    std::vector<std::uint8_t> data(frame.begin(), frame.end());
    int sent = 0;
    const int r = libusb_bulk_transfer(impl_->handle, impl_->bulkOut, data.data(), static_cast<int>(data.size()), &sent,
                                       kSendTimeoutMs);
    return r == 0 && sent == static_cast<int>(data.size());
}

void LibusbTransport::poll()
{
    std::deque<Impl::Event> events;
    {
        const std::lock_guard lock(impl_->queueMutex);
        events.swap(impl_->queue);
    }
    if (!sink_)
        return;
    for (const auto& e : events) {
        switch (e.kind) {
        case Impl::Kind::Arrived: sink_->deviceArrived(); break;
        case Impl::Kind::Removed: sink_->deviceRemoved(); break;
        case Impl::Kind::Interrupt: sink_->interruptPacket(e.data); break;
        case Impl::Kind::Bulk: sink_->bulkIn(e.data); break;
        }
    }
}

} // namespace g2::usb
