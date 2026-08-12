package lsm;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.util.Random;

/** Throughput of the LSM tree: sequential-ish writes (append to WAL + memtable,
 * flushing to SSTables as it fills) and random reads (served through the Bloom
 * filters and sparse indexes). */
public final class Bench {
    public static void main(String[] args) throws IOException {
        File dir = Files.createTempDirectory("lsm-bench-").toFile();
        int n = 500_000;
        try (LsmTree t = new LsmTree(dir, 4 << 20, 4)) {   // 4 MiB memtable
            long w0 = System.nanoTime();
            for (int i = 0; i < n; i++) t.put(key(i), "value-" + i);
            t.flush();
            double wsec = (System.nanoTime() - w0) / 1e9;
            System.out.printf("  writes: %,d puts in %.2f s = %,.0f puts/s (%d SSTables)%n",
                    n, wsec, n / wsec, t.sstableCount());

            Random rng = new Random(1);
            int reads = 200_000, hits = 0;
            long r0 = System.nanoTime();
            for (int i = 0; i < reads; i++) if (t.getString(key(rng.nextInt(n))) != null) hits++;
            double rsec = (System.nanoTime() - r0) / 1e9;
            System.out.printf("  reads:  %,d random gets in %.2f s = %,.0f gets/s (%d hits)%n",
                    reads, rsec, reads / rsec, hits);

            // negative lookups: keys that were never written — the Bloom filters
            // let most of these skip the SSTables entirely
            int neg = 200_000, found = 0;
            long m0 = System.nanoTime();
            for (int i = 0; i < neg; i++) if (t.getString("absent-" + i) != null) found++;
            double msec = (System.nanoTime() - m0) / 1e9;
            System.out.printf("  misses: %,d absent-key gets in %.2f s = %,.0f gets/s (%d false hits)%n",
                    neg, msec, neg / msec, found);
        }
        deleteTree(dir);
    }

    static String key(int i) { return String.format("key-%08d", i); }

    static void deleteTree(File d) {
        File[] fs = d.listFiles();
        if (fs != null) for (File f : fs) f.delete();
        d.delete();
    }
}
