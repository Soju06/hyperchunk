package dev.hyperchunk.stagedump.mixin;

import java.util.function.Function;

import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

import dev.hyperchunk.stagedump.FeatureTrace;
import net.minecraft.core.BlockPos;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.levelgen.feature.OreFeature;
import net.minecraft.world.level.levelgen.feature.configurations.OreConfiguration;

/**
 * DEBUG-ONLY candidate log (-Dhyperchunk.dump.oretrace=true on top of
 * trace mode): one o-line per canPlaceOre call — the definitive ground
 * truth for bisecting ore-blob write sets against the C replay. Not part
 * of the canonical trace format; the flag is never set by the bundle
 * harnesses.
 */
@Mixin(OreFeature.class)
public abstract class OreFeatureMixin {

    @Inject(method = "canPlaceOre", at = @At("RETURN"))
    private static void hyperchunk$traceCandidate(BlockState state,
            Function<BlockPos, BlockState> getter, RandomSource random,
            OreConfiguration cfg, OreConfiguration.TargetBlockState target,
            BlockPos.MutableBlockPos pos, CallbackInfoReturnable<Boolean> cir) {
        if (FeatureTrace.enabled()) {
            FeatureTrace.onOreCandidate(pos, state, cir.getReturnValueZ());
        }
    }
}
