// Prints the disassembly of several functions (call targets and referenced
// strings resolved), for the Patch Mutator / randomizer study
// (re/notes/randomize-mutate.md).
//
// Usage (headless): -postScript MutateDisasm.java <outfile> <hexaddr>...
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

public class MutateDisasm extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		try (PrintWriter pw = new PrintWriter(new File(args[0]), StandardCharsets.UTF_8)) {
			for (int i = 1; i < args.length; i++) {
				Address a = toAddr(Long.parseLong(args[i], 16));
				Function f = getFunctionContaining(a);
				if (f == null) { pw.println("# no function at " + args[i]); continue; }
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
						else if (d != null && d.getValue() != null) sb.append("  ; =").append(d.getValue());
					}
					pw.println(sb);
				}
				pw.println();
			}
		}
	}
}
