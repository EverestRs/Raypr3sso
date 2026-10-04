#!/usr/bin/env python3
"""
Render comparison tool.
Compares two render outputs (EXR or HDR images) and computes error metrics.
"""

import argparse
import sys
import math


def main():
    parser = argparse.ArgumentParser(description="Compare two render outputs")
    parser.add_argument("reference", help="Reference image (EXR/HDR)")
    parser.add_argument("target", help="Target image (EXR/HDR)")
    parser.add_argument("--output", "-o", help="Output difference image")
    args = parser.parse_args()

    # TODO: Implement image comparison
    print(f"Comparing {args.reference} vs {args.target}")
    print("Image comparison not yet implemented.")


if __name__ == "__main__":
    main()
