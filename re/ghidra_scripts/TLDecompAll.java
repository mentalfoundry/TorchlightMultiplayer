// Decompiles every function into re/decomp_all/part_NNN.c (500 functions per file) so the
// whole program can be searched with grep.
//@category TLMP
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

import java.io.File;
import java.io.PrintWriter;

public class TLDecompAll extends GhidraScript {
	@Override
	public void run() throws Exception {
		File dir = new File(getScriptArgs()[0]);
		dir.mkdirs();
		DecompInterface ifc = new DecompInterface();
		ifc.openProgram(currentProgram);
		FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
		int n = 0, part = 0;
		PrintWriter w = null;
		while (it.hasNext() && !monitor.isCancelled()) {
			Function f = it.next();
			if (n % 500 == 0) {
				if (w != null) w.close();
				w = new PrintWriter(new File(dir, String.format("part_%03d.c", part++)), "UTF-8");
			}
			DecompileResults r = ifc.decompileFunction(f, 60, monitor);
			w.println("// ==== " + f.getName(true) + " @ " + f.getEntryPoint());
			w.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed: " + r.getErrorMessage());
			n++;
			if (n % 2000 == 0) println("decompiled " + n);
		}
		if (w != null) w.close();
		ifc.dispose();
		println("done: " + n + " functions");
	}
}
