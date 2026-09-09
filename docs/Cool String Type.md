String handling is kind of hard. Every method that accepts a string needs to decide how to manage its memory:
- StrView if it's not retained
- Str - local copy - if it's going to be needed later

But StrView *should* be enough if the string is in static storage.

There could also be ref-counted string (Ptr<Str> but with less indirection) - if the same string is stored in many places.

Maybe we could have a string type that encodes its memory management in the high bits of the size?

struct CoolString {
  char* data;
  uint32_t size;

  enum Storage {
    // concurrent-safe
    kStorageView = 0,
    kStorageOwned = 1,
    kStoragePtr = 2, // control block is 8 bytes before `*data`.
    kStorageWeakPtr = 3, // (same)
    // concurrent-unsafe
    kStorageRefcnt = 4, // control block is 4 bytes before `*data`.

    // TODO: maybe some copy-on-write storage modes?
  } storage;
};

This should allow most functions to avoid copies.