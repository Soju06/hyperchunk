import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

import net.minecraft.world.level.levelgen.Beardifier;
import net.minecraft.world.level.levelgen.DensityFunction;
import net.minecraft.world.level.levelgen.structure.BoundingBox;
import net.minecraft.world.level.levelgen.structure.TerrainAdjustment;
import net.minecraft.world.level.levelgen.structure.pools.JigsawJunction;
import net.minecraft.world.level.levelgen.structure.pools.StructureTemplatePool;

/**
 * Golden vector for the hyperchunk Beardifier port (core/src/beard.c).
 *
 * Constructs a Beardifier via its @VisibleForTesting constructor with a
 * fixed set of rigids (one per TerrainAdjustment kind) plus junctions,
 * mirroring forStructuresInChunk output shape, and dumps compute() bits
 * over a 3D grid that covers inside/outside/edge of the affected box.
 * The C unit test (tests/unit/test_beard_math.c) rebuilds the identical
 * hc_beard_t and demands bit equality.
 *
 * Output: <outdir>/beard_compute.txt
 *   header lines '# ...', then "x y z bits64hex" per grid point.
 */
public final class BeardProbe {
    public static void main(String[] args) throws Exception {
        net.minecraft.SharedConstants.tryDetectVersion();
        net.minecraft.server.Bootstrap.bootStrap();

        // Rigids: 실제 trial_chambers 피스 규모의 상자들 (encapsulate가
        // 주 대상) + bury/beard_thin/beard_box 각 1개 (경로 핀).
        List<Beardifier.Rigid> rigids = List.of(
            new Beardifier.Rigid(new BoundingBox(0, -28, 0, 30, -10, 18),
                                 TerrainAdjustment.ENCAPSULATE, 1),
            new Beardifier.Rigid(new BoundingBox(-9, -35, 7, 12, -22, 40),
                                 TerrainAdjustment.ENCAPSULATE, -13),
            new Beardifier.Rigid(new BoundingBox(20, -20, -15, 45, 5, 3),
                                 TerrainAdjustment.ENCAPSULATE, 0),
            new Beardifier.Rigid(new BoundingBox(-30, 0, -30, -14, 12, -12),
                                 TerrainAdjustment.BURY, 2),
            new Beardifier.Rigid(new BoundingBox(40, -5, 30, 58, 9, 47),
                                 TerrainAdjustment.BEARD_THIN, 0),
            new Beardifier.Rigid(new BoundingBox(-25, -60, 25, -5, -45, 44),
                                 TerrainAdjustment.BEARD_BOX, 3));
        List<JigsawJunction> junctions = List.of(
            new JigsawJunction(5, -27, 9, 1,
                               StructureTemplatePool.Projection.RIGID),
            new JigsawJunction(-2, -34, 33, 14,
                               StructureTemplatePool.Projection.RIGID),
            new JigsawJunction(31, -19, -4, 0,
                               StructureTemplatePool.Projection.RIGID),
            new JigsawJunction(47, -3, 38, -2,
                               StructureTemplatePool.Projection.RIGID));

        // affectedBox: forStructuresInChunk 규약 — rigid bb ∪ junction
        // 점상자, inflatedBy(24).
        BoundingBox union = null;
        for (Beardifier.Rigid r : rigids)
            union = union == null ? r.box() : BoundingBox.encapsulating(union, r.box());
        for (JigsawJunction j : junctions) {
            BoundingBox jb = new BoundingBox(new net.minecraft.core.BlockPos(
                j.getSourceX(), j.getSourceGroundY(), j.getSourceZ()));
            union = BoundingBox.encapsulating(union, jb);
        }
        BoundingBox affected = union.inflatedBy(24);

        Beardifier b = new Beardifier(rigids, junctions, affected);

        Path out = Path.of(args[0], "beard_compute.txt");
        Files.createDirectories(out.getParent());
        try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(out))) {
            w.printf("# beardifier compute golden (BeardProbe)%n");
            w.printf("# affected %d %d %d %d %d %d%n", affected.minX(),
                     affected.minY(), affected.minZ(), affected.maxX(),
                     affected.maxY(), affected.maxZ());
            for (int x = -70; x <= 100; x += 7)
                for (int y = -95; y <= 45; y += 3)
                    for (int z = -70; z <= 100; z += 7) {
                        double v = b.compute(
                            new DensityFunction.SinglePointContext(x, y, z));
                        w.printf("%d %d %d %016x%n", x, y, z,
                                 Double.doubleToRawLongBits(v));
                    }
        }
        System.out.println("wrote " + out);
    }
}
