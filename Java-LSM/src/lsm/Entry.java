package lsm;

/** One key/value record. A {@code tombstone} marks a deletion — a real record
 * that must be written and merged like any other, so that a delete can shadow an
 * older value living in a lower SSTable. */
public final class Entry {
    public final String key;
    public final byte[] value;      // null iff tombstone
    public final boolean tombstone;

    public Entry(String key, byte[] value, boolean tombstone) {
        this.key = key;
        this.value = value;
        this.tombstone = tombstone;
    }

    public static Entry value(String key, byte[] value) { return new Entry(key, value, false); }
    public static Entry deleted(String key) { return new Entry(key, null, true); }
}
