// Headless query helper for the SC6 Ghidra project.
// Usage (script args): <outFile> <mode> <items...>
//   strings <s1> <s2> ...  find each string as UTF-16LE and ASCII, list xrefs and their functions
//   decomp  <addr> ...     decompile the function that contains each address
//   xrefs   <addr> ...     list references to each address, with the containing function
//   func    <addr> ...     print the function that contains each address
//@category SC6
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.nio.charset.StandardCharsets;

public class Probe extends GhidraScript {
    private PrintWriter out;
    private DecompInterface decomp;

    @Override
    protected void run() throws Exception {
        String[] a = getScriptArgs();
        out = new PrintWriter(new FileWriter(a[0]));
        String mode = a[1];
        for (int i = 2; i < a.length; i++) {
            switch (mode) {
                case "strings": strings(a[i]); break;
                case "decomp": decompile(toAddr(a[i])); break;
                case "xrefs": xrefs(toAddr(a[i])); break;
                case "func": out.println(a[i] + "  " + fn(toAddr(a[i]))); break;
                default: throw new IllegalArgumentException(mode);
            }
        }
        out.close();
    }

    private String fn(Address at) {
        Function f = getFunctionContaining(at);
        return f == null ? "<no function>" : f.getName() + " @ " + f.getEntryPoint();
    }

    private void strings(String s) throws Exception {
        out.println("=== string \"" + s + "\"");
        for (String enc : new String[] {"UTF-16LE", "US-ASCII"}) {
            // Terminate the pattern so "FrameRate" does not match inside "FixedFrameRate" ends.
            byte[] body = s.getBytes(enc);
            byte[] pat = new byte[body.length + (enc.startsWith("UTF") ? 2 : 1)];
            System.arraycopy(body, 0, pat, 0, body.length);
            Address start = currentProgram.getMinAddress();
            Address hit;
            while ((hit = currentProgram.getMemory().findBytes(start, pat, null, true, monitor)) != null) {
                out.println("  " + enc + " at " + hit);
                for (Reference r : getReferencesTo(hit)) {
                    out.println("    ref from " + r.getFromAddress() + " in " + fn(r.getFromAddress()));
                }
                start = hit.add(1);
            }
        }
    }

    private void xrefs(Address at) {
        out.println("=== xrefs to " + at);
        for (Reference r : getReferencesTo(at)) {
            out.println("  " + r.getReferenceType() + " from " + r.getFromAddress() + " in " + fn(r.getFromAddress()));
        }
    }

    private void decompile(Address at) {
        if (decomp == null) {
            decomp = new DecompInterface();
            decomp.openProgram(currentProgram);
        }
        Function f = getFunctionContaining(at);
        out.println("=== decomp " + at + " -> " + fn(at));
        if (f == null) return;
        DecompileResults r = decomp.decompileFunction(f, 120, monitor);
        out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "  failed: " + r.getErrorMessage());
    }
}
