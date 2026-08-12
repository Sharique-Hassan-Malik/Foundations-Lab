# Architecture

A B+ tree is a search tree tuned for the reality that comparisons are cheap but
following a pointer (to another cache line, or a disk page) is expensive. So it
is **wide and shallow**: each node holds many keys, the height stays tiny, and a
lookup touches only a handful of nodes. Two design choices define it, and the
whole implementation follows from keeping them true at all times.

## Node layout

- **Leaves** hold the actual keys *and* values, and a `next` pointer to the leaf
  to their right. All data lives at the leaf level, and the leaves form a sorted
  linked list — which is why a range scan is just "find the starting leaf, then
  walk the chain," with no tree traversal per element.
- **Internal nodes** hold only *separator keys* and child pointers; they route a
  search downward and store no values. A node with `k` keys has `k+1` children,
  where child `i` covers keys in `[keys[i-1], keys[i])`.

An `order` of *m* means an internal node has at most *m* children (so at most
`m−1` keys) and, unless it is the root, at least `⌈m/2⌉` children. That lower
bound is what guarantees the tree can't degenerate into a sparse, tall structure.

## Search

Trivial and the reason the shape matters: from the root, at each internal node
scan the separators to pick the child whose range contains the key, descend, and
repeat until a leaf; then look the key up in that leaf. The number of nodes
visited is the height, which for high fan-out is `~log_m(n)` — four for a million
keys at order 128.

## Insert, and the split that moves a key *up*

Insertion always happens at a leaf. Descend to the correct leaf, insert the
key/value in sorted position, and if the leaf now has more than `m−1` keys it is
**split**: the right half moves into a new leaf spliced into the `next` chain,
and a copy of the new right leaf's first key is pushed up to the parent as a
separator. The parent inserts that separator (and the new child pointer) and may
itself overflow and split — an internal split promotes its *middle* key upward
(it moves, it isn't copied, because internal separators aren't data). Splits thus
propagate toward the root, and if the root itself splits, a new root is created
one level up. That is the *only* way the tree grows in height, and it grows at
the top, which is what keeps every leaf at the same depth.

## Delete, and the merge that pulls a key *down*

Deletion is the intricate half. Remove the key from its leaf; if the leaf (or,
on the way back up, any node) drops below `⌈m/2⌉−1` keys it has **underflowed**
and must be repaired against a sibling:

- **Borrow** — if an adjacent sibling has a key to spare, rotate one across
  through the parent: for leaves, move the sibling's edge key/value over and
  reset the parent separator to the new boundary; for internal nodes, rotate the
  parent separator down into the child and the sibling's edge key up into the
  parent, carrying the corresponding child pointer.
- **Merge** — if no sibling can spare a key, fuse the child with a sibling,
  pulling the parent separator *down* between them (for internal nodes) and
  dropping the now-redundant separator and child pointer from the parent. Merging
  shrinks the parent, which may underflow in turn, so the repair propagates back
  up. If the root ends up with a single child, that child becomes the new root
  and the tree loses a level — the mirror image of a root split.

Borrow-or-merge is where B-tree implementations usually have bugs, so the tests
lean on it hard: tens of thousands of random operations (a third of them deletes)
at orders 3, 4, 8 and 32, comparing every result against a `TreeMap` and running
a full structural audit between batches.

## The invariant checker

`checkInvariants` is what turns "I believe it's balanced" into a test. It
verifies, over the whole tree:

1. **Balance** — collect the depth of every leaf; they must all be equal.
2. **Occupancy** — every non-root node has between `⌈m/2⌉−1` and `m−1` keys.
3. **Order and bounds** — keys are sorted within each node, and every key lies
   inside the `[low, high)` window inherited from the separators of its
   ancestors (so the routing is internally consistent).
4. **Structure** — every internal node has exactly `keys+1` children.
5. **The leaf chain** — walking `next` from the first leaf yields a strictly
   increasing key sequence whose length equals the tree's reported `size`.

Together these are the definition of a well-formed B+ tree; asserting them after
every batch of random operations is the project's correctness guarantee.
