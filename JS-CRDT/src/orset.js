// An Observed-Remove Set (OR-Set): a set that survives concurrent add/remove of
// the same element with "add wins" semantics.
//
// The trick is unique tags. Every add stamps the element with a fresh globally
// unique tag; a remove only retires the tags it has actually *observed*. An
// element is in the set iff it has at least one add-tag that has not been
// removed. So if replica A removes an element while replica B concurrently adds
// it again, B's add carries a new tag that A never saw and therefore never
// removed — the element stays. Merging is the union of add-tags and the union of
// removed-tags, which is order-independent and idempotent: convergence.

export class ORSet {
  constructor(replicaId) {
    this.replicaId = replicaId;
    this.adds = new Map();      // element -> Set of tags
    this.removed = new Set();   // retired tags
    this.clock = 0;
  }

  add(element) {
    const tag = `${this.replicaId}:${++this.clock}`;
    if (!this.adds.has(element)) this.adds.set(element, new Set());
    this.adds.get(element).add(tag);
    return this;
  }

  /** Remove only the tags for this element that we have observed so far. */
  remove(element) {
    const tags = this.adds.get(element);
    if (tags) for (const t of tags) this.removed.add(t);
    return this;
  }

  has(element) {
    const tags = this.adds.get(element);
    if (!tags) return false;
    for (const t of tags) if (!this.removed.has(t)) return true;
    return false;
  }

  values() {
    const out = [];
    for (const e of this.adds.keys()) if (this.has(e)) out.push(e);
    return out.sort();
  }

  merge(other) {
    for (const [e, tags] of other.adds) {
      if (!this.adds.has(e)) this.adds.set(e, new Set());
      const mine = this.adds.get(e);
      for (const t of tags) mine.add(t);
    }
    for (const t of other.removed) this.removed.add(t);
    return this;
  }

  clone() {
    const c = new ORSet(this.replicaId);
    for (const [e, tags] of this.adds) c.adds.set(e, new Set(tags));
    c.removed = new Set(this.removed);
    c.clock = this.clock;
    return c;
  }

  equals(other) {
    const a = new Set(this.values());
    const b = new Set(other.values());
    if (a.size !== b.size) return false;
    for (const e of a) if (!b.has(e)) return false;
    return true;
  }
}
