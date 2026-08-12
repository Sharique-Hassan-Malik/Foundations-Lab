package lsm;

import java.io.Closeable;
import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.MappedByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

/**
 * An immutable, sorted, on-disk table — the persistent half of the LSM tree.
 *
 * File layout:
 * <pre>
 *   [ data:  entries sorted by key, each  keyLen|key|flag|(valLen|val)  ]
 *   [ index: a SPARSE sample — every Nth key with its byte offset        ]
 *   [ bloom: the serialized Bloom filter over all keys                   ]
 *   [ footer: indexOffset | bloomOffset | count | magic  (fixed 28 B)    ]
 * </pre>
 *
 * A point lookup is cheap: the Bloom filter rejects keys the table can't hold
 * without any disk read; for the rest, the sparse index (kept in memory) gives
 * the nearest earlier key's offset, and the reader seeks there and scans forward
 * a bounded number of entries. The index is sparse on purpose — one entry per N
 * keys — so it costs a fraction of the memory a full index would while keeping
 * lookups to a short linear probe.
 */
public final class SSTable implements Closeable {
    static final int SPARSE = 64;
    static final long MAGIC = 0x4C534D5F53535431L;   // "LSM_SST1"
    static final int FOOTER_BYTES = 8 + 8 + 4 + 8;

    private final File file;
    private final MappedByteBuffer data;   // the whole file, memory-mapped for fast reads
    private final TreeMap<String, Long> sparseIndex = new TreeMap<>();
    private final BloomFilter bloom;
    private final int dataEnd;
    private final int count;

    /** The result of a point lookup: not-here, a value, or a tombstone. */
    public static final class Result {
        public final boolean found;
        public final boolean tombstone;
        public final byte[] value;
        private Result(boolean found, boolean tombstone, byte[] value) {
            this.found = found; this.tombstone = tombstone; this.value = value;
        }
        static Result notFound() { return new Result(false, false, null); }
        static Result tombstone() { return new Result(true, true, null); }
        static Result value(byte[] v) { return new Result(true, false, v); }
    }

    // -------------------------------------------------------------- writing

    /** Write `sorted` (ascending by key) out as a new SSTable. */
    public static void write(File file, Iterator<Entry> sorted, int expectedCount) throws IOException {
        BloomFilter bloom = new BloomFilter(Math.max(64, expectedCount), 0.01);
        TreeMap<String, Long> sparse = new TreeMap<>();
        try (RandomAccessFile raf = new RandomAccessFile(file, "rw")) {
            raf.setLength(0);
            int count = 0;
            while (sorted.hasNext()) {
                Entry e = sorted.next();
                long off = raf.getFilePointer();
                if (count % SPARSE == 0) sparse.put(e.key, off);
                byte[] k = e.key.getBytes(StandardCharsets.UTF_8);
                raf.writeInt(k.length);
                raf.write(k);
                if (e.tombstone) {
                    raf.writeByte(0);
                } else {
                    raf.writeByte(1);
                    raf.writeInt(e.value.length);
                    raf.write(e.value);
                }
                bloom.add(k);
                count++;
            }
            long indexOffset = raf.getFilePointer();
            raf.writeInt(sparse.size());
            for (Map.Entry<String, Long> en : sparse.entrySet()) {
                byte[] k = en.getKey().getBytes(StandardCharsets.UTF_8);
                raf.writeInt(k.length);
                raf.write(k);
                raf.writeLong(en.getValue());
            }
            long bloomOffset = raf.getFilePointer();
            bloom.writeTo(raf);
            raf.writeLong(indexOffset);
            raf.writeLong(bloomOffset);
            raf.writeInt(count);
            raf.writeLong(MAGIC);
        }
    }

    // -------------------------------------------------------------- reading

    public SSTable(File file) throws IOException {
        this.file = file;
        try (RandomAccessFile raf = new RandomAccessFile(file, "r")) {
            long len = raf.length();
            raf.seek(len - FOOTER_BYTES);
            long indexOffset = raf.readLong();
            long bloomOffset = raf.readLong();
            this.count = raf.readInt();
            long magic = raf.readLong();
            if (magic != MAGIC) throw new IOException("not an SSTable (bad magic): " + file);
            this.dataEnd = (int) indexOffset;

            raf.seek(indexOffset);
            int n = raf.readInt();
            for (int i = 0; i < n; i++) {
                int kl = raf.readInt();
                byte[] k = new byte[kl];
                raf.readFully(k);
                long off = raf.readLong();
                sparseIndex.put(new String(k, StandardCharsets.UTF_8), off);
            }
            raf.seek(bloomOffset);
            this.bloom = BloomFilter.readFrom(raf);

            // memory-map the whole file: lookups then read from memory, not via a
            // syscall per field — the difference between ~350 µs and ~1 µs a get
            this.data = raf.getChannel().map(FileChannel.MapMode.READ_ONLY, 0, len);
        }
    }

    public Result get(String key) {
        byte[] k = key.getBytes(StandardCharsets.UTF_8);
        if (!bloom.mightContain(k)) return Result.notFound();       // certain miss, no read at all

        Map.Entry<String, Long> floor = sparseIndex.floorEntry(key);
        int pos = (floor == null) ? 0 : floor.getValue().intValue();
        while (pos < dataEnd) {
            int kl = data.getInt(pos); pos += 4;
            byte[] kk = new byte[kl]; data.get(pos, kk); pos += kl;
            String cur = new String(kk, StandardCharsets.UTF_8);
            byte flag = data.get(pos); pos += 1;
            byte[] val = null;
            if (flag == 1) { int vl = data.getInt(pos); pos += 4; val = new byte[vl]; data.get(pos, val); pos += vl; }
            int cmp = cur.compareTo(key);
            if (cmp == 0) return flag == 1 ? Result.value(val) : Result.tombstone();
            if (cmp > 0) return Result.notFound();                  // passed it: absent (sorted)
        }
        return Result.notFound();
    }

    /** All entries with fromKey ≤ key < toKey, in ascending order. Used by scan. */
    public List<Entry> range(String fromKey, String toKey) {
        List<Entry> out = new ArrayList<>();
        Map.Entry<String, Long> floor = (fromKey == null) ? null : sparseIndex.floorEntry(fromKey);
        int pos = (floor == null) ? 0 : floor.getValue().intValue();
        while (pos < dataEnd) {
            int kl = data.getInt(pos); pos += 4;
            byte[] kk = new byte[kl]; data.get(pos, kk); pos += kl;
            String cur = new String(kk, StandardCharsets.UTF_8);
            byte flag = data.get(pos); pos += 1;
            byte[] val = null;
            boolean tomb = flag != 1;
            if (flag == 1) { int vl = data.getInt(pos); pos += 4; val = new byte[vl]; data.get(pos, val); pos += vl; }
            if (fromKey != null && cur.compareTo(fromKey) < 0) continue;
            if (toKey != null && cur.compareTo(toKey) >= 0) break;
            out.add(new Entry(cur, val, tomb));
        }
        return out;
    }

    public int count() { return count; }
    public File file() { return file; }
    public BloomFilter bloom() { return bloom; }

    @Override
    public void close() { /* the mapping is released when GC'd; the file may be
                             deleted while mapped (POSIX unlink semantics) */ }
}
