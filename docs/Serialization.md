# Why don't Automat objects have UUIDs?

This is because UUIDs are not particularly human-readable.

While Objects are in memory, they are identified by their addresses (and only their addresses).

While Objects are serialized, they are identiefed by short, human-readable identifiers.

UUIDs would be necessary if Automat needed a way to stream updates from a source that's not aware of the memory addresses. This is not the case as of the time of writing. (but may change in the future)
