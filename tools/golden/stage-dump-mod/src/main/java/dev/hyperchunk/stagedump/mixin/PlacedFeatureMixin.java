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
import net.minecraft.world.level.levelgen.placement.PlacedFeature;

/**
 * Brackets each decoration item (one placeWithBiomeCheck call from
 * ChunkGenerator#applyBiomeDecoration) for {@link FeatureTrace}: the RETURN
 * emits the per-item f-line (position count + placed flag + registry id).
 * Depth==0 gating in FeatureTrace excludes any nested use. Inert unless
 * -Dhyperchunk.dump.trace=true.
 */
@Mixin(PlacedFeature.class)
public abstract class PlacedFeatureMixin {

    @Inject(method = "placeWithBiomeCheck", at = @At("HEAD"))
    private void hyperchunk$traceItemHead(WorldGenLevel level, ChunkGenerator generator,
            RandomSource random, BlockPos origin, CallbackInfoReturnable<Boolean> cir) {
        if (FeatureTrace.enabled()) {
            FeatureTrace.onPlacedFeatureHead();
        }
    }

    @Inject(method = "placeWithBiomeCheck", at = @At("RETURN"))
    private void hyperchunk$traceItemReturn(WorldGenLevel level, ChunkGenerator generator,
            RandomSource random, BlockPos origin, CallbackInfoReturnable<Boolean> cir) {
        if (FeatureTrace.enabled()) {
            FeatureTrace.onPlacedFeatureReturn((PlacedFeature) (Object) this, level,
                    cir.getReturnValueZ());
        }
    }
}
