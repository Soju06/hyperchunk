package dev.hyperchunk.timeline.mixin;

import java.util.concurrent.CompletableFuture;

import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

import dev.hyperchunk.timeline.TimelineLog;
import net.minecraft.server.level.GenerationChunkHolder;
import net.minecraft.util.StaticCache2D;
import net.minecraft.world.level.chunk.ChunkAccess;
import net.minecraft.world.level.chunk.status.ChunkPyramid;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import net.minecraft.world.level.chunk.status.ChunkStep;
import net.minecraft.world.level.chunk.status.WorldGenContext;

/**
 * Same choke point as the golden harness's ChunkStepMixin
 * (tools/golden/stage-dump-mod): {@code ChunkStep.apply} returns the stage's
 * CompletableFuture; we append a continuation that timestamps the completion
 * and hand the composed future back. Vanilla behavior is unchanged when the
 * probe is disabled (no -Dhyperchunk.timeline.file).
 *
 * Unlike the golden mixin we record BOTH pyramids: 'g' (GENERATION_PYRAMID)
 * for freshly generated chunks, 'l' (LOADING_PYRAMID) for chunks re-loaded
 * from disk — boot spawn-prep saves ~144 r.0.0 chunks before t0 (B-6 §3
 * census), and their only post-t0 events are loading-pyramid steps; without
 * 'l' events those chunks would have no servable timestamp at all.
 */
@Mixin(ChunkStep.class)
public abstract class ChunkStepTimelineMixin {

    @Inject(method = "apply", at = @At("RETURN"), cancellable = true)
    private void hyperchunk$timelineAfterStage(WorldGenContext ctx,
            StaticCache2D<GenerationChunkHolder> cache, ChunkAccess chunk,
            CallbackInfoReturnable<CompletableFuture<ChunkAccess>> cir) {
        if (!TimelineLog.enabled()) {
            return;
        }
        ChunkStep self = (ChunkStep) (Object) this;
        ChunkStatus status = self.targetStatus();
        final char kind;
        if (isStepOf(ChunkPyramid.GENERATION_PYRAMID, self, status)) {
            kind = 'g';
        } else if (isStepOf(ChunkPyramid.LOADING_PYRAMID, self, status)) {
            kind = 'l';
        } else {
            return;
        }
        if (!ctx.level().dimension().identifier().toString()
                .equals(TimelineLog.dimension())) {
            return;
        }
        cir.setReturnValue(cir.getReturnValue().thenApply(result -> {
            TimelineLog.record(kind, status, result.getPos().x(), result.getPos().z());
            return result;
        }));
    }

    private static boolean isStepOf(ChunkPyramid pyramid, ChunkStep step, ChunkStatus status) {
        try {
            return pyramid.getStepTo(status) == step;
        } catch (RuntimeException e) {
            return false; // e.g. EMPTY has no step
        }
    }
}
