# Scheduler

Scheduler should:

1. Make it easy to arrange execution in time from within Automat
2. Only be neecessary when the CPU has more than one thing to do at the same time
3. Not waste time
   - Fully utilize the hardware
   - Avoid dynamic allocations if possible
4. Allow serialization of work if Automat is closed

# How scheduling works?

Scheduling uses Tasks - they represent something to be done and may carry extra
information necessary to do the work: arguments, continuation information.
