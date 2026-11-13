# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT

from extension_helper import ExtensionHelper
import build

tracy = ExtensionHelper('tracy', globals())
tracy.FetchFromGit('https://github.com/wolfpld/tracy.git', 'v0.15.0')
tracy.SkipConfigure()
# From a parrot:
# > This patch makes each frame show the line of the call instead of the line after it.
# >
# > The address the unwinder reports for a frame is the return address, which is the instruction after the call. Tracy looks up the source line of that address, so
# > every frame except the innermost showed the next line. When the call is the last instruction of a function, the address even belongs to the following function.
# > Subtracting 1 puts the address inside the call instruction.
# >
# > A frame that was interrupted by a signal is the exception: its address is the exact instruction, so nothing is subtracted. _Unwind_GetIPInfo reports which of the
# > two cases a frame is, and that is why it replaces _Unwind_GetIP.
# >
# > The other hunk of the same patch, the count going from + 1 to - 1, is separate: it removes the two junk entries at the end of every stack.
tracy.PatchSources('''--- public/client/TracyCallstack.hpp
+++ public/client/TracyCallstack.hpp
@@ -136,11 +136,12 @@
 static _Unwind_Reason_Code tracy_unwind_callback( struct _Unwind_Context* ctx, void* arg )
 {
     auto state = (BacktraceState*)arg;
-    uintptr_t pc = _Unwind_GetIP( ctx );
+    int ipBefore = 0;
+    uintptr_t pc = _Unwind_GetIPInfo( ctx, &ipBefore );
     if( pc )
     {
         if( state->current == state->end ) return _URC_END_OF_STACK;
-        *state->current++ = (void*)pc;
+        *state->current++ = (void*)( ipBefore ? pc : pc - 1 );
     }
     return _URC_NO_REASON;
 }
@@ -153,7 +154,7 @@
     BacktraceState state = { (void**)(trace+1), (void**)(trace+1+depth) };
     _Unwind_Backtrace( tracy_unwind_callback, &state );

-    *trace = (uintptr_t*)state.current - trace + 1;
+    *trace = (uintptr_t*)state.current - trace - 1;

     return trace;
 }
''')
# Tracy assumes that some signals would crash the program so it doesn't propagate them.
# But in Automat these  signals are not fatal. They're part of "normal" operation (error recovery).
# So here we change Tracy to only handle these signals when no pre-existing handlers are present.
tracy.PatchSources('''--- public/client/TracyProfiler.cpp
+++ public/client/TracyProfiler.cpp
@@ -1673,12 +1673,16 @@
     struct sigaction crashHandler = {};
     crashHandler.sa_sigaction = TracyCrashHandler;
     crashHandler.sa_flags = SA_SIGINFO;
-    sigaction( SIGILL, &crashHandler, &m_prevSignal.ill );
-    sigaction( SIGFPE, &crashHandler, &m_prevSignal.fpe );
-    sigaction( SIGSEGV, &crashHandler, &m_prevSignal.segv );
-    sigaction( SIGPIPE, &crashHandler, &m_prevSignal.pipe );
-    sigaction( SIGBUS, &crashHandler, &m_prevSignal.bus );
-    sigaction( SIGABRT, &crashHandler, &m_prevSignal.abrt );
+    auto install = [&]( int signum, struct sigaction* prev ) {
+        sigaction( signum, nullptr, prev );
+        if( prev->sa_handler == SIG_DFL ) sigaction( signum, &crashHandler, nullptr );
+    };
+    install( SIGILL, &m_prevSignal.ill );
+    install( SIGFPE, &m_prevSignal.fpe );
+    install( SIGSEGV, &m_prevSignal.segv );
+    install( SIGPIPE, &m_prevSignal.pipe );
+    install( SIGBUS, &m_prevSignal.bus );
+    install( SIGABRT, &m_prevSignal.abrt );
 #endif
 
 #if defined _WIN32 && !defined TRACY_WIN32_NO_DESKTOP && !defined TRACY_NO_CRASH_HANDLER
@@ -1708,8 +1712,8 @@
     {
         auto restore = []( int signum, struct sigaction* prev ) {
             struct sigaction old;
-            sigaction( signum, prev, &old );
-            if( old.sa_sigaction != TracyCrashHandler ) sigaction( signum, &old, nullptr ); // A different signal handler was installed over ours => put it back
+            sigaction( signum, nullptr, &old );
+            if( old.sa_sigaction == TracyCrashHandler || old.sa_handler == ThreadFreezer ) sigaction( signum, prev, nullptr );
         };
         restore( TRACY_CRASH_SIGNAL, &m_prevSignal.pwr );
         restore( SIGILL, &m_prevSignal.ill );
''')

# Warning: Tracy likes to pick a wrong MSVC runtime library (dynamic instead of
# static or release instead of debug) when built with CMake or Meson.

tracy.InstallWhenIncluded(r'^tracy/.*')
tracy.AddCompileArgs('-I', tracy.src_dir / 'public')

if not build.release:
  tracy.AddCompileArgs('-DTRACY_ENABLE', '-DTRACY_ON_DEMAND', '-DTRACY_ONLY_LOCALHOST')
