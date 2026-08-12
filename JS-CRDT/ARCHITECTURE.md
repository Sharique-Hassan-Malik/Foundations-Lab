# Architecture

Every type here is designed around one goal: **replicas converge without coordination**. The way each achieves it is a small algebraic property — merge is a join, or operations commute — and the tests check exactly that property under randomized delivery.

## Why merge-by-max converges (the counters)

A state-based CRDT is a value plus a `merge` that must be **commutative, associative, and idempotent** — the axioms of a join on a semilattice. If merge is a join, then no matter how replicas exchange states (in any order, with duplicates, in any grouping), they all climb to the same least-upper-bound. Convergence is then automatic.

`GCounter` gets there by keeping a separate count *per replica* and merging with the element-wise maximum. Max is the canonical join: `max(a,b)=max(b,a)`, `max(max(a,b),c)=max(a,max(b,c))`, `max(a,a)=a`. Each replica only ever increases its own entry, so an entry never disagrees except by being stale, and max resolves staleness. The counter's value is the sum of the entries. `PNCounter` is just two `GCounter`s — one counting increments, one counting decrements — with the value being their difference, so it inherits the same convergence.

## Why the OR-Set is add-wins (the set)

The hard case for a replicated set is a concurrent add and remove of the *same* element: which wins? A naive set can't tell a stale add from a fresh one. The **observed-remove** set fixes this with unique tags:

- `add(e)` attaches a globally-unique tag to `e` (here `replicaId:localCounter`).
- `remove(e)` retires only the tags for `e` that this replica has **observed** so far.
- `e` is in the set iff it has at least one tag that has not been retired.
- `merge` is the union of all add-tags and the union of all retired-tags.

Now run the concurrent case: replica A removes `e` (retiring the tags it has seen) while replica B, not having seen the removal, calls `add(e)` — minting a *new* tag. When the states merge, that new tag is in the add-set and is **not** in the retired-set (A never saw it to retire it), so `e` remains: **add wins**. Union of two sets is itself a join (commutative, associative, idempotent), so the whole structure converges.

## How the RGA orders concurrent edits (the sequence)

A sequence is the subtle one, because "position 3" means different things on different replicas after concurrent edits. The RGA avoids positions entirely in its identity model:

- Each inserted character is a **node** with a globally-unique id `(lamportClock, replicaId)` and the id of the node it was inserted **after**. The "after" relation forms a tree rooted at the start of the document.
- The visible sequence is a **depth-first traversal** of that tree. When several nodes were inserted after the same node — concurrent inserts at the same spot — they are **siblings**, and are ordered by a fixed rule: higher Lamport clock first, ties broken by replica id. Because that rule is a total order computed purely from ids, every replica sorts the siblings identically.
- A delete is a **tombstone** — the node stays in the tree (so later inserts can still reference it) but is skipped when rendering.

Two replicas that hold the same set of nodes therefore produce byte-identical output: same tree, same sibling order, same traversal. Convergence reduces to "did they receive the same operations," which is the network's job, not the data structure's.

Two engineering details make that robust against a real, unreliable network:

- **Idempotence.** Applying an operation checks whether the node id already exists (for inserts) or is already tombstoned (for deletes) and does nothing on a repeat — so a message delivered twice is harmless.
- **Causal buffering.** An insert names the node it follows, and a delete names its target; if that dependency has not arrived yet, the operation is parked in a pending map keyed by the missing id and replayed the moment that id appears (which can cascade to unblock further operations). This is what lets the convergence test deliver every operation in a fully shuffled order and still land every replica on the same text with nothing left buffered.

Local editing bridges the position-based API (`insertAt(3, 'x')`) and the id-based model: it reads the current visible sequence, finds the id of the node just before the target position, and emits an insert-after operation against that id — which is then applied locally and broadcast unchanged.
