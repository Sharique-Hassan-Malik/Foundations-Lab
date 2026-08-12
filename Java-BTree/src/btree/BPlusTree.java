package btree;

import java.util.ArrayList;
import java.util.List;

/**
 * An in-memory B+ tree — the balanced, high-fan-out search tree that backs the
 * indexes of almost every relational database.
 *
 * All values live in the leaves, which are chained left-to-right, so an ordered
 * range scan is a walk along that chain rather than a tree traversal. Internal
 * nodes hold only separator keys and route searches. Inserts split a full node
 * and push a separator up; deletes borrow from a sibling or merge, pulling a
 * separator down. Both keep the tree perfectly balanced — every leaf sits at the
 * same depth — which is the property {@link #checkInvariants} verifies and the
 * headline the tests assert over millions of random operations.
 *
 * @param <K> comparable key type
 * @param <V> value type
 */
public final class BPlusTree<K extends Comparable<K>, V> {

    private final int order;        // maximum number of children an internal node may have
    private final int maxKeys;      // = order - 1
    private final int minKeys;      // minimum keys in a non-root node
    private Node root;
    private Leaf firstLeaf;         // head of the leaf chain, for ordered scans
    private int size;

    public BPlusTree() { this(64); }

    public BPlusTree(int order) {
        if (order < 3) throw new IllegalArgumentException("order must be >= 3");
        this.order = order;
        this.maxKeys = order - 1;
        this.minKeys = (order - 1) / 2;
        Leaf leaf = new Leaf();
        this.root = leaf;
        this.firstLeaf = leaf;
    }

    // ---------------------------------------------------------------- nodes

    private abstract class Node {
        final List<K> keys = new ArrayList<>();
        abstract boolean isLeaf();
    }

    private final class Leaf extends Node {
        final List<V> values = new ArrayList<>();
        Leaf next;
        boolean isLeaf() { return true; }
    }

    private final class Internal extends Node {
        final List<Node> children = new ArrayList<>();
        boolean isLeaf() { return false; }
    }

    // ---------------------------------------------------------------- search

    public V get(K key) {
        Node node = root;
        while (!node.isLeaf()) {
            Internal in = (Internal) node;
            node = in.children.get(childIndex(in, key));
        }
        Leaf leaf = (Leaf) node;
        int i = leafIndex(leaf, key);
        return (i >= 0) ? leaf.values.get(i) : null;
    }

    public boolean containsKey(K key) {
        Node node = root;
        while (!node.isLeaf()) {
            Internal in = (Internal) node;
            node = in.children.get(childIndex(in, key));
        }
        return leafIndex((Leaf) node, key) >= 0;
    }

    public int size() { return size; }

    public int height() {
        int h = 1;
        for (Node n = root; !n.isLeaf(); n = ((Internal) n).children.get(0)) h++;
        return h;
    }

    // -- index helpers (keys are kept sorted; a linear scan is fine for typical
    //    small orders, and simplest to get right) -----------------------------

    private int childIndex(Internal in, K key) {
        int i = 0;
        while (i < in.keys.size() && key.compareTo(in.keys.get(i)) >= 0) i++;
        return i;                              // child i covers keys < keys[i]
    }

    private int leafIndex(Leaf leaf, K key) {
        for (int i = 0; i < leaf.keys.size(); i++) {
            int c = key.compareTo(leaf.keys.get(i));
            if (c == 0) return i;
            if (c < 0) return -1;
        }
        return -1;
    }

    // ---------------------------------------------------------------- insert

    /** Insert or overwrite. Returns the previous value, or null. */
    public V put(K key, V value) {
        Split split = insert(root, key, value);
        if (split != null) {                   // root split → grow a new root
            Internal newRoot = new Internal();
            newRoot.keys.add(split.key);
            newRoot.children.add(root);
            newRoot.children.add(split.right);
            root = newRoot;
        }
        return lastPut;                              // previous value, set inside insert
    }

    private V lastPut;   // scratch: previous value from the deepest insert

    private final class Split { K key; Node right; }

    private Split insert(Node node, K key, V value) {
        if (node.isLeaf()) {
            Leaf leaf = (Leaf) node;
            int pos = 0;
            while (pos < leaf.keys.size() && key.compareTo(leaf.keys.get(pos)) > 0) pos++;
            if (pos < leaf.keys.size() && key.compareTo(leaf.keys.get(pos)) == 0) {
                lastPut = leaf.values.set(pos, value);       // overwrite
                return null;
            }
            lastPut = null;
            leaf.keys.add(pos, key);
            leaf.values.add(pos, value);
            size++;
            return (leaf.keys.size() > maxKeys) ? splitLeaf(leaf) : null;
        }

        Internal in = (Internal) node;
        int ci = childIndex(in, key);
        Split childSplit = insert(in.children.get(ci), key, value);
        if (childSplit == null) return null;
        in.keys.add(ci, childSplit.key);
        in.children.add(ci + 1, childSplit.right);
        return (in.keys.size() > maxKeys) ? splitInternal(in) : null;
    }

