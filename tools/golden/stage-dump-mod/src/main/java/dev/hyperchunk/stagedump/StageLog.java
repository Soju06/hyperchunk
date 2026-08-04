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
 * stages.log — every generation-step COMPLETION in the dump dimension, for
 * EVERY chunk (not just the dump grid), in actual completion order.
 *
 * Why: the 09_light dumps are snapshots of the light engine at the moment a
 * grid chunk's light stage completed. Which OTHER chunks (especially the
 * off-grid ring) had already run their own light stage by then determines
 * the border influx those dumps saw. order.manifest records the features
 * order only; without this file a replay must GUESS ring light enablement
 * from decoration prefixes — measured on the Task-13 unified bundles, that
 * guess is what produced the residual 09 light diffs (blocks/heightmaps were
 * already 0-diff once the noSave capture removed the save/unload race).
 * 26.2 serializes all worldgen step bodies of a dimension on one
 * ConsecutiveExecutor (task9pre-order A5), so this log is a real total order.
 *
 * Line: stageIndex stageName chunkX chunkZ featuresSeq nanos
 * (featuresSeq = OrderManifest counter sampled at completion; file order is
 * the completion order — one lock, eager flush.)
 */
public final class StageLog {
    private static final Object LOCK = new Object();
    private static BufferedWriter out; // guarded by LOCK

    private StageLog() {}

    public static void record(int stageIndex, String stageName, ChunkPos pos) {
        long seq = OrderManifest.currentSeq();
        long nanos = System.nanoTime();
        synchronized (LOCK) {
            try {
                if (out == null) {
                    open();
                }
                out.write(String.format(Locale.ROOT, "%02d %s %d %d %d %d",
                        stageIndex, stageName, pos.x(), pos.z(), seq, nanos));
                out.newLine();
                out.flush();
            } catch (IOException e) {
                System.out.println("[hyperchunk-stagedump] ERROR writing stages.log: " + e);
                e.printStackTrace();
            }
        }
    }

    // Must hold LOCK.
    private static void open() throws IOException {
        Path file = StageDumper.dumpDir().resolve("stages.log");
        Files.createDirectories(file.getParent());
        out = Files.newBufferedWriter(file, StandardCharsets.UTF_8,
                StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);
        out.write("# hyperchunk stage completion log v1");
        out.newLine();
        out.write("# every generation-step completion in the dump dimension, all chunks,");
        out.newLine();
        out.write("#   in completion order (ChunkStep.apply future continuation)");
        out.newLine();
        out.write("# columns stageIndex stageName chunkX chunkZ featuresSeq nanos");
        out.newLine();
        System.out.println("[hyperchunk-stagedump] stage log: " + file);
    }
}
