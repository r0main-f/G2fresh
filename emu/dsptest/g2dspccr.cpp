// Regression test for the condition-code fix that emu/CMakeLists.txt applies to the dsp56300
// library (re/notes/g2-hardware-and-emulation.md 3.8): an interrupt whose vector is a jsr, taken
// between a `cmp` and the `ble` that tests it, must not change where the branch goes. Without the
// fix the interpreter's lazily computed N bit was not on the stack, and the branch fell through.
// No firmware involved: a three-instruction loop written here.
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

#include <cstdio>

using namespace dsp56k;

int main()
{
	DefaultMemoryValidator validator;
	Memory mem{validator, 0x080000, 0x840000, 0x800000};
	Peripherals56367 periphY;
	Peripherals56362 periphX{&periphY};
	DSP dsp{mem, &periphX, &periphY};

	// $100: move #$43,r1   $101: clr b x:(r1),a   $102: cmp #3,a   $103: ble $100   $104: (fell through)
	const TWord loop[] = {0x314300, 0x56e11b, 0x014385, 0x05f7dd};  // ble: 9-bit displacement -3
	for(TWord i = 0; i < 4; ++i) dsp.memWriteP(0x100 + i, loop[i]);
	dsp.memWriteP(0x104, 0x000000);
	dsp.memWriteP(0x76, 0x0bf080);   // ESAI_1 receive-last-slot vector: jsr $300
	dsp.memWriteP(0x77, 0x000300);
	dsp.memWriteP(0x300, 0x000000);  // nop
	dsp.memWriteP(0x301, 0x000004);  // rti
	dsp.memWrite(MemArea_X, 0x43, 0);              // 0 <= 3: the branch is always taken
	dsp.memWrite(MemArea_X, 0xFFFFFF, 0xaa0000);   // IPRC, as the G2's stage 1 sets it
	dsp.memWrite(MemArea_X, 0xFFFFFE, 0x000431);   // IPRP
	auto sr = dsp.regs().sr;
	sr.var &= ~0x300u;                             // interrupt mask 0
	dsp.regs().sr = sr;
	dsp.setPC(0x100);

	int wrong = 0, tries = 0;
	for(int iter = 0; iter < 400; ++iter)
	{
		for(int k = 0; k < 8 && dsp.getPC().toWord() != 0x100; ++k) dsp.execInterpreter();
		for(int k = 0; k < iter % 4; ++k) dsp.execInterpreter();   // iter % 4 == 3: after the cmp
		dsp.injectInterrupt(0x76);
		for(int k = 0; k < 10; ++k)
		{
			dsp.execInterpreter();
			if(dsp.getPC().toWord() == 0x104) { ++wrong; dsp.setPC(0x100); break; }
		}
		++tries;
	}
	std::printf("g2dspccr: %d wrong branches in %d interrupted loops\n", wrong, tries);
	return wrong ? 1 : 0;
}
