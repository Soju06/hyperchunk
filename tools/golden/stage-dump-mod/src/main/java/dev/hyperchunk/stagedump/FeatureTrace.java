package dev.hyperchunk.stagedump;

import java.io.BufferedWriter;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Locale;

import net.minecraft.SharedConstants;
import net.minecraft.core.BlockPos;
import net.minecraft.core.registries.Registries;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.WorldGenLevel;
import net.minecraft.world.level.chunk.ChunkAccess;
import net.minecraft.world.level.chunk.status.WorldGenContext;
import net.minecraft.world.level.levelgen.placement.PlacedFeature;
import net.minecraft.world.level.levelgen.structure.Structure;

/**
 * Records, per features-stage chunk application, the executed decoration
 * items and the exact positions that survived each placed feature's
 * placement-modifier pipeline — the Task 9 bisect ladder between "decoration
 * seed matches" and "blocks match" (ADR-007 Tier 2).
 *
 * Off by default; enabled with -Dhyperchunk.dump.trace=true on top of an
 * enabled StageDumper. When disabled, every hook returns immediately —
 * bundle regeneration behavior is unchanged.
 *
 * One file per decorated chunk: {@code <dump.dir>/traces/c.<x>.<z>.trace.txt}
 * with header lines, then events in execution order:
 *
 * <pre>
 * begin <cx> <cz> <decorationSeedHex>
 * s <step> <index> <structure_id>              # StructureStart.placeInChunk fired
 * p <step> <index> <x> <y> <z> <placed01>      # TOP-LEVEL ConfiguredFeature.place
 *                                              #   at a pipeline-surviving position
 * f <step> <index> <npos> <placed01> <placed_feature_id>
 *                                              # PlacedFeature.placeWithBiomeCheck
 *                                              #   returned; npos = its p-line count
 * end <cx> <cz> <f_lines>
 * </pre>
 *
 * (step,index) are the exact setFeatureSeed(decorationSeed, index, step)
 * salts (recon A2 §2.1): index = per-step topo-list position for features,
 * per-step registry counter for structures. Nested ConfiguredFeature.place
 * calls (tree/selector internals) are depth-tracked and NOT recorded — the C
 * replay reproduces top-level pipeline output, not vanilla's inner
 * composition. Depth is reset on every setFeatureSeed so an exception inside
 * one feature (caught per-feature by vanilla) cannot skew later items.
 *
 * Threading: all worldgen decoration of one chunk is synchronous on one
 * thread (recon A1); state is kept in a ThreadLocal and each chunk's file is
 * owned by the decorating thread. A global lock guards directory creation.
 */
public final class FeatureTrace {
    private static final boolean ENABLED =
            StageDumper.enabled() && Boolean.getBoolean("hyperchunk.dump.trace");
    private static final Object DIR_LOCK = new Object();
    private static boolean dirLogged;

    private static final class Ctx {
        final int chunkX;
        final int chunkZ;
        BufferedWriter out;     // opened lazily at the decoration-seed capture
        int step = -1;          // last setFeatureSeed salt
        int index = -1;
        int depth;              // ConfiguredFeature.place nesting depth
        boolean inItem;         // between placeWithBiomeCheck HEAD and RETURN
        int itemPositions;      // p-lines of the current item
        int fLines;             // f-lines of this chunk

        Ctx(int chunkX, int chunkZ) {
            this.chunkX = chunkX;
            this.chunkZ = chunkZ;
        }
    }

    private static final ThreadLocal<Ctx> CTX = new ThreadLocal<>();

    private FeatureTrace() {}

    public static boolean enabled() {
        return ENABLED;
    }

    /** ChunkStatusTasks#generateFeatures HEAD (same arming as OrderManifest). */
    public static void armFeatures(WorldGenContext ctx, ChunkAccess chunk) {
        if (!ENABLED
                || !ctx.level().dimension().identifier().toString().equals(StageDumper.dimension())) {
            return;
        }
        ChunkPos pos = chunk.getPos();
        CTX.set(new Ctx(pos.x(), pos.z()));
    }

    /** WorldgenRandom#setDecorationSeed RETURN (after OrderManifest records). */
    public static void onDecorationSeed(int minBlockX, int minBlockZ, long decorationSeed) {
        Ctx c = CTX.get();
        if (c == null || (minBlockX >> 4) != c.chunkX || (minBlockZ >> 4) != c.chunkZ
                || c.out != null) {
            return;
        }
        try {
            c.out = open(c);
            write(c, String.format(Locale.ROOT, "begin %d %d %016x",
                    c.chunkX, c.chunkZ, decorationSeed));
        } catch (IOException e) {
            fail(c, e);
        }
    }

    /** WorldgenRandom#setFeatureSeed(JII)V HEAD. */
    public static void onFeatureSeed(int index, int step) {
        Ctx c = CTX.get();
        if (c == null || c.out == null) {
            return;
        }
        c.index = index;
        c.step = step;
        c.depth = 0;      // defensive: a per-feature exception may skip RETURNs
        c.inItem = false;
    }

