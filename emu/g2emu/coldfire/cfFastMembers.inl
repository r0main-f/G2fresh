// Members of coldfire::Cpu added by G2fresh (cfFast.h), declared inside the class. Defined in cfFast.inl.

		// step() with its body inlined into run()
		G2_CF_INLINE uint32_t stepInline();

		// m_table as plain function pointers for run()'s dispatch: a direct call into the handler, which the compiler
		// inlines into a function of its own per handler (m_table's member function pointers take 16 bytes and a test
		// for a virtual function on every call)
		using FastHandler = void (*)(Cpu&, uint16_t);
		template<Handler H> static void callHandler(Cpu& _cpu, const uint16_t _op) { (_cpu.*H)(_op); }
		std::array<FastHandler, 65536> m_fastTable{};
		void buildFastTable();

		// The most frequent instructions, one handler per operand size and effective address mode (with the register
		// of an absolute, PC-relative or immediate source), so that the address decoding and the timing fold into
		// constants. Each is the general handler's code with those fields as template arguments.
		template<uint32_t Line, uint32_t SrcMode, uint32_t SrcReg, uint32_t DstMode> void opMoveT(uint16_t _op);
		template<uint32_t Line, uint32_t SrcMode, uint32_t SrcReg> void opMoveaT(uint16_t _op);
		template<uint32_t Sz, uint32_t Mode, uint32_t Reg> void opClrT(uint16_t _op);
		template<uint32_t Sz, uint32_t Mode, uint32_t Reg> void opTstT(uint16_t _op);
		template<uint32_t Mode, uint32_t Reg> void opCmpT(uint16_t _op);
		template<uint32_t Mode, uint32_t Reg> void opLeaT(uint16_t _op);
		template<uint32_t Mode, uint32_t Reg> void opJsrT(uint16_t _op);
		template<uint32_t Mode, uint32_t Reg> void opPeaT(uint16_t _op);
		template<uint32_t Mode, uint32_t Reg> void opAddqSubqT(uint16_t _op);
		template<uint32_t Cc, uint32_t Disp> void opBccT(uint16_t _op);
