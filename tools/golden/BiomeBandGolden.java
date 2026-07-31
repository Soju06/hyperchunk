import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

import net.minecraft.SharedConstants;
import net.minecraft.core.registries.Registries;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.levelgen.NoiseGeneratorSettings;
import net.minecraft.world.level.levelgen.RandomState;

/**
 * Dumps the raw quart biome grid for chunks [-6..6]^2 (quarts [-24..27]^2,
 * qy [-16..79]) from the vanilla (unobfuscated) 26.2 server jar, for Plan
 * Task 10 (light): the 08/09 stage gates replay ring-chunk decoration up to
 * chessboard radius 4, whose surface/features stages need biomes beyond the
 * radius-2 coverage of golden 03_biomes dumps + the surface golden's
 * quart_biomes section (chunks [-2..2]^2).
 *
 * Exactly the same sampling path as NoiseGolden.dumpSurface's quart_biomes
 * section (and stage 03's stored palette):
 *   MultiNoiseBiomeSource.createFromPreset(OVERWORLD).getNoiseBiome(qx,qy,qz,
 *       RandomState.create(overworld_settings, noises, SEED).sampler())
 *
 * Biome *generation* (multi-noise climate sampler + parameter tree) remains
 * a future C-side task; this file is a pure-function-of-seed golden input,
 * same category as the other golden/rng probes.
 *
 * Coherence gate: if the surface golden (arg 2) is given, the overlap region
 * (qx,qz in [-8..11]) must match its quart_biomes section name-for-name, or
 * this tool exits nonzero and writes nothing.
 *
 * Output (tracked in git): golden/rng/biome_band_seed<seed>.txt
 */
public final class BiomeBandGolden {
    static final long SEED = Long.parseLong(System.getProperty("hyperchunk.seed", "1234567890"));
    static final int QMIN = -24, QMAX = 27;      // chunks -6..6
    static final int QYMIN = -16, QYMAX = 79;    // 26.2 overworld quart y

    public static void main(String[] args) throws Exception {
        Path out = Path.of(args.length > 0 ? args[0]
                : "golden/rng/biome_band_seed" + SEED + ".txt");
        Path surfaceGolden = args.length > 1 ? Path.of(args[1]) : null;

        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();

        var lookup = net.minecraft.data.registries.VanillaRegistries.createLookup();
        NoiseGeneratorSettings settings = lookup.lookupOrThrow(Registries.NOISE_SETTINGS)
                .getOrThrow(NoiseGeneratorSettings.OVERWORLD).value();
        RandomState rs = RandomState.create(settings,
                lookup.lookupOrThrow(Registries.NOISE), SEED);
        var preset = lookup.lookupOrThrow(Registries.MULTI_NOISE_BIOME_SOURCE_PARAMETER_LIST)
                .getOrThrow(net.minecraft.world.level.biome.MultiNoiseBiomeSourceParameterLists.OVERWORLD);
        var source = net.minecraft.world.level.biome.MultiNoiseBiomeSource.createFromPreset(preset);
        var sampler = rs.sampler();

        int nxz = QMAX - QMIN + 1;
        String[][][] names = new String[QYMAX - QYMIN + 1][nxz][nxz];
        for (int qy = QYMIN; qy <= QYMAX; qy++)
            for (int qz = QMIN; qz <= QMAX; qz++)
                for (int qx = QMIN; qx <= QMAX; qx++)
                    names[qy - QYMIN][qz - QMIN][qx - QMIN] =
                            source.getNoiseBiome(qx, qy, qz, sampler)
                                    .unwrapKey().orElseThrow().identifier().toString();

        if (surfaceGolden != null)
            checkOverlap(surfaceGolden, names);

        List<String> pal = new ArrayList<>();
        try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(out))) {
            w.println("# MC 26.2 raw quart biome band, seed " + SEED
                    + " (Plan Task 10 ring replay input)");
            w.println("seed " + SEED);
            StringBuilder data = new StringBuilder();
            for (int qy = QYMIN; qy <= QYMAX; qy++) {
                data.append("qy ").append(qy).append('\n');
                for (int qz = QMIN; qz <= QMAX; qz++) {
                    for (int qx = QMIN; qx <= QMAX; qx++) {
                        String name = names[qy - QYMIN][qz - QMIN][qx - QMIN];
                        int idx = pal.indexOf(name);
                        if (idx < 0) { idx = pal.size(); pal.add(name); }
                        data.append(idx);
                        data.append(qx == QMAX ? '\n' : ' ');
                    }
                }
            }
            w.println("section quart_biomes qx " + QMIN + " " + QMAX
                    + " qy " + QYMIN + " " + QYMAX
                    + " qz " + QMIN + " " + QMAX);
            for (int i = 0; i < pal.size(); i++)
                w.println("palette " + i + " " + pal.get(i));
            w.print(data);
        }
        System.out.println("biome band: " + (QYMAX - QYMIN + 1) * nxz * nxz
                + " quarts, " + pal.size() + " distinct biomes -> " + out);
    }

    /** Parse the surface golden's quart_biomes section and assert the
     *  overlap (qx,qz in [-8..11], all qy) matches name-for-name. */
    static void checkOverlap(Path surfaceGolden, String[][][] names) throws Exception {
        List<String> pal = new ArrayList<>();
        int mismatches = 0, checked = 0;
        int qy = Integer.MIN_VALUE, qz = 0;
        boolean inSection = false;
        for (String line : Files.readAllLines(surfaceGolden)) {
            if (line.startsWith("section quart_biomes")) { inSection = true; continue; }
            if (line.startsWith("section ")) { inSection = false; continue; }
            if (!inSection) continue;
            if (line.startsWith("palette ")) {
                String[] t = line.split(" ", 3);
                if (Integer.parseInt(t[1]) != pal.size())
                    throw new IllegalStateException("palette out of order: " + line);
                pal.add(t[2]);
            } else if (line.startsWith("qy ")) {
                qy = Integer.parseInt(line.substring(3).trim());
                qz = -8;
            } else if (qy != Integer.MIN_VALUE && !line.isBlank()) {
                String[] t = line.trim().split("\\s+");
                if (t.length != 20)
                    throw new IllegalStateException("bad quart row: " + line);
                for (int i = 0; i < 20; i++) {
                    int qx = -8 + i;
                    String want = pal.get(Integer.parseInt(t[i]));
                    String got = names[qy - QYMIN][qz - QMIN][qx - QMIN];
                    if (!want.equals(got)) {
                        mismatches++;
                        if (mismatches <= 10)
                            System.err.println("OVERLAP MISMATCH quart (" + qx
                                    + "," + qy + "," + qz + "): band=" + got
                                    + " surface_golden=" + want);
                    }
                    checked++;
                }
                qz++;
            }
        }
        if (checked != 96 * 20 * 20)
            throw new IllegalStateException("surface golden overlap incomplete: " + checked);
        if (mismatches != 0)
            throw new IllegalStateException(mismatches + " overlap mismatches");
        System.out.println("overlap gate: " + checked + " quarts match surface golden");
    }
}
