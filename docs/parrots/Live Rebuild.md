# Live Rebuild

`python run.py --live automat` keeps Automat running while its sources and assets are edited. A change under `src/` or `assets/` starts a build while the running instance stays usable. After a successful build, the running instance saves its state and exits, and the new build starts from that state. On Windows the windows that the running instance embeds stay bound to the same running programs.

## The active recipe

The loop is in `run_py/__main__.py`. Each iteration starts the file watcher (`run_py/inotify.py`, which exits at the first change) and executes a freshly loaded recipe (`Recipe.execute` in `run_py/make.py`). The step that runs the program is a regular step of the recipe: it has no outputs, it depends on the executable, and it lasts until the program exits.

A change that the watcher reports while steps with outputs still run or wait to start interrupts the build, because their outputs are already stale. When only steps without outputs still run, the build is complete, and the change does not interrupt them. `Recipe.execute` returns success instead, and the loop keeps the recipe as `active_recipe` while the next recipe is built, so the program stays usable during the next build.

The next recipe interrupts `active_recipe` right before it starts its own step without outputs. The program is therefore replaced only after a successful build, and a failed build, or a build that a further change interrupts, leaves `active_recipe` running. The old program has to exit before the new one starts, because the new instance reads the state that the old one saves when it exits, and on Windows it binds the embedded windows only after the process that embedded them has exited. The run step has no outputs, so every successful build replaces the program, also when the build did not change the executable.

On Linux the build waits for any child process (`os.wait`). When the program of `active_recipe` exits during the next build, the build collects it and reports it as an unregistered child process.

## Linking while the previous executable runs

Windows does not let the linker replace an executable that a running process was started from. In `--live` mode the link step (`Link` in `run_py/build.py`) therefore renames the previous output to `<name>.<nanoseconds>.old` first (`fs_utils.move_aside`), and renames it back when the link fails, so a failed build leaves the previous executable in place. A later link deletes the renamed copies whose programs have exited. The renamed executable stays in the same directory, so the instance that is being replaced still saves `automat_state.json` where the new build reads it (`StatePath` in `src/persistence.cpp`).

Without `--live` the link step does not rename the executable. An Automat that was started in another way keeps its executable, and a build while it runs fails to link. Renaming the executable of such an instance would let `run.py automat` start a second instance on the same state.

## Asking the program to exit

`Recipe.interrupt` asks the processes of the running steps to exit, and it kills the processes that still run 3 seconds after the request. By default it sends the stop request of the platform: SIGTERM on Linux, and on Windows `WM_CLOSE` posted to the only visible top-level window of the process (`close_window` in `run_py/windows.py`). With `restart=True`, which the next recipe uses for `active_recipe`, it sends a restart request instead. The protocol belongs to the build system and works the same for every program that it runs. A program that handles the restart request can tell a replacement from a stop, and a program that does not handle it exits as it would at a stop.

- **Linux.** The restart request is SIGUSR2 alone. The default action of SIGUSR2 terminates the process, so a program without a handler ends at once. SIGUSR1 is not used, because `SignalController` in `src/machine_code.cpp` installs its own SIGUSR1 handler while an Assembler executes machine code.
- **Windows.** The restart request is the registered window message `RestartNotice` followed by `WM_CLOSE`. The default window procedure ignores an unknown registered message, so a program that does not handle the notice receives only the stop request. `post_to_main_windows` in `run_py/windows.py` posts each message to the top-level windows of the process that have no owner and have a window menu (`WS_SYSMENU`), which puts the close button on the title bar. Hidden windows are included, so a program that is hidden in the tray receives the messages too. Windows without a window menu are left out, because programs, system libraries and modules injected into a process use such windows as hidden helpers, and the default window procedure destroys a window on `WM_CLOSE`. A program whose main window has no window menu therefore receives no message and is killed after 3 seconds. In Automat the windows without a window menu are the hidden owner of the embedded windows (`hidden_owner` in `src/win32_window_manager.cpp`) and the windows of modules that other programs inject into the process, such as the game capture hook of OBS Studio. Messages posted to one window are processed in the order in which they were posted, so the notice is handled before `WM_CLOSE`.

## Automat's side of the protocol

SIGTERM saves and exits (`StopOnSigterm` in `src/automat.cpp`), and so does `WM_CLOSE`, like a close from the title bar. SIGINT, which the terminal sends on Ctrl+C, ends Automat without saving, so that a test setup in `automat_state.json` is kept for the next run.

Automat can be closed in the normal way while it runs under `run.py --live`: from the title bar, with Alt+F4, or with Quit in the tray menu. Such a close must give the embedded windows back, so the way Automat was started cannot tell a replacement from a stop. Only a replacement sends the restart request, and the restart request of each platform sets `restarting` (`src/automat.hpp`). On Linux the SIGUSR2 handler that `StopForRestartOnSigusr2` installs in `src/automat.cpp` sets the flag and requests the same stop as SIGTERM, through the eventfd of `SigtermListener`. A relaxed atomic store and a `write` to an eventfd are both safe in a signal handler. On Windows `WndProc` in `src/win32_window.cpp` sets the flag when the registered message arrives, and the `WM_CLOSE` that follows takes the same path as any other stop. On Linux the flag has no effect, because the programs that Automat embeds there are clients of its own Wayland compositor and X11 server, and they lose their display connection when Automat exits.

## Embedded windows on Windows

When `restarting` is set, neither `~AppWindow` nor `win32_wm::Stop` reveals or closes the embedded windows (`src/win32_window_manager.cpp`): they stay cloaked and keep the `AutomatPID` property, and the saved state records the handle of every bound window. The next instance runs `RecoverOrphans` before it relaunches anything, and it binds every window whose `AutomatPID` process has exited to the App Window that saved the same handle. The program keeps running and is not launched again.

The Launch that started such a program exists only in the instance that started it, so after a replacement the Program Launcher does not show the program as running.

A stop without the restart request reveals the embedded windows and closes them, and the next instance launches their programs again from the saved recipes.
