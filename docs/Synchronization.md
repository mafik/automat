# Synchronization

TODO: explain the idea behind interfaces.hh

## One-way sync

Syncables may wish to:

- be notified that an event has happened
- notify other Syncables that an event has happened

Currently sync implies both of those, but for some situations it may be good to limit this.

- be notified of events from other Syncables, but ignore the local notifications sent by this Syncable
- ignore notifications from other Syncables, but still notify other Syncables that an event has happened

When there are only two syncables, then this distinction doesn't matter - it only matters for 3+ syncables.

Question 1: How to store the directionality information

- as nullptr pointers
  - slightly more efficient - some desired properties of syncables "fall out" of this naturally
  - less data redundancy
- as booleans
  - readable (no magic)
