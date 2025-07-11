# Mouse Playback

Games tend to rely on raw input (delta X & Y coming from mouse).
Desktop applications tend to rely on absolute pointer coordinates that accounts for mouse acceleration.

Currently Automat records ACCELERATED deltas so to make it work in games, mouse acceleration has to be switched off.

Automat needs to synthesize events that include both of these.

There are a couple strategies that could achieve this:

## Pointer emulation

This is the strategy used by AutoHotKey. It keeps track of pointer
position internally & sends events that assume it has full control
over it.

This approach works when user may carefully adjust the script to make
the macro work exactly how you want but it fails for event record & replay.

## A) Record both RAW & ACCELERATED positions and try to somehow inject custom crafted events

The problem is that it's not possible with XTEST.

### Solution 1

Maybe it would be possible with libei. TODO: check this

### Solution 2

Roll out an integrated X11 server.

Very tempting because it's a gateway to many fun abilities.

A lot of work but absolutely worth it in the long term.

### Solution 3

Hook into external Xorg server.

This may break eventually if function signatures change.

### Solution 4

Send the emulated events directly to the game window.

## B) Record just RAW events & find a way to reproduce original acceleration

TODO: will need X timestamp recording & replay

### Approach 1

Set up the same acceleration on the XTEST device as input pointer device.

But when would we do this? (automat startup, new object controlled by timeline) It HAS to happen automatically.

How to set this up so it doesn't mess up with other apps XTEST usage?

### Approach 2

Replay those events directly on the mouse device (acceleration issue goes away).

TODO: Will need to check if the user belongs to input group.

TODO: would have to spoof HID report (need to parse the device HID descriptor)

### Approach 3

Create a new input device (probably kernel-level)

TODO: user needs to be in the input group

TODO: would have to replicate libinput config

# Plan

We're going with plan B2.

If this doesn't work, plan A4 also sounds good.

As a last resort, A2 is guaranteed to work.