    /** ConfiguredFeature#place HEAD. */
    public static void onPlaceHead() {
        Ctx c = CTX.get();
        if (c == null || c.out == null) {
            return;
        }
        c.depth++;
    }

    /** ConfiguredFeature#place RETURN. */
    public static void onPlaceReturn(BlockPos pos, boolean placed) {
        Ctx c = CTX.get();
        if (c == null || c.out == null) {
            return;
        }
        if (--c.depth == 0 && c.inItem) {
            c.itemPositions++;
            write(c, "p " + c.step + " " + c.index + " "
                    + pos.getX() + " " + pos.getY() + " " + pos.getZ() + " "
                    + (placed ? 1 : 0));
        }
    }

    /** PlacedFeature#placeWithBiomeCheck HEAD. */
    public static void onPlacedFeatureHead() {
        Ctx c = CTX.get();
        if (c == null || c.out == null || c.depth != 0) {
            return;
        }
        c.inItem = true;
        c.itemPositions = 0;
    }

    /** PlacedFeature#placeWithBiomeCheck RETURN. */
    public static void onPlacedFeatureReturn(PlacedFeature self, WorldGenLevel level, boolean placed) {
        Ctx c = CTX.get();
        if (c == null || c.out == null || c.depth != 0 || !c.inItem) {
            return;
        }
        c.inItem = false;
        c.fLines++;
        String id = level.registryAccess().lookupOrThrow(Registries.PLACED_FEATURE)
                .getResourceKey(self)
                .map(k -> k.identifier().toString())
                .orElse("<unregistered>");
        write(c, "f " + c.step + " " + c.index + " " + c.itemPositions + " "
                + (placed ? 1 : 0) + " " + id);
    }

    private static final boolean ORE_TRACE = Boolean.getBoolean("hyperchunk.dump.oretrace");

    /** OreFeature#canPlaceOre RETURN — debug-only candidate ground truth. */
    public static void onOreCandidate(net.minecraft.core.BlockPos pos,
            net.minecraft.world.level.block.state.BlockState state, boolean result) {
        if (!ORE_TRACE) {
            return;
        }
        Ctx c = CTX.get();
        if (c == null || c.out == null) {
            return;
        }
        write(c, "o " + c.step + " " + c.index + " " + pos.getX() + " " + pos.getY()
                + " " + pos.getZ() + " "
                + net.minecraft.commands.arguments.blocks.BlockStateParser.serialize(state)
                + " " + (result ? 1 : 0));
    }

    /** StructureStart#placeInChunk HEAD — evidence that a structure placed. */
    public static void onStructurePlace(Structure structure, WorldGenLevel level) {
        Ctx c = CTX.get();
        if (c == null || c.out == null) {
            return;
        }
        String id = level.registryAccess().lookupOrThrow(Registries.STRUCTURE)
                .getResourceKey(structure)
                .map(k -> k.identifier().toString())
                .orElse("<unregistered>");
        write(c, "s " + c.step + " " + c.index + " " + id);
    }

    /** ChunkStatusTasks#generateFeatures RETURN. */
    public static void finishFeatures(ChunkAccess chunk) {
        Ctx c = CTX.get();
        if (c == null) {
            return;
        }
        CTX.remove();
        if (c.out == null) {
            System.out.println("[hyperchunk-stagedump] ERROR trace armed but no decoration seed for chunk "
                    + c.chunkX + " " + c.chunkZ);
            return;
        }
        write(c, "end " + c.chunkX + " " + c.chunkZ + " " + c.fLines);
        try {
            c.out.close();
        } catch (IOException e) {
            fail(c, e);
        }
    }

    private static BufferedWriter open(Ctx c) throws IOException {
        Path dir = StageDumper.dumpDir().resolve("traces");
        synchronized (DIR_LOCK) {
            Files.createDirectories(dir);
            if (!dirLogged) {
                dirLogged = true;
                System.out.println("[hyperchunk-stagedump] feature traces: " + dir);
            }
        }
        Path file = dir.resolve("c." + c.chunkX + "." + c.chunkZ + ".trace.txt");
        BufferedWriter w = Files.newBufferedWriter(file, StandardCharsets.UTF_8);
        w.write("# hyperchunk features trace v1");
        w.newLine();
        w.write("# target_version " + SharedConstants.getCurrentVersion().name());
        w.newLine();
        w.write("# dimension " + StageDumper.dimension());
        w.newLine();
        w.write("# events begin|s|p|f|end — see FeatureTrace.java / golden/features-trace/FORMAT.md");
        w.newLine();
        return w;
    }

    private static void write(Ctx c, String line) {
        try {
            c.out.write(line);
            c.out.newLine();
            c.out.flush();
        } catch (IOException e) {
            fail(c, e);
        }
    }

    // Never break vanilla generation; a truncated trace fails the harness.
    private static void fail(Ctx c, IOException e) {
        System.out.println("[hyperchunk-stagedump] ERROR writing trace for chunk "
                + c.chunkX + " " + c.chunkZ + ": " + e);
        e.printStackTrace();
    }
}
