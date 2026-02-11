# Assembler

Describes the goals and design decisions for the Automat's Assembler object.

## Problem statement

**Users of Automat need a way to steer the flow of execution.**

There are many potential solutions to this problem:

* flow-chart approach, similar to DRAKON
* built-in scripting engine such as Lua, Python or JavaScript
* block-based languages (like Scratch or Human Resource Machine)
* interface to external software that would hold the decision logic
    * C-compatible plugin system
	* tools for interfacing with GUI applications, using keyboard input and screenshots
* many, many more...

Each specific situation may call for a different approach so each of them must eventually be made available. Limited development capacity necessitates for some of these approaches to be prioritized over the others.

## Evaluation criteria

Let's restate the goals of Automat.

*The ultimate mission of Automat is to create the best environment for solving computational problems. The first milestone towards this goal is a desktop automation environment. Automat tries to be (1) intuitive, (2) interoperable, (3) future-proof, (4) efficient and (5) free.*

When discussing any design, it's important to remember the mission, the initial "desktop automation" milestone and the five core values of Automat. Especially the five core values, since they are the final criteria when choosing one design vs another.

## Preferred solution to control-flow

Following project values, Automat will implement a control flow solution based on DRAKON-like blocks, where the blocks will be able to use arbitrary internal system for performing actions and making decisions.

Depending on the situation, the high-level DRAKON-like system may be replaced with another framework. For example a state machine, or an LLM-based agent, or a message-passing system of actors, etc. Whatever tool is best for the job.

