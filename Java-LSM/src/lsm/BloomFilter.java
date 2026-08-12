package lsm;

import java.io.DataInput;
import java.io.DataOutput;
import java.io.IOException;

/**
 * A Bloom filter: a compact bit array that answers "is this key in the set?"
 * with either "definitely not" or "probably yes". It never gives a false
 * negative, so an LSM tree can use one per SSTable to skip, without touching
 * disk, any table that certainly does not hold a key — turning most negative
 * lookups into a handful of memory probes.
 *
 * The bit count m and hash count k are chosen from the expected number of keys
 * and a target false-positive rate; the k hash positions are produced by the
 * standard double-hashing trick (h1 + i·h2) over two 64-bit hashes of the key,
 * which behaves like k independent hashes without computing k of them.
 */
public final class BloomFilter {
    private final long[] bits;
    private final long numBits;
    private final int numHashes;

    public BloomFilter(int expectedKeys, double falsePositiveRate) {
        long m = optimalBits(Math.max(1, expectedKeys), falsePositiveRate);
        this.numBits = Math.max(64, m);
        this.bits = new long[(int) ((numBits + 63) / 64)];
        this.numHashes = optimalHashes(Math.max(1, expectedKeys), numBits);
    }

    private BloomFilter(long numBits, int numHashes, long[] bits) {
        this.numBits = numBits;
        this.numHashes = numHashes;
        this.bits = bits;
    }

    static long optimalBits(long n, double p) {
        return (long) Math.ceil(-n * Math.log(p) / (Math.log(2) * Math.log(2)));
    }

    static int optimalHashes(long n, long m) {
        return Math.max(1, (int) Math.round((double) m / n * Math.log(2)));
    }

    public void add(byte[] key) {
        long h1 = hash(key, 0x9E3779B97F4A7C15L);
        long h2 = hash(key, 0xC2B2AE3D27D4EB4FL);
        for (int i = 0; i < numHashes; i++) {
            long bit = Math.floorMod(h1 + (long) i * h2, numBits);
            bits[(int) (bit >>> 6)] |= (1L << (bit & 63));
        }
    }

    public boolean mightContain(byte[] key) {
        long h1 = hash(key, 0x9E3779B97F4A7C15L);
        long h2 = hash(key, 0xC2B2AE3D27D4EB4FL);
        for (int i = 0; i < numHashes; i++) {
            long bit = Math.floorMod(h1 + (long) i * h2, numBits);
            if ((bits[(int) (bit >>> 6)] & (1L << (bit & 63))) == 0) return false;
        }
        return true;
    }

    /** A 64-bit FNV-1a-style hash seeded so two calls give two independent hashes. */
    private static long hash(byte[] key, long seed) {
        long h = seed ^ 0xCBF29CE484222325L;
        for (byte b : key) {
            h ^= (b & 0xFF);
            h *= 0x100000001B3L;
        }
        h ^= (h >>> 33);
        h *= 0xFF51AFD7ED558CCDL;
        h ^= (h >>> 33);
        return h;
    }

    public void writeTo(DataOutput out) throws IOException {
        out.writeLong(numBits);
        out.writeInt(numHashes);
        out.writeInt(bits.length);
        for (long word : bits) out.writeLong(word);
    }

    public static BloomFilter readFrom(DataInput in) throws IOException {
        long numBits = in.readLong();
        int numHashes = in.readInt();
        int len = in.readInt();
        long[] bits = new long[len];
        for (int i = 0; i < len; i++) bits[i] = in.readLong();
        return new BloomFilter(numBits, numHashes, bits);
    }

    public long bitCount() { return numBits; }
    public int hashCount() { return numHashes; }
}
