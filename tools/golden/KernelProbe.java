import java.io.PrintWriter;
import java.lang.reflect.Field;
import java.nio.file.Files;
import java.nio.file.Path;

/**
 * Dump Beardifier.BEARD_KERNEL (13824 floats, raw IEEE-754 bits) from the
 * pinned vanilla server classes. The table is built in <clinit> via
 * (float)Math.pow(E, -((dx)^2+(dy+0.5)^2+(dz)^2)/16) — Math.pow is a JDK
 * intrinsic, so hyperchunk pins the observed bits (core/src/beard_kernel.h)
 * instead of recomputing in C.
 *
 * Output: <outdir>/beard_kernel_bits.txt — one lowercase hex u32 per line,
 * index order [zi*24*24 + xi*24 + yi] (the array's natural order).
 */
public final class KernelProbe {
    public static void main(String[] args) throws Exception {
        // Beardifier implements BeardifierOrMarker → DensityFunction <clinit>
        // touches built-in registries, which demand the vanilla bootstrap.
        net.minecraft.SharedConstants.tryDetectVersion();
        net.minecraft.server.Bootstrap.bootStrap();
        Class<?> c = Class.forName("net.minecraft.world.level.levelgen.Beardifier");
        Field f = c.getDeclaredField("BEARD_KERNEL");
        f.setAccessible(true);
        float[] kernel = (float[]) f.get(null);
        if (kernel.length != 13824)
            throw new IllegalStateException("BEARD_KERNEL length " + kernel.length);
        Path out = Path.of(args[0], "beard_kernel_bits.txt");
        Files.createDirectories(out.getParent());
        try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(out))) {
            for (float v : kernel)
                w.println(Integer.toHexString(Float.floatToRawIntBits(v)));
        }
        System.out.println("wrote " + out + " (" + kernel.length + " entries)");
    }
}
