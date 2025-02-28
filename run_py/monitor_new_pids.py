#!/usr/bin/env python3

import os
import time
import argparse

def get_current_pids():
    """Get a set of all current PIDs from /proc directory."""
    pids = set()
    for filename in os.listdir('/proc'):
        if filename.isdigit():
            pids.add(int(filename))
    return pids

def get_cmdline(pid):
    """Get the command line for a given PID."""
    try:
        with open(f'/proc/{pid}/cmdline', 'r') as f:
            cmdline = f.read().replace('\0', ' ').strip()
            if not cmdline:
                # If cmdline is empty, try to get the process name from /proc/[pid]/comm
                with open(f'/proc/{pid}/comm', 'r') as comm_file:
                    cmdline = f"[{comm_file.read().strip()}]"
        return cmdline
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        return "Unknown (process may have terminated)"

def monitor_new_pids(interval=1.0, show_existing=False):
    """
    Monitor for new PIDs and print their command lines.
    
    Args:
        interval: Time in seconds between checks
        show_existing: Whether to show existing processes on startup
    """
    # Get initial set of PIDs
    current_pids = get_current_pids()
    
    if show_existing:
        print(f"Existing processes ({len(current_pids)}):")
        for pid in sorted(current_pids):
            cmdline = get_cmdline(pid)
            print(f"PID {pid}: {cmdline}")
        print("\nMonitoring for new processes...")
    
    print(f"Started monitoring at {time.strftime('%Y-%m-%d %H:%M:%S')}")
    print("Press Ctrl+C to stop monitoring")
    
    try:
        while True:
            time.sleep(interval)
            
            # Get new set of PIDs
            new_pids = get_current_pids()
            
            # Find PIDs that weren't there before
            appeared_pids = new_pids - current_pids
            
            # Print new PIDs and their command lines
            for pid in sorted(appeared_pids):
                cmdline = get_cmdline(pid)
                print(f"New process: PID {pid}: {cmdline}")
            
            # Update current PIDs
            current_pids = new_pids
            
    except KeyboardInterrupt:
        print("\nMonitoring stopped")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Monitor new processes appearing in the system")
    parser.add_argument("-i", "--interval", type=float, default=1.0,
                        help="Interval in seconds between checks (default: 1.0)")
    parser.add_argument("-e", "--show-existing", action="store_true",
                        help="Show existing processes on startup")
    args = parser.parse_args()
    
    monitor_new_pids(interval=args.interval, show_existing=args.show_existing) 