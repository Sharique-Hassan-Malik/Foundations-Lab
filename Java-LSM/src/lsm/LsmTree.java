package lsm;

import java.io.Closeable;
import java.io.File;
import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

/**
 * A log-structured merge-tree key-value store — the design under LevelDB,
 * RocksDB, Cassandra and friends.
 *
 * Writes are fast and sequential: append to the write-ahead log for durability,
 * then update the in-memory sorted memtable. When the memtable fills, it is
 * flushed in one pass to an immutable, sorted SSTable on disk and the log is
 * truncated. Reads check the memtable first, then each SSTable newest-to-oldest,
 * using a per-table Bloom filter to skip tables that can't hold the key and a
 * sparse index to seek near it. Deletes are tombstones that shadow older values
 * until compaction — which merges SSTables, keeps the newest version of each
 * key, and drops the tombstones — reclaims the space.
 *
 * Durability is the headline: because every write hits the log before it is
 * acknowledged, a crash with an unflushed memtable loses nothing — reopening the
 * tree replays the log and every acknowledged write comes back.
 */
public final class LsmTree implements Closeable {
    private final File dir;
    private final long memtableLimitBytes;
    private final int compactionThreshold;

    private MemTable memtable = new MemTable();
    private WriteAheadLog wal;
    private final List<SSTable> sstables = new ArrayList<>();  // index 0 = newest
    private int maxGen = 0;

    public LsmTree(File dir) throws IOException { this(dir, 1 << 20, 4); }

    public LsmTree(File dir, long memtableLimitBytes, int compactionThreshold) throws IOException {
        this.dir = dir;
        this.memtableLimitBytes = memtableLimitBytes;
        this.compactionThreshold = compactionThreshold;
        if (!dir.exists() && !dir.mkdirs()) throw new IOException("cannot create " + dir);
        openExistingSSTables();
        this.wal = new WriteAheadLog(walFile());
        recoverFromLog();
    }

    // -------------------------------------------------------------- writes

    public void put(String key, String value) {
        put(key, value.getBytes(StandardCharsets.UTF_8));
    }

    public synchronized void put(String key, byte[] value) {
        try {
            wal.append(key, value, false);
            memtable.put(key, value);
            maybeFlush();
        } catch (IOException e) {
            throw new UncheckedIOException(e);
        }
    }

    public synchronized void delete(String key) {
        try {
            wal.append(key, null, true);
            memtable.delete(key);
            maybeFlush();
        } catch (IOException e) {
            throw new UncheckedIOException(e);
        }
    }

    // -------------------------------------------------------------- reads

    /** The value for key, or null if absent or deleted. */
    public synchronized byte[] get(String key) {
        if (memtable.contains(key)) return memtable.get(key);   // null here == tombstone
        for (SSTable sst : sstables) {                          // newest first
            SSTable.Result r = sst.get(key);
            if (r.found) return r.tombstone ? null : r.value;
        }
        return null;
    }

    public String getString(String key) {
        byte[] v = get(key);
        return v == null ? null : new String(v, StandardCharsets.UTF_8);
    }

    /** Ordered scan of keys in [from, to). Either bound may be null for open-ended.
     * Deletes are honoured — tombstoned keys do not appear. */
    public synchronized List<Entry> scan(String from, String to) {
        TreeMap<String, Entry> merged = new TreeMap<>();
        // oldest first, so newer sources overwrite older ones
        for (int i = sstables.size() - 1; i >= 0; i--)
            for (Entry e : sstables.get(i).range(from, to)) merged.put(e.key, e);
        for (Map.Entry<String, byte[]> e : memtable.entries().entrySet()) {
            String k = e.getKey();
            if (from != null && k.compareTo(from) < 0) continue;
            if (to != null && k.compareTo(to) >= 0) continue;
            merged.put(k, new Entry(k, e.getValue(), e.getValue() == null));
        }
        List<Entry> out = new ArrayList<>();
        for (Entry e : merged.values()) if (!e.tombstone) out.add(e);
        return out;
    }

    // -------------------------------------------------------- flush & compact

    private void maybeFlush() throws IOException {
        if (memtable.approxBytes() >= memtableLimitBytes) flush();
    }

    /** Persist the current memtable as a new SSTable and truncate the log. */
    public synchronized void flush() throws IOException {
        if (memtable.isEmpty()) return;
        int gen = ++maxGen;
        File f = sstFile(gen);
        Iterator<Entry> it = memtable.entries().entrySet().stream()
                .map(e -> new Entry(e.getKey(), e.getValue(), e.getValue() == null))
                .iterator();
        SSTable.write(f, it, memtable.size());
        sstables.add(0, new SSTable(f));            // newest at the front
        memtable.clear();
        wal.truncate();
        maybeCompact();
    }

    private void maybeCompact() throws IOException {
        if (sstables.size() > compactionThreshold) compact();
    }

    /** Merge every SSTable into one: keep the newest version of each key and drop
     * tombstones (safe in a full compaction, since nothing older remains). */
    public synchronized void compact() throws IOException {
        if (sstables.size() <= 1) return;
        TreeMap<String, Entry> merged = new TreeMap<>();
        for (int i = sstables.size() - 1; i >= 0; i--)         // oldest → newest
            for (Entry e : sstables.get(i).range(null, null)) merged.put(e.key, e);

        int gen = ++maxGen;
        File f = sstFile(gen);
        Iterator<Entry> live = merged.values().stream().filter(e -> !e.tombstone).iterator();
        SSTable.write(f, live, merged.size());

        List<SSTable> old = new ArrayList<>(sstables);
        sstables.clear();
        sstables.add(new SSTable(f));
        for (SSTable s : old) {
            File sf = s.file();
            s.close();
            sf.delete();
        }
    }

    // -------------------------------------------------------- open / recover

    private void openExistingSSTables() throws IOException {
        File[] files = dir.listFiles((d, n) -> n.startsWith("sstable-") && n.endsWith(".db"));
        if (files == null) return;
        Arrays.sort(files);                                    // ascending gen (= oldest first)
        for (File f : files) {
            int gen = Integer.parseInt(f.getName().substring("sstable-".length(),
                    f.getName().length() - ".db".length()));
            maxGen = Math.max(maxGen, gen);
            sstables.add(0, new SSTable(f));                   // newest ends up at index 0
        }
    }

    private void recoverFromLog() throws IOException {
        WriteAheadLog.replay(walFile(), (key, value, tombstone) -> {
            if (tombstone) memtable.delete(key);
            else memtable.put(key, value);
        });
    }

    // -------------------------------------------------------- lifecycle

    /** Clean shutdown: flush the memtable to disk, then close everything. */
    @Override
    public synchronized void close() throws IOException {
        flush();
        wal.close();
        for (SSTable s : sstables) s.close();
    }

    /** Simulate a crash for tests: drop the in-memory memtable WITHOUT flushing,
     * closing only the file handles. The write-ahead log on disk is the only
     * record of the unflushed writes — reopening the tree must recover them. */
    public synchronized void simulateCrash() throws IOException {
        wal.close();
        for (SSTable s : sstables) s.close();
        memtable = new MemTable();
    }

    // -------------------------------------------------------- introspection

    public synchronized int sstableCount() { return sstables.size(); }
    public synchronized int memtableSize() { return memtable.size(); }

    private File walFile() { return new File(dir, "wal.log"); }
    private File sstFile(int gen) { return new File(dir, String.format("sstable-%08d.db", gen)); }
}
