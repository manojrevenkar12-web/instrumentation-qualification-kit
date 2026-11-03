# 1. The interface documents are the single source of the wire formats

## Context
Interface code drifts from interface documents: a field is widened in code and not in the ICD, a byte order
is assumed. In qualification the ICD is what the assessor reads, so the code must provably match it.

## Decision
Each ICD is a YAML file (`icd/`). A generator writes the C++ codecs (encode, decode with exact lengths, range
checks in raw units, bit accessors), a dispatch table, and the human-readable ICD (`docs/icd/`). Generated
files are committed as configuration items; CI fails when they drift. An independent Python codec reads the
same YAML at runtime, and property tests compare it with the C++ codecs byte for byte.

## Consequences
A wire change is one reviewed edit. The structural rules (a CAN payload fits 8 bytes, ranges fit their types)
are checked when the ICD loads. Behaviour beyond the wire (what a module does with a register) still needs
prose in the ICD and tests.
