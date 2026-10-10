// G2fresh's speed-ups to Gearmulator's ColdFire core: definitions (see cfFast.h), included at the end of cfCpu.cpp.
//
// The specialised handlers below are copies of the general ones in cfCpu.cpp with the opcode's size and effective
// address fields as template arguments; everything else (the order of the fetches and bus accesses, the flags, the
// cycle counts, the exceptions) is the same code. buildFastTable() installs one only where buildTable() put the
// general handler of the same instruction.

#include <array>
#include <cstddef>
#include <stdexcept>
#include <utility>

namespace coldfire
{
	namespace
	{
		// an effective address as an index 0-11: modes 0-6 (any register), then mode 7 with register 0-4 (absolute
		// word, absolute long, PC + d16, PC + index, immediate)
		constexpr uint32_t g_eaIndices = 12;
		constexpr uint32_t eaModeOf(const uint32_t _i) { return _i < 7 ? _i : 7; }
		constexpr uint32_t eaRegOf(const uint32_t _i) { return _i < 7 ? 0 : _i - 7; }
		uint32_t eaIndex(const uint32_t _op)
		{
			const uint32_t mode = (_op >> 3) & 7;
			return mode < 7 ? mode : 7 + (_op & 7);  // >= g_eaIndices: none
		}

		template<std::size_t N, typename F> void staticFor(F&& _f)
		{
			[&]<std::size_t... I>(std::index_sequence<I...>) { (_f(std::integral_constant<std::size_t, I>{}), ...); }(std::make_index_sequence<N>{});
		}
	}

	// ---- specialised handlers (as the general ones of the same name without the T) ----

	template<uint32_t Line, uint32_t SrcMode, uint32_t SrcReg, uint32_t DstMode>
	void Cpu::opMoveT(const uint16_t _op)
	{
		constexpr Size size = Line == 1 ? Size::Byte : (Line == 3 ? Size::Word : Size::Long);

		const uint32_t srcReg = SrcMode == 7 ? SrcReg : (_op & 7u);
		const uint32_t dstReg = (_op >> 9) & 7;

		const Ea src = decodeEa(SrcMode, srcReg, size);
		const uint32_t v = readEa(src, size);
		const Ea dst = decodeEa(DstMode, dstReg, size);
		writeEa(dst, v, size);
		setLogicFlags(v, size);

		const uint32_t srcClass = eaClass(SrcMode, srcReg);
		const uint32_t dstClass = eaClass(DstMode, dstReg);
		constexpr bool srcImm = SrcMode == 7 && SrcReg == 4;

		uint32_t c;
		if(srcImm)
			c = dstClass == 0 ? 1 : (size == Size::Long ? 2 : 3);
		else if(srcClass == 0)
			c = dstClass == 2 ? 2 : 1;
		else
			c = (size == Size::Long ? 2 : 3) + (srcClass == 2 || dstClass == 2 ? 1 : 0);
		m_cycles += c;
	}

