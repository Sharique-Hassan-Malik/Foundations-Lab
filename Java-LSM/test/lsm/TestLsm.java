package lsm;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.util.List;
import java.util.Map;
import java.util.Random;
import java.util.TreeMap;

/**
 * Tests for the LSM tree, as a plain runner (exit 0 on success) so no test
 * framework is needed. The headline is crash recovery: writes acknowledged
 * before a simulated crash are recovered in full from the write-ahead log.
 */
public final class TestLsm {
    static int failures = 0;

    static void check(boolean cond, String msg) {
        System.out.println((cond ? "  ok:   " : "  FAIL: ") + msg);
        if (!cond) failures++;
    }

    static File freshDir(String name) throws IOException {
        File d = Files.createTempDirectory("lsm-" + name + "-").toFile();
        return d;
    }

    static void deleteTree(File d) {
        File[] fs = d.listFiles();
        if (fs != null) for (File f : fs) f.delete();
        d.delete();
    }

    // -------------------------------------------------------- basics

    static void testBasics() throws IOException {
        System.out.println("basics: put / get / overwrite / delete");
        File dir = freshDir("basics");
        try (LsmTree t = new LsmTree(dir)) {
            t.put("a", "1");
            t.put("b", "2");
            check("1".equals(t.getString("a")), "get returns the value");
            check(t.getString("missing") == null, "absent key returns null");
            t.put("a", "11");
            check("11".equals(t.getString("a")), "overwrite wins");
            t.delete("b");
            check(t.getString("b") == null, "deleted key returns null");
        }
        deleteTree(dir);
    }

    // -------------------------------------------------------- vs reference

    static void testAgainstReference() throws IOException {
        System.out.println("randomised: matches an in-memory reference across flushes & compactions");
        File dir = freshDir("ref");
        TreeMap<String, String> ref = new TreeMap<>();
        // small memtable + low compaction threshold so both fire during the run
        try (LsmTree t = new LsmTree(dir, 4096, 3)) {
            Random rng = new Random(12345);
            for (int op = 0; op < 60000; op++) {
                String key = "k" + rng.nextInt(2000);
                if (rng.nextInt(4) != 0) {
                    String val = "v" + rng.nextInt(1_000_000);
                    t.put(key, val);
                    ref.put(key, val);
                } else {
                    t.delete(key);
                    ref.remove(key);
                }
            }
            // every key in the space agrees with the reference
            boolean pointOk = true;
            for (int k = 0; k < 2000; k++) {
                String key = "k" + k;
                String a = t.getString(key), b = ref.get(key);
                if (a == null ? b != null : !a.equals(b)) { pointOk = false; break; }
            }
            check(pointOk, "every point lookup matches the reference");

            // a full ordered scan equals the reference map, in order
            List<Entry> scan = t.scan(null, null);
            boolean scanOk = scan.size() == ref.size();
            if (scanOk) {
                int i = 0;
                for (Map.Entry<String, String> e : ref.entrySet()) {
                    Entry got = scan.get(i++);
                    if (!got.key.equals(e.getKey())
                            || !new String(got.value).equals(e.getValue())) { scanOk = false; break; }
                }
            }
            check(scanOk, "full ordered scan matches the reference, in key order");

            // a bounded range scan
            List<Entry> r = t.scan("k100", "k200");
            boolean rangeOk = true;
            for (Entry e : r) if (e.key.compareTo("k100") < 0 || e.key.compareTo("k200") >= 0) rangeOk = false;
            check(rangeOk, "range scan stays within [from, to)");
            check(t.sstableCount() >= 1, "data really was flushed to SSTables (count " + t.sstableCount() + ")");
        }
        deleteTree(dir);
    }

    // -------------------------------------------------------- persistence

    static void testPersistenceAcrossReopen() throws IOException {
        System.out.println("persistence: data survives a clean close and reopen");
        File dir = freshDir("persist");
        try (LsmTree t = new LsmTree(dir)) {
            for (int i = 0; i < 5000; i++) t.put("key" + i, "value" + i);
            t.flush();
        }  // close() flushes + closes
        try (LsmTree t = new LsmTree(dir)) {
            boolean ok = true;
            for (int i = 0; i < 5000; i++) if (!("value" + i).equals(t.getString("key" + i))) { ok = false; break; }
            check(ok, "all values readable from SSTables after reopen");
            check(t.sstableCount() >= 1, "reopened tree loaded its SSTables");
        }
        deleteTree(dir);
    }

