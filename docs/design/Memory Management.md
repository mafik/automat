Automat is multi-threaded and in order to allow dynamic object creation &
destruction each object derives from ReferenceCounted - it tracks how many
references exist and only calls its destructor once the last strong reference
goes away.

But it's not the intended way for objects to operate.

A foundational pattern that Automat tries to encourage is to avoid modelling of
data as objects and to treat them as an interface to data. This means that
Objects should not be constructed & destroyed during program lifetime. Instead,
they should not worry about their own memory management - and assume that their
lifetime is static. Objects are never born and never die. All object pointers
can be assumed to be correct - and there is no need to manage any memory.

Variable-sized data, obviously still can exist in this model - it is just
managed by a fixed number of static objects - representing arrays & indexes.

This static model only works if Objects are truly static - if they are used in
an interactive environment, where objects & links are created & destroyed by the
user, memory management is needed again.

In order for this static / dynamic switching to work, the Object class should be
decoupled from ReferenceCounted. This is a major change that may introduce some
overhead, but the nature of this overhead depends on how this static / dynamic
switching happens. Maybe there will be an optimized build of Automat that can
only execute existing boards - and can't be used interactively (then we can
strip memory management code at compile-time - the only cost will be complexity
of conditionally compiled code). Or maybe the switch will be possible at
runtime, by flicking some toggle and "freezing" a board. If that's the case
then the switching to static mode could be implemented in a number of ways,
ranging from duplicating all Object logic into two specialized variants (dynamic
& static) to sprinkling if-checks throughout all of the code.

For now Object & ReferenceCounted are one - but this may (and should) change in
the future!

Many Toys at the moment are constructed using two pointers:

- pointer to parent widget
- pointer to object (derives from ReferenceCounted)

But the intended construction should use three pointers:

- pointer to parent widget
- pointer to reference counting block (could be separate from the object)
- pointer to object (not derived from ReferenceCounted)
