# Code Reuse

Three different ways to do it:

- functions are fundamental
  - stack is fairly inefficient
    - constant initialization & lifetime management
    - arguments & return values need to be copied around
  - stack clashes with multi-threading
    - each thread may need extra memory - single linear stack is not enough
    - we need atomic management
  - (assuming tail-call optimization) allows infinite recursion
- macros
  - variables are promoted to enclosing scopes (eventually they become static)
  - no need to pass values through registers
  - multithreading is simplified
- copy & paste
  - simplest but labour-intensive

Plan for Automat:
1. Start with copy & paste
2. Move upwards, to more formal

Design objectives:
- Discoverable
- Memorable
- Fast to use
- Easy to master

# Abstraction

It's orthogonal to Code Reuse!

Hide internals of complex objects in a way that is: *faithful* & *skimmable*.
