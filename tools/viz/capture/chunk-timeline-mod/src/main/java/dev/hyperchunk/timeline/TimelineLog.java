package dev.hyperchunk.timeline;

import java.io.BufferedWriter;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Locale;
import java.util.concurrent.ConcurrentLinkedQueue;

import net.minecraft.world.level.chunk.status.ChunkStatus;

/**
 * Per-chunk worldgen stage completion timestamps for the VIZ-5 demo
 * timelines. Unlike the golden harness's StageLog (eager per-line flush under
 * one lock — deliberate there, the order manifest needs durable ordering),
 * this logger is built for the overhead gate: the hot path is one
 * System.nanoTime() + one small allocation + one lock-free queue append. No
 * disk I/O, no lock, until a single flush from a JVM shutdown hook.
 *
 * Configuration (JVM system properties):
 *   hyperchunk.timeline.file       output TSV path; absent => mod completely inert
 *   hyperchunk.timeline.dimension  dimension id to record (default minecraft:overworld)
 *
 * Clock mapping: event timestamps are System.nanoTime(). The header records
 * two (epochMillis, nanoTime) reference pairs — one at first use, one at
 * flush — so the converter can place events on the driver's epoch clock
 * (runner t0 = `date +%s.%N`). Drift between the two clocks over a ~12 s run
 * is far below the 1 ms epoch granularity; the second pair exists to verify
 * that.
 */
public final class TimelineLog {
    private static final String PROP_PREFIX = "hyperchunk.timeline.";

    private static final Path FILE;
    private static final String DIMENSION;
    private static final long REF_NANO;
    private static final long REF_EPOCH_MS;
    private static final ConcurrentLinkedQueue<Event> EVENTS = new ConcurrentLinkedQueue<>();

    /** kind: 'g' = generation-pyramid step, 'l' = loading-pyramid step. */
    private record Event(long nano, char kind, ChunkStatus status, int cx, int cz) {}

    static {
        String file = System.getProperty(PROP_PREFIX + "file");
        FILE = file == null ? null : Path.of(file);
        DIMENSION = System.getProperty(PROP_PREFIX + "dimension", "minecraft:overworld");
        REF_NANO = System.nanoTime();
        REF_EPOCH_MS = System.currentTimeMillis();
        if (FILE != null) {
            Runtime.getRuntime().addShutdownHook(
                    new Thread(TimelineLog::flush, "hyperchunk-timeline-flush"));
            System.out.println("[hyperchunk-chunk-timeline] enabled: file=" + FILE
                    + " dimension=" + DIMENSION);
        }
    }

    private TimelineLog() {}

    public static boolean enabled() {
        return FILE != null;
    }

    public static String dimension() {
        return DIMENSION;
    }

    /** Hot path — called once per (chunk, stage) completion. */
    public static void record(char kind, ChunkStatus status, int cx, int cz) {
        EVENTS.add(new Event(System.nanoTime(), kind, status, cx, cz));
    }

    private static String stageName(ChunkStatus status) {
        String name = status.getName();
        return name.startsWith("minecraft:") ? name.substring("minecraft:".length()) : name;
    }

    private static void flush() {
        long flushNano = System.nanoTime();
        long flushEpochMs = System.currentTimeMillis();
        try {
            if (FILE.getParent() != null) {
                Files.createDirectories(FILE.getParent());
            }
            try (BufferedWriter w = Files.newBufferedWriter(FILE, StandardCharsets.UTF_8)) {
                w.write("# hyperchunk chunk-timeline v1");
                w.newLine();
                w.write("# ref epoch_ms=" + REF_EPOCH_MS + " nano=" + REF_NANO);
                w.newLine();
                w.write("# flush epoch_ms=" + flushEpochMs + " nano=" + flushNano);
                w.newLine();
                w.write("# event: kind stageIndex stageName cx cz nano");
                w.newLine();
                for (Event e : EVENTS) {
                    w.write(String.format(Locale.ROOT, "%c\t%02d\t%s\t%d\t%d\t%d",
                            e.kind, e.status.getIndex(), stageName(e.status),
                            e.cx, e.cz, e.nano));
                    w.newLine();
                }
            }
            System.out.println("[hyperchunk-chunk-timeline] flushed "
                    + EVENTS.size() + " events to " + FILE);
        } catch (IOException e) {
            System.out.println("[hyperchunk-chunk-timeline] ERROR flushing " + FILE + ": " + e);
            e.printStackTrace();
        }
    }
}
