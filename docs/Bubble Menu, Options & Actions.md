# Bubble Menu, Options & Actions

Automat's includes a mechanism which allows Objects to expose various available Actions as a tree of Options.

This mechanism is supposed to be parallel to the Widget system. In theory Automat could have a text-based interface (or a VR-based interface, or a REST API) and it would be able to re-use the same Options exposed by the Objects.

Menu/Option/Action design is optimized towards muscle memory development & speed of use. Because of that every Widget (or Object) is responsible for meaningful grouping of available options. It can provide a maximum of eight Options. One special direction is reserved for accessing the widgets below.

## Option

An in-memory structure that describes a potential Action (created upon Activation).

Options are reference-counted & managed through Ptrs.

Options are maintained by Widgets.

The number of Options that each object exposes should be O(1). They may receive parameters upon their activation that can affect their behaviour.

The list of available Options may be dependent on the region on the Widget. Widget may only allow some Options in some areas (like "Drag" or "Resize" zones).

Options may have assigned activators: mouse buttons or keyboard keys. This makes it possible to quickly invoke some Action without opening any menu.

Options may have assigned menu directions (one of eight directions).

### [IDEA] Option index

Right now options are enumerated using a visitor pattern: through the Options() method. Many Widgets build the Options on the stack & make expensive tests to check if they're available.

Most of Option enumeration is not really necessary though - only the top-most LMB-activated Option is needed to decide the current mouse cursor. If other kinds of UI feedback become available - they would also only need to access the top-most X-activated Option.

Would it be possible to create some kind of system that will accelerate option selection?

Maybe Widgets could expose a list (rather than a visitor) of available Options? Similar to how they include a LayerStack?

### [IDEA] Custom Options

Eventually Automat should allow user to define custom Objects (& their Widgets).

The plan is that every object has some customization points (virtual methods) that must be defined. There should be a Skeleton Object that allows user to fill them with some custom behaviours. The Skeleton Object might be usable directly - which would directly pass control to the customized behaviours. Or the Skeleton Object might be "compiled" into a new Frankenstein Object. This Frankenstein Object would hold a copy of the original behaviors - which then wouldn't have to be Ptr-managed any more - and could be stored sequentially in memory.

How could Options work in such model (Skeleton & Frankenstein Objects) ?
- Option Index - very doable - both of these custom objects could hold some Options internally and expose them through the customization points...
- Options Visitor - also doable - as a wrapper around Option Index or by constructing Options ad-hoc (like Widgets currently do)

### [TODO] Engine-space Options

Currently Options are a semi-Engine-space entity. They are constructed by a Widget but generally work on Objects. They're also already managed through Ptr. Would it be possible to turn them into entirely Engine-space entity?

Consequences:
- Options are produced by Objects rather than Widgets
- Options can be converted into first-class Objects and used as parts to build various contraptions

Requirements:
- Options would have to drop all Widget references (Engine=>UI is forbidden). Open Question: Is it even possible to only keep WeakPtrs into Engine?

[IDEA: Maybe Options could have different activators depending on the region? This would make it possible to access remote options (do we want it?). This forces regions & activators to be a part of Widgets]

## Action

Represents an ongoing operation on some Object.

(Actions should generally operate on Objects - because all operations on Widgets are ephemeral. Widgets are never serialized.)

The idea with Action is that the user actively controls it through some Pointer (2d input). It corresponds to a click & drag or finger movement on a touch screen.

### [TODO] Design Undo / Redo system for Actions

As the Action progresses it may construct an "Undo" Option. This Undo Option might be recorded somewhere (where?) to allow undos in the UI.

## Bubble Menu

Bubble Menu is an eight-directional radial menu.

Menu is associated with some origin position on the Widget. This position is passed to the activated Option.

### Swipe Mode

When the user moves the pointer in some direction and crosses the bubble boundary, then an Option stored at that direction is activated. The produced Action is immediately assigned to the activating Pointer - making it possible to select & perform an Action in a single swipe.

### [TODO] Click Mode

Swipe mode has two problems:
- Options must be activated manually - it's not possible to automate some operation
- Swipe Mode is not an established UI idiom - new users find it surprising that the Bubble Menu closes instantly

Click Mode is supposed to fix this by keeping the Menu on the Board as a draggable Object.

Once opened with a short click (<200ms), the bubble will stay. The central part of the bubble might then be used to drag it around. Clicking any option has the same effect as activating that option.

Should the Click Mode work like a:
- a pop-up (dismissed after another option is selected)
- a persistent window (stays after an option is selected)
- an ephemeral widget (with a time-out represented as a ring and counting down when the mouse is away) - this removes the requirement to serialize the Menu or treat it like an Object

Orthogonal properties (might help with the decision above):
- does it disappear after click?
- does it disappear after a timeout?
- does it disappear after mouse away?

#### [TODO: Design] Menu Serialization

Because it's a persistent Board entity - it should be serialized... Somehow? Right now Menu is a Widget which prevents serialization. So it would have to become an Object... Eh.

How could a Menu be serialized?

### Parent Menus

Objects in Automat form a hierarchy (Object/Interface->Owner hierarchy - not related to Widgets or Toy stacks). Right clicking on any Widget will open a menu with the seven options for that Object. The eight option (south) should allow the user to access the options of the parent Object (Location). It's the responsibilty of the Object to add that eight option for the parent.

This convention only applies to top-level menus. Once user enters sub-menu, there is no need to show an option to access the parent Object.

The sequence of menus should be:
[(Interface Menu)]
(Object Menu)
(Location Menu)
(Board Menu)
(Root Widget Menu)

Destructive actions (such as Delete or Quit) should be placed on the NW direction.

### Option Inheritance

Maybe it would be cool if menus could include inherited options - from the parent Objects? This should only apply to the top-level menus though. Sub-category menus don't really need it.

This could be the main mechanism that puts the "Delete" option at top-level (it's a Location option really).
