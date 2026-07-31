package dev.hyperchunk.stagedump.mixin;

import java.util.concurrent.CompletableFuture;

import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

import dev.hyperchunk.stagedump.OrderManifest;
import net.minecraft.server.level.GenerationChunkHolder;
import net.minecraft.util.StaticCache2D;
import net.minecraft.world.level.chunk.ChunkAccess;
import net.minecraft.world.level.chunk.status.ChunkStatusTasks;
import net.minecraft.world.level.chunk.status.ChunkStep;
import net.minecraft.world.level.chunk.status.WorldGenContext;

/**
 * Arms/finishes the order-manifest capture around the features stage task.
 * generateFeatures is the sole worldgen decoration path (recon A1) and its
 * body is fully synchronous, so HEAD/RETURN bracket exactly one decoration.
 * The actual manifest line is emitted by {@link WorldgenRandomMixin} at the
 * RNG-determining instant in between.
 */
@Mixin(ChunkStatusTasks.class)
public abstract class ChunkStatusTasksMixin {

    @Inject(method = "generateFeatures", at = @At("HEAD"))
    private static void hyperchunk$armOrderCapture(WorldGenContext ctx, ChunkStep step,
            StaticCache2D<GenerationChunkHolder> cache, ChunkAccess chunk,
            CallbackInfoReturnable<CompletableFuture<ChunkAccess>> cir) {
        OrderManifest.armFeatures(ctx, chunk);
    }

    @Inject(method = "generateFeatures", at = @At("RETURN"))
    private static void hyperchunk$finishOrderCapture(WorldGenContext ctx, ChunkStep step,
            StaticCache2D<GenerationChunkHolder> cache, ChunkAccess chunk,
            CallbackInfoReturnable<CompletableFuture<ChunkAccess>> cir) {
        OrderManifest.finishFeatures(chunk);
    }
}
