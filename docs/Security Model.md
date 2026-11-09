# Full User Access

Automat's security model is rooted in the physical access to the computer. There is just one level of access - full - which Automat also calls "User". User access is assumed to come from kernel input devices.

User may create virtual devices or machines that will act on his/her behalf. Delegation through user created objects is also the main mechanism of access control. To expose some function externally, the user would compose a set of objects that would receive external requests, authenticate them and perform the desired function.

Automat's notion of "user" does not imply that there is exactly one person using the computer - a hypothetical public device running Automat might have any number of people using it - all of whom would have the same "user" access.

Important omissions:
- no identity / users / groups
- no granular permissions

Automat's objects may store some data internally but their private / public division is neither an API, nor a security boundary. It's mostly a recommendation for how to access object's functions and a best-effort promise (not a guarantee) of future compatibility.

The goal of this simple model with full user access is to reduce the number of barriers that are typically encountered when doing work on a computer.

## Untrusted Inputs

The "Full User Access" model works great when a computer is isolated from malicious inputs - but may fail when malicious inputs exploit bugs in the system to gain remote access or perform actions that the user didn't expect.

### Untrusted Inputs Are Radioactive

When dealing with potentially malicious inputs the primary mechanism of ensuring security is clear communication of whether any piece of data could pose a risk. In Automat this is signalled through a metaphor of radioactivity: green light, geiger-counter audio effects and radiation hazard markings.

Processing of untrusted inputs is possible within sandboxed environments. The mechanism of sandboxing may vary (web browsers, emulators, OS-level isolation). Sandboxing mechanisms support the radioactivity metaphor through their own design - they imitate industrial / safety equipment.

### What is Untrusted

Untrusted data is code coming from the network.

Examples of trusted data:
- data loaded from the local filesystem

## Shades of Trust

Clear division of code/data into trusted & untrusted is a simple model - but fails to capture the fuzziness of real software - which may include bugs, where the malicious code may be present but dormant, or which performs actions right under the threshold of what would count as "malicious".

The main mechanism that Automat uses to tackle this fuzziness is to communicate the key security signals of the system through audio-visual feedback as the user is using it. Examples:

* subtle audio feedback when filesystem is being accessed / modified
* visual highlighting for high entropy blobs of data
* visual feedback that shows which code has executed

## Interop with OS isolation mechanisms

Modern operating systems introduce user accounts & isolate different processes (UID & GIDs, POSIX permissions, dumpable flag, Linux Yama).

By default Automat will follow this security model. But it will also include a "Pandora's Box" that upon opening asks the user for admin permissions restarts Automat in privileged mode (sticky - until the box is closed & persisting across restarts).

Closing the box would remove these capabilities.

While the box is open, toolbar will include objects that would normally require elevated privileges.

All "Pandora's Box" objects represent the same underlying state (their `Clone` may return `AcquirePtr`) - so they may literally be the same object.

While "Full User Access" mode is active, Automat effectively becomes an admin-level access panel to the whole system.

Important: Any other processes having access to Automat state & binary may also get the full user access. This is by-design part of the "Full User Access" security model. All local apps may act with "full user access".

### Linux

On Linux, Automat will still run as whatever user started it (no suid, no sudo). When the box is opened, it will add (through `setcap`-equivalent syscalls) any security capabilities (`CAP_*`) that it might need later.

Privileged objects would be present also when Automat starts without the extra capabilities (so that they can be deserialized) but would not be presented in the Toolbar. They may also fail to work properly due to reduced privileges (they should be on fire).

Process of gaining privileges:
- Automat starts normally
- User grabs Pandora's Box from the Toolbar and places it on some Board
- User clicks on a Pandora's Box lock
- Graphical sudo window pops up
- If the sudo request fails - lock goes back to closed
- If the sudo request succeeds - it starts a child process `automat --unlock-full-user-access`
- Child process grants the extra security capabilities on the `automat` binary through extended attributes (`+p`)
- Once the child process completes successfully, Automat saves & restarts itself (exec at the end)
- Automat detects that it has extra privileges and enables them in threads that need them (`capset`)

Process of dropping privileges:
- User clicks the Pandora's Box lock
- Automat removes the extra security capabilities from its own file (no need for privileged child process)
- Once done, Automat saves & restarts itself

### Windows

TODO
