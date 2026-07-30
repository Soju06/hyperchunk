import java.io.PrintWriter;
import java.lang.reflect.Field;
import java.nio.file.Files;
import java.nio.file.Path;

import net.minecraft.util.Mth;

/**
 * Dumps the Mth SIN table (65536 floats) and sin/cos probe values from the
 * vanilla (unobfuscated) 26.2 server jar, for Plan Task 8 (carvers).
 *
 * Why: Mth.<clinit> builds the table as (float) Math.sin((double) i /
 * 10430.378350470453). java.lang.Math.sin has a 1-ulp spec (HotSpot stub),
 * so the C port must prove its libm produces the SAME 65536 floats after
 * the d2f rounding (task8 note A7 §4.1 / OPEN). Seed-independent.
 *
 * Output (tracked in git): golden/rng/mth_sin_table.txt
 *   section sin_table 65536   - one "i bits=0x%08x" line per entry
 *   section sin_probes / cos_probes - Mth.sin/cos(double) at angles that
 *     exercise the d2l truncation-toward-zero index math (negative angles,
 *     sub-1/SCALE magnitudes) plus carver-typical angles.
 */
public final class MthGolden {
    public static void main(String[] args) throws Exception {
        Path outDir = Path.of(args.length > 0 ? args[0] : "golden/rng");
        Files.createDirectories(outDir);
        try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(
                outDir.resolve("mth_sin_table.txt")))) {
            Field f = Mth.class.getDeclaredField("SIN");
            f.setAccessible(true);
            float[] tab = (float[]) f.get(null);
            w.println("# Mth.SIN table, 26.2 server, JDK " + System.getProperty("java.version"));
            w.println("section sin_table " + tab.length);
            for (int i = 0; i < tab.length; i++)
                w.printf("%d bits=0x%08x%n", i, Float.floatToRawIntBits(tab[i]));

            double[] probes = {
                0.0, -0.0, 1.0, -1.0, Math.PI, -Math.PI,
                6.2831854820251465, // (double) Mth.TWO_PI (float 6.2831855f widened)
                -6.2831854820251465,
                3.1415927410125732, // (double) Mth.PI
                1.5707963705062866, // (double) Mth.HALF_PI
                0.5, -0.5, 2.0, -2.0, 100.0, -100.0,
                4.789e-5, -4.789e-5, // |x*SCALE| < 1 -> index 0 vs trunc-to-zero
                9.587379924285257e-5, -9.587379924285257e-5, // ~1/SCALE
                0.7853981633974483, -0.7853981633974483,
            };
            w.println("section sin_probes " + probes.length);
            for (double a : probes)
                w.printf("%s -> bits=0x%08x%n",
                        String.format("0x%016x", Double.doubleToRawLongBits(a)),
                        Float.floatToRawIntBits(Mth.sin(a)));
            w.println("section cos_probes " + probes.length);
            for (double a : probes)
                w.printf("%s -> bits=0x%08x%n",
                        String.format("0x%016x", Double.doubleToRawLongBits(a)),
                        Float.floatToRawIntBits(Mth.cos(a)));
        }
        System.out.println("golden Mth vectors written to " + outDir.toAbsolutePath());
    }
}
