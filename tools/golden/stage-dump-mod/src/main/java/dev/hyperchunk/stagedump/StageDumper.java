package dev.hyperchunk.stagedump;

import java.io.IOException;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicBoolean;

import net.minecraft.commands.arguments.blocks.BlockStateParser;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Holder;
import net.minecraft.core.QuartPos;
import net.minecraft.core.registries.Registries;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.LightLayer;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.ChunkAccess;
import net.minecraft.world.level.chunk.status.ChunkPyramid;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import net.minecraft.world.level.chunk.status.ChunkStep;
import net.minecraft.world.level.chunk.status.WorldGenContext;
import net.minecraft.world.level.levelgen.Heightmap;
import net.minecraft.world.level.lighting.LayerLightEventListener;

/**
 * Dumps intermediate chunk state after each worldgen ChunkStatus stage.
 * See tools/golden/stage-dump-mod/ and golden/stages/FORMAT.md.
 *
 * Configuration (JVM system properties):
 *   hyperchunk.dump.dir      output root; absent => harness completely inert
 *   hyperchunk.dump.centerX  center chunk x (default 0)
 *   hyperchunk.dump.centerZ  center chunk z (default 0)
 *   hyperchunk.dump.radius   chebyshev radius in chunks (default 1 => 3x3)
 *   hyperchunk.dump.stages   comma list of stage names without the
 *                            "minecraft:" prefix, or "all" (default) for every
 *                            generation stage the 26.2 pipeline actually runs
 *   hyperchunk.dump.dimension dimension id to dump (default minecraft:overworld)
 *
 * All dumping happens inside the stage's own CompletableFuture continuation
 * (see ChunkStepMixin), i.e. after the stage finished and before dependents
 * observe completion — the chunk is quiescent while we read it.
 */
public final class StageDumper {
    private static final String PROP_PREFIX = "hyperchunk.dump.";

    private static final Path DUMP_DIR;
    private static final int CENTER_X;
    private static final int CENTER_Z;
    private static final int RADIUS;
    private static final Set<String> STAGES; // null => all
    private static final String DIMENSION;
    private static final Set<String> WRITTEN = ConcurrentHashMap.newKeySet();
    private static final AtomicBoolean Y_RANGE_LOGGED = new AtomicBoolean();

    static {
        String dir = System.getProperty(PROP_PREFIX + "dir");
        DUMP_DIR = dir == null ? null : Path.of(dir);
        CENTER_X = Integer.getInteger(PROP_PREFIX + "centerX", 0);
        CENTER_Z = Integer.getInteger(PROP_PREFIX + "centerZ", 0);
        RADIUS = Integer.getInteger(PROP_PREFIX + "radius", 1);
        String stages = System.getProperty(PROP_PREFIX + "stages", "all");
        STAGES = "all".equals(stages) ? null : Set.of(stages.toLowerCase(Locale.ROOT).split(","));
        DIMENSION = System.getProperty(PROP_PREFIX + "dimension", "minecraft:overworld");
        if (DUMP_DIR != null) {
            log("enabled: dir=" + DUMP_DIR + " center=(" + CENTER_X + "," + CENTER_Z
                    + ") radius=" + RADIUS + " stages=" + (STAGES == null ? "all" : STAGES)
                    + " dimension=" + DIMENSION);
        }
    }

    private StageDumper() {}

    private static void log(String msg) {
        System.out.println("[hyperchunk-stagedump] " + msg);
    }

    public static boolean enabled() {
        return DUMP_DIR != null;
    }

    // Configuration accessors for OrderManifest (same run parameters, one source).
    public static Path dumpDir() {
        return DUMP_DIR;
    }

    public static String dimension() {
        return DIMENSION;
    }

    public static int centerX() {
        return CENTER_X;
    }

    public static int centerZ() {
        return CENTER_Z;
    }

    public static int radius() {
        return RADIUS;
    }

    /** Only steps of the generation pyramid — never dump on chunk *loading*. */
    public static boolean isGenerationStep(ChunkStep step, ChunkStatus status) {
        try {
            return ChunkPyramid.GENERATION_PYRAMID.getStepTo(status) == step;
        } catch (RuntimeException e) {
            return false; // e.g. EMPTY has no step
        }
    }

    public static String stageName(ChunkStatus status) {
        String name = status.getName();
        return name.startsWith("minecraft:") ? name.substring("minecraft:".length()) : name;
    }

    public static boolean wantsStage(ChunkStatus status) {
        return STAGES == null || STAGES.contains(stageName(status));
    }

