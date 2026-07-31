import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Path;

/**
 * Dumps JDK Math.sin/Math.cos bit vectors over the exact input domain
 * OreFeature.place feeds them (Task 9a): angle = nextFloat()*(float)PI
 * widened to double, i.e. (double)((k * 0x1p-24f) * 3.1415927f) for
 * k in [0, 2^24). java.lang.Math.sin/cos only has a 1-ulp/semi-monotonic
 * SPEC; the value the golden dumps actually contain is whatever THIS
 * machine's HotSpot stub produced, so the C port must bit-match this
 * dump (glibc sin/cos), not the spec. Pure JDK — no Minecraft classes.
 *
 * Output (tracked): golden/rng/jdk_sincos.txt
 *   one line per probe: "k <k> in=0x%016x sin=0x%016x cos=0x%016x"
 *   k sampled on a prime stride over [0, 2^24) plus edge values.
 */
public final class JdkSinCosGolden {
    public static void main(String[] args) throws Exception {
        Path outDir = Path.of(args.length > 0 ? args[0] : "golden/rng");
        Files.createDirectories(outDir);
        try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(
                outDir.resolve("jdk_sincos.txt")))) {
            w.println("# JDK Math.sin/cos over ore-angle domain, JDK "
                    + System.getProperty("java.version") + ", os.arch "
                    + System.getProperty("os.arch"));
            w.println("# angle = (double)((k * 0x1p-24f) * 3.1415927f)");
            final int N = 1 << 24;
            final int STRIDE = 4099; // prime => 4093 samples, full-range coverage
            emit(w, 0);
            emit(w, 1);
            emit(w, N - 1);
            emit(w, N / 2);     // ~pi/2
            emit(w, N / 4);     // ~pi/4
            emit(w, 3 * (N / 4));
            for (int k = STRIDE; k < N; k += STRIDE) {
                emit(w, k);
            }
        }
        System.out.println("wrote golden/rng/jdk_sincos.txt");
    }

    private static void emit(PrintWriter w, int k) {
        double angle = (double) ((k * 0x1p-24f) * 3.1415927f);
        w.printf("k %d in=0x%016x sin=0x%016x cos=0x%016x%n", k,
                Double.doubleToRawLongBits(angle),
                Double.doubleToRawLongBits(Math.sin(angle)),
                Double.doubleToRawLongBits(Math.cos(angle)));
    }
}
