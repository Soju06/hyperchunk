import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.IdentityHashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeSet;

import net.minecraft.SharedConstants;
import net.minecraft.core.Holder;
import net.minecraft.core.HolderSet;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.FeatureSorter;
import net.minecraft.world.level.biome.MultiNoiseBiomeSource;
import net.minecraft.world.level.biome.MultiNoiseBiomeSourceParameterLists;
import net.minecraft.world.level.levelgen.GenerationStep;
import net.minecraft.world.level.levelgen.placement.PlacedFeature;

/**
 * Dumps the FeatureSorter per-step ordered placed-feature lists (the
 * setFeatureSeed index space of ChunkGenerator.applyBiomeDecoration) from
 * the vanilla (unobfuscated) 26.2 server jar, for Plan Task 9a.
 *
 * This is EXACTLY the computation ChunkGenerator.<init> memoizes
 * (lambda$new$1 / lambda$new$2, verified in .hermes/notes/task9pre-order/
 * A2-applybiomedecoration.md section 3):
 *
 *   FeatureSorter.buildFeaturesPerStep(
 *       List.copyOf(biomeSource.possibleBiomes()),
 *       b -> b.value().getGenerationSettings().features(), true)
 *
 * with biomeSource = the overworld MultiNoiseBiomeSource, built the way the
 * server builds it: MultiNoiseBiomeSource.createFromPreset(lookup of
 * MultiNoiseBiomeSourceParameterLists.OVERWORLD). Registries come from
 * VanillaRegistries.createLookup() (datapack defaults - same JSON the
 * dedicated server ships). No seed enters this computation.
 *
 * PlacedFeature -> registry id resolution is by REFERENCE IDENTITY against
 * the same lookup's placed_feature registry (Holder.Reference instances are
 * shared between the registry and every biome's HolderSets), never by value.
 *
 * Output file (tracked in git): reference/features_order-26.2.txt
 * Everything else (sanity checks, possibleBiomes order, per-biome and
 * biome-union index sets) goes to stdout for the recon note.
 */
public final class FeatureOrderGolden {

