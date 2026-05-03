#!/usr/bin/env python3
"""
phase3_remove_dead_code.py - Remove unused hex dump functions

Since Phase 1 removed all calls to hex_dump_packet() and print_ascii_payload(),
these function definitions are now dead code. This script removes them.
"""

import re
import sys

def remove_function_definition(lines, function_name):
    """
    Find and remove a static function definition
    
    Returns: (modified_lines, removed_count)
    """
    output_lines = []
    removed_count = 0
    
    i = 0
    while i < len(lines):
        line = lines[i]
        
        # Detect function definition
        if f'static void {function_name}(' in line:
            # This is the function signature, find the opening brace
            func_start = i
            
            # Function signature might span multiple lines
            j = i
            while j < len(lines) and '{' not in lines[j]:
                j += 1
            
            if j >= len(lines):
                # No opening brace found, keep as-is
                output_lines.append(line)
                i += 1
                continue
            
            # Found opening brace at line j
            # Now find matching closing brace
            brace_depth = 0
            k = j
            while k < len(lines):
                for char in lines[k]:
                    if char == '{':
                        brace_depth += 1
                    elif char == '}':
                        brace_depth -= 1
                        if brace_depth == 0:
                            # Found matching close brace
                            func_end = k
                            removed_count = func_end - func_start + 1
                            print(f"  Removing {function_name}() at lines {func_start+1}-{func_end+1} ({removed_count} lines)")
                            
                            # Skip this entire function
                            i = func_end + 1
                            return output_lines, removed_count, i
                k += 1
            
            # Couldn't find matching brace, keep as-is
            output_lines.append(line)
            i += 1
        else:
            output_lines.append(line)
            i += 1
    
    return output_lines, 0, len(lines)

def process_file(input_path, output_path, dry_run=False):
    """Remove dead hex dump functions"""
    
    with open(input_path, 'r', encoding='utf-8', errors='ignore') as f:
        lines = f.readlines()
    
    stats = {"total_removed": 0}
    
    # Remove hex_dump_packet()
    output, removed, _ = remove_function_definition(lines, 'hex_dump_packet')
    if removed > 0:
        stats["total_removed"] += removed
        lines = output
    
    # Remove print_ascii_payload()
    output, removed, _ = remove_function_definition(lines, 'print_ascii_payload')
    if removed > 0:
        stats["total_removed"] += removed
        lines = output
    
    if not dry_run:
        with open(output_path, 'w', encoding='utf-8') as f:
            f.writelines(lines)
    
    return stats

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 phase3_remove_dead_code.py <file.c> [--dry-run]")
        sys.exit(1)
    
    input_file = sys.argv[1]
    dry_run = '--dry-run' in sys.argv
    
    print(f"Processing: {input_file}")
    print(f"Mode: {'DRY RUN' if dry_run else 'LIVE'}")
    print("")
    
    stats = process_file(input_file, input_file, dry_run)
    
    print("")
    print(f"Total lines removed: {stats['total_removed']}")
    
    if not dry_run:
        print(f"✓ File modified: {input_file}")

