#!/usr/bin/env python3
"""Thin wrapper to invoke setup_multinode as a module.

Usage:
    python3 run_multinode.py [OPTIONS] [ROCM_IMAGE]

Equivalent to:
    python3 -m setup_multinode [OPTIONS] [ROCM_IMAGE]

This script exists so that remote nodes can be invoked with a single
path (``python3 /path/to/docker/run_multinode.py --run ...``) without
needing PYTHONPATH or ``-m`` syntax.
"""

import os
import sys

# Ensure the docker/ directory is on sys.path so that
# ``import setup_multinode`` resolves to the local package.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from setup_multinode.__main__ import main  # noqa: E402

if __name__ == "__main__":
    main()