    public static void main(String[] args) throws Exception {
        Path outFile = Path.of(args.length > 0 ? args[0] : "reference/features_order-26.2.txt");
        if (outFile.getParent() != null)
            Files.createDirectories(outFile.getParent());

        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        var lookup = net.minecraft.data.registries.VanillaRegistries.createLookup();

        var preset = lookup.lookupOrThrow(Registries.MULTI_NOISE_BIOME_SOURCE_PARAMETER_LIST)
                .getOrThrow(MultiNoiseBiomeSourceParameterLists.OVERWORLD);
        MultiNoiseBiomeSource source = MultiNoiseBiomeSource.createFromPreset(preset);

        // exactly ChunkGenerator.lambda$new$2
        List<Holder<Biome>> biomes = List.copyOf(source.possibleBiomes());
        List<FeatureSorter.StepFeatureData> steps = FeatureSorter.buildFeaturesPerStep(
                biomes, b -> b.value().getGenerationSettings().features(), true);

        // PlacedFeature instance -> id, identity-keyed, from the SAME lookup
        IdentityHashMap<PlacedFeature, String> ids = new IdentityHashMap<>();
        lookup.lookupOrThrow(Registries.PLACED_FEATURE).listElements()
                .forEach(ref -> ids.put(ref.value(), ref.key().identifier().toString()));

        /* ---- the committed artifact ---- */
        try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(outFile))) {
            w.println("# hyperchunk features order v1");
            w.println("# target_version 26.2");
            w.println("# source FeatureSorter.buildFeaturesPerStep over overworld MultiNoiseBiomeSource possibleBiomes");
            for (int s = 0; s < steps.size(); s++) {
                List<PlacedFeature> list = steps.get(s).features();
                w.println("step " + s + " count " + list.size());
                for (int i = 0; i < list.size(); i++) {
                    String id = ids.get(list.get(i));
                    if (id == null)
                        throw new IllegalStateException("step " + s + " index " + i
                                + ": PlacedFeature instance not found in placed_feature registry"
                                + " by reference - identity assumption broken");
                    w.println(i + " " + id);
                }
            }
        }
        System.out.println("features order written to " + outFile.toAbsolutePath());

        /* ---- stdout: step names / counts ---- */
        GenerationStep.Decoration[] decs = GenerationStep.Decoration.values();
        System.out.println("decoration_steps " + decs.length);
        for (int i = 0; i < decs.length; i++)
            System.out.println("decoration_step " + i + " " + decs[i].name());
        System.out.println("sorter_steps " + steps.size());
        for (int s = 0; s < steps.size(); s++)
            System.out.println("step " + s + " count " + steps.get(s).features().size()
                    + " name " + (s < decs.length ? decs[s].name() : "?"));

        /* ---- sanity (a): every biome's step-k HolderSet maps into the
         *      step-k list via indexMapping, and the mapped slot holds the
         *      IDENTICAL instance ---- */
        int checked = 0;
        for (Holder<Biome> b : biomes) {
            List<HolderSet<PlacedFeature>> perStep = b.value().getGenerationSettings().features();
            for (int s = 0; s < perStep.size(); s++) {
                FeatureSorter.StepFeatureData data = steps.get(s);
                for (Holder<PlacedFeature> h : perStep.get(s)) {
                    PlacedFeature pf = h.value();
                    int idx = data.indexMapping().applyAsInt(pf);
                    if (idx < 0 || idx >= data.features().size())
                        throw new IllegalStateException("SANITY a FAIL: " + biomeId(b)
                                + " step " + s + " feature " + ids.get(pf) + " -> idx " + idx);
                    if (data.features().get(idx) != pf)
                        throw new IllegalStateException("SANITY a FAIL: " + biomeId(b)
                                + " step " + s + " idx " + idx + " not reference-identical");
                    checked++;
                }
            }
        }
        System.out.println("PASS sanity_a all biome step-HolderSet entries map into per-step lists"
                + " (reference-identical), checks=" + checked);

        /* ---- sanity (b)+(c): possibleBiomes count and order ---- */
        System.out.println("possible_biomes_count " + biomes.size());
        for (int i = 0; i < biomes.size(); i++)
            System.out.println("possible_biome " + i + " " + biomeId(biomes.get(i)));
        System.out.println("PASS sanity_b possibleBiomes enumerated, first=" + biomeId(biomes.get(0))
                + " last=" + biomeId(biomes.get(biomes.size() - 1)));

        /* ---- per-biome and biome-union step index sets (mirrors the
         *      applyBiomeDecoration IntSet construction: indexMapping over
         *      each biome's step HolderSet, dedup, ascending sort) ---- */
        var biomeLookup = lookup.lookupOrThrow(Registries.BIOME);
        String[] names = {"minecraft:jungle", "minecraft:lush_caves",
                          "minecraft:beach", "minecraft:river"};
        Map<String, Holder<Biome>> byName = new LinkedHashMap<>();
        for (String n : names) {
            Holder<Biome> h = biomeLookup.getOrThrow(
                    ResourceKey.create(Registries.BIOME, Identifier.parse(n)));
            if (!biomes.contains(h))    // Holder.Reference: identity equals
                throw new IllegalStateException("SANITY FAIL: " + n + " not in possibleBiomes");
            byName.put(n, h);
            System.out.println("possible_biome_pos " + n + " " + biomes.indexOf(h));
        }
        System.out.println("PASS sanity_c jungle/lush_caves/beach/river all in possibleBiomes");

        for (var e : byName.entrySet())
            printIndexSets("biome " + e.getKey(), List.of(e.getValue()), steps);

        List<Holder<Biome>> j_l = List.of(byName.get("minecraft:jungle"),
                byName.get("minecraft:lush_caves"));
        List<Holder<Biome>> j_l_b = List.of(byName.get("minecraft:jungle"),
                byName.get("minecraft:lush_caves"), byName.get("minecraft:beach"));
        List<Holder<Biome>> j_l_r = List.of(byName.get("minecraft:jungle"),
                byName.get("minecraft:lush_caves"), byName.get("minecraft:river"));
        List<Holder<Biome>> j_l_b_r = List.of(byName.get("minecraft:jungle"),
                byName.get("minecraft:lush_caves"), byName.get("minecraft:beach"),
                byName.get("minecraft:river"));
        printIndexSets("union {jungle,lush_caves}", j_l, steps);
        printIndexSets("union {jungle,lush_caves,beach}", j_l_b, steps);
        printIndexSets("union {jungle,lush_caves,river}", j_l_r, steps);
        printIndexSets("union {jungle,lush_caves,beach,river}", j_l_b_r, steps);
    }

    static String biomeId(Holder<Biome> b) {
        return b.unwrapKey().orElseThrow().identifier().toString();
    }

    /** Mirrors applyBiomeDecoration's per-step IntSet + Arrays.sort walk. */
    static void printIndexSets(String tag, List<Holder<Biome>> set,
                               List<FeatureSorter.StepFeatureData> steps) {
        for (int s = 0; s < steps.size(); s++) {
            TreeSet<Integer> indices = new TreeSet<>();
            for (Holder<Biome> b : set) {
                List<HolderSet<PlacedFeature>> perStep = b.value().getGenerationSettings().features();
                if (s >= perStep.size()) continue;
                FeatureSorter.StepFeatureData data = steps.get(s);
                perStep.get(s).stream().map(Holder::value)
                        .forEach(pf -> indices.add(data.indexMapping().applyAsInt(pf)));
            }
            List<Integer> sorted = new ArrayList<>(indices);
            System.out.println("indexset [" + tag + "] step " + s
                    + " n=" + sorted.size() + " " + sorted);
        }
    }
}
