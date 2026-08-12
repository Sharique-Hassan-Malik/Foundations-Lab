package btree;
import java.util.Random;
public final class Bench {
    public static void main(String[] a) {
        int n = 2_000_000;
        BPlusTree<Long, Long> t = new BPlusTree<>(128);
        long w0 = System.nanoTime();
        for (long i = 0; i < n; i++) t.put(i, i);
        double ws = (System.nanoTime() - w0) / 1e9;
        System.out.printf("  insert: %,d keys in %.2f s = %,.0f puts/s (height %d)%n", n, ws, n/ws, t.height());
        Random rng = new Random(1);
        long r0 = System.nanoTime(); long sink = 0;
        for (int i = 0; i < n; i++) { Long v = t.get((long) rng.nextInt(n)); if (v != null) sink += v; }
        double rs = (System.nanoTime() - r0) / 1e9;
        System.out.printf("  lookup: %,d random gets in %.2f s = %,.0f gets/s%n", n, rs, n/rs);
        if (sink == -1) System.out.println();
    }
}
