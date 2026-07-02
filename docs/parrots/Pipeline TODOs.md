1. GStreamer should not be linked in from the system packages - it should be fetched into third_party, configured & built as a static library & linked in just like every other Automat library (the only allowed exceptions are Vulkan & PipeWire - as they are official APIs for interacting with host drivers/services - GStreamer is not like that). This may require compiling & linking statically even more dependencies - that should be OK. It is work worth doing.
2. Same for ffmpeg.
3. Same for GEGL & Tensorflow - they should not be linked dynamically.
4. 'step' bool on Runnable interface must go. There seems to be misunderstanding around Runnable / LongRunning semantics. LongRunning is used for objects that can get "blocked" executing a heavy computational job. Runnable interface is used for starting the object's work. This is why Automat blocks overlapping Running invocations while object's LongRunning indicates it's active. The 'step' bool shows that this system has been fundamentally misunderstood in your design. My recommendation is to change the "Running" interface into a more generic "Signal" interface. Within this interaface's table there could be a bool that controls behavior during LongRunning: 'while_long_running': 'deliver', 'inhibit'. Then the Runnable from base.hpp becomes a Signal with 'while_long_running = inhibit'.
5. I wasn't able to test the current because of a startup crash. Here is report from an agent running on my (Gentoo) machine:

  Crash report: SIGSEGV in PrototypeButton::CoarseBounds

  Full stack trace (Render Thread)

  Thread 42 "Render Thread" received signal SIGSEGV, Segmentation fault.

  #0  automat::ui::PrototypeButton::CoarseBounds (this=0x5555611384a0)
          at src/library_toolbar.hpp:27
          27  RRect CoarseBounds() const override { return proto_widget->CoarseBounds(); }
  #1  automat::ui::Toolbar::UpdateChildTransform (this=0x5555610c6800)
          at src/library_toolbar.cpp:163      // Rect src = buttons[i]->CoarseBounds().rect;   (i = 22)
  #2  automat::ui::Toolbar::Tick (this=0x5555610c6800, timer=...)
          at src/library_toolbar.cpp:118      // UpdateChildTransform();
  #3  automat::PackFrame (rw=..., request=..., pack=...)
          at src/renderer.cpp:858
  #4  automat::RenderFrame (canvas=..., rw=...)          at src/renderer.cpp
  #5  automat::ui::VulkanPaint (rw=...)                  at src/root_widget.cpp:84
  #6  automat::ui::RenderThread (rw=..., stop_token=...) at src/root_widget.cpp:123
  #7–#14  std::thread / libc thread bootstrap

  Proximate cause

  PrototypeButton::CoarseBounds() (and Shape(), line 25) unconditionally dereference proto_widget, which is a MortalPtr<Widget>
  (src/library_toolbar.hpp:16). MortalPtr auto-nulls itself when its target is destroyed (src/mortal.hpp:47-102). The virtual call through a null
  proto_widget is the segfault.

  Key evidence from the core

  - Crash is at button i = 22, the last of 23 buttons (width_targets array holds 23 entries).
  - width_targets[22] == 0 while every other button is 0.0096–0.016. Since width_targets[i] = buttons[i]->natural_width (library_toolbar.cpp:57), button 
  22's natural_width is 0.
  - natural_width and proto_widget are both set together in PrototypeButton::Init() (library_toolbar.cpp:21-27). So at Init() time proto_widget was valid;
  it became bad later ⇒ the prototype's Toy was destroyed after Init(), nulling the MortalPtr.

  Mechanism (confirmed)

  Within a single PackFrame:
  1. renderer.cpp:692 → rw.toys.Tick() runs std::erase_if(container, [](e){ return e.second->dead; }) (src/toy.cpp:34) — destroys any toy flagged dead,
  which nulls all MortalPtrs to it (incl. proto_widget).
  2. renderer.cpp:858 → Toolbar::Tick → UpdateChildTransform → buttons[22]->CoarseBounds() → deref of the now-null proto_widget.

  So the prototype's Toy was dead at frame start, got swept at line 692, and was dereferenced at line 163 in the same frame. This is a Toy-lifetime / 
  use-after-free bug: PrototypeButton treats proto_widget as always-live, but the Toy it points to (stored in RootWidget::toys, keyed {proto, nullptr}) can
  be reclaimed.

  What I ruled out

  - Broken Clone() — all pipeline objects return proper copies (MAKE_PTR(Type, *this)).
  - Cross-thread race on buttons — BuildToolbar/AddObjectPrototype/Init all run on the render thread, sequentially before VulkanPaint
  (root_widget.cpp:88-123).
  - Minimize path — toys.container.clear() and toolbar.reset() happen together, then BuildToolbar rebuilds both (root_widget.cpp:98-121).

  Open question (not fully pinned)

  What sets that prototype Toy's dead flag? The Toy's owner (proto) is a strong Ptr held by the button, so it isn't owner-death. The only MarkDead path that
  fits is LocationWidget::Tick (src/location.cpp:212-213), which marks its cached object toy dead when its Location dies — but that is a clone's toy
  {clone, nullptr}, a different key from the prototype's {proto, nullptr}. For it to hit the prototype's toy, the prototype's Toy would have to be
  reparented into a Location via a second FindOrMake(*proto, …) with the same key (toy.cpp FindOrMake reparents on parent mismatch). I could not statically
  find the code path that calls FindOrMake with a prototype (rather than a clone).

  Two threads worth pulling:
  - Button 22 is a brand-new, under-development pipeline object (23-button build ⇒ near the end of prototypes.cpp:83-93:
  GStreamer/AppSink/AppSrc/MediaFile/Ffmpeg/Gegl/PipeWire…). Its natural_width == 0 means its Toy's CoarseBounds() was zero-width even at Init() — a
  degenerate/not-yet-realized toy. Worth checking whether that object's MakeToy/Shape/CoarseBounds is fully implemented and whether its toy ends up flagged
  dead.
  - Whether anything calls root.toys.FindOrMake(prototype_object, …) and later lets that subtree die.

  Recommended fix options

  1. Make PrototypeButton null-safe (minimal, stops the crash): guard proto_widget in Shape()/CoarseBounds() (return an empty SkPath/RRect when null) and
  skip null-proto_widget buttons in Toolbar::UpdateChildTransform/Tick. Cheap, but masks the underlying lifetime bug.
  2. Re-acquire in Init is not enough — pin the toy's lifetime: have PrototypeButton keep the prototype toy alive for as long as the button exists (own it
  directly, or re-FindOrMake lazily each frame instead of caching a MortalPtr), so it can't be swept out from under the toolbar.
  3. Fix the real trigger (preferred, once identified): stop whatever reparents/marks the prototype's Toy dead. Likely the culprit is button 22's new
  pipeline object — its toy shouldn't be sharing a ToyStore key with a draggable clone, or shouldn't be flagged dead while the prototype lives.

  I'd suggest the other agent do options 1 and 3 (defensive guard + root-cause fix), and confirm null-vs-dangling first.

  ---

Be critical about recommended fixes as this model is not very good and may have hallucinated some information. Be extra weary of bug-hiding recommendations like null-guards. If a property should never be null but turns out to be - null-checking is *not* the right fix.
6. The number of new objects in the toolbar makes it extend beyond window boundaries. I think it would be workth it to create a new "shelf" for beta objects - and put all of the current beta objects there: Command, LeptonicaShelf + all of your new ones. The new "beta" object may even be just a simple "beta" mark - with the objects being accessible from the bubble menu - this avoids rendering of a large number of widgets.