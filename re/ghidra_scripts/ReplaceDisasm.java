// Prints the disassembly (call targets and referenced strings resolved) of the
// functions behind the module Replace feature, one file per function.
// CReplaceDataBase::AddData (0010ae74) cannot be decompiled (timeout) and is the
// source of tools/moduledb/replace_facts.json.
//
// Usage (headless): -postScript ReplaceDisasm.java <outdir> <hexaddr>...
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

public class ReplaceDisasm extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File dir = new File(args[0]);
		dir.mkdirs();
		for (int i = 1; i < args.length; i++) {
			Address a = toAddr(Long.parseLong(args[i], 16));
			Function f = getFunctionContaining(a);
			File out = new File(dir, args[i] + ".asm");
			try (PrintWriter pw = new PrintWriter(out, StandardCharsets.UTF_8)) {
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
}
