package dev.hyperchunk.stagedump;

import java.io.BufferedWriter;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.Locale;

import net.minecraft.SharedConstants;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.chunk.ChunkAccess;
import net.minecraft.world.level.chunk.status.WorldGenContext;

/**
 * Records the features-stage execution order of a golden run — the ADR-007
 * Tier-2 replay input. Design and bytecode evidence:
 * .hermes/notes/task9pre-order/A0-manifest-design-decision.md.
 *
 * One line per features-stage chunk application, in actual execution order,
 * emitted at the RNG-determining instant: WorldgenRandom#setDecorationSeed
 * returning inside ChunkStatusTasks#generateFeatures. setDecorationSeed has a
 * second caller (NoiseBasedChunkGenerator#spawnOriginalMobs, SPAWN stage), so
 * capture is armed per-thread by the generateFeatures HEAD and consumed
 * one-shot iff the seeded block origin matches the armed chunk.
 *
 * The manifest records EVERY features application in the dump dimension
 * (spawn area and dependency ring included, not just the dump grid): ring
 * decorations write into grid chunks, so replay needs their order too.
 *
 * Sequence assignment and the file append happen in one critical section, so
 * file order == seq order even if step bodies ever ran concurrently (26.2
 * serializes them per dimension — see A5 note). Lines are flushed eagerly;
 * the file needs no close on server stop.
 */
public final class OrderManifest {
    private record Pending(int chunkX, int chunkZ, long levelSeed) {}

    private static final ThreadLocal<Pending> PENDING = new ThreadLocal<>();
    private static final Object LOCK = new Object();
    private static BufferedWriter out; // guarded by LOCK
    private static long seq;           // guarded by LOCK

    private OrderManifest() {}

    /** ChunkStatusTasks#generateFeatures HEAD: arm capture for this thread. */
    public static void armFeatures(WorldGenContext ctx, ChunkAccess chunk) {
        if (!StageDumper.enabled()
                || !ctx.level().dimension().identifier().toString().equals(StageDumper.dimension())) {
            return;
        }
        ChunkPos pos = chunk.getPos();
        PENDING.set(new Pending(pos.x(), pos.z(), ctx.level().getSeed()));
    }

    /** WorldgenRandom#setDecorationSeed RETURN: the RNG-determining instant. */
    public static void recordDecorationSeed(int minBlockX, int minBlockZ, long decorationSeed) {
        Pending p = PENDING.get();
        if (p == null || (minBlockX >> 4) != p.chunkX() || (minBlockZ >> 4) != p.chunkZ()) {
            return;
        }
        PENDING.remove();
        long nanos = System.nanoTime();
        String thread = Thread.currentThread().getName();
        synchronized (LOCK) {
            write(p, String.format(Locale.ROOT, "%d %d %d %016x %s %d",
                    seq, p.chunkX(), p.chunkZ(), decorationSeed, thread, nanos));
            seq++;
        }
    }

    /**
     * ChunkStatusTasks#generateFeatures RETURN: an armed-but-unconsumed record
     * means the seed capture missed (harness bug or vanilla change). Emit an
     * ERROR line so the run fails loudly instead of yielding a silently
     * incomplete manifest.
     */
    public static void finishFeatures(ChunkAccess chunk) {
        Pending p = PENDING.get();
        if (p == null) {
            return;
        }
        PENDING.remove();
        synchronized (LOCK) {
            write(p, "# ERROR unconsumed features application for chunk "
                    + p.chunkX() + " " + p.chunkZ());
        }
    }

    // Must hold LOCK.
    private static void write(Pending p, String line) {
        try {
            if (out == null) {
                open(p.levelSeed());
            }
            out.write(line);
            out.newLine();
            out.flush();
        } catch (IOException e) {
            // Never break vanilla generation; the harness fails on a missing
            // or short manifest instead.
            System.out.println("[hyperchunk-stagedump] ERROR writing order.manifest: " + e);
            e.printStackTrace();
        }
    }

    // Must hold LOCK.
    private static void open(long levelSeed) throws IOException {
        Path file = StageDumper.dumpDir().resolve("order.manifest");
        Files.createDirectories(file.getParent());
        out = Files.newBufferedWriter(file, StandardCharsets.UTF_8,
                StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);
        out.write("# hyperchunk features order manifest v1");
        out.newLine();
        out.write("# target_version " + SharedConstants.getCurrentVersion().name());
        out.newLine();
        out.write("# seed " + levelSeed);
        out.newLine();
        out.write("# dimension " + StageDumper.dimension());
        out.newLine();
        out.write("# grid center=(" + StageDumper.centerX() + "," + StageDumper.centerZ()
                + ") radius=" + StageDumper.radius());
        out.newLine();
        out.write("# hook net.minecraft.world.level.levelgen.WorldgenRandom#setDecorationSeed(JII)J@RETURN"
                + " armed-by net.minecraft.world.level.chunk.status.ChunkStatusTasks#generateFeatures");
        out.newLine();
        out.write("# columns seq chunkX chunkZ decorationSeedHex thread nanos");
        out.newLine();
        System.out.println("[hyperchunk-stagedump] order manifest: " + file);
    }
}