	template<uint32_t Line, uint32_t SrcMode, uint32_t SrcReg>
	void Cpu::opMoveaT(const uint16_t _op)
	{
		constexpr bool word = Line == 3;
		const uint32_t srcReg = SrcMode == 7 ? SrcReg : (_op & 7u);

		const Ea src = decodeEa(SrcMode, srcReg, word ? Size::Word : Size::Long);
		uint32_t v = readEa(src, word ? Size::Word : Size::Long);
		if(word)
			v = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v)));
		m_a[(_op >> 9) & 7] = v;

		const uint32_t srcClass = eaClass(SrcMode, srcReg);
		m_cycles += srcClass == 0 ? 1 : (word ? 3 : 2) + (srcClass == 2 ? 1 : 0);
	}

	template<uint32_t Sz, uint32_t Mode, uint32_t Reg>
	void Cpu::opClrT(const uint16_t _op)
	{
		constexpr Size size = Sz == 0 ? Size::Byte : (Sz == 1 ? Size::Word : Size::Long);
		const uint32_t reg = Mode == 7 ? Reg : (_op & 7u);

		const Ea ea = decodeEa(Mode, reg, size);
		writeEa(ea, 0, size);
		m_sr = static_cast<uint16_t>((m_sr & ~(SrN | SrV | SrC)) | SrZ);
		m_cycles += eaClass(Mode, reg) == 2 ? 2 : 1;
	}

	template<uint32_t Sz, uint32_t Mode, uint32_t Reg>
	void Cpu::opTstT(const uint16_t _op)
	{
		constexpr Size size = Sz == 0 ? Size::Byte : (Sz == 1 ? Size::Word : Size::Long);
		const uint32_t reg = Mode == 7 ? Reg : (_op & 7u);

		const Ea ea = decodeEa(Mode, reg, size);
		setLogicFlags(readEa(ea, size), size);

		const uint32_t cls = eaClass(Mode, reg);
		m_cycles += cls == 0 ? 1 : (size == Size::Long ? 2 : 3) + (cls == 2 ? 1 : 0);
	}

	template<uint32_t Mode, uint32_t Reg>
	void Cpu::opCmpT(const uint16_t _op)
	{
		const uint32_t reg = Mode == 7 ? Reg : (_op & 7u);
		const Ea ea = decodeEa(Mode, reg, Size::Long);
		cmp(readEa(ea, Size::Long), m_d[(_op >> 9) & 7]);
		m_cycles += aluCycles(eaClass(Mode, reg), false);
	}

	template<uint32_t Mode, uint32_t Reg>
	void Cpu::opLeaT(const uint16_t _op)
	{
		const uint32_t reg = Mode == 7 ? Reg : (_op & 7u);
		m_a[(_op >> 9) & 7] = controlAddress(Mode, reg);
		m_cycles += eaClass(Mode, reg) == 2 ? 2 : 1;
	}

	template<uint32_t Mode, uint32_t Reg>
	void Cpu::opJsrT(const uint16_t _op)
	{
		const uint32_t reg = Mode == 7 ? Reg : (_op & 7u);
		const uint32_t target = controlAddress(Mode, reg);
		push32(m_pc);
		m_cycles += eaClass(Mode, reg) == 2 ? 4 : 3;
		jump(target);
	}

	template<uint32_t Mode, uint32_t Reg>
	void Cpu::opPeaT(const uint16_t _op)
	{
		const uint32_t reg = Mode == 7 ? Reg : (_op & 7u);
		const uint32_t addr = controlAddress(Mode, reg);
		push32(addr);
		m_cycles += eaClass(Mode, reg) == 2 ? 3 : 2;
	}

	template<uint32_t Mode, uint32_t Reg>
	void Cpu::opAddqSubqT(const uint16_t _op)
	{
		uint32_t q = (_op >> 9) & 7;
		if(!q)
			q = 8;
		const bool isSub = (_op & 0x100) != 0;
		const uint32_t reg = Mode == 7 ? Reg : (_op & 7u);

		if constexpr(Mode == 1)
		{
			// address register destination: no condition codes
			m_a[reg] = isSub ? m_a[reg] - q : m_a[reg] + q;
			m_cycles += 1;
			return;
		}
		else
		{
			const Ea ea = decodeEa(Mode, reg, Size::Long);
			const uint32_t v = readEa(ea, Size::Long);
			writeEa(ea, isSub ? sub(q, v, false) : add(q, v, false), Size::Long);

			const uint32_t cls = eaClass(Mode, reg);
			m_cycles += cls == 0 ? 1 : (cls == 2 ? 4 : 3);
		}
	}

	// Disp: 0 an 8-bit displacement in the opcode, 1 a 16-bit one (opcode byte $00), 2 a 32-bit one (opcode byte $FF)
	template<uint32_t Cc, uint32_t Disp>
	void Cpu::opBccT(const uint16_t _op)
	{
		const uint32_t base = m_pc;

		int32_t disp;
		if constexpr(Disp == 0)
			disp = static_cast<int8_t>(_op & 0xff);
		else if constexpr(Disp == 1)
			disp = static_cast<int16_t>(fetch16());
		else
			disp = static_cast<int32_t>(fetch32());

		const uint32_t target = base + static_cast<uint32_t>(disp);

		if constexpr(Cc == 1)
		{
			// BSR
			push32(m_pc);
			m_cycles += 3;
			jump(target);
			return;
		}
		else
		{
			// UM table 3-11: backward branches are predicted taken, forward ones not taken
			const bool forward = disp >= 0;

			if constexpr(Cc == 0)
			{
				m_cycles += 2;
				jump(target);
				return;
			}
			else
			{
				if(testCondition(Cc))
				{
					m_cycles += forward ? 3 : 2;
					jump(target);
				}
				else
				{
					m_cycles += forward ? 1 : 3;
				}
			}
		}
	}

	// ---- the dispatch table ----

	void Cpu::buildFastTable()
	{
		static const std::pair<Handler, FastHandler> handlers[] =
		{
#define G2_CF_HANDLER(_name) {&Cpu::_name, &Cpu::callHandler<&Cpu::_name>},
			G2_CF_HANDLER(opIllegal) G2_CF_HANDLER(opLineA) G2_CF_HANDLER(opLineF) G2_CF_HANDLER(opOriL)
			G2_CF_HANDLER(opAndiL) G2_CF_HANDLER(opSubiL) G2_CF_HANDLER(opAddiL) G2_CF_HANDLER(opEoriL)
			G2_CF_HANDLER(opCmpiL) G2_CF_HANDLER(opBitImm) G2_CF_HANDLER(opBitReg) G2_CF_HANDLER(opMove)
			G2_CF_HANDLER(opMovea) G2_CF_HANDLER(opNegxL) G2_CF_HANDLER(opMoveFromSr) G2_CF_HANDLER(opLea)
			G2_CF_HANDLER(opClr) G2_CF_HANDLER(opMoveFromCcr) G2_CF_HANDLER(opNegL) G2_CF_HANDLER(opMoveToCcr)
			G2_CF_HANDLER(opNotL) G2_CF_HANDLER(opMoveToSr) G2_CF_HANDLER(opSwap) G2_CF_HANDLER(opPea)
			G2_CF_HANDLER(opExt) G2_CF_HANDLER(opMovemToMem) G2_CF_HANDLER(opMovemFromMem) G2_CF_HANDLER(opTst)
			G2_CF_HANDLER(opHalt) G2_CF_HANDLER(opPulse) G2_CF_HANDLER(opMulL) G2_CF_HANDLER(opDivL)
			G2_CF_HANDLER(opTrap) G2_CF_HANDLER(opLink) G2_CF_HANDLER(opUnlk) G2_CF_HANDLER(opNop)
			G2_CF_HANDLER(opStop) G2_CF_HANDLER(opRte) G2_CF_HANDLER(opRts) G2_CF_HANDLER(opMovec)
			G2_CF_HANDLER(opJsr) G2_CF_HANDLER(opJmp) G2_CF_HANDLER(opAddqSubq) G2_CF_HANDLER(opScc)
			G2_CF_HANDLER(opTrapf) G2_CF_HANDLER(opBcc) G2_CF_HANDLER(opMoveq) G2_CF_HANDLER(opOr)
			G2_CF_HANDLER(opDivW) G2_CF_HANDLER(opSub) G2_CF_HANDLER(opSuba) G2_CF_HANDLER(opSubx)
			G2_CF_HANDLER(opCmp) G2_CF_HANDLER(opCmpa) G2_CF_HANDLER(opEor) G2_CF_HANDLER(opAnd)
			G2_CF_HANDLER(opMulW) G2_CF_HANDLER(opAdd) G2_CF_HANDLER(opAdda) G2_CF_HANDLER(opAddx)
			G2_CF_HANDLER(opShift) G2_CF_HANDLER(opCpushl) G2_CF_HANDLER(opWddata) G2_CF_HANDLER(opWdebug)
#undef G2_CF_HANDLER
		};

		for(uint32_t op = 0; op < 0x10000; ++op)
		{
			m_fastTable[op] = nullptr;
			for(const auto& [h, f] : handlers)
			{
				if(h == m_table[op])
				{
					m_fastTable[op] = f;
					break;
				}
			}
			if(!m_fastTable[op])
				throw std::logic_error("coldfire: a handler of m_table is missing from buildFastTable's list");
		}

		// the specialised handlers, by size (or line, or condition) and effective address index
		using ByEa = std::array<FastHandler, g_eaIndices>;
		static const auto moves = []
		{
			std::array<std::array<std::array<FastHandler, 8>, g_eaIndices>, 4> t{};  // [line][source][destination mode]
			staticFor<3>([&](auto l)
			{
				constexpr uint32_t line = static_cast<uint32_t>(decltype(l)::value) + 1;
				staticFor<g_eaIndices>([&](auto s)
				{
					constexpr uint32_t src = static_cast<uint32_t>(decltype(s)::value);
					staticFor<8>([&](auto d)
					{
						constexpr uint32_t dst = static_cast<uint32_t>(decltype(d)::value);
						t[line][src][dst] = &Cpu::callHandler<&Cpu::opMoveT<line, eaModeOf(src), eaRegOf(src), dst>>;
					});
				});
			});
			return t;
		}();
		static const auto moveas = []
		{
			std::array<ByEa, 4> t{};  // [line]
			staticFor<g_eaIndices>([&](auto s)
			{
				constexpr uint32_t src = static_cast<uint32_t>(decltype(s)::value);
				t[2][src] = &Cpu::callHandler<&Cpu::opMoveaT<2, eaModeOf(src), eaRegOf(src)>>;
				t[3][src] = &Cpu::callHandler<&Cpu::opMoveaT<3, eaModeOf(src), eaRegOf(src)>>;
			});
			return t;
		}();
		static const auto sized = []
		{
			std::array<std::array<ByEa, 3>, 2> t{};  // [clr, tst][size]
			staticFor<3>([&](auto z)
			{
				constexpr uint32_t sz = static_cast<uint32_t>(decltype(z)::value);
				staticFor<g_eaIndices>([&](auto s)
				{
					constexpr uint32_t ea = static_cast<uint32_t>(decltype(s)::value);
					t[0][sz][ea] = &Cpu::callHandler<&Cpu::opClrT<sz, eaModeOf(ea), eaRegOf(ea)>>;
					t[1][sz][ea] = &Cpu::callHandler<&Cpu::opTstT<sz, eaModeOf(ea), eaRegOf(ea)>>;
				});
			});
			return t;
		}();
		static const auto unsized = []
		{
			std::array<ByEa, 5> t{};  // cmp, lea, jsr, pea, addq/subq
			staticFor<g_eaIndices>([&](auto s)
			{
				constexpr uint32_t ea = static_cast<uint32_t>(decltype(s)::value);
				t[0][ea] = &Cpu::callHandler<&Cpu::opCmpT<eaModeOf(ea), eaRegOf(ea)>>;
				t[1][ea] = &Cpu::callHandler<&Cpu::opLeaT<eaModeOf(ea), eaRegOf(ea)>>;
				t[2][ea] = &Cpu::callHandler<&Cpu::opJsrT<eaModeOf(ea), eaRegOf(ea)>>;
				t[3][ea] = &Cpu::callHandler<&Cpu::opPeaT<eaModeOf(ea), eaRegOf(ea)>>;
				t[4][ea] = &Cpu::callHandler<&Cpu::opAddqSubqT<eaModeOf(ea), eaRegOf(ea)>>;
			});
			return t;
		}();
		static const auto branches = []
		{
			std::array<std::array<FastHandler, 3>, 16> t{};  // [condition][displacement size]
			staticFor<16>([&](auto c)
			{
				constexpr uint32_t cc = static_cast<uint32_t>(decltype(c)::value);
				t[cc][0] = &Cpu::callHandler<&Cpu::opBccT<cc, 0>>;
				t[cc][1] = &Cpu::callHandler<&Cpu::opBccT<cc, 1>>;
				t[cc][2] = &Cpu::callHandler<&Cpu::opBccT<cc, 2>>;
			});
			return t;
		}();

		for(uint32_t op = 0; op < 0x10000; ++op)
		{
			const Handler h = m_table[op];
			if(h == &Cpu::opBcc)
			{
				const uint32_t low = op & 0xff;
				m_fastTable[op] = branches[(op >> 8) & 0xf][low == 0 ? 1 : (low == 0xff ? 2 : 0)];
				continue;
			}
			const uint32_t ea = eaIndex(op);
			if(ea >= g_eaIndices)
				continue;
			const uint32_t sz = (op >> 6) & 3;
			if(h == &Cpu::opMove)
				m_fastTable[op] = moves[op >> 12][ea][(op >> 6) & 7];
			else if(h == &Cpu::opMovea)
				m_fastTable[op] = moveas[op >> 12][ea];
			else if(h == &Cpu::opClr && sz < 3)
				m_fastTable[op] = sized[0][sz][ea];
			else if(h == &Cpu::opTst && sz < 3)
				m_fastTable[op] = sized[1][sz][ea];
			else if(h == &Cpu::opCmp)
				m_fastTable[op] = unsized[0][ea];
			else if(h == &Cpu::opLea)
				m_fastTable[op] = unsized[1][ea];
			else if(h == &Cpu::opJsr)
				m_fastTable[op] = unsized[2][ea];
			else if(h == &Cpu::opPea)
				m_fastTable[op] = unsized[3][ea];
			else if(h == &Cpu::opAddqSubq)
				m_fastTable[op] = unsized[4][ea];
		}
	}

	// ---- running ----

	// As `while(m_totalCycles < _cycleLimit && !_stop && !m_stopped && !m_halted) step();`, with step()'s body
	// inlined, the dispatch through m_fastTable, and what cannot change while it runs (the instruction hook, the
	// fast memory window) read once.
	void Cpu::run(const uint64_t _cycleLimit, const bool& _stop)
	{
		if(m_instructionHook)
		{
			while(m_totalCycles < _cycleLimit && !_stop && !m_stopped && !m_halted)
				stepInline();
			return;
		}

		const uint8_t* const fastMem = m_fastMem;
		const uint32_t fastBase = m_fastBase;
		const uint32_t fastEnd2 = m_fastEnd2;

		while(m_totalCycles < _cycleLimit && !_stop && !m_stopped && !m_halted)
		{
			// step(), not halted, not stopped, no instruction hook
			m_cycles = 0;

			if(!m_inhibitInterrupts)
			{
				const uint32_t mask = (m_sr & SrIpl) >> 8;
				const bool take = m_interruptLevel == 7 ? m_nmiEdge : m_interruptLevel > mask;

				if(take)
				{
					if(m_interruptLevel == 7)
						m_nmiEdge = false;
					takeInterrupt(m_interruptLevel);
					m_cycles += m_g2WaitCycles; m_g2WaitCycles = 0;
					m_totalCycles += m_cycles;
					continue;
				}
			}

			m_inhibitInterrupts = false;
			m_instructionPc = m_pc;
			const bool trace = (m_sr & SrT) != 0;

			try
			{
				// fetch16()
				const uint32_t pc = m_pc;
				const uint32_t fo = pc - fastBase;
				const uint16_t op = fo < fastEnd2 ? static_cast<uint16_t>(fastMem[fo] << 8 | fastMem[fo + 1]) : m_bus.fetch16(pc);
				m_pc = pc + 2;

				m_fastTable[op](*this, op);
				++m_instructionCount;

				if(trace && !m_inhibitInterrupts)
					exception(VecTrace, m_pc);
			}
			catch(const Fault& _fault)
			{
				exception(_fault.vector, _fault.faultPc ? m_instructionPc : m_pc);
			}

			m_cycles += m_g2WaitCycles; m_g2WaitCycles = 0;
			m_totalCycles += m_cycles;
		}
	}
}
