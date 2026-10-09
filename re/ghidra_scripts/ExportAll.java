// Exports the analyzed program for offline study: decompiled C grouped by
// namespace, a symbol table, a call graph and the defined strings with the
// functions that reference them.
//
// Usage (headless): -postScript ExportAll.java <outdir>
//@category G2

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.Reference;

public class ExportAll extends GhidraScript {

	private static String json(String s) {
		StringBuilder b = new StringBuilder("\"");
		for (char c : s.toCharArray()) {
			switch (c) {
				case '"': b.append("\\\""); break;
				case '\\': b.append("\\\\"); break;
				case '\n': b.append("\\n"); break;
				case '\r': b.append("\\r"); break;
				case '\t': b.append("\\t"); break;
				default:
					if (c < 0x20 || c > 0x7e) b.append(String.format("\\u%04x", (int) c));
					else b.append(c);
			}
		}
		return b.append('"').toString();
	}

	private static String fileSafe(String s) {
		String r = s.replaceAll("[^A-Za-z0-9_.-]", "_");
		return r.length() > 120 ? r.substring(0, 120) : r;
	}

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File out = new File(args.length > 0 ? args[0] : "re/out");
		File decompDir = new File(out, "decomp");
		decompDir.mkdirs();

		DecompInterface ifc = new DecompInterface();
		ifc.setOptions(new DecompileOptions());
		ifc.openProgram(currentProgram);

		// namespace -> decompiled functions, written one file per namespace
		Map<String, StringBuilder> byNs = new TreeMap<>();
		List<String> symbols = new ArrayList<>();
		List<String> calls = new ArrayList<>();

		FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
		int n = 0, total = currentProgram.getFunctionManager().getFunctionCount();
		while (it.hasNext() && !monitor.isCancelled()) {
			Function f = it.next();
			n++;
			if (n % 500 == 0) println("decompiled " + n + "/" + total);
			String ns = f.getParentNamespace().getName(true);
			String full = f.getName(true);
			String sig = f.getPrototypeString(false, false);
			symbols.add(String.format("{\"addr\":\"%s\",\"name\":%s,\"ns\":%s,\"size\":%d,\"sig\":%s,\"thunk\":%b}",
				f.getEntryPoint(), json(full), json(ns), f.getBody().getNumAddresses(), json(sig), f.isThunk()));

			StringBuilder callees = new StringBuilder();
			for (Function c : f.getCalledFunctions(monitor)) {
				if (callees.length() > 0) callees.append(',');
				callees.append(json(c.getName(true)));
			}
			calls.add(String.format("{\"addr\":\"%s\",\"name\":%s,\"calls\":[%s]}", f.getEntryPoint(), json(full), callees));

			if (f.isThunk() || f.isExternal()) continue;
			DecompileResults r = ifc.decompileFunction(f, 60, monitor);
			String c = (r != null && r.decompileCompleted()) ? r.getDecompiledFunction().getC()
				: "/* decompilation failed: " + (r == null ? "null" : r.getErrorMessage()) + " */\n";
			byNs.computeIfAbsent(ns, k -> new StringBuilder())
				.append("// ").append(f.getEntryPoint()).append("  ").append(full).append('\n')
				.append(c).append('\n');
		}
		ifc.dispose();

		for (Map.Entry<String, StringBuilder> e : byNs.entrySet()) {
			try (PrintWriter w = new PrintWriter(new File(decompDir, fileSafe(e.getKey()) + ".c"), StandardCharsets.UTF_8)) {
				w.print(e.getValue());
			}
		}
		writeJsonArray(new File(out, "symbols.json"), symbols);
		writeJsonArray(new File(out, "calls.json"), calls);

		// defined strings and the functions referencing them
		List<String> strings = new ArrayList<>();
		DataIterator di = currentProgram.getListing().getDefinedData(true);
		while (di.hasNext() && !monitor.isCancelled()) {
			Data d = di.next();
			if (!d.hasStringValue()) continue;
			StringBuilder refs = new StringBuilder();
			for (Reference ref : getReferencesTo(d.getAddress())) {
				Function rf = getFunctionContaining(ref.getFromAddress());
				if (rf == null) continue;
				if (refs.length() > 0) refs.append(',');
				refs.append(json(rf.getName(true)));
			}
			strings.add(String.format("{\"addr\":\"%s\",\"value\":%s,\"refs\":[%s]}",
				d.getAddress(), json(String.valueOf(d.getValue())), refs));
		}
		writeJsonArray(new File(out, "strings.json"), strings);
		println("ExportAll: " + n + " functions, " + byNs.size() + " namespaces, " + strings.size() + " strings -> " + out);
	}

	private static void writeJsonArray(File f, List<String> rows) throws Exception {
		try (PrintWriter w = new PrintWriter(f, StandardCharsets.UTF_8)) {
			w.println("[");
			for (int i = 0; i < rows.size(); i++) {
				w.print(rows.get(i));
				w.println(i + 1 < rows.size() ? "," : "");
			}
			w.println("]");
		}
	}
}
