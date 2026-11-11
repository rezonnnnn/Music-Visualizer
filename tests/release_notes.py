"""Prints the CHANGELOG.md section for a version, for use as GitHub release notes: python tests/release_notes.py 0.3.0"""
import re
import sys

version = sys.argv[1].lstrip("v")
text = open("CHANGELOG.md", encoding="utf-8").read()
match = re.search(rf"^## {re.escape(version)}\b.*?\n(.*?)(?=^## |\Z)", text, re.S | re.M)
if not match:
    sys.exit(f"CHANGELOG.md has no section for {version}")
print(match.group(1).strip())
