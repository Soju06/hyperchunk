package dev.hyperchunk.stagedump;

import java.io.BufferedWriter;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.Locale;

import net.minecraft.world.level.ChunkPos;

/**
 * order.snapshots — positions every stage dump in the features order.
 *
 * The features-order manifest (order.manifest) determines a chunk's
 * 07_features dump (taken synchronously inside the serialized step body) and
 * the final features-complete block state. Dumps of LATER stages are
 * snapshots taken when that chunk's stage future completed — for the
 * async-completing stages (initialize_light/light, full conversion) that
 * moment races the still-running features sequence, so which neighbor
 * spill-ins a 08..11 dump contains is timing the manifest alone does not
 * record. This file records it: for each (stage, chunk) dump, the manifest
 * seq counter sampled before (seqBegin) and after (seqEnd) the dump's file
 * writes. seqBegin == seqEnd means the snapshot is a defined function of the
 * first seqBegin features applications; seqBegin != seqEnd flags a torn
 * snapshot (features landed mid-dump).
 *
 * Line order is dump-completion order (appended under a lock).
 */
public final class SnapshotLog {
    private static final Object LOCK = new Object();
    private static BufferedWriter out; // guarded by LOCK

    private SnapshotLog() {}

    public static void record(String stagePrefix, ChunkPos pos, long seqBegin, long seqEnd) {
        // single column in a space-separated format ("Server thread" etc.)
        String thread = Thread.currentThread().getName().replaceAll("\\s", "_");
        long nanos = System.nanoTime();
        synchronized (LOCK) {
            try {
                if (out == null) {
                    open();
                }
                out.write(String.format(Locale.ROOT, "%s %d %d %d %d %s %d",
                        stagePrefix, pos.x(), pos.z(), seqBegin, seqEnd, thread, nanos));
                out.newLine();
                out.flush();
            } catch (IOException e) {
                System.out.println("[hyperchunk-stagedump] ERROR writing order.snapshots: " + e);
                e.printStackTrace();
            }
        }
    }

    // Must hold LOCK.
    private static void open() throws IOException {
        Path file = StageDumper.dumpDir().resolve("order.snapshots");
        Files.createDirectories(file.getParent());
        out = Files.newBufferedWriter(file, StandardCharsets.UTF_8,
                StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);
        out.write("# hyperchunk stage snapshot order v1");
        out.newLine();
        out.write("# seqBegin/seqEnd = order.manifest seq counter sampled before/after the dump;");
        out.newLine();
        out.write("#   the dump saw exactly the first seqBegin features applications iff seqBegin == seqEnd");
        out.newLine();
        out.write("# columns stage chunkX chunkZ seqBegin seqEnd thread nanos");
        out.newLine();
        System.out.println("[hyperchunk-stagedump] snapshot log: " + file);
    }
}