    public static boolean wants(WorldGenContext ctx, ChunkPos pos, ChunkStatus status) {
        return enabled()
                && wantsStage(status)
                && ctx.level().dimension().identifier().toString().equals(DIMENSION)
                && Math.abs(pos.x() - CENTER_X) <= RADIUS
                && Math.abs(pos.z() - CENTER_Z) <= RADIUS;
    }

    /** Called from the mixin once per (chunk, stage), after the stage completed. */
    public static void dump(WorldGenContext ctx, ChunkStatus status, ChunkAccess chunk) {
        String stage = stageName(status);
        ChunkPos pos = chunk.getPos();
        String key = "c." + pos.x() + "." + pos.z() + "/" + stage;
        if (!WRITTEN.add(key)) {
            return;
        }
        try {
            Path chunkDir = DUMP_DIR.resolve("c." + pos.x() + "." + pos.z());
            Files.createDirectories(chunkDir);
            String prefix = String.format(Locale.ROOT, "%02d_%s", status.getIndex(), stage);

            if (Y_RANGE_LOGGED.compareAndSet(false, true)) {
                log("observed y-range: minY=" + chunk.getMinY() + " maxY=" + chunk.getMaxY()
                        + " height=" + chunk.getHeight()
                        + " dimension=" + ctx.level().dimension().identifier());
            }

            dumpKind(key, () -> dumpBlocks(chunkDir.resolve(prefix + ".blocks.txt"), chunk, stage));
            // ProtoChunk has no biome storage before the biomes stage
            // (26.2 throws "Asking for biomes before we have biomes").
            if (status.isOrAfter(ChunkStatus.BIOMES)) {
                dumpKind(key, () -> dumpBiomes(chunkDir.resolve(prefix + ".biomes.txt"), chunk, stage));
            }
            dumpKind(key, () -> dumpHeightmaps(chunkDir.resolve(prefix + ".heightmaps.txt"), chunk, stage));
            if ("initialize_light".equals(stage) || "light".equals(stage)) {
                dumpKind(key, () -> dumpLight(chunkDir.resolve(prefix + ".light_block.txt"), ctx, chunk, stage, LightLayer.BLOCK));
                dumpKind(key, () -> dumpLight(chunkDir.resolve(prefix + ".light_sky.txt"), ctx, chunk, stage, LightLayer.SKY));
            }
            log("dumped " + key);
        } catch (Throwable t) {
            // Never break vanilla generation; a missing dump is visible in the
            // collected file set and fails the harness loudly instead.
            log("ERROR dumping " + key + ": " + t);
            t.printStackTrace();
        }
    }

    @FunctionalInterface
    private interface DumpKind {
        void run() throws IOException;
    }

    /** One failing dump kind must not swallow the remaining kinds. */
    private static void dumpKind(String key, DumpKind kind) {
        try {
            kind.run();
        } catch (Throwable t) {
            log("ERROR dumping " + key + ": " + t);
            t.printStackTrace();
        }
    }

    private static void header(PrintWriter w, ChunkAccess chunk, String stage, String kind, String order) {
        ChunkPos pos = chunk.getPos();
        w.println("# hyperchunk golden stage dump v1");
        w.println("# kind " + kind);
        w.println("# chunk " + pos.x() + " " + pos.z());
        w.println("# stage " + stage);
        w.println("# minY " + chunk.getMinY() + " maxY " + chunk.getMaxY() + " height " + chunk.getHeight());
        w.println("# order " + order);
    }

    private static void dumpBlocks(Path file, ChunkAccess chunk, String stage) throws IOException {
        ChunkPos pos = chunk.getPos();
        int minY = chunk.getMinY();
        int maxY = chunk.getMaxY();
        Map<BlockState, Integer> palette = new LinkedHashMap<>();
        List<String> paletteNames = new ArrayList<>();
        StringBuilder data = new StringBuilder(1 << 20);
        BlockPos.MutableBlockPos cursor = new BlockPos.MutableBlockPos();
        for (int y = minY; y <= maxY; y++) {
            for (int z = 0; z < 16; z++) {
                for (int x = 0; x < 16; x++) {
                    BlockState state = chunk.getBlockState(
                            cursor.set(pos.getMinBlockX() + x, y, pos.getMinBlockZ() + z));
                    Integer idx = palette.get(state);
                    if (idx == null) {
                        idx = palette.size();
                        palette.put(state, idx);
                        paletteNames.add(BlockStateParser.serialize(state));
                    }
                    if (x > 0) {
                        data.append(' ');
                    }
                    data.append(idx.intValue());
                }
                data.append('\n');
            }
        }
        try (PrintWriter w = writer(file)) {
            header(w, chunk, stage, "blocks",
                    "y in [minY..maxY] asc, then z in [0..15], one row of 16 x-asc palette indices");
            for (int i = 0; i < paletteNames.size(); i++) {
                w.println("palette " + i + " " + paletteNames.get(i));
            }
            w.println("data");
            w.print(data);
        }
    }

