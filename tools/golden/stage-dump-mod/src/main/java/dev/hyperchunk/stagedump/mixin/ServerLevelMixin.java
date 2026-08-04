package dev.hyperchunk.stagedump.mixin;

import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

import dev.hyperchunk.stagedump.StageDumper;
import net.minecraft.server.level.ServerLevel;

/**
 * -Dhyperchunk.dump.nosave=true: force ServerLevel.noSave from construction.
 *
 * Why a mixin and not /save-off: the damaging save/unload/reload wave of the
 * 2026-07 recordings landed DURING boot spawn-area generation (before the
 * console exists) — reloaded ring chunks carried mid-carve heightmap
 * baselines, the root cause of the unreplayable 09_light residuals
 * (tools/golden/NOTES.md "Recording-lifecycle artifacts"). noSave=true from
 * t=0 suppresses (a) periodic autosave chunk writes
 * (MinecraftServer.saveAllChunks skips levels with noSave && !force), (b) ALL
 * unload processing (ChunkMap.tick: if (!level.noSave()) processUnloads(..)),
 * and (c) eager background saves (invoked only from processUnloads) — i.e.
 * chunks are never saved nor unloaded mid-recording. The harness's final
 * `save-all flush` still works: force=true bypasses noSave. A plain `stop`
 * would NOT save chunks (force=false), so the harness must save explicitly.
 */
@Mixin(ServerLevel.class)
public abstract class ServerLevelMixin {

    @Inject(method = "<init>", at = @At("RETURN"))
    private void hyperchunk$forceNoSave(CallbackInfo ci) {
        if (StageDumper.noSave()) {
            ServerLevel self = (ServerLevel) (Object) this;
            self.noSave = true;
            System.out.println("[hyperchunk-stagedump] noSave forced for "
                    + self.dimension().identifier() + " (autosave+unload suppressed)");
        }
    }
}