Within this framework, assembly will be one of the many systems that can perform actions and make decisions. It will be recommended as the default way for implementing logic (although it won't be the only one).

The Assembler object will wrap assembly in a modern (Scratch-like) user interface with extra facilities that should address the issues of pure assembly. Those will include:

* drag-and-drop interface
* fast keyboard-based instruction search
* instructions visualized as graphical diagrams (in addition to mnemonics)
  * usage of colour to signal important information
  * selection of input/output registers alters diagrams
* built-in documentation for every instruction
* register values visualized in multiple representations
  * hex, decimal, binary, ASCII
* executed at native speed, with zero overhead
* human-friendly error messages
* real-time code reloading
* ability to annotate registers and sections of code with names and comments
* support for all LLVM target instruction sets (including extensions)
* import/export into traditional assembly (Intel syntax)
* ability to read values from other Automat objects
* ability to write and send signals to other Automat objects
* executed either on a dedicated thread (safer) or directly on the current thread

Usage of stack will be discouraged (due to lack of good visualization), but still supported.

The end goal of the Assembler object is to be a novice-friendly environment for defining control flow that is still a fast and convenient place for experienced programmers to work in.

Assembler object also has the potential for creating extremely performant Automat programs.

## Discussion

### Beginners' perspective on assembly

Usage of assembly may seem counterintuitive, since assembly is often seen as an advanced topic, even among programmers. The choice of assembly is based on the assumption that this apparent difficulty may come from bad tooling, rather than its inherent complexity. Hopefully with better tooling, assembly is going to be usable also by computing newbies.

First of all, assembly requires fewer concepts than regular programming to get started - no identifiers, variables, functions etc.

Second of all, the top issue affecting starting programmers in *any* programming language are *syntax issues*. Assembly, with its simple structure naturally fits into a *structural editor* (imagine a Scratch-like environment) which elliminates this class of errors completely.

Significant part of the assembly difficulty comes from few areas, which are not a priority for Automat. Those are *stack manipulation*, *memory access*, *calling conventions*. Logic & control use-case can mostly be solved using just the registers & the instructions that operate on them. This makes stack & memory access instructions *optional* for most use cases.

Unlike most programming systems, with its parsing & compilation, the execution model of assembly is very simple - instructions are translated to machine code (1:1 mapping), and executed by the CPU. Less moving pieces means that it's easier for beginners to understand what's happening. Ability to work with actively executing programs with debuggers (hot-reloading code & tweaking register values) as the program executes makes the connection even more direct.

Assembly, being a relatively closed system is also less daunting. It's not an endless rabbit hole of syntax features, libraries and best practices. There are just the built-in instructions (arch-dependent, about fifty important ones and a couple hundred in total) and that's it. Assembly can also be treated more pragmatically. Within small code blocks, code style doesn't matter. Newcomers don't need to learn any conventions, styles or traditions. Whatever works, works.

Lastly, 

### Composition of mathematical and app-like components

*This section argues that compiled languages hide the hardware details, leading to similar inefficiencies as black-box glue-ing approach to software.*

Solving all potential computational problems using just the built-in components may be theoretically possible but is rarely practical. This is because the built in components are either (A) mathematically simple and abstract, which necessitates a large number of them to solve any practical problem (with all of its edge cases), or (B) turn-key and robust, which requires the environment to provide a large number of them, with various parametrizations to cover all potential tasks. Therefore, to cover all possible computational problems, the primitive components are always combined into higher level components. Typically we take it for granted when dealing with mathematical concepts but we rarely assume that higher level, "app-like" components can also be combined. There is no fundamental reason that would prevent them from being combined together though. This is often the case with Linux shell commands for example. Node-based user interfaces which are becoming more common are another one. The early vision for the Android OS also assumed that apps will be combined by users.

The two cases (mathematical and app-like components) correspond to two different levels at which the user typically combines computational components. The former is often seen in professional environments, where the user is also expected to have the theoretical knowledge to use the mathematical concepts, and often follows a slower, analytical development process. The latter, on the other hand, often appears in no-code environments, optimized towards an inexperienced user and a rapid, iterative development process. Because of the different use cases the latter approach is often looked down upon. It's seen as more childish (often sure to being designed for kids) and less capable. While it's definitely easier to use (because it doesn't require formal education), the idea that it's less performant may be challenged.

The mechanism for combining mathematical and app-like components is fundamentally different. App-like components are typically instantiated and executed directly, according to the blueprint sketched by the user. Mathematical components, thanks to their regular structure, can be analyzed and optimized to produce a much more performant machine code. When compared side by side, programs produced by gluing of black-box components are almost always slower than their optimized counterparts. This side by side comparison reveals the key factor in the performance of the "mathematical components" - it's the machine code. While internally the machine code used by app-like components is comparable to the mathematically-defined ones, the code necessary to exchange data must serialize and serialize it, following the data exchange protocols. Mathematically defined components may, on the other hand, exchange data in place, or even not at all, if analysis indicates that it's not used.

// TODO...


## Plan

* Object that models a sequence of MCStreamer method calls.
* Initially all registers set to 0.
* Prints the final value of all registers.
* Live re-running. Buttons for execution control (play, step, pause).
* Dedicated thread that executes the assembly.
* Registers accessed with ptrace.
* Ability to load contents of other objects (integers, floats, text, bitmaps...).
* Ability to update contents of other objects.
* Ability to send signals to other objects.
* Object maintains register values (stateful, code + data).

https://www.linuxjournal.com/article/6210

## Notes on deck animation

* Each card has some parameters
** Rotation Z
** Animation type, direction, distance & rotation
* Each card defines its own animation type
** no animation
Card is immediately adjusted to the right position / angle. Z-order is not changed.
** shift up
Card is displaced outwards until it leaves the deck, then its z order changes to its target z order. Then it flies back into the deck.
Rotation Z slowly changes towards target rotation.
** shift down
Same as above
** move to back
Card is displaced & rotated outwards until it leaves the deck. Then its z order changes and it flies back into the deck. It disappears once fully in the deck.
Rotation Z slowly changes towards vertical.
** move to front
Card starts half-rotated at the side of the deck & proper z order. Then it flies back into the deck.
** remove from deck
Card is displaced out of the deck and disappears in flames
https://www.shadertoy.com/view/XsVfWz
** add to deck
Card appears outside of deck and slides in at an angle that matches its rotation Z
