#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Print Mach-O headers for build metadata generation."""

import subprocess
import sys


if __name__ == "__main__":
    path = next((arg for arg in reversed(sys.argv[1:]) if not arg.startswith("-")), None)
    if path is None:
        sys.exit("missing Mach-O input")
    sys.exit(subprocess.call(["xcrun", "otool", "-hvl", path]))
