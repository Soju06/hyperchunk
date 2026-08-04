package dev.hyperchunk.stagedump.mixin;

import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

import dev.hyperchunk.stagedump.PostProcessLog;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.level.chunk.LevelChunk;

/**
 * Records the postProcessGeneration promotion order + marked positions at
 * HEAD, before vanilla drains and clears the per-section ShortLists. The
 * golden .mca's live tick rows (fluid t=5, sand t=2) and post-promotion block
 * mutations depend on this order — see PostProcessLog.
 */
@Mixin(LevelChunk.class)
public abstract class LevelChunkMixin {

    @Inject(method = "postProcessGeneration", at = @At("HEAD"))
    private void hyperchunk$recordPostProcess(ServerLevel level, CallbackInfo ci) {
        PostProcessLog.record((LevelChunk) (Object) this, level);
    }
}
