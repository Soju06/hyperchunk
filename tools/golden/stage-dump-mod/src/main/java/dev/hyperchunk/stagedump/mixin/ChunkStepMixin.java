package dev.hyperchunk.stagedump.mixin;

import java.util.concurrent.CompletableFuture;

import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

import dev.hyperchunk.stagedump.StageDumper;
import dev.hyperchunk.stagedump.StageLog;
import net.minecraft.server.level.GenerationChunkHolder;
import net.minecraft.util.StaticCache2D;
import net.minecraft.world.level.chunk.ChunkAccess;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import net.minecraft.world.level.chunk.status.ChunkStep;
import net.minecraft.world.level.chunk.status.WorldGenContext;

/**
 * Hooks the single choke point every worldgen stage goes through:
 * {@code ChunkStep.apply} returns the stage's CompletableFuture. We append a
 * continuation that dumps the chunk state after the stage completed but
 * before any dependent stage can observe completion, then hand the composed
 * future back to vanilla. No vanilla behavior changes when the harness is
 * disabled (no -Dhyperchunk.dump.dir).
 *
 * 26.2 is fully unobfuscated, so targets use Mojang names directly
 * (ADR-006 D3) — no mapping layer.
 */
@Mixin(ChunkStep.class)
public abstract class ChunkStepMixin {

    @Inject(method = "apply", at = @At("RETURN"), cancellable = true)
    private void hyperchunk$dumpAfterStage(WorldGenContext ctx,
            StaticCache2D<GenerationChunkHolder> cache, ChunkAccess chunk,
            CallbackInfoReturnable<CompletableFuture<ChunkAccess>> cir) {
        if (!StageDumper.enabled()) {
            return;
        }
        ChunkStep self = (ChunkStep) (Object) this;
        ChunkStatus status = self.targetStatus();
        if (!StageDumper.isGenerationStep(self, status)
                || !ctx.level().dimension().identifier().toString()
                        .equals(StageDumper.dimension())) {
            return;
        }
        // stages.log records EVERY chunk's stage completions (ring light
        // timing is replay input — see StageLog); the full dump additionally
        // runs for grid chunks only.
        boolean wantsDump = StageDumper.wants(ctx, chunk.getPos(), status);
        cir.setReturnValue(cir.getReturnValue().thenApply(result -> {
            StageLog.record(status.getIndex(), StageDumper.stageName(status),
                    result.getPos());
            if (wantsDump) {
                StageDumper.dump(ctx, status, result);
            }
            return result;
        }));
    }
}
