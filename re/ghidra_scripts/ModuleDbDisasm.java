// Prints the disassembly of one function (with call targets and referenced
// strings resolved) for functions the decompiler cannot handle, e.g.
// CReplaceDataBase::AddData.
//
// Usage (headless): -postScript ModuleDbDisasm.java <hexaddr> <outfile>
//@category G2

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;

public class ModuleDbDisasm extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		Address a = toAddr(Long.parseLong(args[0], 16));
		Function f = getFunctionContaining(a);
		try (PrintWriter pw = new PrintWriter(new File(args[1]), StandardCharsets.UTF_8)) {
			pw.println("# " + f.getName(true) + " " + f.getBody());
			InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
			while (it.hasNext()) {
				Instruction ins = it.next();
				StringBuilder sb = new StringBuilder();
				sb.append(ins.getAddress()).append("  ").append(ins.toString());
				for (Reference r : ins.getReferencesFrom()) {
					Address to = r.getToAddress();
					Symbol s = getSymbolAt(to);
					if (s != null) sb.append("  ; ").append(s.getName(true));
					Data d = getDataAt(to);
					if (d != null && d.hasStringValue()) sb.append("  ; \"").append(d.getValue()).append('"');
				}
				pw.println(sb);
			}
		}
	}
}
