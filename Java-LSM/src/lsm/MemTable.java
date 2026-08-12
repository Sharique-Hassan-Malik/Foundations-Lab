package lsm;

import java.nio.charset.StandardCharsets;
import java.util.Map;
import java.util.TreeMap;

/**
 * The in-memory write buffer: a sorted map of the most recent writes. A value of
 * {@code null} in the map is a tombstone (a recorded delete), which is distinct
 * from a key being absent — that difference is what lets a delete in the memtable
 * correctly shadow an older value sitting in an SSTable.
 *
 * Keeping it sorted (a {@link TreeMap}) is what makes flushing to an SSTable a
 * single linear pass, and makes range scans a simple merge.
 */
public final class MemTable {
    private final TreeMap<String, byte[]> map = new TreeMap<>();
    private long approxBytes = 0;

    public void put(String key, byte[] value) { update(key, value); }
    public void delete(String key) { update(key, null); }

    private void update(String key, byte[] value) {
        boolean existed = map.containsKey(key);
        byte[] old = map.put(key, value);
        long oldSize = existed ? (key.getBytes(StandardCharsets.UTF_8).length
                                  + (old == null ? 0 : old.length) + 24) : 0;
        long newSize = key.getBytes(StandardCharsets.UTF_8).length
                       + (value == null ? 0 : value.length) + 24;
        approxBytes += newSize - oldSize;
    }

    /** True iff the key has a record here (value or tombstone). */
    public boolean contains(String key) { return map.containsKey(key); }

    /** The stored value, or null for a tombstone; use {@link #contains} to tell a
     * tombstone from an absent key. */
    public byte[] get(String key) { return map.get(key); }

    public TreeMap<String, byte[]> entries() { return map; }
    public long approxBytes() { return approxBytes; }
    public int size() { return map.size(); }
    public boolean isEmpty() { return map.isEmpty(); }

    public void clear() {
        map.clear();
        approxBytes = 0;
    }

    public static boolean isTombstone(Map.Entry<String, byte[]> e) { return e.getValue() == null; }
}
