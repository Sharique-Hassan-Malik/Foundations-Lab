// State-based (CvRDT) counters. Each replica keeps a per-replica tally; merging
// takes the element-wise maximum, which is a join on a semilattice — so merge is
// commutative, associative, and idempotent, and any two replicas that have seen
// the same updates converge to the same value regardless of order.

/** A grow-only counter: only increments. */
export class GCounter {
  constructor(replicaId) {
    this.replicaId = replicaId;
    this.counts = new Map(); // replicaId -> count
  }

  increment(n = 1) {
    if (n < 0) throw new Error('GCounter cannot decrease');
    this.counts.set(this.replicaId, (this.counts.get(this.replicaId) ?? 0) + n);
    return this;
  }

  value() {
    let sum = 0;
    for (const v of this.counts.values()) sum += v;
    return sum;
  }

  /** Merge another replica's state in place: element-wise max. */
  merge(other) {
    for (const [id, v] of other.counts) {
      this.counts.set(id, Math.max(this.counts.get(id) ?? 0, v));
    }
    return this;
  }

  clone() {
    const c = new GCounter(this.replicaId);
    c.counts = new Map(this.counts);
    return c;
  }

  equals(other) {
    if (this.counts.size !== other.counts.size) return false;
    for (const [id, v] of this.counts) if (other.counts.get(id) !== v) return false;
    return true;
  }
}

/** A counter that both increments and decrements: two grow-only counters, one
 *  for increments (P) and one for decrements (N); the value is P − N. */
export class PNCounter {
  constructor(replicaId) {
    this.replicaId = replicaId;
    this.p = new GCounter(replicaId);
    this.n = new GCounter(replicaId);
  }

  increment(x = 1) { this.p.increment(x); return this; }
  decrement(x = 1) { this.n.increment(x); return this; }
  value() { return this.p.value() - this.n.value(); }

  merge(other) {
    this.p.merge(other.p);
    this.n.merge(other.n);
    return this;
  }

  clone() {
    const c = new PNCounter(this.replicaId);
    c.p = this.p.clone();
    c.n = this.n.clone();
    return c;
  }

  equals(other) { return this.p.equals(other.p) && this.n.equals(other.n); }
}
