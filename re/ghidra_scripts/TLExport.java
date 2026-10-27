// Headless helper for the Torchlight RE workflow.
//   TLExport.java symbols <outFile>              dump every function: addr, size, name, callers
//   TLExport.java decomp <outDir> <addr> [...]   decompile functions containing each address
//@category TLMP
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

import java.io.File;
import java.io.PrintWriter;

public class TLExport extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 2) {
			printerr("usage: symbols <out> | decomp <outDir> <addr>...");
			return;
		}
		if (args[0].equals("symbols")) {
			dumpSymbols(new File(args[1]));
		}
		else if (args[0].equals("decomp")) {
			File dir = new File(args[1]);
			dir.mkdirs();
			DecompInterface ifc = new DecompInterface();
			ifc.openProgram(currentProgram);
			for (int i = 2; i < args.length; i++) {
				decompile(ifc, dir, args[i]);
			}
			ifc.dispose();
		}
	}

	private void dumpSymbols(File out) throws Exception {
		try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
			FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
			while (it.hasNext() && !monitor.isCancelled()) {
				Function f = it.next();
				w.printf("%s %d %s callers=%d%n", f.getEntryPoint(), f.getBody().getNumAddresses(),
					f.getName(true), f.getCallingFunctions(monitor).size());
			}
		}
		println("wrote " + out);
	}

	private void decompile(DecompInterface ifc, File dir, String addrText) throws Exception {
		Address a = toAddr(addrText);
		Function f = getFunctionContaining(a);
		if (f == null) {
			println("no function at " + addrText);
			return;
		}
		DecompileResults r = ifc.decompileFunction(f, 120, monitor);
		String body = r.decompileCompleted() ? r.getDecompiledFunction().getC() : ("// failed: " + r.getErrorMessage());
		File out = new File(dir, f.getEntryPoint().toString() + ".c");
		try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
			w.println("// " + f.getName(true) + " @ " + f.getEntryPoint());
			w.print(body);
		}
		println("decompiled " + f.getName(true) + " -> " + out.getName());
	}
}
