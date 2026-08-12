package btree;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Random;
import java.util.TreeMap;

/**
 * Tests for the B+ tree, as a plain runner (exit 0 on success).
 *
 * The headline is that the tree stays a *correct, balanced* B+ tree through
 * arbitrary insert/overwrite/delete traffic: it matches an in-memory reference
 * on every lookup and ordered scan, and — checked after every batch — every leaf
 * is at the same depth and every node's occupancy is within the B+ bounds.
 */
public final class TestBTree {
    static int failures = 0;

    static void check(boolean cond, String msg) {
        System.out.println((cond ? "  ok:   " : "  FAIL: ") + msg);
        if (!cond) failures++;
    }

    // -------------------------------------------------------- basics

    static void testBasics() {
        System.out.println("basics: put / get / overwrite / remove");
        BPlusTree<Integer, String> t = new BPlusTree<>(4);   // small order → lots of splits
        for (int i = 0; i < 100; i++) t.put(i, "v" + i);
        check(t.size() == 100, "size after 100 inserts");
        check("v42".equals(t.get(42)), "get returns the value");
        check(t.get(999) == null, "absent key returns null");
        check("v42".equals(t.put(42, "V42")), "put returns the previous value");
        check("V42".equals(t.get(42)), "overwrite wins");
        check("V42".equals(t.remove(42)), "remove returns the old (overwritten) value");
        check(t.get(42) == null && t.size() == 99, "removed key is gone");
        check(t.checkInvariants() == null, "invariants hold: " + t.checkInvariants());
    }

    // -------------------------------------------------------- vs reference

    static void testAgainstReference() {
        System.out.println("randomised: matches TreeMap, and stays a valid balanced B+ tree");
        for (int order : new int[]{3, 4, 8, 32}) {
            BPlusTree<Integer, Integer> t = new BPlusTree<>(order);
            TreeMap<Integer, Integer> ref = new TreeMap<>();
            Random rng = new Random(order * 7 + 1);
            String inv = null;

            for (int op = 0; op < 40000; op++) {
                int key = rng.nextInt(1500);
                if (rng.nextInt(3) != 0) {
                    int val = rng.nextInt();
                    t.put(key, val);
                    ref.put(key, val);
                } else {
                    check2(t.remove(key), ref.remove(key));
                }
                if (op % 500 == 0) {                     // periodic full structural audit
                    inv = t.checkInvariants();
                    if (inv != null) break;
                }
            }
            boolean structureOk = (inv == null) && (t.checkInvariants() == null);
            check(structureOk, "order " + order + ": invariants held throughout (" + inv + ")");

            boolean pointOk = t.size() == ref.size();
            if (pointOk) for (int k = 0; k < 1500; k++) {
                Integer a = t.get(k), b = ref.get(k);
                if (a == null ? b != null : !a.equals(b)) { pointOk = false; break; }
            }
            check(pointOk, "order " + order + ": every lookup matches TreeMap");

            List<BPlusTree.Entry<Integer, Integer>> scan = t.entries();
            boolean scanOk = scan.size() == ref.size();
            if (scanOk) {
                int i = 0;
                for (Map.Entry<Integer, Integer> e : ref.entrySet()) {
                    var got = scan.get(i++);
                    if (!got.key.equals(e.getKey()) || !got.value.equals(e.getValue())) { scanOk = false; break; }
                }
            }
            check(scanOk, "order " + order + ": full ordered scan matches TreeMap");
        }
    }

    static int mismatches = 0;
    static void check2(Object a, Object b) {
        if (a == null ? b != null : !a.equals(b)) mismatches++;
    }

    // -------------------------------------------------------- range scans

    static void testRangeScans() {
        System.out.println("range scans honour [from, to) and stay sorted");
        BPlusTree<Integer, Integer> t = new BPlusTree<>(8);
        TreeMap<Integer, Integer> ref = new TreeMap<>();
        for (int i = 0; i < 5000; i++) { t.put(i * 2, i); ref.put(i * 2, i); }   // even keys

        boolean ok = true;
        Random rng = new Random(9);
        for (int trial = 0; trial < 200; trial++) {
            int a = rng.nextInt(10000), b = a + rng.nextInt(2000);
            List<BPlusTree.Entry<Integer, Integer>> got = t.range(a, b);
            List<Integer> want = new ArrayList<>(ref.subMap(a, b).keySet());
            if (got.size() != want.size()) { ok = false; break; }
            for (int i = 0; i < got.size(); i++)
                if (!got.get(i).key.equals(want.get(i))) { ok = false; break; }
        }
        check(ok, "200 random range scans match TreeMap.subMap");
    }

    // -------------------------------------------------------- balance

    static void testStaysShallow() {
        System.out.println("height stays logarithmic (the whole point of a B-tree)");
        BPlusTree<Integer, Integer> t = new BPlusTree<>(64);
        int n = 1_000_000;
        for (int i = 0; i < n; i++) t.put(i, i);
        int h = t.height();
        // with order 64 a node holds up to 63 keys; height should be ~4
        check(h <= 5, "1,000,000 keys fit in height " + h + " (≤ 5)");
        check(t.checkInvariants() == null, "1M-key tree is a valid B+ tree");
    }

    public static void main(String[] args) {
        testBasics();
        testAgainstReference();
        check(mismatches == 0, "remove() return values matched TreeMap throughout");
        testRangeScans();
        testStaysShallow();
        if (failures == 0) System.out.println("\nALL TESTS PASSED");
        else System.out.println("\n" + failures + " TEST(S) FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