    private Split splitLeaf(Leaf leaf) {
        int mid = leaf.keys.size() / 2;
        Leaf right = new Leaf();
        right.keys.addAll(leaf.keys.subList(mid, leaf.keys.size()));
        right.values.addAll(leaf.values.subList(mid, leaf.values.size()));
        leaf.keys.subList(mid, leaf.keys.size()).clear();
        leaf.values.subList(mid, leaf.values.size()).clear();
        right.next = leaf.next;
        leaf.next = right;
        Split s = new Split();
        s.key = right.keys.get(0);             // B+ separator = first key of right leaf
        s.right = right;
        return s;
    }

    private Split splitInternal(Internal in) {
        int mid = in.keys.size() / 2;
        K up = in.keys.get(mid);               // middle key moves up, not copied
        Internal right = new Internal();
        right.keys.addAll(in.keys.subList(mid + 1, in.keys.size()));
        right.children.addAll(in.children.subList(mid + 1, in.children.size()));
        in.keys.subList(mid, in.keys.size()).clear();
        in.children.subList(mid + 1, in.children.size()).clear();
        Split s = new Split();
        s.key = up;
        s.right = right;
        return s;
    }

    // ---------------------------------------------------------------- delete

    /** Remove a key. Returns the removed value, or null if it was absent. */
    public V remove(K key) {
        lastRemoved = null;
        delete(root, key);
        if (!root.isLeaf() && ((Internal) root).children.size() == 1) {
            root = ((Internal) root).children.get(0);   // root shrank to one child
        }
        return lastRemoved;
    }

    private V lastRemoved;

    private void delete(Node node, K key) {
        if (node.isLeaf()) {
            Leaf leaf = (Leaf) node;
            int i = leafIndex(leaf, key);
            if (i >= 0) {
                leaf.keys.remove(i);
                lastRemoved = leaf.values.remove(i);
                size--;
            }
            return;
        }
        Internal in = (Internal) node;
        int ci = childIndex(in, key);
        Node child = in.children.get(ci);
        delete(child, key);
        if (childSize(child) < minKeys) rebalance(in, ci);
    }

    private int childSize(Node n) { return n.keys.size(); }

    /** Fix an under-full child at index ci: borrow from a sibling or merge. */
    private void rebalance(Internal parent, int ci) {
        Node child = parent.children.get(ci);
        Node left = ci > 0 ? parent.children.get(ci - 1) : null;
        Node right = ci < parent.children.size() - 1 ? parent.children.get(ci + 1) : null;

        if (left != null && left.keys.size() > minKeys) { borrowFromLeft(parent, ci); return; }
        if (right != null && right.keys.size() > minKeys) { borrowFromRight(parent, ci); return; }
        if (left != null) mergeInto(parent, ci - 1);       // merge child into left
        else              mergeInto(parent, ci);           // merge right into child
        // (mergeInto adjusts parent; the parent's own underflow is handled by its
        //  caller after this returns, as delete() checks childSize on the way up)
        assert child != null;
    }

    private void borrowFromLeft(Internal parent, int ci) {
        Node child = parent.children.get(ci);
        Node left = parent.children.get(ci - 1);
        if (child.isLeaf()) {
            Leaf c = (Leaf) child, l = (Leaf) left;
            c.keys.add(0, l.keys.remove(l.keys.size() - 1));
            c.values.add(0, l.values.remove(l.values.size() - 1));
            parent.keys.set(ci - 1, c.keys.get(0));        // new separator
        } else {
            Internal c = (Internal) child, l = (Internal) left;
            c.keys.add(0, parent.keys.get(ci - 1));
            parent.keys.set(ci - 1, l.keys.remove(l.keys.size() - 1));
            c.children.add(0, l.children.remove(l.children.size() - 1));
        }
    }

    private void borrowFromRight(Internal parent, int ci) {
        Node child = parent.children.get(ci);
        Node right = parent.children.get(ci + 1);
        if (child.isLeaf()) {
            Leaf c = (Leaf) child, r = (Leaf) right;
            c.keys.add(r.keys.remove(0));
            c.values.add(r.values.remove(0));
            parent.keys.set(ci, r.keys.get(0));            // new separator
        } else {
            Internal c = (Internal) child, r = (Internal) right;
            c.keys.add(parent.keys.get(ci));
            parent.keys.set(ci, r.keys.remove(0));
            c.children.add(r.children.remove(0));
        }
    }

