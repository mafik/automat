# Automat Scripting

Status: not implemented (assembly works but does not play with other objects)

Automat attempts to reimagine computer interaction starting at it's most basic level - machine code. It takes a fresh look at how to drive the CPU without following the established approach of interpreters, abstract virtual machines, the stack or even function calling.

The most basic unit of automat's code is called a block. Each block is an executable region of memory at some well known address.

Blocks may pass control through direct threading. The address of the next block is typically hardcoded within the block and is updated by the interactive environment through code manipulation. Blocks don't call each other through the stack.

Blocks may call into foreign code and are allowed to be blocked for a long time. The interactive environment can inspect the stack of a thread to see which block is currently executing. The environment can easily identify the block based on the fact that they occupy non-overlapping segments of memory.

The environment may interrupt execution of a block by throwing an exception (from a signal handler or by attaching a debugger).

The arguments of blocks are managed similarly to control flow - they're hardcoded within the machine code of a block. They are stored in memory at some address so many blocks may share a mutable state.

Blocks don't use registers to pass values. They are used exclusively as a scratch space. The state of registers at the end of a block is simply the starting state for the next block. When calling foreign functions, the caller-preserved registers are not preserved by default, but the user may use an option to preserve them.

The interactive environment has a special function that transitions from the C mode into the block mode. This "gateway" takes care of locking all the weak pointers (Automat's objects are managed concurrently and must be converted into strong references before use), restoring the saved state of registers, jumping into block code, and then, upon exit, saving the registers and unlocking the pointers.
