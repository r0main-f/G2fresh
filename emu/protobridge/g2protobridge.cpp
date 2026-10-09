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
//   g2p_send_kbd_performance  upload a performance with a .pch2 in slot A and the keyboard on slot A
//   g2p_send_module_patch  build and upload a patch with one module (+ an output)
//   g2p_set_param / g2p_set_mode / g2p_play_note   live edits (tools/firmware/g2catalog.py)
//   g2p_module_cost     the editor's resource estimate of a module type (core/patch_load)
#include "g2/edit.hpp"
#include "g2/file.hpp"
#include "g2/module_db.hpp"
#include "g2/patch.hpp"
#include "g2/patch_load.hpp"
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

	// A performance with the patch of `path` in slot A (default patches elsewhere), slot A
	// focused and keyboard-enabled, so that PlayNote (56) reaches slot A (the synth's default
	// performance has the keyboard off on every slot): for tools/firmware/g2blackbox.py.
	int g2p_send_kbd_performance(void* h, const char* path)
	{
		std::ifstream f(path, std::ios::binary);
		if(!f) return 1;
		const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		try
		{
			Performance perf;
			for(auto& p : perf.slots) p = Patch::makeDefault();
			perf.slots[0] = Patch::fromFile(file::read(bytes));
			perf.header.focusedSlot = 0;
			for(int i = 0; i < 4; ++i)
			{
				perf.header.slots[std::size_t(i)].patchName = i == 0 ? "Test" : "Init";
				perf.header.slots[std::size_t(i)].enabled = i == 0 ? 1 : 0;
				perf.header.slots[std::size_t(i)].keyboard = i == 0 ? 1 : 0;
			}
			static_cast<Handle*>(h)->client->sendPerformance(perf, "KbdTest");
		}
		catch(const std::exception& e)
		{
			std::fprintf(stderr, "proto: cannot build a performance from %s: %s\n", path, e.what());
			return 2;
		}
		return 0;
	}

	// Builds a patch with one module of `type` in area `loc` (0 FX, 1 VA) and, when
	// `withOutput` and the module has outputs, a 2-Out fed by its first output(s),
	// so that the OS keeps the module. type 0: only the 2-Out (the baseline).
	// Returns the module's index, or -1 when the patch cannot be built.
	int g2p_send_module_patch(void* h, int slot, int loc, int type, int withOutput)
	{
		try
		{
			Patch patch = Patch::makeDefault();
			const auto area = loc ? Location::Va : Location::Fx;
			const db::ModuleDef* out = nullptr;
			for(const auto& m : db::modules())
				if(std::string(m.shortName) == "2-Out") out = &m;
			int index = 0;
			if(type)
				index = edit::addModule(patch, area, std::uint8_t(type), 0, 0);
			const auto* def = type ? db::find(std::uint8_t(type)) : nullptr;
			if(out && withOutput && (!type || !def->outputs.empty()))
			{
				const auto o = edit::addModule(patch, area, out->typeId, 1, 0);
				if(type)
				{
					const auto n = def->outputs.size();
					edit::connect(patch, area, {std::uint8_t(index), 0, true}, {o, 0, false});
					edit::connect(patch, area, {std::uint8_t(index), std::uint8_t(n > 1 ? 1 : 0), true}, {o, 1, false});
				}
				if(!type) index = o;
			}
			static_cast<Handle*>(h)->client->sendPatch(slot, patch, type ? def->shortName : "Out");
			return index;
		}
		catch(const std::exception& e)
		{
			std::fprintf(stderr, "proto: cannot build a patch with type %d: %s\n", type, e.what());
			return -1;
		}
	}
	// A source module's first output feeding input `input` (-1: every input) of the module, the module's
	// output(s) feeding a 2-Out: for response measurements. Returns the module's index.
	int g2p_send_chain_patch(void* h, int slot, int loc, int srcType, int type, int input)
	{
		try
		{
			Patch patch = Patch::makeDefault();
			const auto area = loc ? Location::Va : Location::Fx;
			const db::ModuleDef* out = nullptr;
			for(const auto& m : db::modules())
				if(std::string(m.shortName) == "2-Out") out = &m;
			const auto src = edit::addModule(patch, area, std::uint8_t(srcType), 0, 0);
			const auto mod = edit::addModule(patch, area, std::uint8_t(type), 1, 0);
			const auto* def = db::find(std::uint8_t(type));
			if(input >= 0)
				edit::connect(patch, area, {src, 0, true}, {mod, std::uint8_t(input), false});
			else  // every input
				for(std::size_t i = 0; i < def->inputs.size(); ++i)
					edit::connect(patch, area, {src, 0, true}, {mod, std::uint8_t(i), false});
			if(out && !def->outputs.empty())
			{
				const auto o = edit::addModule(patch, area, out->typeId, 2, 0);
				const auto n = def->outputs.size();
				edit::connect(patch, area, {mod, 0, true}, {o, 0, false});
				edit::connect(patch, area, {mod, std::uint8_t(n > 1 ? 1 : 0), true}, {o, 1, false});
			}
			static_cast<Handle*>(h)->client->sendPatch(slot, patch, def->shortName);
			return mod;
		}
		catch(const std::exception& e)
		{
			std::fprintf(stderr, "proto: cannot build a chain patch for type %d: %s\n", type, e.what());
			return -1;
		}
	}
	void g2p_set_param(void* h, int slot, int loc, int module, int param, int value, int variation)
	{
		static_cast<Handle*>(h)->client->setParam(slot, loc ? Location::Va : Location::Fx, std::uint8_t(module),
		                                          std::uint8_t(param), std::uint8_t(value), std::uint8_t(variation));
	}
	void g2p_set_mode(void* h, int slot, int loc, int module, int mode, int value)
	{
		static_cast<Handle*>(h)->client->setMode(slot, loc ? Location::Va : Location::Fx, std::uint8_t(module),
		                                         std::uint8_t(mode), std::uint8_t(value));
	}
	void g2p_play_note(void* h, int note, int on) { static_cast<Handle*>(h)->client->playNote(std::uint8_t(note), on != 0); }
	// cyclesA, cyclesB, zp, xA, yA, pA, xB, yB, pB, dynRam, qMem, rMem; 0 if the type is unknown
	int g2p_module_cost(int type, int uprate, std::uint32_t* out)
	{
		if(!patchload::moduleSpec(std::uint8_t(type))) return 0;
		const auto r = patchload::moduleCost(std::uint8_t(type), uprate != 0);
		const std::uint32_t v[12] = {r.cyclesA, r.cyclesB, r.zpMem, r.xMemA, r.yMemA, r.pMemA, r.xMemB, r.yMemB, r.pMemB,
		                            r.dynRam, r.qMem, r.rMem};
		std::memcpy(out, v, sizeof v);
		return 1;
	}
}
