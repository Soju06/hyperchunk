package dev.hyperchunk.stagedump;

import java.io.BufferedWriter;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.Locale;

import it.unimi.dsi.fastutil.shorts.ShortList;
import net.minecraft.commands.arguments.blocks.BlockStateParser;
import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.level.chunk.ProtoChunk;

/**
 * postprocess.manifest — records every {@code LevelChunk.postProcessGeneration}
 * invocation (ticking promotion) of the dump dimension, in actual execution
 * order, BEFORE vanilla drains the per-section ShortLists.
 *
 * Why: the golden .mca's live tick rows (fluid t=5, sand/gravel t=2) and the
 * post-promotion block mutations (water spread, bubble_column) are produced by
 * this pass. Their NBT list order is the global Level.scheduleTick order,
 * which is a deterministic function of (a) the order chunks were promoted and
 * (b) the per-chunk drain order (section index asc, ShortList append order —
 * verified against the 26.2 bytecode: postProcessGeneration iterates
 * postProcessing[i] with ShortList.iterator, unpackOffsetCoordinates, then
 * FluidState.tick / BlockState.tick / updateFromNeighbourShapes+setBlock,
 * then ShortList.clear). Recording (a) plus the marked positions makes the
 * pass replayable; see .hermes/notes/task12-region/R-D-bytecode-ticks.md §3.
 *
 * Two line types, one file, appended under one lock (file order == seq order):
 *   chunk line:    seq chunkX chunkZ gameTime nMarked thread nanos
 *   position line: p seq k sectionY x y z blockStateBefore
 * where k is the drain index within the chunk (0..nMarked-1), (x,y,z) are
 * absolute block coordinates decoded exactly like the vanilla drain (via
 * ProtoChunk.unpackOffsetCoordinates itself, no re-derivation; packed short
 * layout is x bits 0-3, y bits 4-7, z bits 8-11), and
 * blockStateBefore is the state at HEAD (before any postProcess mutation of
 * this chunk; earlier chunks' spreads are already visible, matching what the
 * vanilla pass itself reads first).
 */
public final class PostProcessLog {
    private static final Object LOCK = new Object();
    private static BufferedWriter out; // guarded by LOCK
    private static long seq;           // guarded by LOCK

    private PostProcessLog() {}

    /** LevelChunk#postProcessGeneration HEAD. */
    public static void record(LevelChunk chunk, ServerLevel level) {
        if (!StageDumper.enabled()
                || !level.dimension().identifier().toString().equals(StageDumper.dimension())) {
            return;
        }
        try {
            ChunkPos pos = chunk.getPos();
            ShortList[] lists = chunk.getPostProcessing();
            String thread = Thread.currentThread().getName().replaceAll("\\s", "_");
            // One critical section per promotion: snapshot positions +
            // pre-drain states in exact drain order, then append. postProcess
            // runs on the server thread only (ChunkMap promotion callback),
            // so the lock is uncontended belt-and-braces.
            synchronized (LOCK) {
                if (out == null) {
                    open();
                }
                StringBuilder positions = new StringBuilder();
                int n = 0;
                for (int i = 0; i < lists.length; i++) {
                    if (lists[i] == null) {
                        continue;
                    }
                    int sectionY = chunk.getSectionYFromSectionIndex(i);
                    for (int j = 0; j < lists[i].size(); j++) {
                        short packed = lists[i].getShort(j);
                        BlockPos bp = ProtoChunk.unpackOffsetCoordinates(packed, sectionY, pos);
                        String state = BlockStateParser.serialize(chunk.getBlockState(bp));
                        positions.append(String.format(Locale.ROOT, "p %d %d %d %d %d %d %s%n",
                                seq, n, sectionY, bp.getX(), bp.getY(), bp.getZ(), state));
                        n++;
                    }
                }
                out.write(String.format(Locale.ROOT, "%d %d %d %d %d %s %d",
                        seq, pos.x(), pos.z(), level.getGameTime(), n, thread, System.nanoTime()));
                out.newLine();
                out.write(positions.toString());
                out.flush();
                seq++;
            }
        } catch (Throwable t) {
            // Never break vanilla; harness validation fails on a short file.
            System.out.println("[hyperchunk-stagedump] ERROR writing postprocess.manifest: " + t);
            t.printStackTrace();
        }
    }

    // Must hold LOCK.
    private static void open() throws IOException {
        Path file = StageDumper.dumpDir().resolve("postprocess.manifest");
        Files.createDirectories(file.getParent());
        out = Files.newBufferedWriter(file, StandardCharsets.UTF_8,
                StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);
        out.write("# hyperchunk postProcessGeneration order manifest v1");
        out.newLine();
        out.write("# hook net.minecraft.world.level.chunk.LevelChunk#postProcessGeneration(ServerLevel)@HEAD");
        out.newLine();
        out.write("# chunk line: seq chunkX chunkZ gameTime nMarked thread nanos");
        out.newLine();
        out.write("# position line (pre-drain state, drain order): p seq k sectionY x y z blockStateBefore");
        out.newLine();
        System.out.println("[hyperchunk-stagedump] postprocess manifest: " + file);
    }
}
