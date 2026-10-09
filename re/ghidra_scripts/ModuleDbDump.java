// Dumps raw memory blocks and all non-function symbols so that the module
// tables (SModuleInfo, param specs, ...) can be decoded offline in Python.
//
// Usage (headless): -postScript ModuleDbDump.java <outdir>
// Writes <outdir>/blocks.json, <outdir>/block_<name>.bin, <outdir>/datasyms.tsv
//@category G2

import java.io.File;
import java.io.FileOutputStream;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.symbol.SymbolType;

public class ModuleDbDump extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File out = new File(args.length > 0 ? args[0] : "moduledb_dump");
		out.mkdirs();
		try (PrintWriter pw = new PrintWriter(new File(out, "blocks.json"), StandardCharsets.UTF_8)) {
			pw.println("[");
			boolean first = true;
			int i = 0;
			for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
				if (!b.isInitialized()) continue;
				String fn = "block_" + (i++) + ".bin";
				byte[] data = new byte[(int) b.getSize()];
				b.getBytes(b.getStart(), data);
				try (FileOutputStream fo = new FileOutputStream(new File(out, fn))) { fo.write(data); }
				pw.print((first ? "" : ",\n") + "{\"name\":\"" + b.getName().replace("\"", "") + "\",\"start\":\""
					+ b.getStart().toString() + "\",\"size\":" + b.getSize() + ",\"file\":\"" + fn + "\"}");
				first = false;
			}
			pw.println("\n]");
		}
		try (PrintWriter pw = new PrintWriter(new File(out, "datasyms.tsv"), StandardCharsets.UTF_8)) {
			SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
			while (it.hasNext()) {
				Symbol s = it.next();
				SymbolType t = s.getSymbolType();
				if (t == SymbolType.FUNCTION) continue;
				if (!s.getAddress().isMemoryAddress()) continue;
				pw.println(s.getAddress().toString() + "\t" + t + "\t" + s.getName(true));
			}
		}
	}
}
