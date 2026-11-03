# 2. A traceability link is evidence only when a mutant proves it

## Context
Traceability matrices usually record that a test "verifies" a requirement because someone wrote so. Coverage
does not help: a test can execute every line of the code and still assert nothing the requirement says. With
coding agents drafting tests, hollow tests become cheap to produce.

## Decision
Every testable requirement has mutants (`qual/mutants.yaml`): the plausible ways to break it in the code. A
test's claim to verify the requirement counts as evidence only if the test fails on at least one of them, and
the requirement is verified only if every mutant is caught by a test linked to it. Mutants that do not
compile are reported, not counted; mutants that cannot fail any test (equivalent) are removed with a reason.

## Consequences
The matrix says which links are proven, unproven or system-level. Building it found weak tests the review had
passed: a frame truncated by lost bytes and samples held up behind module frames were not covered. Mutation
costs minutes per run (each mutant rebuilds), so CI runs it as its own job. System-level requirements (time
accuracy end to end, replay, the fault campaign) are verified on the bench without mutants and say so.
