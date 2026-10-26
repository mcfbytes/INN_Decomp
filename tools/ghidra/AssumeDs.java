// Pre-analysis script: assume DS holds one segment everywhere (EXPORT_DS in export_module.sh).
// A large-model program keeps DS = DGROUP, so the analyzers then resolve every DS-relative operand
// and the decompiler names globals and strings DAT_<dgroup>_xxxx instead of absolute addresses.
// An instruction that loads DS itself still overrides the value from there on.
// Argument: the segment as image paragraphs, hex (Ghidra's MZ load base is segment 0x1000).
import java.math.BigInteger;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.SegmentedAddress;
import ghidra.program.model.lang.Register;
import ghidra.program.model.mem.MemoryBlock;

public class AssumeDs extends GhidraScript {

    private static final int IMAGE_BASE = 0x1000;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 1) {
            println("usage: AssumeDs.java <image segment, hex>");
            return;
        }
        int seg = (Integer.parseInt(args[0].trim(), 16) + IMAGE_BASE) & 0xffff;
        Register ds = currentProgram.getLanguage().getRegister("DS");
        if (ds == null) {
            println("no DS register in this language");
            return;
        }
        int blocks = 0;
        for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
            if (!block.isInitialized() || !(block.getStart() instanceof SegmentedAddress)) {
                continue;
            }
            currentProgram.getProgramContext().setValue(ds, block.getStart(), block.getEnd(),
                BigInteger.valueOf(seg));
            blocks++;
        }
        println(String.format("DS assumed %04X in %d blocks", seg, blocks));
    }
}
