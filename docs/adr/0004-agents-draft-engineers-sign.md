# 4. Coding agents draft; engineers own and sign the evidence

## Context
Coding agents are fast at drafting tests, interface code and emulators. Their output looks like evidence
whether or not it is.

## Decision
Agents work in a workspace without the project's tests, mutants or reports, with file tools confined to it
and a command allow-list (build and run tests only). Their tests are measured like anyone's: they must compile,
pass, and fail on the requirement's mutants. They are proposals until an engineer reviews them. The release
manifest is signed by the pipeline (an ephemeral key) and must be signed again by an engineer with a key the
agents never see.

## Consequences
`iqk agents` reports, per model, how many drafts are proven, partly proven or hollow (pass but catch nothing).
That number, not a demo, is what decides where agents are used.
