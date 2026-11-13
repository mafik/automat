# SPDX-FileCopyrightText: Copyright 2026 Automat Authors
# SPDX-License-Identifier: MIT

# Warning: coded with a stochastic parrot

import functools
import os
import subprocess
from pathlib import Path
from sys import platform

import build
import build_variant
import cmake
import extension_helper
import make
import ninja
import src

if platform == 'linux':
    TARGET = 'x86_64-unknown-linux-musl'

    sysroot = build.BASE / 'sysroot'
    libc = sysroot / 'lib' / 'libc.a'
    libcxx = sysroot / 'lib' / 'libc++.a'
    libunwind = sysroot / 'lib' / 'libunwind.a'
    builtins = sysroot / 'lib' / 'linux' / 'libclang_rt.builtins-x86_64.a'
    linux_uapi_include = build.BASE / 'linux_uapi'
    clang_config = build.BASE / 'musl.cfg'
    runtimes_build_dir = build.BASE / 'runtimes'
    ubsan_runtime = sysroot / 'lib' / 'linux' / 'libclang_rt.ubsan_minimal-x86_64.a'
    ubsan_runtime_build_dir = build.BASE / 'ubsan_runtime'

    build.compiler += ['--no-default-config', f'--config={clang_config}']
    build.compiler_c += ['--no-default-config', f'--config={clang_config}']
    build.compile_inputs += [clang_config, libc, libcxx, libunwind, builtins, linux_uapi_include]
    build.link_args += ['-Wl,-z,stack-size=8388608']
    if build_variant.ubsan:
        build.compile_args += ['-fsanitize-minimal-runtime']
        build.link_args += ['-fsanitize-minimal-runtime']
        build.compile_inputs.append(ubsan_runtime)
    os.environ['PKG_CONFIG_LIBDIR'] = os.pathsep.join(str(build.PREFIX / x / 'pkgconfig') for x in ('lib64', 'lib', 'share'))

    llvm = src.load_extension('llvm').hook

    def link_host_linux_uapi_headers():
        linux_uapi_include.mkdir(parents=True, exist_ok=True)
        host_linux_uapi_dirs = {name: Path('/usr/include') / name for name in ('linux', 'asm-generic', 'drm')}
        host_linux_uapi_dirs['asm'] = next((path for path in (Path('/usr/include/x86_64-linux-gnu/asm'), Path('/usr/include/asm')) if path.is_dir()), Path('/usr/include/asm'))
        for name, target in host_linux_uapi_dirs.items():
            if not target.is_dir():
                raise Exception(f'{target} is missing. Install the Linux kernel headers of the host (Debian package linux-libc-dev).')
            link = linux_uapi_include / name
            if not link.is_symlink():
                link.symlink_to(target)

    def write_toolchain_configuration():
        host_resource_include = Path(subprocess.check_output([build.compiler_c[0], '-print-resource-dir'], text=True).strip()) / 'include'
        clang_config.write_text('\n'.join([
            f'--target={TARGET}',
            f'--sysroot={sysroot}',
            f'-resource-dir={sysroot}',
            '-idirafter', str(host_resource_include),
            '-idirafter', str(linux_uapi_include),
            '-stdlib=libc++',
            '-stdlib++-isystem', str(sysroot / 'include' / 'c++' / 'v1'),
            '-mstack-protector-guard=global',
            '-rtlib=compiler-rt',
            '-unwindlib=libunwind',
            '-fuse-ld=lld',
            '-static',
            '']))

    def configure_musl():
        musl.build_dir.mkdir(parents=True, exist_ok=True)
        return make.Popen([musl.src_dir / 'configure', f'--prefix={sysroot}', '--disable-shared', '--disable-wrapper', '--enable-debug'],
                          env=os.environ | {'CC': f'{build.compiler_c[0]} --no-default-config --target={TARGET}', 'CFLAGS': '-fasynchronous-unwind-tables'}, cwd=musl.build_dir)

    def hook_recipe(recipe):
        recipe.add_step(configure_musl,
                        outputs=[musl.build_dir / 'config.mak'],
                        inputs=[__file__, *musl.beam],
                        state=[musl.build_dir],
                        desc='Configuring musl',
                        shortcut='configure musl')
        recipe.add_step(functools.partial(make.Popen, ['make', '-j', str(os.cpu_count()), 'install'], cwd=musl.build_dir),
                        outputs=[libc],
                        inputs=[musl.build_dir / 'config.mak', musl.checkout_dir],
                        desc='Installing musl',
                        shortcut='install musl')
        recipe.add_step(link_host_linux_uapi_headers,
                        outputs=[linux_uapi_include],
                        inputs=[__file__],
                        desc='Symlinking the kernel headers of the host',
                        shortcut=linux_uapi_include.name)
        recipe.add_step(write_toolchain_configuration,
                        outputs=[clang_config],
                        inputs=[__file__],
                        desc='Writing the configuration of the musl toolchain',
                        shortcut=clang_config.name)
        recipe.add_step(functools.partial(make.Popen, [
                            'cmake', '-G', 'Ninja',
                            f'-DCMAKE_MAKE_PROGRAM={ninja.BIN}',
                            f'-DCMAKE_BUILD_TYPE={cmake.CMAKE_BUILD_TYPE}',
                            f'-DCMAKE_C_COMPILER={";".join(build.compiler_c)}',
                            f'-DCMAKE_CXX_COMPILER={";".join(build.compiler)}',
                            f'-DCMAKE_ASM_COMPILER={";".join(build.compiler_c)}',
                            f'-DCMAKE_C_COMPILER_TARGET={TARGET}',
                            f'-DCMAKE_INSTALL_PREFIX={sysroot}',
                            '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY',
                            '-DLLVM_ENABLE_RUNTIMES=compiler-rt;libunwind;libcxxabi;libcxx',
                            '-DLLVM_INCLUDE_TESTS=OFF',
                            '-DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON',
                            '-DCOMPILER_RT_BUILD_SANITIZERS=OFF',
                            '-DCOMPILER_RT_BUILD_XRAY=OFF',
                            '-DCOMPILER_RT_BUILD_LIBFUZZER=OFF',
                            '-DCOMPILER_RT_BUILD_PROFILE=OFF',
                            '-DCOMPILER_RT_BUILD_CTX_PROFILE=OFF',
                            '-DCOMPILER_RT_BUILD_MEMPROF=OFF',
                            '-DCOMPILER_RT_BUILD_ORC=OFF',
                            '-DCOMPILER_RT_BUILD_GWP_ASAN=OFF',
                            '-DLIBUNWIND_ENABLE_SHARED=OFF',
                            '-DLIBCXXABI_ENABLE_SHARED=OFF',
                            '-DLIBCXXABI_HAS_CXA_THREAD_ATEXIT_IMPL=OFF',
                            '-DLIBCXX_ENABLE_SHARED=OFF',
                            '-DLIBCXX_HAS_MUSL_LIBC=ON',
                            '-DLIBCXX_USE_COMPILER_RT=ON',
                            '-DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON',
                            '-S', llvm.checkout_dir / 'runtimes', '-B', runtimes_build_dir]),
                        outputs=[runtimes_build_dir / 'build.ninja'],
                        inputs=[__file__, llvm.checkout_dir, clang_config, libc, linux_uapi_include],
                        state=[runtimes_build_dir],
                        desc='Configuring the C++ runtime',
                        shortcut='configure runtimes')
        recipe.add_step(functools.partial(make.Popen, [ninja.BIN, '-C', runtimes_build_dir, 'install']),
                        outputs=[libcxx, libunwind, builtins],
                        inputs=[runtimes_build_dir / 'build.ninja', ninja.BIN],
                        desc='Installing the C++ runtime',
                        shortcut='install runtimes')
        if build_variant.ubsan:
            recipe.add_step(functools.partial(make.Popen, [
                                'cmake', '-G', 'Ninja',
                                f'-DCMAKE_MAKE_PROGRAM={ninja.BIN}',
                                f'-DCMAKE_BUILD_TYPE={cmake.CMAKE_BUILD_TYPE}',
                                f'-DCMAKE_C_COMPILER={";".join(build.compiler_c)}',
                                f'-DCMAKE_CXX_COMPILER={";".join(build.compiler)}',
                                f'-DCMAKE_ASM_COMPILER={";".join(build.compiler_c)}',
                                f'-DCMAKE_C_COMPILER_TARGET={TARGET}',
                                f'-DCMAKE_INSTALL_PREFIX={sysroot}',
                                '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY',
                                '-DLLVM_ENABLE_RUNTIMES=compiler-rt',
                                '-DLLVM_INCLUDE_TESTS=OFF',
                                '-DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON',
                                '-DCOMPILER_RT_BUILD_BUILTINS=OFF',
                                '-DCOMPILER_RT_BUILD_CRT=OFF',
                                '-DCOMPILER_RT_SANITIZERS_TO_BUILD=ubsan_minimal',
                                '-S', llvm.checkout_dir / 'runtimes', '-B', ubsan_runtime_build_dir]),
                            outputs=[ubsan_runtime_build_dir / 'build.ninja'],
                            inputs=[__file__, llvm.checkout_dir, clang_config, libc, libcxx, linux_uapi_include],
                            state=[ubsan_runtime_build_dir],
                            desc='Configuring the UBSan runtime',
                            shortcut='configure ubsan runtime')
            recipe.add_step(functools.partial(make.Popen, [ninja.BIN, '-C', ubsan_runtime_build_dir, 'install-clang_rt.ubsan_minimal-x86_64']),
                            outputs=[ubsan_runtime],
                            inputs=[ubsan_runtime_build_dir / 'build.ninja', ninja.BIN],
                            desc='Installing the UBSan runtime',
                            shortcut='install ubsan runtime')
        recipe.generated.add(str(clang_config))

    musl = extension_helper.ExtensionHelper('musl', globals())
    musl.FetchFromURL('https://musl.libc.org/releases/musl-1.2.6.tar.gz',
                      sha256='d585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a')
    # Helpers that enable Automat's static TLS within host crt threads
    musl.PatchSources('''--- arch/x86_64/pthread_arch.h
+++ arch/x86_64/pthread_arch.h
@@ -1,8 +1,6 @@
 static inline uintptr_t __get_tp()
 {
-	uintptr_t tp;
-	__asm__ ("mov %%fs:0,%0" : "=r" (tp) );
-	return tp;
+	return __thread_pointer;
 }
 
 #define MC_PC gregs[REG_RIP]
--- src/env/__init_tls.c
+++ src/env/__init_tls.c
@@ -11,12 +11,15 @@
 
 volatile int __thread_list_lock;
 
+__thread uintptr_t __thread_pointer;
+
 int __init_tp(void *p)
 {
 	pthread_t td = p;
 	td->self = td;
 	int r = __set_thread_area(TP_ADJ(p));
 	if (r < 0) return -1;
+	__thread_pointer = (uintptr_t)TP_ADJ(p);
 	if (!r) libc.can_do_threads = 1;
 	td->detach_state = DT_JOINABLE;
 	td->tid = __syscall(SYS_set_tid_address, &__thread_list_lock);
@@ -78,6 +81,7 @@
 #endif
 
 extern weak hidden const size_t _DYNAMIC[];
+weak int __start_host(Phdr *);
 
 static void static_init_tls(size_t *aux)
 {
@@ -130,6 +134,13 @@
 		+ main_tls.size + main_tls.align
 		+ MIN_TLS_ALIGN-1 & -MIN_TLS_ALIGN;
 
+	if (__start_host && __start_host(tls_phdr)) {
+		libc.can_do_threads = 1;
+		__pthread_attach_host(0, 0, 0);
+		__init_threaded();
+		return;
+	}
+
 	if (libc.tls_size > sizeof builtin_tls) {
 #ifndef SYS_mmap2
 #define SYS_mmap2 SYS_mmap
--- src/env/__reset_tls.c
+++ src/env/__reset_tls.c
@@ -7,9 +7,13 @@
 	pthread_t self = __pthread_self();
 	struct tls_module *p;
 	size_t i, n = self->dtv[0];
+	sigset_t set;
+	__block_all_sigs(&set);
 	if (n) for (p=libc.tls_head, i=1; i<=n; i++, p=p->next) {
 		char *mem = (char *)(self->dtv[i] - DTP_OFFSET);
 		memcpy(mem, p->image, p->len);
 		memset(mem+p->len, 0, p->size - p->len);
 	}
+	__thread_pointer = (uintptr_t)TP_ADJ(self);
+	__restore_sigs(&set);
 }
--- src/internal/pthread_impl.h
+++ src/internal/pthread_impl.h
@@ -11,6 +11,9 @@
 #include "atomic.h"
 #include "futex.h"
 
+extern hidden __thread uintptr_t __thread_pointer
+	__attribute__((__tls_model__("initial-exec")));
+
 #include "pthread_arch.h"
 
 #define pthread __pthread
@@ -139,10 +142,12 @@
 	 0x80000000 })
 
 void *__tls_get_addr(tls_mod_off_t *);
+void __pthread_attach_host(void *, size_t, size_t);
 hidden int __init_tp(void *);
 hidden void *__copy_tls(unsigned char *);
 hidden void __reset_tls();
 
+hidden void __init_threaded(void);
 hidden void __membarrier_init(void);
 hidden void __dl_thread_cleanup(void);
 hidden void __testcancel();
--- src/thread/pthread_create.c
+++ src/thread/pthread_create.c
@@ -234,6 +234,21 @@
 	if (f && f->lock<0) f->lock = 0;
 }
 
+void __init_threaded(void)
+{
+	if (libc.threaded) return;
+	for (FILE *f=*__ofl_lock(); f; f=f->next)
+		init_file_lock(f);
+	__ofl_unlock();
+	init_file_lock(__stdin_used);
+	init_file_lock(__stdout_used);
+	init_file_lock(__stderr_used);
+	__syscall(SYS_rt_sigprocmask, SIG_UNBLOCK, SIGPT_SET, 0, _NSIG/8);
+	__pthread_self()->tsd = (void **)__pthread_tsd_main;
+	__membarrier_init();
+	libc.threaded = 1;
+}
+
 int __pthread_create(pthread_t *restrict res, const pthread_attr_t *restrict attrp, void *(*entry)(void *), void *restrict arg)
 {
 	int ret, c11 = (attrp == __ATTRP_C11_THREAD);
@@ -248,18 +263,7 @@
 
 	if (!libc.can_do_threads) return ENOSYS;
 	self = __pthread_self();
-	if (!libc.threaded) {
-		for (FILE *f=*__ofl_lock(); f; f=f->next)
-			init_file_lock(f);
-		__ofl_unlock();
-		init_file_lock(__stdin_used);
-		init_file_lock(__stdout_used);
-		init_file_lock(__stderr_used);
-		__syscall(SYS_rt_sigprocmask, SIG_UNBLOCK, SIGPT_SET, 0, _NSIG/8);
-		self->tsd = (void **)__pthread_tsd_main;
-		__membarrier_init();
-		libc.threaded = 1;
-	}
+	__init_threaded();
 	if (attrp && !c11) attr = *attrp;
 
 	__acquire_ptc();
@@ -311,6 +315,8 @@
 	}
 
 	new = __copy_tls(tsd - libc.tls_size);
+	*(uintptr_t *)((char *)TP_ADJ(new) + ((char *)&__thread_pointer
+		- (char *)__builtin_thread_pointer())) = (uintptr_t)TP_ADJ(new);
 	new->map_base = map;
 	new->map_size = size;
 	new->stack = stack;
--- src/thread/x86_64/pthread_host.c
+++ src/thread/x86_64/pthread_host.c
@@ -0,0 +1,43 @@
+#include "pthread_impl.h"
+
+static __thread struct pthread host_thread;
+static __thread uintptr_t host_dtv[2];
+static __thread void *host_tsd[PTHREAD_KEYS_MAX];
+static pthread_t main_thread;
+
+void __pthread_attach_host(void *stack_addr, size_t stack_size, size_t guard_size)
+{
+	pthread_t td = &host_thread;
+
+	td->dtv = host_dtv;
+	td->dtv[1] = (uintptr_t)__builtin_thread_pointer() - libc.tls_head->offset;
+	td->tid = __syscall(SYS_gettid);
+	td->stack = (char *)stack_addr + stack_size;
+	td->stack_size = stack_size;
+	td->guard_size = guard_size;
+	td->tsd = host_tsd;
+	td->robust_list.head = &td->robust_list.head;
+	td->locale = &libc.global_locale;
+	__thread_pointer = (uintptr_t)TP_ADJ(td);
+
+	__tl_lock();
+	if (!main_thread) main_thread = td->next = td;
+	if (!libc.threads_minus_1++) libc.need_locks = 1;
+	td->next = main_thread->next;
+	td->prev = main_thread;
+	td->next->prev = td;
+	td->prev->next = td;
+	__tl_unlock();
+}
+
+void __pthread_detach_host(void)
+{
+	pthread_t td = &host_thread;
+
+	__pthread_tsd_run_dtors();
+	__tl_lock();
+	libc.threads_minus_1--;
+	td->next->prev = td->prev;
+	td->prev->next = td->next;
+	__tl_unlock();
+}
''')
    # Automat's exception-based signal recovery requires .eh_frame unwind information.
   # Can be removed once musl puts the unwind information of its x86-64 assembly into .eh_frame.
    musl.PatchSources('''--- tools/add-cfi.x86_64.awk
+++ tools/add-cfi.x86_64.awk
@@ -1,8 +1,7 @@
 # Insert GAS CFI directives ("control frame information") into x86-64 asm input
 
 BEGIN {
-  # don't put CFI data in the .eh_frame ELF section (which we don't keep)
-  print ".cfi_sections .debug_frame"
+  print ".cfi_sections .eh_frame, .debug_frame"
 
   # only emit CFI directives inside a function
   in_function = 0
''')
    # Add unwind information to Musl's signal restorer, _start & __clone.
    # Can be removed once the upstream Musl adds it.
    musl.PatchSources('''--- arch/x86_64/crt_arch.h
+++ arch/x86_64/crt_arch.h
@@ -3,6 +3,8 @@
 ".global " START " \\n"
 ".type " START ",%function \\n"
 START ": \\n"
+".cfi_startproc \\n"
+".cfi_undefined rip \\n"
 "	xor %rbp,%rbp \\n"
 "	mov %rsp,%rdi \\n"
 ".weak _DYNAMIC \\n"
@@ -10,4 +12,6 @@
 "	lea _DYNAMIC(%rip),%rsi \\n"
 "	andq $-16,%rsp \\n"
 "	call " START "_c \\n"
+".cfi_endproc \\n"
+".size " START ", .-" START " \\n"
 );
--- src/signal/x86_64/restore.s
+++ src/signal/x86_64/restore.s
@@ -1,8 +1,12 @@
+.cfi_startproc simple
+.cfi_signal_frame
+.cfi_escape 0x0f,4,0x77,0xa0,0x01,0x06
+.cfi_escape 0x10,16,3,0x77,0xa8,0x01
 	nop
 .global __restore_rt
 .hidden __restore_rt
-.type __restore_rt,@function
 __restore_rt:
 	mov $15, %rax
 	syscall
+.cfi_endproc
 .size __restore_rt,.-__restore_rt
--- src/thread/x86_64/clone.s
+++ src/thread/x86_64/clone.s
@@ -17,6 +17,8 @@
 	syscall
 	test %eax,%eax
 	jnz 1f
+.cfi_remember_state
+.cfi_undefined rip
 	xor %ebp,%ebp
 	pop %rdi
 	call *%r9
@@ -25,4 +27,6 @@
 	mov $60,%al
 	syscall
 	hlt
+.cfi_restore_state
 1:	ret
+.size __clone,.-__clone
''')
    musl.SkipConfigure()

