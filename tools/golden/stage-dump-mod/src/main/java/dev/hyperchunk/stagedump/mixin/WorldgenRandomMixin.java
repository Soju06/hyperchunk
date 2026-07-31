package dev.hyperchunk.stagedump.mixin;

import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

import dev.hyperchunk.stagedump.FeatureTrace;
import dev.hyperchunk.stagedump.OrderManifest;
import net.minecraft.world.level.levelgen.WorldgenRandom;

/**
 * Captures the decoration seed actually computed for a chunk's features
 * application — the RNG-determining instant of the features stage (recon A0
 * §2). Gated by the per-thread arm from {@link ChunkStatusTasksMixin}:
 * setDecorationSeed's other caller (spawnOriginalMobs, SPAWN stage) never
 * fires while armed, and the block-origin cross-check in
 * {@link OrderManifest#recordDecorationSeed} rejects any mismatch.
 */
@Mixin(WorldgenRandom.class)
public abstract class WorldgenRandomMixin {

    @Inject(method = "setDecorationSeed(JII)J", at = @At("RETURN"))
    private void hyperchunk$recordDecorationSeed(long levelSeed, int minBlockX, int minBlockZ,
            CallbackInfoReturnable<Long> cir) {
        OrderManifest.recordDecorationSeed(minBlockX, minBlockZ, cir.getReturnValueJ());
        if (FeatureTrace.enabled()) {
            FeatureTrace.onDecorationSeed(minBlockX, minBlockZ, cir.getReturnValueJ());
        }
    }

    /**
     * The (step, index) salt of the item about to run — pairs the following
     * top-level placement calls for {@link FeatureTrace}. Fires for both the
     * structure loop and the placed-feature loop of applyBiomeDecoration.
     */
    @Inject(method = "setFeatureSeed(JII)V", at = @At("HEAD"))
    private void hyperchunk$recordFeatureSeed(long decorationSeed, int index, int step,
            CallbackInfo ci) {
        if (FeatureTrace.enabled()) {
            FeatureTrace.onFeatureSeed(index, step);
        }
    }
}
