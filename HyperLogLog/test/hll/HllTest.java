package hll;

/**
 * Tests plus a zero-dependency runner — same approach as the regex engine, so
 * the whole project builds and checks with nothing but {@code javac} and
 * {@code java}.
 *
 * HyperLogLog is deterministic (no randomness inside), so a fixed set of inputs
 * always produces the same estimate. That lets accuracy be asserted with real
 * tolerances rather than statistical hedging: the numbers below are reproducible.
 */
public final class HllTest {

    private static final long STRIDE = 2654435761L;

    // -- construction --------------------------------------------------------

    void testPrecisionBounds() {
        throwsIAE(() -> new HyperLogLog(3));
        throwsIAE(() -> new HyperLogLog(19));
        new HyperLogLog(4);   // ok
        new HyperLogLog(18);  // ok
    }

    void testMemoryIsTwoToThePrecision() {
        eq(new HyperLogLog(10).memoryBytes(), 1024);
        eq(new HyperLogLog(14).memoryBytes(), 16384);
        // Memory is fixed at construction and never grows with items added.
        HyperLogLog h = new HyperLogLog(12);
        for (int i = 0; i < 5_000_000; i++) {
            h.add((long) i * STRIDE);
        }
        eq(h.memoryBytes(), 4096);
    }

    void testStandardErrorFormula() {
        approx(new HyperLogLog(14).standardError(), 1.04 / Math.sqrt(16384), 1e-9);
    }

    // -- estimation ----------------------------------------------------------

    void testEmptyEstimatesZero() {
        eq(new HyperLogLog(14).estimate(), 0L);
    }

    void testSmallCountsAreNearlyExact() {
        // Linear counting makes the small regime very accurate.
        HyperLogLog h = new HyperLogLog(14);
        for (int i = 0; i < 100; i++) {
            h.add((long) i * STRIDE);
        }
        long est = h.estimate();
        assertTrue(Math.abs(est - 100) <= 2, "100 distinct estimated as " + est);
    }

    void testIdempotentOnDuplicates() {
        HyperLogLog h = new HyperLogLog(14);
        for (int i = 0; i < 10_000; i++) {
            h.add((long) i * STRIDE);
        }
        long once = h.estimate();
        // Add every item again, many times: distinct count must not move.
        for (int pass = 0; pass < 5; pass++) {
            for (int i = 0; i < 10_000; i++) {
                h.add((long) i * STRIDE);
            }
        }
        eq(h.estimate(), once);
    }

    void testAccuracyWithinBound() {
        // At p=14 the standard error is ~0.81%. A fixed input set gives a fixed
        // estimate; assert it is comfortably within a few standard errors.
        for (int n : new int[] {10_000, 100_000, 1_000_000}) {
            HyperLogLog h = new HyperLogLog(14);
            for (int i = 0; i < n; i++) {
                h.add((long) i * STRIDE + 7);
            }
            double error = (double) Math.abs(h.estimate() - n) / n;
            assertTrue(error < 0.03, "n=" + n + " error " + (error * 100) + "% exceeded 3%");
        }
    }

    void testHigherPrecisionIsMoreAccurate() {
        int n = 500_000;
        double errLow = relativeError(new HyperLogLog(8), n);
        double errHigh = relativeError(new HyperLogLog(16), n);
        assertTrue(errHigh < errLow, "p=16 (" + errHigh + ") should beat p=8 (" + errLow + ")");
    }

    // -- merging -------------------------------------------------------------

    void testMergeIsUnion() {
        HyperLogLog a = new HyperLogLog(14);
        HyperLogLog b = new HyperLogLog(14);
        for (int i = 0; i < 600_000; i++) {
            a.add((long) i * STRIDE);
        }
        for (int i = 400_000; i < 1_000_000; i++) {
            b.add((long) i * STRIDE);
        }
        long union = HyperLogLog.union(a, b).estimate();     // true = 1,000,000
        double error = Math.abs(union - 1_000_000) / 1_000_000.0;
        assertTrue(error < 0.03, "union estimate " + union + " off by " + (error * 100) + "%");
    }

