// armb02: names the Black Ops II functions we have identified (from names.csv next to this
// script: address,name per line, # comments) and exports every function as
// address,name,size,callers to the CSV given as the first script argument. An optional second
// argument is a file of function start addresses (hex, one per line; for example XenonRecomp's
// function table) to create first, so Ghidra's boundaries match the recompiler's.
// GPL-3.0-or-later (see COPYING). No game data: run it on your own analysed executable.
//
//   analyzeHeadless private/ghidra bo2 -process default_patched.xex -noanalysis \
//       -scriptPath tools/ghidra -postScript BO2Names.java private/ghidra/functions.csv [starts.txt]
//@category armb02

import java.io.File;
import java.io.PrintWriter;
import java.nio.file.Files;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.SourceType;

public class BO2Names extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length > 1) {
            int made = 0;
            for (String line : Files.readAllLines(new File(args[1]).toPath())) {
                line = line.strip();
                if (line.isEmpty() || line.startsWith("#"))
                    continue;
                Address address = toAddr(Long.parseLong(line, 16));
                if (getFunctionAt(address) != null)
                    continue;
                disassemble(address);
                if (createFunction(address, null) != null)
                    made++;
            }
            println("created " + made + " functions from " + args[1]);
        }

        File names = new File(getSourceFile().getParentFile().getFile(false), "names.csv");
        int named = 0, created = 0;
        for (String line : Files.readAllLines(names.toPath())) {
            line = line.strip();
            if (line.isEmpty() || line.startsWith("#"))
                continue;
            String[] parts = line.split(",", 2);
            Address address = toAddr(Long.parseLong(parts[0].strip(), 16));
            Function function = getFunctionAt(address);
            if (function == null) {
                disassemble(address);
                function = createFunction(address, null);
                if (function == null) {
                    printerr("no function at " + address);
                    continue;
                }
                created++;
            }
            function.setName(parts[1].strip(), SourceType.USER_DEFINED);
            named++;
        }
        println("named " + named + " functions (" + created + " created)");

        if (args.length == 0)
            return;
        int count = 0;
        try (PrintWriter out = new PrintWriter(args[0])) {
            out.println("address,name,size,callers");
            FunctionIterator functions = currentProgram.getFunctionManager().getFunctions(true);
            while (functions.hasNext() && !monitor.isCancelled()) {
                Function f = functions.next();
                out.printf("%s,%s,%d,%d%n", f.getEntryPoint(), f.getName(), f.getBody().getNumAddresses(),
                    f.getCallingFunctions(monitor).size());
                count++;
            }
        }
        println("exported " + count + " functions to " + args[0]);
    }
}
