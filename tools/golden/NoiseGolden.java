import java.io.PrintWriter;
import java.lang.reflect.Array;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.IdentityHashMap;
import java.util.List;

import net.minecraft.SharedConstants;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.Bootstrap;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.levelgen.DensityFunction;
import net.minecraft.world.level.levelgen.NoiseGeneratorSettings;
import net.minecraft.world.level.levelgen.RandomState;
import net.minecraft.world.level.levelgen.XoroshiroRandomSource;
import net.minecraft.world.level.levelgen.synth.BlendedNoise;
import net.minecraft.world.level.levelgen.synth.ImprovedNoise;
import net.minecraft.world.level.levelgen.synth.NormalNoise;
import net.minecraft.world.level.levelgen.synth.PerlinNoise;

/**
 * Dumps octave-noise / fork-by-string / density-router golden vectors from
 * the vanilla (unobfuscated) 26.2 server jar, for Plan Task 6.
 *
 * Unlike RngGolden this DOES bootstrap the game registries: the router and
 * every NormalNoise instance are obtained from the same RandomState the
 * server would use for level seed SEED, so these vectors capture vanilla's
 * exact instantiation (fork-by-string via md5, per-octave seeding, blended
 * noise seeding) with zero reimplementation on the Java side.
 *
 * Output files (tracked in git):
 *   golden/rng/fork_seed<seed>.txt      - positional fork / fromHashOf / at
 *   golden/rng/octaves_seed<seed>.txt   - NormalNoise internals + samples
 *   golden/rng/router_seed<seed>.txt    - router slot values at block points
 *
 * Doubles are printed as %.17g plus raw IEEE-754 bits; the C side compares
 * bits only.
 */
public final class NoiseGolden {
    static final long SEED = Long.parseLong(System.getProperty("hyperchunk.seed", "1234567890"));

    public static void main(String[] args) throws Exception {
        Path outDir = Path.of(args.length > 0 ? args[0] : "golden/rng");
        Files.createDirectories(outDir);

        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();

        var lookup = net.minecraft.data.registries.VanillaRegistries.createLookup();
        NoiseGeneratorSettings settings = lookup.lookupOrThrow(Registries.NOISE_SETTINGS)
                .getOrThrow(NoiseGeneratorSettings.OVERWORLD).value();
        RandomState rs = RandomState.create(settings,
                lookup.lookupOrThrow(Registries.NOISE), SEED);

        dumpFork(outDir.resolve("fork_seed" + SEED + ".txt"));
        dumpOctaves(outDir.resolve("octaves_seed" + SEED + ".txt"), rs);
        dumpRouter(outDir.resolve("router_seed" + SEED + ".txt"), rs);
        System.out.println("noise golden vectors written to " + outDir.toAbsolutePath());
    }

    static String dbl(double d) {
        return String.format("%.17g bits=0x%016x", d, Double.doubleToRawLongBits(d));
    }

    /* ---- low-level positional fork vectors (no registry needed) ---- */

