package dev.hyperchunk.stagedump.mixin;

import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

import dev.hyperchunk.stagedump.FeatureTrace;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.StructureManager;
import net.minecraft.world.level.WorldGenLevel;
import net.minecraft.world.level.chunk.ChunkGenerator;
import net.minecraft.world.level.levelgen.structure.BoundingBox;
import net.minecraft.world.level.levelgen.structure.StructureStart;

/**
 * Evidence line for every structure placement during decoration. The Task 9
 * grid is expected to have none — an s-line in a trace falsifies that and
 * fails the harness loudly. Inert unless -Dhyperchunk.dump.trace=true.
 */
@Mixin(StructureStart.class)
public abstract class StructureStartMixin {

    @Inject(method = "placeInChunk", at = @At("HEAD"))
    private void hyperchunk$traceStructure(WorldGenLevel level, StructureManager structureManager,
            ChunkGenerator generator, RandomSource random, BoundingBox box, ChunkPos pos,
            CallbackInfo ci) {
        if (FeatureTrace.enabled()) {
            FeatureTrace.onStructurePlace(((StructureStart) (Object) this).getStructure(), level);
        }
    }
}
