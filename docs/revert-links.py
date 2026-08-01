#!/usr/bin/env python3


def swap_prefix(file, old, new):
    with open(file, "r") as f:
        lines = f.read()
    lines = lines.replace(old, new)
    with open(file, "wt") as f:
        f.write(lines)


for filename in [
    "../README.md",
    "../README2.md",
    "../src/dpl/doc/LegalizationAlgorithm.md",
]:
    swap_prefix(filename, "(../", "(docs/")
    swap_prefix(filename, "```{mermaid}\n:align: center\n", "```mermaid")

# The exact inverses of the per-file mappings docs/conf.py applies to
# src/dpl/README.md and to the dpl deep dive, so that a build leaves the working
# tree byte-identical. src/dpl/README.md is kept out of the list above on
# purpose: the unconditional "(../" swap would corrupt its parent-relative
# links. Each swap below is a no-op on an already-reverted tree.
swap_prefix(
    "../src/dpl/README.md",
    "](doc/LegalizationAlgorithm)",
    "](doc/LegalizationAlgorithm.md)",
)
swap_prefix(
    "../src/dpl/README.md",
    "](#dpl-known-gotchas)",
    "](doc/LegalizationAlgorithm.md#known-gotchas-determinism-and-limitations)",
)
swap_prefix(
    "../src/dpl/doc/LegalizationAlgorithm.md",
    "\n(dpl-known-gotchas)=\n## Known Gotchas, Determinism, and Limitations\n",
    "\n## Known Gotchas, Determinism, and Limitations\n",
)