    private static void dumpBiomes(Path file, ChunkAccess chunk, String stage) throws IOException {
        ChunkPos pos = chunk.getPos();
        // Biomes are stored per 4x4x4 quart. Quart coords are world >> 2.
        int minQY = QuartPos.fromBlock(chunk.getMinY());
        int maxQY = QuartPos.fromBlock(chunk.getMaxY());
        int baseQX = QuartPos.fromBlock(pos.getMinBlockX());
        int baseQZ = QuartPos.fromBlock(pos.getMinBlockZ());
        Map<String, Integer> palette = new LinkedHashMap<>();
        StringBuilder data = new StringBuilder(1 << 14);
        for (int qy = minQY; qy <= maxQY; qy++) {
            for (int qz = 0; qz < 4; qz++) {
                for (int qx = 0; qx < 4; qx++) {
                    Holder<Biome> biome = chunk.getNoiseBiome(baseQX + qx, qy, baseQZ + qz);
                    String name = biome.unwrapKey()
                            .map(k -> k.identifier().toString())
                            .orElse("<inline:" + biome + ">");
                    Integer idx = palette.computeIfAbsent(name, n -> palette.size());
                    if (qx > 0) {
                        data.append(' ');
                    }
                    data.append(idx.intValue());
                }
                data.append('\n');
            }
        }
        try (PrintWriter w = writer(file)) {
            header(w, chunk, stage, "biomes",
                    "quartY in [minY>>2..maxY>>2] asc, then quartZ in [0..3], one row of 4 quartX-asc palette indices");
            int i = 0;
            for (String name : palette.keySet()) {
                w.println("palette " + (i++) + " " + name);
            }
            w.println("data");
            w.print(data);
        }
    }

    private static void dumpHeightmaps(Path file, ChunkAccess chunk, String stage) throws IOException {
        try (PrintWriter w = writer(file)) {
            header(w, chunk, stage, "heightmaps",
                    "per heightmap type: 16 rows (z asc) of 16 x-asc values; value = highest blocking y + 1");
            for (Map.Entry<Heightmap.Types, Heightmap> e : chunk.getHeightmaps()) {
                w.println("heightmap " + e.getKey().getSerializationKey());
                Heightmap hm = e.getValue();
                for (int z = 0; z < 16; z++) {
                    StringBuilder row = new StringBuilder();
                    for (int x = 0; x < 16; x++) {
                        if (x > 0) {
                            row.append(' ');
                        }
                        row.append(hm.getFirstAvailable(x, z));
                    }
                    w.println(row);
                }
            }
        }
    }

    private static void dumpLight(Path file, WorldGenContext ctx, ChunkAccess chunk,
            String stage, LightLayer layer) throws IOException {
        ChunkPos pos = chunk.getPos();
        LayerLightEventListener listener = ctx.lightEngine().getLayerListener(layer);
        BlockPos.MutableBlockPos cursor = new BlockPos.MutableBlockPos();
        try (PrintWriter w = writer(file)) {
            header(w, chunk, stage, "light_" + layer.name().toLowerCase(Locale.ROOT),
                    "y in [minY..maxY] asc, then z in [0..15], one row of 16 x-asc hex light values");
            w.println("data");
            StringBuilder row = new StringBuilder(16);
            for (int y = chunk.getMinY(); y <= chunk.getMaxY(); y++) {
                for (int z = 0; z < 16; z++) {
                    row.setLength(0);
                    for (int x = 0; x < 16; x++) {
                        int v = listener.getLightValue(
                                cursor.set(pos.getMinBlockX() + x, y, pos.getMinBlockZ() + z));
                        row.append(Character.forDigit(v & 0xF, 16));
                    }
                    w.println(row);
                }
            }
        }
    }

    private static PrintWriter writer(Path file) throws IOException {
        return new PrintWriter(Files.newBufferedWriter(file, StandardCharsets.UTF_8));
    }
}