    static void dumpFork(Path out) throws Exception {
        try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(out))) {
            w.println("# MC 26.2 XoroshiroRandomSource positional-fork golden vectors");
            w.println("# base = new XoroshiroRandomSource(" + SEED + "L)");
            w.println("# factory = base.forkPositional()  (each section re-creates base)");
            w.println("seed " + SEED);

            String[] keys = {
                "minecraft:temperature", "minecraft:vegetation",
                "minecraft:continentalness", "minecraft:offset",
                "minecraft:aquifer", "minecraft:ore", "minecraft:terrain",
                "octave_-9", "octave_0", "a",
            };
            w.println("section fromHashOf " + keys.length);
            for (String k : keys) {
                RandomSource r = new XoroshiroRandomSource(SEED).forkPositional().fromHashOf(k);
                w.println("fromHashOf \"" + k + "\" " + r.nextLong() + " " + r.nextLong()
                        + " " + r.nextLong() + " " + r.nextLong());
            }

            int[][] pts = {{0, 0, 0}, {1, 2, 3}, {-15, 63, 17}, {1000000, -64, -1000000}};
            w.println("section at " + pts.length);
            for (int[] p : pts) {
                RandomSource r = new XoroshiroRandomSource(SEED).forkPositional().at(p[0], p[1], p[2]);
                w.println("at(" + p[0] + "," + p[1] + "," + p[2] + ") "
                        + r.nextLong() + " " + r.nextLong());
            }

            w.println("section fork 2");
            RandomSource base = new XoroshiroRandomSource(SEED);
            for (int i = 0; i < 2; i++) {
                RandomSource f = base.fork();
                w.println("fork[" + i + "] " + f.nextLong() + " " + f.nextLong());
            }
        }
    }

    /* ---- NormalNoise internals via reflection on live instances ---- */

    static Object field(Object o, String name) throws Exception {
        Class<?> c = o.getClass();
        while (c != null) {
            try {
                Field f = c.getDeclaredField(name);
                f.setAccessible(true);
                return f.get(o);
            } catch (NoSuchFieldException e) {
                c = c.getSuperclass();
            }
        }
        throw new NoSuchFieldException(name + " in " + o.getClass());
    }

    static void dumpPerlin(PrintWriter w, String tag, PerlinNoise pn) throws Exception {
        // field names verified against 26.2 bytecode: noiseLevels, amplitudes,
        // firstOctave, lowestFreqValueFactor, lowestFreqInputFactor, maxValue
        ImprovedNoise[] levels = (ImprovedNoise[]) field(pn, "noiseLevels");
        Object amps = field(pn, "amplitudes");
        w.println(tag + ".firstOctave " + field(pn, "firstOctave"));
        w.println(tag + ".amplitudes " + amps);
        w.println(tag + ".lowestFreqInputFactor " + dbl((Double) field(pn, "lowestFreqInputFactor")));
        w.println(tag + ".lowestFreqValueFactor " + dbl((Double) field(pn, "lowestFreqValueFactor")));
        w.println(tag + ".octave_count " + levels.length);
        for (int i = 0; i < levels.length; i++) {
            ImprovedNoise oct = levels[i];
            if (oct == null) {
                w.println(tag + ".octave[" + i + "] null");
            } else {
                w.println(tag + ".octave[" + i + "].xo " + dbl(oct.xo));
                w.println(tag + ".octave[" + i + "].yo " + dbl(oct.yo));
                w.println(tag + ".octave[" + i + "].zo " + dbl(oct.zo));
            }
        }
        double[][] pts = {{0.0, 0.0, 0.0}, {123.456, -64.0, -789.012}, {-3000.5, 80.25, 3000.75}};
        for (double[] p : pts) {
            w.println(tag + ".getValue(" + p[0] + "," + p[1] + "," + p[2] + ") "
                    + dbl(pn.getValue(p[0], p[1], p[2])));
        }
    }

    static void dumpOctaves(Path out, RandomState rs) throws Exception {
        String[] keys = {
            // every noise the noise-stage router subgraph references
            "minecraft:aquifer_barrier", "minecraft:aquifer_fluid_level_floodedness",
            "minecraft:aquifer_fluid_level_spread", "minecraft:aquifer_lava",
            "minecraft:cave_cheese", "minecraft:cave_entrance", "minecraft:cave_layer",
            "minecraft:continentalness", "minecraft:erosion", "minecraft:jagged",
            "minecraft:noodle", "minecraft:noodle_ridge_a", "minecraft:noodle_ridge_b",
            "minecraft:noodle_thickness", "minecraft:offset",
            "minecraft:ore_gap", "minecraft:ore_vein_a", "minecraft:ore_vein_b",
            "minecraft:ore_veininess", "minecraft:pillar", "minecraft:pillar_rareness",
            "minecraft:pillar_thickness", "minecraft:ridge",
            "minecraft:spaghetti_2d", "minecraft:spaghetti_2d_elevation",
            "minecraft:spaghetti_2d_modulator", "minecraft:spaghetti_2d_thickness",
            "minecraft:spaghetti_3d_1", "minecraft:spaghetti_3d_2",
            "minecraft:spaghetti_3d_rarity", "minecraft:spaghetti_3d_thickness",
            "minecraft:spaghetti_roughness", "minecraft:spaghetti_roughness_modulator",
            "minecraft:temperature", "minecraft:vegetation",
        };
        try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(out))) {
            w.println("# MC 26.2 NormalNoise instances from RandomState(seed=" + SEED + ", overworld)");
            w.println("# exactly what NoiseChunk sees: rs.getOrCreateNoise(key)");
            w.println("# valueFactor/internals dumped via reflection from the LIVE instance");
            w.println("seed " + SEED);
            w.println("noise_count " + keys.length);
            for (String k : keys) {
                NormalNoise n = rs.getOrCreateNoise(
                        ResourceKey.create(Registries.NOISE, Identifier.parse(k)));
                w.println("noise \"" + k + "\"");
                w.println(".valueFactor " + dbl((Double) field(n, "valueFactor")));
                dumpPerlin(w, ".first", (PerlinNoise) field(n, "first"));
                dumpPerlin(w, ".second", (PerlinNoise) field(n, "second"));
                double[][] pts = {{0.0, 0.0, 0.0}, {0.25, -16.0, 0.75},
                        {1234.5, 63.0, -987.25}, {-8.125, 320.0, 8.5}};
                for (double[] p : pts) {
                    w.println(".getValue(" + p[0] + "," + p[1] + "," + p[2] + ") "
                            + dbl(n.getValue(p[0], p[1], p[2])));
                }
            }
        }
    }

    /* ---- router slots evaluated at block points (SinglePointContext) ---- */

    static void findInstances(Object o, Class<?> want, IdentityHashMap<Object, Boolean> seen,
                              List<Object> out) throws Exception {
        if (o == null || seen.put(o, Boolean.TRUE) != null)
            return;
        if (want.isInstance(o)) {
            out.add(o);
            return;
        }
        Class<?> c = o.getClass();
        if (c.isArray()) {
            if (!c.componentType().isPrimitive())
                for (int i = 0; i < Array.getLength(o); i++)
                    findInstances(Array.get(o, i), want, seen, out);
            return;
        }
        String pkg = c.getPackageName();
        if (!pkg.startsWith("net.minecraft") && !pkg.startsWith("java.util"))
            return;
        if (o instanceof Iterable<?> it) {
            for (Object e : it)
                findInstances(e, want, seen, out);
            return;
        }
        for (; c != null && c.getPackageName().startsWith("net.minecraft");
                c = c.getSuperclass()) {
            for (Field f : c.getDeclaredFields()) {
                if (Modifier.isStatic(f.getModifiers()) || f.getType().isPrimitive())
                    continue;
                try {
                    f.setAccessible(true);
                    findInstances(f.get(o), want, seen, out);
                } catch (java.lang.reflect.InaccessibleObjectException e) {
                    // JDK-internal field (module-guarded) - never worldgen data
                }
            }
        }
    }

    static void dumpSlot(PrintWriter w, String name, DensityFunction fn,
                         int[] xs, int[] ys, int[] zs) {
        w.println("slot " + name);
        for (int x : xs)
            for (int z : zs)
                for (int y : ys) {
                    double v = fn.compute(new DensityFunction.SinglePointContext(x, y, z));
                    w.println("v " + x + " " + y + " " + z + " " + dbl(v));
                }
    }

    static void dumpRouter(Path out, RandomState rs) throws Exception {
        var r = rs.router();
        try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(out))) {
            w.println("# MC 26.2 overworld NoiseRouter slot values, seed " + SEED);
            w.println("# fn.compute(SinglePointContext(x,y,z)) on the RandomState router -");
            w.println("# markers (interpolated/flat_cache/cache_*) pass through outside");
            w.println("# NoiseChunk, so these are PURE MATH-GRAPH values at block coords.");
            w.println("seed " + SEED);

            // cell-corner lattice of chunks (0,0)/(1,0) + odd coords + far points
            int[] xs = {0, 4, 12, 16, 20, 29, -3, 1000};
            int[] zs = {0, 8, 16, 5, -177};
            int[] ysFull = {-64, -56, -40, -33, 0, 8, 23, 56, 63, 64, 100, 128, 240, 251, 256, 319, 320};
            int[] ysFew = {-64, 0, 63, 128, 320};
            int[] y0 = {0};

            dumpSlot(w, "final_density", r.finalDensity(), xs, ysFull, zs);
            dumpSlot(w, "preliminary_surface_level", r.preliminarySurfaceLevel(), xs, y0, zs);
            dumpSlot(w, "barrier", r.barrierNoise(), xs, ysFew, zs);
            dumpSlot(w, "fluid_level_floodedness", r.fluidLevelFloodednessNoise(), xs, ysFew, zs);
            dumpSlot(w, "fluid_level_spread", r.fluidLevelSpreadNoise(), xs, ysFew, zs);
            dumpSlot(w, "lava", r.lavaNoise(), xs, ysFew, zs);
            dumpSlot(w, "depth", r.depth(), xs, ysFew, zs);
            dumpSlot(w, "continents", r.continents(), xs, y0, zs);
            dumpSlot(w, "erosion", r.erosion(), xs, y0, zs);
            dumpSlot(w, "ridges", r.ridges(), xs, y0, zs);
            dumpSlot(w, "temperature", r.temperature(), xs, y0, zs);
            dumpSlot(w, "vegetation", r.vegetation(), xs, y0, zs);
            dumpSlot(w, "vein_toggle", r.veinToggle(), xs, ysFew, zs);
            dumpSlot(w, "vein_ridged", r.veinRidged(), xs, ysFew, zs);
            dumpSlot(w, "vein_gap", r.veinGap(), xs, ysFew, zs);

            // the LIVE seeded BlendedNoise inside final_density (reflection walk)
            List<Object> found = new ArrayList<>();
            findInstances(r.finalDensity(), BlendedNoise.class, new IdentityHashMap<>(), found);
            w.println("blended_noise_instances " + found.size());
            if (!found.isEmpty()) {
                BlendedNoise bn = (BlendedNoise) found.get(0);
                StringBuilder sb = new StringBuilder();
                bn.parityConfigString(sb);
                for (String line : sb.toString().split("\n"))
                    w.println("# blended " + line);
                dumpSlot(w, "old_blended_noise", bn, xs, ysFull, zs);
            }
        }
    }
}
