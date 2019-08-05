TLS Support -- Work In Progress
===============================

This is a brief note to capture current thoughts/ideas and track pending action
items.

Connections
-----------

Connection abstraction API is mostly done and seems to hold well for hiding
implementation details between TLS and TCP.

1. Still need to implement the equivalent of AE_BARRIER.  Because TLS
   socket-level read/write events don't correspond to logical operations, this
   should probably be done at the Read/Write handler level.

2. Multi-threading I/O is not supported.  The main issue to address is the need
   to manipulate AE based on OpenSSL return codes.  We can either propagate this
   out of the thread, or explore ways of further optimizing MT I/O by having
   event loops that live inside the thread and borrow connections in/out.

3. Finish cleaning up the implementation.  Make sure all error cases are handled
   and reflected into connection state, connection state validated before
   certain operations, etc.
    - Clean (non-errno) interface to report would-block.
    - Consistent error reporting.

4. Sync IO for TLS is currently implemented in a hackish way, i.e. making the
   socket blocking and configuring socket-level timeout.  This means the timeout
   value may not be so accurate, and there would be a lot of syscall overhead.
   However I believe that getting rid of syncio completely in favor of pure
   async work is probably a better move than trying to fix that. For replication
   it would probably not be so hard. For cluster keys migration it might be more
   difficult, but there are probably other good reasons to improve that part
   anyway.

5. A mechanism to re-trigger read callbacks for connections with unread buffers
   (the case of reading partial TLS frames):

    a) Before sleep should iterate connections looking for those with a read handler,
       SSL_pending() != 0 and no read event.
    b) If found, trigger read handler for these conns.
    c) After iteration if this state persists, epoll should be called in a way
       that won't block so the process continues and this behave the same as a
       level trigerred epoll.

Replication
-----------

Replication is broken until the child/parent connection sharing issue is
handled.

Apparently OpenSSL's session caching mechanism cannot be used as-is, and as we
don't want to rely on any OpenSSL patches we may need to revert to proxying.  In
that case, it may be possible to hide that in the connection implementation as
well.


TLS Features
------------

1. Add metrics to INFO.
2. Add certificate authentication configuration (i.e. option to skip client
auth, master auth, etc.).
3. Add TLS cipher configuration options.
4. [Optional] Add session caching support. Check if/how it's handled by clients
   to assess how useful/important it is.


redis-cli
---------

1. Check the status of TLS support in hiredis, identify gaps.
2. Pull in latest hiredis + TLS support and integrate with redis-cli.


redis-benchmark
---------------

Does it use hiredis?
Anyway TLS support will be required there for completeness.


Others
------

Consider the implications of allowing TLS to be configured on a separate port,
making Redis listening on multiple ports.

This impacts many things, like
1. Startup banner port notification
2. Proctitle
3. How slaves announce themselves
4. Cluster bus port calculation
