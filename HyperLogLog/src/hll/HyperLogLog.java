package hll;

/**
 * HyperLogLog: estimate how many <em>distinct</em> items a stream contained,
 * using a fixed few kilobytes no matter how many items there were.
 *
 * <h2>The idea in one paragraph</h2>
 *
 * Hash each item to a uniform 64-bit value. In a stream of many distinct items,
 * the rarest event you expect to see is a hash beginning with a long run of
 * leading zeros — a run of {@code k} zeros happens about once per {@code 2^k}
 * distinct items, so the longest run observed is a (very noisy) estimate of
 * {@code log2(cardinality)}. HyperLogLog tames the noise by splitting the stream
 * into {@code m = 2^precision} buckets (using the first few hash bits), tracking
 * the longest zero-run in each, and combining the buckets with a harmonic mean.
 * The result is a cardinality estimate whose relative error is about
 * {@code 1.04 / sqrt(m)} — and whose memory is just {@code m} one-byte registers,
 * <em>independent of the cardinality</em>.
 *
 * <h2>Why it matters</h2>
 *
 * Counting distinct values exactly means remembering every value: a
 * {@code HashSet} of ten million longs is hundreds of megabytes. HyperLogLog
 * answers the same question to within a percent or two from a fixed 16 KB, and —
 * the property that makes it indispensable in distributed systems — two sketches
 * <em>merge</em> by taking the element-wise maximum of their registers. The
 * union of two sets' cardinalities is recovered without ever shipping the sets,
 * and without double-counting their overlap, which naive per-shard counting
 * cannot do. (This is the same mergeability argument that makes cumulative
 * histograms the right shape in {@code inference-observability}.)
 */
public final class HyperLogLog {
    private final int precision; // p: register count is 2^p
    private final int registerCount; // m
    private final byte[] registers; // each holds a small "longest zero-run + 1"
    private final double alpha; // bias constant, depends on m

    /**
     * @param precision {@code p} in 4..18. Registers = {@code 2^p} bytes, and the
     *                  standard error is about {@code 1.04 / sqrt(2^p)}: p=14 gives
     *                  16 KB and ~0.8% error, p=10 gives 1 KB and ~3.25%.
     */
    public HyperLogLog(int precision) {
        if (precision < 4 || precision > 18) {
            throw new IllegalArgumentException("precision must be in 4..18, got " + precision);
        }
        this.precision = precision;
        this.registerCount = 1 << precision;
        this.registers = new byte[registerCount];
        this.alpha = alphaFor(registerCount);
    }

    // -- adding --------------------------------------------------------------

    public void add(long item) {
        addHash(mix64(item));
    }

    public void add(String item) {
        addHash(hashString(item));
    }

    public void add(byte[] item) {
        addHash(hashBytes(item));
    }

    /**
     * The core update. The top {@code p} bits of the hash pick a register; the
     * remaining bits contribute their leading-zero run. Each register keeps only
     * the maximum run it has seen, so adding the same item twice is idempotent —
     * which is exactly why the structure counts <em>distinct</em> items.
     */
    private void addHash(long hash) {
        int index = (int) (hash >>> (64 - precision));
        // The remaining (64 - p) bits, shifted to the top so leading-zero count
        // measures the run directly. If they are all zero, the run is the maximum
        // possible, 64 - p + 1.
        long remaining = hash << precision;
        int rank = remaining == 0 ? (64 - precision + 1) : Long.numberOfLeadingZeros(remaining) + 1;
        if (rank > registers[index]) {
            registers[index] = (byte) rank;
        }
    }

    // -- estimating ----------------------------------------------------------

    /**
     * The estimated number of distinct items added.
     *
     * The raw estimator is the harmonic-mean formula. It is biased for small
     * cardinalities — when many registers are still zero — so in that regime the
     * method switches to <em>linear counting</em>, which estimates cardinality
     * from the fraction of empty registers and is far more accurate there. The
     * large-range correction from the original paper is unnecessary here because
     * a 64-bit hash makes collisions vanishingly unlikely below billions of
     * items.
     */
    public long estimate() {
        double sum = 0.0;
        int zeros = 0;
        for (byte register : registers) {
            sum += 1.0 / (1L << register); // 2^-register
            if (register == 0) {
                zeros++;
            }
        }

        double estimate = alpha * registerCount * registerCount / sum;

        // Small-range correction: harmonic mean is unreliable when the sketch is
        // sparse, so below ~2.5m use linear counting on the empty registers.
        if (estimate <= 2.5 * registerCount && zeros != 0) {
            estimate = registerCount * Math.log((double) registerCount / zeros);
        }
        return Math.round(estimate);
    }

    // -- merging -------------------------------------------------------------

    /**
     * Merge {@code other} into this sketch: register-wise maximum.
     *
     * This is the whole reason to use a sketch instead of counting exactly. The
     * maximum zero-run seen for a bucket across the union of two streams is the
     * maximum of the two individual maxima, so after this call {@code estimate()}
     * gives the cardinality of the <em>union</em> — with the overlap counted
     * once, automatically. Ten shards each keep a 16 KB sketch and the
     * coordinator merges them; no element ever leaves its shard.
     */
    public void merge(HyperLogLog other) {
        if (other.precision != this.precision) {
            throw new IllegalArgumentException(
                "cannot merge sketches of different precision (" + this.precision + " vs " + other.precision + ")");
        }
        for (int i = 0; i < registerCount; i++) {
            if (other.registers[i] > this.registers[i]) {
                this.registers[i] = other.registers[i];
            }
        }
    }

    /** A merged copy of {@code a} and {@code b}, leaving both untouched. */
    public static HyperLogLog union(HyperLogLog a, HyperLogLog b) {
        HyperLogLog result = new HyperLogLog(a.precision);
        result.merge(a);
        result.merge(b);
        return result;
    }

    // -- introspection -------------------------------------------------------

    /** Bytes of state, independent of how many items were added. */
    public int memoryBytes() {
        return registerCount;
    }

    public int precision() {
        return precision;
    }

    /** The theoretical relative standard error, {@code 1.04 / sqrt(m)}. */
    public double standardError() {
        return 1.04 / Math.sqrt(registerCount);
    }

    // -- hashing -------------------------------------------------------------

    private static double alphaFor(int m) {
        return switch (m) {
            case 16 -> 0.673;
            case 32 -> 0.697;
            case 64 -> 0.709;
            default -> 0.7213 / (1.0 + 1.079 / m);
        };
    }

    /**
     * A 64-bit finalizer (SplitMix64's mixing step). HyperLogLog assumes hashes
     * are uniformly distributed; a weak hash puts that assumption — and the whole
     * error bound — at risk, so this scrambles even sequential inputs thoroughly.
     */
    static long mix64(long z) {
        z += 0x9E3779B97F4A7C15L;
        z = (z ^ (z >>> 30)) * 0xBF58476D1CE4E5B9L;
        z = (z ^ (z >>> 27)) * 0x94D049BB133111EBL;
        return z ^ (z >>> 31);
    }

    static long hashBytes(byte[] data) {
        // FNV-1a to fold the bytes, then the strong finalizer to scramble.
        long h = 0xCBF29CE484222325L;
        for (byte b : data) {
            h ^= (b & 0xFF);
            h *= 0x100000001B3L;
        }
        return mix64(h);
    }

    static long hashString(String s) {
        return hashBytes(s.getBytes(java.nio.charset.StandardCharsets.UTF_8));
    }
}
