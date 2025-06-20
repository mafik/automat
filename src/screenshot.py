#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT
"""
Screenshot extension for Automat build system.

Adds a 'screenshot' target that builds automat, runs it in background,
takes a screenshot, and then kills the process to prevent state saving.
"""

import subprocess
import time
from pathlib import Path
from functools import partial
import build


def take_screenshot(automat_binary_path, screenshot_output_path):
    """Take a screenshot of the running automat application."""
    print(f"Starting automat from {automat_binary_path}...")

    # Run Automat in background
    automat_process = subprocess.Popen(
        [str(automat_binary_path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )

    try:
        # Wait for Automat to start up (longer delay for debug builds)
        startup_delay = 10.0 if build.debug else 2.0
        print(f"Waiting {startup_delay} seconds for automat to start...")
        time.sleep(startup_delay)

        # Check if process is still running
        if automat_process.poll() is not None:
            stdout, stderr = automat_process.communicate()
            raise RuntimeError(f"Automat exited early:\n"
                             f"Exit code: {automat_process.returncode}\n"
                             f"Stdout: {stdout.decode()}\n"
                             f"Stderr: {stderr.decode()}")

        # Take screenshot
        print(f"Taking screenshot to {screenshot_output_path}...")
        screenshot_result = subprocess.run(
            ["gnome-screenshot", "-f", str(screenshot_output_path)],
            capture_output=True,
            text=True
        )

        if screenshot_result.returncode != 0:
            raise RuntimeError(f"Screenshot failed:\n"
                             f"Stdout: {screenshot_result.stdout}\n"
                             f"Stderr: {screenshot_result.stderr}")

        print(f"Screenshot saved to {screenshot_output_path}")

    finally:
        # Kill Automat process immediately to prevent state saving
        if automat_process.poll() is None:
            print("Killing automat...")
            automat_process.kill()
            automat_process.wait()


def hook_plan(srcs, objs, bins, recipe):
    """Add screenshot target to the build recipe."""
    # Find the automat binary for the current build variant
    automat_binary = None
    for binary in bins:
        if binary.path.stem == 'automat':
            automat_binary = binary
            break

    if not automat_binary:
        print("Warning: No automat binary found, skipping screenshot target")
        return

    # Screenshot output path in the current build variant directory
    screenshot_output = build.current.BASE / "screenshot.png"

    # Create the screenshot step
    recipe.add_step(
        partial(take_screenshot, automat_binary.path, screenshot_output),
        outputs=[screenshot_output],
        inputs=[automat_binary.path],
        desc="Taking screenshot of automat",
        shortcut="screenshot"
    )
