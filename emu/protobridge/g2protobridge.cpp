// G2fresh's own editor protocol client (proto::Client) behind a C interface,
// for tools/firmware/g2hostemu.py: the host emulator models the G2's USB chip
// and moves bytes between it and this client, so that our editor code talks
// to the G2's real OS running in the emulator (re/notes/usb-protocol.md).
//
//   g2p_create / g2p_destroy
//   g2p_arrive          the device is "plugged in"
//   g2p_interrupt       a 16-byte interrupt-IN packet from the device
//   g2p_bulk_in         one bulk-IN transfer from the device
//   g2p_take_out        the next bulk-OUT frame for the device (0 if none)
//   g2p_tick            advance the client's clock (ms) and run it
//   g2p_send_patch      upload a .pch2 file into a slot
#include "g2/file.hpp"
#include "g2/patch.hpp"
#include "g2/proto/client.hpp"
#include "g2/proto/transport.hpp"

#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace g2;
using namespace g2::proto;

namespace
{
struct QueueTransport : Transport
{
	struct In { bool bulk; std::vector<std::uint8_t> data; };
	std::deque<std::vector<std::uint8_t>> out;
	std::deque<In> in;
	bool arrived = false, announced = false;

	bool send(std::span<const std::uint8_t> frame) override
	{
		out.emplace_back(frame.begin(), frame.end());
		return true;
	}

	void poll() override
	{
		if(!sink_) return;
		if(arrived && !announced) { announced = true; sink_->deviceArrived(); }
		while(!in.empty())
		{
			In x = std::move(in.front());
			in.pop_front();
			if(x.bulk) sink_->bulkIn(x.data);
			else sink_->interruptPacket(x.data);
		}
	}
};

struct Logger : Client::Listener
{
	void statusChanged(Status s) override { std::fprintf(stderr, "proto: status %s\n", statusText(s)); }
	void synced() override { std::fprintf(stderr, "proto: synced\n"); }
	void slotChanged(int slot) override { std::fprintf(stderr, "proto: slot %d changed\n", slot); }
	void error(std::uint8_t code) override { std::fprintf(stderr, "proto: synth exception %02x\n", code); }
};

struct Handle
{
	QueueTransport transport;
	ManualClock clock;
	Logger logger;
	std::unique_ptr<Client> client;

	Handle()
	{
		client = std::make_unique<Client>(transport, clock);
		client->setListener(&logger);
	}
};
}

extern "C"
{
	void* g2p_create() { return new Handle(); }
	void g2p_destroy(void* h) { delete static_cast<Handle*>(h); }
	void g2p_arrive(void* h) { static_cast<Handle*>(h)->transport.arrived = true; }
	void g2p_interrupt(void* h, const std::uint8_t* p, int n)
	{
		static_cast<Handle*>(h)->transport.in.push_back({false, std::vector<std::uint8_t>(p, p + n)});
	}
	void g2p_bulk_in(void* h, const std::uint8_t* p, int n)
	{
		static_cast<Handle*>(h)->transport.in.push_back({true, std::vector<std::uint8_t>(p, p + n)});
	}
	int g2p_take_out(void* h, std::uint8_t* buf, int max)
	{
		auto& q = static_cast<Handle*>(h)->transport.out;
		if(q.empty()) return 0;
		const auto f = std::move(q.front());
		q.pop_front();
		const int n = int(f.size()) < max ? int(f.size()) : max;
		std::memcpy(buf, f.data(), size_t(n));
		return n;
	}
	void g2p_tick(void* h, std::uint64_t ms)
	{
		auto* x = static_cast<Handle*>(h);
		x->clock.set(ms);
		x->client->tick();
	}
	int g2p_status(void* h) { return int(static_cast<Handle*>(h)->client->status()); }
	int g2p_synced(void* h) { return static_cast<Handle*>(h)->client->synced() ? 1 : 0; }
	int g2p_idle(void* h) { return static_cast<Handle*>(h)->client->idle() ? 1 : 0; }
	void g2p_status_line(void* h, char* buf, int len)
	{
		std::snprintf(buf, size_t(len), "%s", static_cast<Handle*>(h)->client->statusLine().c_str());
	}
	// 0 on success
	int g2p_send_patch(void* h, int slot, const char* path, const char* name)
	{
		std::ifstream f(path, std::ios::binary);
		if(!f) return 1;
		const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		try
		{
			const auto patch = Patch::fromFile(file::read(bytes));
			static_cast<Handle*>(h)->client->sendPatch(slot, patch, name);
		}
		catch(const std::exception& e)
		{
			std::fprintf(stderr, "proto: cannot load %s: %s\n", path, e.what());
			return 2;
		}
		return 0;
	}
}