    void testMergeDoesNotDoubleCountOverlap() {
        // The whole point: merged union must be far closer to the truth than the
        // naive sum, which counts the overlap twice.
        HyperLogLog a = new HyperLogLog(14);
        HyperLogLog b = new HyperLogLog(14);
        for (int i = 0; i < 600_000; i++) {
            a.add((long) i * STRIDE);
        }
        for (int i = 400_000; i < 1_000_000; i++) {
            b.add((long) i * STRIDE);
        }
        long merged = HyperLogLog.union(a, b).estimate();
        long naiveSum = a.estimate() + b.estimate();
        assertTrue(Math.abs(merged - 1_000_000) < Math.abs(naiveSum - 1_000_000) / 10,
            "merged (" + merged + ") should be far better than naive sum (" + naiveSum + ")");
    }

    void testMergeIsCommutative() {
        HyperLogLog a = filled(14, 0, 300_000);
        HyperLogLog b = filled(14, 200_000, 500_000);
        eq(HyperLogLog.union(a, b).estimate(), HyperLogLog.union(b, a).estimate());
    }

    void testMergingIdenticalSketchesIsIdentity() {
        HyperLogLog a = filled(14, 0, 400_000);
        HyperLogLog b = filled(14, 0, 400_000);   // same items
        long merged = HyperLogLog.union(a, b).estimate();
        assertTrue(Math.abs(merged - a.estimate()) <= 1, "union of equal sets changed the estimate");
    }

    void testMergeDifferentPrecisionThrows() {
        throwsIAE(() -> new HyperLogLog(12).merge(new HyperLogLog(14)));
    }

    // -- hashing -------------------------------------------------------------

    void testHashIsDeterministic() {
        eq(HyperLogLog.mix64(42L), HyperLogLog.mix64(42L));
        eq(HyperLogLog.hashString("hello"), HyperLogLog.hashString("hello"));
    }

    void testTypesAgreeThroughBytes() {
        // add(String) and add(byte[]) of the same UTF-8 bytes are the same event.
        HyperLogLog h = new HyperLogLog(14);
        h.add("hello");
        h.add("hello".getBytes(java.nio.charset.StandardCharsets.UTF_8));
        eq(h.estimate(), 1L);
    }

    void testSequentialIntegersAreScrambled() {
        // Even the adversarial dense 0..N-1 range must estimate sanely, because
        // the finalizer scrambles sequential inputs.
        HyperLogLog h = new HyperLogLog(14);
        int n = 200_000;
        for (int i = 0; i < n; i++) {
            h.add((long) i);
        }
        double error = (double) Math.abs(h.estimate() - n) / n;
        assertTrue(error < 0.05, "sequential range estimated with " + (error * 100) + "% error");
    }

    // ------------------------------------------------------------------------

    private static double relativeError(HyperLogLog h, int n) {
        for (int i = 0; i < n; i++) {
            h.add((long) i * STRIDE + 1);
        }
        return (double) Math.abs(h.estimate() - n) / n;
    }

    private static HyperLogLog filled(int p, int from, int to) {
        HyperLogLog h = new HyperLogLog(p);
        for (int i = from; i < to; i++) {
            h.add((long) i * STRIDE);
        }
        return h;
    }

    // ---- runner ------------------------------------------------------------

    private int passed, failed;

    public static void main(String[] args) throws Exception {
        HllTest t = new HllTest();
        for (var m : HllTest.class.getDeclaredMethods()) {
            if (m.getName().startsWith("test") && m.getParameterCount() == 0) {
                int before = t.failed;
                try {
                    m.invoke(t);
                    System.out.printf("  %s %s%n", t.failed == before ? "ok  " : "FAIL", m.getName());
                } catch (Exception e) {
                    t.failed++;
                    System.out.printf("  ERR  %s: %s%n", m.getName(), e.getCause() != null ? e.getCause() : e);
                }
            }
        }
        System.out.printf("%n%d passed, %d failed%n", t.passed, t.failed);
        System.exit(t.failed == 0 ? 0 : 1);
    }

    private void eq(Object got, Object want) {
        if (java.util.Objects.equals(got, want)) {
            passed++;
        } else {
            failed++;
            System.out.printf("       got <%s> want <%s>%n", got, want);
        }
    }

    private void approx(double got, double want, double tol) {
        if (Math.abs(got - want) <= tol) {
            passed++;
        } else {
            failed++;
            System.out.printf("       got %s want %s (tol %s)%n", got, want, tol);
        }
    }

    private void assertTrue(boolean cond, String message) {
        if (cond) {
            passed++;
        } else {
            failed++;
            System.out.println("       " + message);
        }
    }

    private void throwsIAE(Runnable r) {
        try {
            r.run();
            failed++;
            System.out.println("       expected IllegalArgumentException");
        } catch (IllegalArgumentException e) {
            passed++;
        }
    }
}
