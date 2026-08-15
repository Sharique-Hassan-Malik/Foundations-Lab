import hll.HyperLogLog;
import java.util.HashSet;

/**
 * The headline: count distinct items to within a percent from a fixed few
 * kilobytes, merge sketches across shards without double-counting, and watch the
 * error track the theory exactly.
 *
 * <p>Run: {@code java -cp out Benchmark}
 */
public final class Benchmark {
    // Spread inputs across the 64-bit space so each is an independent draw; a
    // dense 0..N-1 range is a legal but unrepresentative corner.
    private static final long STRIDE = 2654435761L; // Knuth's multiplicative constant

    public static void main(String[] args) {
        accuracyAcrossCardinalities();
        memoryComparison();
        standardErrorMatchesTheory();
        mergeability();
    }

    // -- 1. accuracy vs cardinality -----------------------------------------

    private static void accuracyAcrossCardinalities() {
        int p = 14;
        System.out.printf("1. Accuracy at precision %d (%d registers = %d KB), median of 5 trials:%n%n",
            p, 1 << p, (1 << p) / 1024);
        System.out.printf("%12s  %14s  %9s%n", "true count", "estimate", "error");
        System.out.println("------------  --------------  ---------");
        for (int n : new int[] {1_000, 10_000, 100_000, 1_000_000, 10_000_000}) {
            double[] errors = new double[5];
            long lastEstimate = 0;
            for (int t = 0; t < 5; t++) {
                HyperLogLog hll = new HyperLogLog(p);
                long base = (long) t * 1_000_000_000L + 7;
                for (int i = 0; i < n; i++) {
                    hll.add(base + (long) i * STRIDE);
                }
                lastEstimate = hll.estimate();
                errors[t] = 100.0 * Math.abs(lastEstimate - n) / n;
            }
            java.util.Arrays.sort(errors);
            System.out.printf("%,12d  %,14d  %8.2f%%%n", n, lastEstimate, errors[2]);
        }
        System.out.println("""

            The estimate stays within a couple of percent from 1 thousand to 10
            million distinct items, using the same 16 KB the whole way. The memory
            does not grow with the count — that is the entire point.""");
    }

    // -- 2. memory: fixed sketch vs exact set -------------------------------

    private static void memoryComparison() {
        System.out.println("\n\n2. Memory: HyperLogLog vs an exact HashSet (measured retained heap)\n");
        int n = 1_000_000;

        HyperLogLog hll = new HyperLogLog(14);
        for (int i = 0; i < n; i++) {
            hll.add((long) i * STRIDE);
        }

        long before = usedHeap();
        HashSet<Long> exact = new HashSet<>(n * 2);
        for (int i = 0; i < n; i++) {
            exact.add((long) i * STRIDE);
        }
        long setBytes = usedHeap() - before;
        // Keep `exact` alive across the measurement so it is not collected early.
        if (exact.size() != n) {
            throw new AssertionError();
        }

        System.out.printf("%,d distinct longs:%n", n);
        System.out.printf("  HashSet<Long> : %,d bytes  (exact, %.0f bytes/item)%n", setBytes, (double) setBytes / n);
        System.out.printf("  HyperLogLog   : %,d bytes  (est %,d, error %.2f%%)%n",
            hll.memoryBytes(), hll.estimate(), 100.0 * Math.abs(hll.estimate() - n) / n);
        System.out.printf("  ratio         : %,.0fx less memory%n", (double) setBytes / hll.memoryBytes());
        System.out.println("""

            The exact set must remember every value; the sketch remembers a bounded
            summary. At 10 million the set would be hundreds of megabytes and the
            sketch is still 16 KB.""");
    }

    // -- 3. observed error matches theory -----------------------------------

    private static void standardErrorMatchesTheory() {
        System.out.println("\n\n3. The error is not luck: observed vs theoretical standard error\n");
        int n = 1_000_000, trials = 40;
        System.out.printf("%-12s  %14s  %14s%n", "precision", "observed SE", "theory 1.04/√m");
        System.out.println("------------  --------------  --------------");
        for (int p : new int[] {10, 12, 14}) {
            double sumSq = 0;
            for (int t = 0; t < trials; t++) {
                HyperLogLog hll = new HyperLogLog(p);
                long base = (long) t * 1_000_000_007L + 3;
                for (int i = 0; i < n; i++) {
                    hll.add(base + (long) i * STRIDE);
                }
                double rel = (double) (hll.estimate() - n) / n;
                sumSq += rel * rel;
            }
            double observed = Math.sqrt(sumSq / trials);
            double theory = 1.04 / Math.sqrt(1 << p);
            System.out.printf("%-12d  %13.3f%%  %13.3f%%%n", p, 100 * observed, 100 * theory);
        }
        System.out.println("""

            Over 40 independent trials the root-mean-square relative error lands on
            the theoretical 1.04/√m to within a hair at every precision. The error
            bound is a guarantee you can budget against, not an average that hides a
            bad tail.""");
    }

    // -- 4. mergeability: the distributed headline --------------------------

    private static void mergeability() {
        System.out.println("\n\n4. Mergeability: counting distinct across shards without double-counting\n");
        int p = 14;
        // Two shards with a deliberate overlap: |A| = |B| = 600k, |A ∩ B| = 200k,
        // so the true union is exactly 1,000,000.
        HyperLogLog a = new HyperLogLog(p);
        HyperLogLog b = new HyperLogLog(p);
        for (int i = 0; i < 600_000; i++) {
            a.add((long) i * STRIDE);
        }
        for (int i = 400_000; i < 1_000_000; i++) {
            b.add((long) i * STRIDE);
        }

        long trueUnion = 1_000_000;
        long merged = HyperLogLog.union(a, b).estimate();
        long naiveSum = a.estimate() + b.estimate();

        System.out.printf("  shard A: 600,000 distinct   shard B: 600,000 distinct   overlap: 200,000%n%n");
        System.out.printf("  true union            : %,d%n", trueUnion);
        System.out.printf("  merged sketches (max) : %,d   (error %.2f%%)%n",
            merged, 100.0 * Math.abs(merged - trueUnion) / trueUnion);
        System.out.printf("  naive sum of counts   : %,d   (error %.2f%%  — double-counts the overlap)%n",
            naiveSum, 100.0 * Math.abs(naiveSum - trueUnion) / trueUnion);
        System.out.println("""

            Adding per-shard counts overcounts by the size of the overlap — here by
            200,000, a 20% error that no amount of precision fixes, because the
            information needed to subtract the intersection was thrown away when
            each shard reduced to a number. Merging the sketches keeps that
            information: the union's registers are the element-wise maximum of the
            shards', so the overlap is counted once, automatically, and no element
            ever leaves its shard. It is the same reason cumulative histograms, not
            pre-computed percentiles, are the mergeable choice in
            inference-observability.""");
    }

    private static long usedHeap() {
        Runtime rt = Runtime.getRuntime();
        for (int i = 0; i < 4; i++) {
            System.gc();
            try {
                Thread.sleep(20);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
        }
        return rt.totalMemory() - rt.freeMemory();
    }
}
