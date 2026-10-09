// Dumps small pieces of data needed for the USB protocol notes:
//  - "sym:NAME"     : address + first 16 bytes of every symbol called NAME
//  - "ptr:ADDR"     : dereference a pointer slot (GOT/non-lazy ptr) and dump
//                     the vtable found there (+8 skip of offset/typeinfo), 48 entries
//  - "vt:ADDR"      : dump 48 vtable entries starting at ADDR
//  - "glob:PATTERN" : list symbols whose name contains PATTERN with 8 bytes each
//
// Usage (headless): -postScript UsbDumpData.java <outfile> arg1 arg2 ...
//@category G2

import java.io.File;
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class UsbDumpData extends GhidraScript {

	private PrintWriter pw;

	private String hex(Address a, int n) {
		Memory m = currentProgram.getMemory();
		StringBuilder b = new StringBuilder();
		for (int i = 0; i < n; i++) {
			try {
				b.append(String.format("%02x ", m.getByte(a.add(i)) & 0xff));
			} catch (Exception e) {
				b.append("?? ");
			}
		}
		return b.toString();
	}

	private String nameAt(Address a) {
		Function f = getFunctionAt(a);
		if (f != null) return f.getName(true);
		Symbol s = getSymbolAt(a);
		return s != null ? s.getName(true) : "?";
	}

	private void dumpVtable(Address vt, int n) throws Exception {
		Memory m = currentProgram.getMemory();
		pw.println("  vtable @" + vt + " (" + nameAt(vt) + ")");
		for (int i = 0; i < n; i++) {
			Address slot = vt.add(i * 4L);
			int v = m.getInt(slot);
			Address t = toAddr(v & 0xffffffffL);
			pw.println(String.format("    +0x%02x -> %08x %s", i * 4, v, nameAt(t)));
		}
	}

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		pw = new PrintWriter(new File(args[0]), "UTF-8");
		Memory m = currentProgram.getMemory();
		for (int k = 1; k < args.length; k++) {
			String a = args[k];
			pw.println("== " + a);
			if (a.startsWith("sym:")) {
				SymbolIterator it = currentProgram.getSymbolTable().getSymbols(a.substring(4));
				while (it.hasNext()) {
					Symbol s = it.next();
					pw.println("  " + s.getName(true) + " @" + s.getAddress() + " : " + hex(s.getAddress(), 16));
				}
			} else if (a.startsWith("glob:")) {
				String pat = a.substring(5);
				SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
				while (it.hasNext()) {
					Symbol s = it.next();
					if (s.getName(true).contains(pat) && getFunctionAt(s.getAddress()) == null) {
						pw.println("  " + s.getName(true) + " @" + s.getAddress() + " : " + hex(s.getAddress(), 8));
					}
				}
			} else if (a.startsWith("ptr:")) {
				Address slot = toAddr(a.substring(4));
				int v = m.getInt(slot);
				Address t = toAddr(v & 0xffffffffL);
				pw.println("  slot " + slot + " -> " + t + " (" + nameAt(t) + ") bytes: " + hex(t, 8));
				try {
					dumpVtable(t.add(8), 48);
				} catch (Exception e) {
					pw.println("  (no vtable) " + e);
				}
			} else if (a.startsWith("vt:")) {
				dumpVtable(toAddr(a.substring(3)), 48);
			}
		}
		pw.close();
	}
}