    // -------------------------------------------------------- crash recovery

    static void testCrashRecovery() throws IOException {
        System.out.println("crash recovery (the headline): unflushed writes survive a crash");
        File dir = freshDir("crash");
        // huge memtable limit so nothing auto-flushes: everything lives only in the
        // memtable and the WAL when we crash
        LsmTree t = new LsmTree(dir, Long.MAX_VALUE, 1000);
        for (int i = 0; i < 3000; i++) t.put("key" + i, "value" + i);
        for (int i = 0; i < 500; i++) t.delete("key" + i);      // also crash-test deletes
        check(t.sstableCount() == 0, "nothing was flushed yet (0 SSTables)");
        t.simulateCrash();                                       // memtable lost; WAL on disk

        try (LsmTree recovered = new LsmTree(dir, Long.MAX_VALUE, 1000)) {
            boolean valuesOk = true, deletesOk = true;
            for (int i = 500; i < 3000; i++)
                if (!("value" + i).equals(recovered.getString("key" + i))) valuesOk = false;
            for (int i = 0; i < 500; i++)
                if (recovered.getString("key" + i) != null) deletesOk = false;
            check(valuesOk, "every un-flushed put recovered from the WAL");
            check(deletesOk, "every un-flushed delete recovered (keys stay gone)");
        }
        deleteTree(dir);
    }

    // -------------------------------------------------------- compaction

    static void testCompaction() throws IOException {
        System.out.println("compaction: SSTables merge, newest wins, tombstones dropped");
        File dir = freshDir("compact");
        try (LsmTree t = new LsmTree(dir, 2048, 3)) {
            for (int i = 0; i < 8000; i++) t.put("k" + i, "first" + i);
            for (int i = 0; i < 4000; i++) t.put("k" + i, "second" + i);   // overwrite half
            for (int i = 0; i < 1000; i++) t.delete("k" + i);              // delete a chunk
            t.flush();
            check(t.sstableCount() <= 4, "compaction kept the SSTable count bounded (" + t.sstableCount() + ")");
            boolean ok = true;
            for (int i = 0; i < 1000; i++) if (t.getString("k" + i) != null) ok = false;      // deleted
            for (int i = 1000; i < 4000; i++) if (!("second" + i).equals(t.getString("k" + i))) ok = false; // overwritten
            for (int i = 4000; i < 8000; i++) if (!("first" + i).equals(t.getString("k" + i))) ok = false;  // original
            check(ok, "after compaction: deletes gone, overwrites win, originals intact");
        }
        deleteTree(dir);
    }

    // -------------------------------------------------------- bloom filter

    static void testBloomFilter() {
        System.out.println("bloom filter: false-positive rate near the 1% target");
        BloomFilter bf = new BloomFilter(10000, 0.01);
        for (int i = 0; i < 10000; i++) bf.add(("present" + i).getBytes());
        boolean noFalseNeg = true;
        for (int i = 0; i < 10000; i++) if (!bf.mightContain(("present" + i).getBytes())) noFalseNeg = false;
        check(noFalseNeg, "no false negatives (every added key reported present)");
        int fp = 0, trials = 100000;
        for (int i = 0; i < trials; i++) if (bf.mightContain(("absent" + i).getBytes())) fp++;
        double rate = (double) fp / trials;
        check(rate < 0.03, String.format("false-positive rate %.3f%% is near the 1%% target", rate * 100));
    }

    public static void main(String[] args) throws IOException {
        testBasics();
        testAgainstReference();
        testPersistenceAcrossReopen();
        testCrashRecovery();
        testCompaction();
        testBloomFilter();
        if (failures == 0) System.out.println("\nALL TESTS PASSED");
        else System.out.println("\n" + failures + " TEST(S) FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
