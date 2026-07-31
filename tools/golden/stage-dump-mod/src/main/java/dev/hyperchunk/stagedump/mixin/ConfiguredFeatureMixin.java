package dev.hyperchunk.stagedump.mixin;

import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

import dev.hyperchunk.stagedump.FeatureTrace;
import net.minecraft.core.BlockPos;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.WorldGenLevel;
import net.minecraft.world.level.chunk.ChunkGenerator;
import net.minecraft.world.level.levelgen.feature.ConfiguredFeature;

/**
 * Depth-tracks ConfiguredFeature#place so {@link FeatureTrace} records only
 * TOP-LEVEL calls — the positions that survived a placed feature's
 * placement-modifier pipeline — and not the nested placements composite
 * features perform internally. Inert unless -Dhyperchunk.dump.trace=true.
 */
@Mixin(ConfiguredFeature.class)
public abstract class ConfiguredFeatureMixin {

    @Inject(method = "place", at = @At("HEAD"))
    private void hyperchunk$tracePlaceHead(WorldGenLevel level, ChunkGenerator generator,
            RandomSource random, BlockPos pos, CallbackInfoReturnable<Boolean> cir) {
        if (FeatureTrace.enabled()) {
            FeatureTrace.onPlaceHead();
        }
    }

    @Inject(method = "place", at = @At("RETURN"))
    private void hyperchunk$tracePlaceReturn(WorldGenLevel level, ChunkGenerator generator,
            RandomSource random, BlockPos pos, CallbackInfoReturnable<Boolean> cir) {
        if (FeatureTrace.enabled()) {
            FeatureTrace.onPlaceReturn(pos, cir.getReturnValueZ());
        }
    }
}