    /** Merge children[ci] and children[ci+1] (with parent.keys[ci]) into one. */
    private void mergeInto(Internal parent, int ci) {
        Node left = parent.children.get(ci);
        Node right = parent.children.get(ci + 1);
        if (left.isLeaf()) {
            Leaf l = (Leaf) left, r = (Leaf) right;
            l.keys.addAll(r.keys);
            l.values.addAll(r.values);
            l.next = r.next;
        } else {
            Internal l = (Internal) left, r = (Internal) right;
            l.keys.add(parent.keys.get(ci));               // pull separator down
            l.keys.addAll(r.keys);
            l.children.addAll(r.children);
        }
        parent.keys.remove(ci);
        parent.children.remove(ci + 1);
    }

    // ---------------------------------------------------------------- scans

    /** Ordered list of (key, value) with from ≤ key < to. Null bounds are open. */
    public List<Entry<K, V>> range(K from, K to) {
        List<Entry<K, V>> out = new ArrayList<>();
        Leaf leaf = (from == null) ? firstLeaf : leafFor(from);
        while (leaf != null) {
            for (int i = 0; i < leaf.keys.size(); i++) {
                K k = leaf.keys.get(i);
                if (from != null && k.compareTo(from) < 0) continue;
                if (to != null && k.compareTo(to) >= 0) return out;
                out.add(new Entry<>(k, leaf.values.get(i)));
            }
            leaf = leaf.next;
        }
        return out;
    }

    public List<Entry<K, V>> entries() { return range(null, null); }

    private Leaf leafFor(K key) {
        Node node = root;
        while (!node.isLeaf()) node = ((Internal) node).children.get(childIndex((Internal) node, key));
        return (Leaf) node;
    }

    public static final class Entry<K, V> {
        public final K key;
        public final V value;
        Entry(K key, V value) { this.key = key; this.value = value; }
    }

    // ---------------------------------------------------------------- invariants

    /** Verify every B+ tree invariant. Returns null if consistent, else a message
     *  describing the first violation. */
    public String checkInvariants() {
        // 1. all leaves at the same depth (perfectly balanced)
        List<Integer> depths = new ArrayList<>();
        collectLeafDepths(root, 1, depths);
        for (int d : depths) if (d != depths.get(0)) return "leaves at differing depths " + depths;

        // 2. occupancy, ordering, and separator correctness
        String err = checkNode(root, true, null, null);
        if (err != null) return err;

        // 3. leaf chain is globally sorted and matches the tree's size
        K prev = null;
        int chainCount = 0;
        for (Leaf l = firstLeaf; l != null; l = l.next) {
            for (K k : l.keys) {
                if (prev != null && k.compareTo(prev) <= 0) return "leaf chain not strictly increasing at " + k;
                prev = k;
                chainCount++;
            }
        }
        if (chainCount != size) return "leaf-chain count " + chainCount + " != size " + size;
        return null;
    }

    private void collectLeafDepths(Node node, int depth, List<Integer> out) {
        if (node.isLeaf()) { out.add(depth); return; }
        for (Node c : ((Internal) node).children) collectLeafDepths(c, depth + 1, out);
    }

    private String checkNode(Node node, boolean isRoot, K lo, K hi) {
        // keys sorted within the node
        for (int i = 1; i < node.keys.size(); i++)
            if (node.keys.get(i - 1).compareTo(node.keys.get(i)) >= 0)
                return "unsorted keys in node " + node.keys;
        // keys within the [lo, hi) window inherited from ancestors
        for (K k : node.keys) {
            if (lo != null && k.compareTo(lo) < 0) return "key " + k + " below lower bound " + lo;
            if (hi != null && k.compareTo(hi) >= 0) return "key " + k + " at/above upper bound " + hi;
        }
        if (!isRoot && node.keys.size() < minKeys) return "underflow: " + node.keys.size() + " < " + minKeys;
        if (node.keys.size() > maxKeys) return "overflow: " + node.keys.size() + " > " + maxKeys;

        if (!node.isLeaf()) {
            Internal in = (Internal) node;
            if (in.children.size() != in.keys.size() + 1)
                return "internal node children/keys mismatch";
            for (int i = 0; i < in.children.size(); i++) {
                K childLo = (i == 0) ? lo : in.keys.get(i - 1);
                K childHi = (i == in.keys.size()) ? hi : in.keys.get(i);
                String err = checkNode(in.children.get(i), false, childLo, childHi);
                if (err != null) return err;
            }
        }
        return null;
    }
}
