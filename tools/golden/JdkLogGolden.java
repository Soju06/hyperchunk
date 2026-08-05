import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Random;

/**
 * Dumps JDK Math.log bit vectors over a fixed deterministic corpus.
 * MarsagliaPolarGaussian calls Math.log(radiusSquared), radiusSquared in
 * (0,1); java.lang.Math.log only has a 1-ulp/semi-monotonic SPEC, and
 * glibc log() differs from the HotSpot x86-64 stub (StubRoutines::dlog,
 * Intel LIBM) by 1 ulp on some inputs. The value the golden contains is
 * whatever THIS machine's HotSpot stub produced, so the C port
 * (core/src/jdk_log.c) must bit-match this dump, not the spec.
 * Pure JDK — no Minecraft classes.
 *
 * Corpus: 65536 uniform (0,1) [the Marsaglia domain], 65536 full-range
 * positive doubles (exponent field 0..2046 uniform), 4096 subnormals,
 * every power of two 2^-1074..2^1023, specials (±0, 1±ulp, ±Inf, NaN
 * payloads, negatives, MIN_VALUE/MAX_VALUE, E, ...), 64 random negatives.
 * All corpus values are warmed up (3 full Math.log passes, checksum
 * printed) before capture so C2-compiled intrinsic output is dumped.
 *
 * Output (tracked): golden/rng/jdk_log.txt
 *   one line per probe: "in=0x%016x log=0x%016x"
 */
public final class JdkLogGolden {
    private static final long MASK52 = (1L << 52) - 1;

    public static void main(String[] args) throws Exception {
        Path outDir = Path.of(args.length > 0 ? args[0] : "golden/rng");
        Files.createDirectories(outDir);
        double[] corpus = buildCorpus();

        // C2 warmup: interpreter and C2 both route Math.log to
        // StubRoutines::dlog, but compile the loop before capture anyway.
        long checksum = 0;
        for (int pass = 0; pass < 3; pass++) {
            for (double v : corpus) {
                checksum ^= Double.doubleToRawLongBits(Math.log(v));
            }
        }

        try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(
                outDir.resolve("jdk_log.txt")))) {
            w.println("# JDK Math.log golden, JDK "
                    + System.getProperty("java.version") + ", os.arch "
                    + System.getProperty("os.arch"));
            w.println("# warmup checksum 0x" + Long.toHexString(checksum));
            for (double v : corpus) {
                w.printf("in=0x%016x log=0x%016x%n",
                        Double.doubleToRawLongBits(v),
                        Double.doubleToRawLongBits(Math.log(v)));
            }
        }
        System.out.println("wrote golden/rng/jdk_log.txt (" + corpus.length
                + " vectors, warmup checksum 0x"
                + Long.toHexString(checksum) + ")");
    }

    private static double[] buildCorpus() {
        ArrayList<Double> c = new ArrayList<>();
        Random rnd = new Random(0x48435f4c4f47L); // "HC_LOG"

        // Marsaglia domain: uniform (0,1)
        for (int i = 0; i < 65536; i++) {
            double v = rnd.nextDouble();
            if (v == 0.0) {
                v = 0.5;
            }
            c.add(v);
        }
        // full positive finite range: exponent field 0..2046 uniform
        for (int i = 0; i < 65536; i++) {
            long e = rnd.nextInt(2047);
            long m = rnd.nextLong() & MASK52;
            long b = (e << 52) | m;
            if (b == 0) {
                b = 1;
            }
            c.add(Double.longBitsToDouble(b));
        }
        // random subnormals
        for (int i = 0; i < 4096; i++) {
            long m = rnd.nextLong() & MASK52;
            if (m == 0) {
                m = 1;
            }
            c.add(Double.longBitsToDouble(m));
        }
        // every power of two
        for (int k = -1074; k <= 1023; k++) {
            c.add(Math.scalb(1.0, k));
        }
        // specials
        double[] specials = {
            0.0, -0.0, 1.0,
            Math.nextUp(1.0), Math.nextDown(1.0),
            Math.nextUp(Math.nextUp(1.0)), Math.nextDown(Math.nextDown(1.0)),
            Double.POSITIVE_INFINITY, Double.NEGATIVE_INFINITY,
            Double.NaN,
            Double.longBitsToDouble(0x7ff0000000000001L), // +SNaN
            Double.longBitsToDouble(0xfff8000000000000L), // -QNaN
            Double.longBitsToDouble(0x7fffffffffffffffL),
            Double.longBitsToDouble(0xfff0000000000001L), // -SNaN
            -1.0, -2.0, -0.5, -Double.MIN_VALUE, -Double.MAX_VALUE,
            -1e300, -1e-300,
            Double.MIN_VALUE, Double.MIN_NORMAL,
            Math.nextDown(Double.MIN_NORMAL), Math.nextUp(Double.MIN_NORMAL),
            Double.MAX_VALUE, Math.nextDown(Double.MAX_VALUE),
            2.0, Math.E, Math.nextUp(Math.E), Math.nextDown(Math.E),
            10.0, 0.1,
        };
        for (double v : specials) {
            c.add(v);
        }
        // random negatives (finite)
        for (int i = 0; i < 64; i++) {
            long e = rnd.nextInt(2047);
            long m = rnd.nextLong() & MASK52;
            c.add(Double.longBitsToDouble(
                    0x8000000000000000L | (e << 52) | m));
        }

        double[] out = new double[c.size()];
        for (int i = 0; i < out.length; i++) {
            out[i] = c.get(i);
        }
        return out;
    }
}
