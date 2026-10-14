// Name image-relative entries, find prologues, export decompiler C and listings.
// Ghidra MZ load base is segment 0x1000; image seg:off is that segment minus 0x1000.
import java.io.BufferedWriter;
import java.io.File;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.address.SegmentedAddress;
import ghidra.program.model.address.SegmentedAddressSpace;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.MemoryAccessException;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;
import ghidra.util.exception.CancelledException;

public class ExportFunctions extends GhidraScript {

    private static final int IMAGE_BASE = 0x1000;
    private static final int DECOMP_TIMEOUT = 60;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1 || args.length > 2) {
            println("usage: ExportFunctions.java <out_dir> [names.tsv]");
            return;
        }
        File outDir = new File(args[0]);
        if (!outDir.isDirectory() && !outDir.mkdirs()) {
            println("cannot create " + outDir);
            return;
        }
        int[] named = new int[] { 0, 0 };
        if (args.length == 2) {
            named = applyNames(new File(args[1]));
        }
        int prologues = findPrologues();
        int[] exported = exportAll(outDir);
        String summary = "exported=" + exported[0] + " decompiled=" + exported[1]
            + " names_applied=" + named[0] + "/" + named[1] + " prologues=" + prologues;
        println(summary);
        System.out.println(summary);
    }

    private SegmentedAddressSpace space() {
        return (SegmentedAddressSpace) currentProgram.getAddressFactory().getDefaultAddressSpace();
    }

    private Address imageAddr(int seg, int off) {
        return space().getAddress((seg + IMAGE_BASE) & 0xffff, off & 0xffff);
    }

    private static String imageLabel(Address addr) {
        SegmentedAddress sa = (SegmentedAddress) addr;
        int seg = (sa.getSegment() - IMAGE_BASE) & 0xffff;
        int off = sa.getSegmentOffset() & 0xffff;
        return String.format("%04X:%04X", seg, off);
    }

    /** @return {applied, total} */
    private int[] applyNames(File names) throws Exception {
        int applied = 0;
        int total = 0;
        for (String raw : Files.readAllLines(names.toPath(), StandardCharsets.UTF_8)) {
            monitor.checkCancelled();
            String line = raw.trim();
            if (line.isEmpty() || line.startsWith("#")) {
                continue;
            }
            String[] parts = line.split("\t", 2);
            if (parts.length != 2 || !parts[0].contains(":")) {
                println("bad names line: " + line);
                continue;
            }
            String[] so = parts[0].split(":");
            int seg = Integer.parseInt(so[0].trim(), 16);
            int off = Integer.parseInt(so[1].trim(), 16);
            String name = parts[1].trim();
            total++;
            Function fn = ensureFunction(imageAddr(seg, off), name);
            if (fn != null && name.equals(fn.getName())) {
                applied++;
            } else {
                println("name not applied: " + parts[0] + " " + name);
            }
        }
        println("names applied " + applied + "/" + total);
        return new int[] { applied, total };
    }

    private int findPrologues() throws Exception {
        int created = 0;
        for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
            monitor.checkCancelled();
            if (!block.isInitialized() || !(block.getStart() instanceof SegmentedAddress)) {
                continue;
            }
            long size = block.getSize();
            if (size < 3 || size > Integer.MAX_VALUE) {
                continue;
            }
            byte[] buf = new byte[(int) size];
            int n;
            try {
                n = block.getBytes(block.getStart(), buf);
            } catch (MemoryAccessException e) {
                println("skip block " + block.getName() + ": " + e.getMessage());
                continue;
            }
            for (int i = 0; i + 2 < n; i++) {
                int b0 = buf[i] & 0xff;
                int b1 = buf[i + 1] & 0xff;
                int b2 = buf[i + 2] & 0xff;
                boolean hit = b0 == 0x55 && b1 == 0x8b && b2 == 0xec;
                if (!hit && b0 == 0xc8 && i + 3 < n && (buf[i + 3] & 0xff) == 0x00) {
                    hit = true;
                }
                if (!hit) {
                    continue;
                }
                Address addr = block.getStart().add(i);
                if (getFunctionContaining(addr) != null) {
                    continue;
                }
                if (ensureFunction(addr, null) != null) {
                    created++;
                }
            }
        }
        println("prologue functions created " + created);
        return created;
    }

    private Function ensureFunction(Address addr, String name) throws CancelledException {
        try {
            Function existing = getFunctionAt(addr);
            if (existing != null) {
                rename(existing, name);
                return existing;
            }
            Function owner = getFunctionContaining(addr);
            if (owner != null && name != null) {
                carve(owner, addr);
            }
            if (getInstructionAt(addr) == null) {
                CodeUnit cu = currentProgram.getListing().getCodeUnitContaining(addr);
                if (cu != null && getFunctionContaining(cu.getMinAddress()) == null
                    && (!cu.getMinAddress().equals(addr) || cu instanceof Data)) {
                    clearListing(cu.getMinAddress(), cu.getMaxAddress());
                }
                disassemble(addr);
            }
            Function created = createFunction(addr, name);
            if (created == null) {
                println("create failed at " + addr + (name == null ? "" : " " + name));
                return null;
            }
            rename(created, name);
            return created;
        } catch (CancelledException e) {
            throw e;
        } catch (Exception e) {
            println("function failed at " + addr + ": " + e.getMessage());
            return null;
        }
    }

    private void rename(Function fn, String name) throws Exception {
        if (name == null || name.equals(fn.getName())) {
            return;
        }
        int tx = currentProgram.startTransaction("rename");
        boolean ok = false;
        try {
            fn.setName(name, SourceType.USER_DEFINED);
            ok = true;
        } finally {
            currentProgram.endTransaction(tx, ok);
        }
    }

    // Drop addr..end of its range from an owning function so a named entry can start there.
    private void carve(Function owner, Address addr) throws Exception {
        AddressSet body = new AddressSet(owner.getBody());
        if (body.getRangeContaining(addr) == null) {
            return;
        }
        body.deleteFromMin(addr);
        int tx = currentProgram.startTransaction("carve");
        boolean ok = false;
        try {
            if (body.isEmpty() || !body.contains(owner.getEntryPoint())) {
                currentProgram.getFunctionManager().removeFunction(owner.getEntryPoint());
            } else {
                owner.setBody(body);
            }
            ok = true;
        } finally {
            currentProgram.endTransaction(tx, ok);
        }
    }

    /** @return {functions written, decompiled without error} */
    private int[] exportAll(File outDir) throws Exception {
        List<Function> funcs = new ArrayList<>();
        for (Function fn : currentProgram.getFunctionManager().getFunctions(true)) {
            if (!fn.isExternal() && fn.getEntryPoint() instanceof SegmentedAddress) {
                funcs.add(fn);
            }
        }
        DecompInterface decomp = new DecompInterface();
        int okCount = 0;
        try {
            decomp.openProgram(currentProgram);
            File index = new File(outDir, "index.tsv");
            try (BufferedWriter rows = writer(index)) {
                rows.write("addr\tname\tsize\tcallers\tcallees\tconvention\n");
                int i = 0;
                for (Function fn : funcs) {
                    monitor.checkCancelled();
                    i++;
                    String label = imageLabel(fn.getEntryPoint());
                    String stem = label.replace(':', '_');
                    writeAsm(new File(outDir, stem + ".asm"), fn);
                    String err = writeC(new File(outDir, stem + ".c"), decomp, fn);
                    if (err == null) {
                        okCount++;
                    }
                    String conv = fn.getCallingConventionName();
                    if (conv == null) {
                        conv = "";
                    }
                    long size = fn.getBody().getNumAddresses();
                    int callers = fn.getCallingFunctions(monitor).size();
                    int callees = fn.getCalledFunctions(monitor).size();
                    rows.write(label + "\t" + fn.getName() + "\t" + size + "\t"
                        + callers + "\t" + callees + "\t" + conv + "\n");
                    if (i % 25 == 0 || err != null) {
                        System.out.println("export " + i + "/" + funcs.size() + " " + label
                            + (err == null ? "" : " ERROR"));
                    }
                }
            }
        } finally {
            decomp.dispose();
        }
        return new int[] { funcs.size(), okCount };
    }

    private String writeC(File file, DecompInterface decomp, Function fn) throws Exception {
        String err = null;
        String c = null;
        try {
            DecompileResults res = decomp.decompileFunction(fn, DECOMP_TIMEOUT, monitor);
            if (res != null && res.decompileCompleted() && res.getDecompiledFunction() != null) {
                c = res.getDecompiledFunction().getC();
            }
            if (c == null || c.isEmpty()) {
                err = res == null ? "decompilation failed" : res.getErrorMessage();
                if (err == null || err.isEmpty()) {
                    err = "decompilation failed";
                }
            }
        } catch (Exception e) {
            if (e instanceof CancelledException) {
                throw e;
            }
            err = e.getMessage() == null ? e.toString() : e.getMessage();
        }
        try (BufferedWriter w = writer(file)) {
            if (err != null) {
                err = err.replace("*/", "* /").replace('\n', ' ').replace('\r', ' ');
                w.write("/* decompile error: " + err + " */\n");
            } else {
                w.write(c);
                if (!c.endsWith("\n")) {
                    w.write("\n");
                }
            }
        }
        return err;
    }

    private void writeAsm(File file, Function fn) throws Exception {
        try (BufferedWriter w = writer(file)) {
            InstructionIterator it = currentProgram.getListing().getInstructions(fn.getBody(), true);
            while (it.hasNext()) {
                Instruction ins = it.next();
                byte[] raw = ins.getBytes();
                StringBuilder bytes = new StringBuilder();
                if (raw != null) {
                    for (int i = 0; i < raw.length; i++) {
                        if (i > 0) {
                            bytes.append(' ');
                        }
                        bytes.append(String.format("%02x", raw[i] & 0xff));
                    }
                }
                StringBuilder text = new StringBuilder(ins.getMnemonicString());
                for (int i = 0; i < ins.getNumOperands(); i++) {
                    text.append(i == 0 ? ' ' : ',');
                    text.append(ins.getDefaultOperandRepresentation(i));
                }
                String line = text.toString().replace('\n', ' ').replace('\r', ' ');
                w.write(imageLabel(ins.getAddress()) + "  " + bytes + "  " + line + "\n");
            }
        }
    }

    private static BufferedWriter writer(File file) throws Exception {
        return new BufferedWriter(new OutputStreamWriter(
            Files.newOutputStream(file.toPath()), StandardCharsets.UTF_8));
    }
}
