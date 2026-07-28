import java.io.PrintWriter;
import java.lang.reflect.Field;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Random;

import net.minecraft.util.RandomSource;
import net.minecraft.world.level.levelgen.LegacyRandomSource;
import net.minecraft.world.level.levelgen.RandomSupport;
import net.minecraft.world.level.levelgen.XoroshiroRandomSource;
import net.minecraft.world.level.levelgen.synth.ImprovedNoise;

/**
 * Dumps RNG and Perlin golden vectors from the vanilla (unobfuscated) 26.2
 * server jar. Runs with the nested server jar + bundled libraries on the
 * classpath; no server bootstrap is required because these classes are
 * self-contained.
 *
 * Everything here mirrors exactly how worldgen seeds its RNG:
 *   new XoroshiroRandomSource(seed)
 *     == Xoroshiro128PlusPlus(RandomSupport.upgradeSeedTo128bit(seed))
 *     where upgradeSeedTo128bit = (seed^SILVER, +GOLDEN) then mixStafford13
 * (verified against the 26.2 bytecode, see tools/golden/NOTES.md).
 *
 * Output files (tracked in git):
 *   golden/rng/xoroshiro_seed<seed>.txt
 *   golden/rng/lcg_seed<seed>.txt
 *   golden/rng/perlin_seed<seed>.txt
 *
 * Doubles are printed both as %.17g (17 significant digits round-trips a
 * binary64 exactly) and as raw IEEE-754 bits, so the C side can compare
 * bit-exactly without parsing decimal.
 */
public final class RngGolden {
    static final long SEED = Long.parseLong(System.getProperty("hyperchunk.seed", "1234567890"));

    public static void main(String[] args) throws Exception {
        Path outDir = Path.of(args.length > 0 ? args[0] : "golden/rng");
        Files.createDirectories(outDir);
        dumpXoroshiro(outDir.resolve("xoroshiro_seed" + SEED + ".txt"));
        dumpLcg(outDir.resolve("lcg_seed" + SEED + ".txt"));
        dumpPerlin(outDir.resolve("perlin_seed" + SEED + ".txt"));
        System.out.println("golden RNG vectors written to " + outDir.toAbsolutePath());
    }

    static String dbl(double d) {
        return String.format("%.17g bits=0x%016x", d, Double.doubleToRawLongBits(d));
    }

    static void dumpXoroshiro(Path out) throws Exception {
        try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(out))) {
            w.println("# MC 26.2 XoroshiroRandomSource golden vectors");
            w.println("# construction: new net.minecraft.world.level.levelgen.XoroshiroRandomSource(" + SEED + "L)");
            w.println("#   == Xoroshiro128PlusPlus(RandomSupport.upgradeSeedTo128bit(seed))");
            w.println("#   upgradeSeedTo128bit(seed) = mixStafford13(seed ^ SILVER_RATIO_64), mixStafford13(lo + GOLDEN_RATIO_64)");
            w.println("# each 'section' below starts from a FRESH instance seeded with the same seed");
            w.println("# longs/ints are printed as signed decimal (Java toString)");
            w.println("seed " + SEED);

            var unmixed = RandomSupport.upgradeSeedTo128bitUnmixed(SEED);
            var mixed = RandomSupport.upgradeSeedTo128bit(SEED);
            w.println("state_unmixed_lo " + unmixed.seedLo());
            w.println("state_unmixed_hi " + unmixed.seedHi());
            w.println("state_mixed_lo " + mixed.seedLo());
            w.println("state_mixed_hi " + mixed.seedHi());

            w.println("section nextLong 16");
            RandomSource r = new XoroshiroRandomSource(SEED);
            for (int i = 0; i < 16; i++) {
                w.println("nextLong[" + i + "] " + r.nextLong());
            }

            w.println("section nextInt(256-i) 8");
            r = new XoroshiroRandomSource(SEED);
            for (int i = 0; i < 8; i++) {
                int bound = 256 - i;
                w.println("nextInt(" + bound + ") " + r.nextInt(bound));
            }

            w.println("section nextDouble 4");
            r = new XoroshiroRandomSource(SEED);
            for (int i = 0; i < 4; i++) {
                w.println("nextDouble[" + i + "] " + dbl(r.nextDouble()));
            }
        }
    }

    static void dumpLcg(Path out) throws Exception {
        try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(out))) {
            w.println("# java.util.Random (legacy 48-bit LCG) golden vectors");
            w.println("# construction: new java.util.Random(" + SEED + "L)");
            w.println("#   state0 = (seed ^ 0x5DEECE66D) & ((1<<48)-1)");
            w.println("# each 'section' below starts from a FRESH instance seeded with the same seed");
            w.println("seed " + SEED);

            w.println("section nextLong 16");
            Random r = new Random(SEED);
            for (int i = 0; i < 16; i++) {
                w.println("nextLong[" + i + "] " + r.nextLong());
            }

            w.println("section nextInt(256-i) 8");
            r = new Random(SEED);
            for (int i = 0; i < 8; i++) {
                int bound = 256 - i;
                w.println("nextInt(" + bound + ") " + r.nextInt(bound));
            }

            w.println("section nextDouble 4");
            r = new Random(SEED);
            for (int i = 0; i < 4; i++) {
                w.println("nextDouble[" + i + "] " + dbl(r.nextDouble()));
            }

            // MC wraps the same LCG as LegacyRandomSource; prove the streams
            // are identical so the C side only needs one LCG implementation.
            w.println("section legacyRandomSource_crosscheck 4");
            RandomSource mc = new LegacyRandomSource(SEED);
            Random jdk = new Random(SEED);
            for (int i = 0; i < 4; i++) {
                long a = mc.nextLong();
                long b = jdk.nextLong();
                w.println("legacy_nextLong[" + i + "] " + a + " matches_jdk " + (a == b));
            }
        }
    }

    static void dumpPerlin(Path out) throws Exception {
        try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(out))) {
            w.println("# MC 26.2 ImprovedNoise (vanilla Perlin) golden samples");
            w.println("# construction: new ImprovedNoise(new XoroshiroRandomSource(" + SEED + "L))");
            w.println("#   consumes: nextDouble()*256 for xo, yo, zo, then the perm-table shuffle");
            w.println("seed " + SEED);

            ImprovedNoise n = new ImprovedNoise(new XoroshiroRandomSource(SEED));
            w.println("xo " + dbl(n.xo));
            w.println("yo " + dbl(n.yo));
            w.println("zo " + dbl(n.zo));

            double[][] pts = {
                {0.5, 0.25, 0.125},
                {1.5, -2.25, 3.0},
                {100.0, 64.0, -100.0},
            };
            for (double[] p : pts) {
                w.println("noise(" + p[0] + ", " + p[1] + ", " + p[2] + ") "
                        + dbl(n.noise(p[0], p[1], p[2])));
            }

            // Full 256-entry permutation table (private byte[] p) — lets the
            // C side verify its shuffle byte-for-byte instead of guessing
            // from noise output alone. Classpath = unnamed module, so plain
            // setAccessible works.
            Field pf = ImprovedNoise.class.getDeclaredField("p");
            pf.setAccessible(true);
            byte[] perm = (byte[]) pf.get(n);
            w.println("perm_table_length " + perm.length);
            StringBuilder sb = new StringBuilder("perm_table");
            for (byte b : perm) sb.append(' ').append(b & 0xFF);
            w.println(sb);
        }
    }
}
