Connection API TODO List
========================

1. AE_BARRIER support in a way that lives with connections doing their own AE
   event handling.
2. Threaded I/O support.  Consider how connections living in threads should
   manipulate events safely.  The best approach may be to have a per-thread
   event loop and a different mechanism that lends connections to threads -
   there could be additional performance benefits to it.
3. Handlers API fix to allow connections to drop.
