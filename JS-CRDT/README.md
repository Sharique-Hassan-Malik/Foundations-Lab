# js-crdt

**Conflict-free Replicated Data Types** (CRDTs) from scratch in JavaScript — the data structures that let many replicas be edited independently, with no central server and no locking, and still converge to the same state. A grow-only and a positive-negative **counter**, an **observed-remove set**, and an **RGA sequence** for collaborative text.

## The headline: strong eventual consistency, tested

A CRDT's guarantee is *strong eventual consistency*: any two replicas that have received the same set of operations are in the same state, regardless of the order in which they arrived. That is not asserted — it is property-tested. The flagship test runs 60 trials, each making ~50 random edits across three replicas of a document, then delivers every operation to every replica in an **independently shuffled order**, and checks they all render the identical text (with nothing left buffered):

```
$ npm test

✔ HEADLINE: replicas converge under random delivery order (strong eventual consistency)
✔ out-of-order delivery is buffered until dependencies arrive
✔ concurrent inserts at the same position converge on both replicas
✔ applying an operation twice is a no-op (idempotent)
✔ concurrent add wins over remove
✔ GCounter merge is the element-wise max and converges both ways
…
ℹ tests 13   ℹ pass 13   ℹ fail 0
```

And the same idea as a live demo — three users typing into a shared document at once:

```
$ npm run demo

  alice sees: "CRDTs are conflict-free."
  bob   sees: "CRDTs are replicated. "
  carol sees: "CRDTs are eventually consistent. "

after the edits sync (in a random order)…
  alice: "CRDTs are eventually consistent. replicated. conflict-free."
  bob:   "CRDTs are eventually consistent. replicated. conflict-free."
  carol: "CRDTs are eventually consistent. replicated. conflict-free."

all three replicas identical: true
```

## The types

**`GCounter` / `PNCounter`** — counters that merge by taking the element-wise maximum of each replica's tally. Max is a join on a semilattice, so `merge` is commutative, associative, and idempotent — the algebraic core of convergence. `PNCounter` is two grow-only counters (increments and decrements) so it can go down as well as up.

**`ORSet`** — an observed-remove set. Every `add` stamps the element with a unique tag; a `remove` retires only the tags it has *seen*. So a concurrent add and remove of the same element resolves **add-wins**: the concurrent add carries a tag the remover never observed, and the element stays.

```js
import { ORSet } from './src/index.js';
const a = new ORSet('A'); a.add('x');
const b = a.clone(); b.replicaId = 'B';
a.remove('x');            // A removes the tag it has seen
b.add('x');               // B concurrently re-adds → a new tag
a.merge(b);               // → has('x') === true  (add wins)
```

**`RGA`** — a Replicated Growable Array, the sequence CRDT behind collaborative text editors. Each character is a uniquely-identified node that records the id of the node it was inserted *after*; the document is a depth-first walk of that "insert-after" tree, with concurrent siblings ordered deterministically by id. Deletes are tombstones. Operations are idempotent, and an operation that arrives before its predecessor is buffered until it can apply — so even shuffled delivery converges.

```js
import { RGA } from './src/index.js';
const doc = new RGA('alice');
const op1 = doc.insertAt(0, 'h');   // returns an op to broadcast
const op2 = doc.insertAt(1, 'i');
doc.toString();                     // "hi"
// another replica applies op2 then op1 (out of order) and still gets "hi"
```

## Run it

```bash
npm test        # node --test — the full suite incl. the convergence property test
npm run demo     # the three-user collaborative-editing simulation
```

Requires Node 18+ (uses the built-in `node:test` runner). **No dependencies** — nothing to `npm install`.

## Layout

| path | what it holds |
|---|---|
| `src/counters.js` | `GCounter` and `PNCounter` (state-based, merge = max) |
| `src/orset.js` | the observed-remove set (add-wins) |
| `src/rga.js` | the RGA sequence CRDT for collaborative text |
| `tests/*.test.js` | per-type tests + the randomized convergence property test |
| `demo/collab.js` | three users editing one document concurrently |

See [`ARCHITECTURE.md`](./ARCHITECTURE.md) for why merge-by-max converges, how the OR-Set's tags give add-wins, and how the RGA orders concurrent edits deterministically.

## License

MIT — see [`LICENSE`](./LICENSE).
