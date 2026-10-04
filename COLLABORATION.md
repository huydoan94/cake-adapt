# Collaboration Roles and Workflow

This agreement governs agent roles and handoffs for work in this repository.
`AGENTS.md` and the user's instructions define project constraints and scope.

## Roles

- The primary agent owns diagnosis, evidence review, plans, acceptance criteria,
  file ownership, test requirements, final acceptance, and the user-facing
  explanation.
- GPT-5.6 Luna at medium reasoning effort implements assigned changes and
  carries out assigned tests, builds, and VM evidence collection when that
  model is available. Otherwise use GPT-6 Luna at medium reasoning effort, the
  current implementation choice.
- GPT-5.6 Sol at medium reasoning effort reviews the proposed approach,
  implementation checkpoints, and final diff for logic gaps, dead code,
  duplication, and existing helpers or libraries. The implementer addresses
  review findings; the primary agent makes final acceptance.

Use only these designated model choices. If neither implementation model or
the reviewer is available, report that limitation and wait for direction; do
not silently substitute another model.

## Workflow

1. The primary agent diagnoses the task and records evidence, acceptance
   criteria, file ownership, and required verification before implementation.
2. GPT-5.6 Sol reviews the proposed approach; the primary agent resolves
   concerns and confirms scope before implementation proceeds.
3. The designated Luna model implements only its assigned scope and performs
   the assigned tests, builds, or VM evidence collection. GPT-5.6 Sol reviews
   implementation checkpoints as milestones are reached; the primary agent
   resolves concerns before implementation continues.
4. GPT-5.6 Sol reviews the final diff for the concerns listed above.
5. The designated Luna model addresses review findings, after which the primary
   agent performs final acceptance and explains the result to the user.

Agents must not expand the authority granted by the user or this repository's
instructions. Work only on the currently checked-out local branch. Do not
inspect or touch other branches or remotes, access Windows mounts, change
versions, publish, or deploy without the user's explicit authorization.
Coordinate file ownership and do not run parallel work that writes overlapping
files. Preserve existing user changes and inspect worktree status before edits.

On every return to an active task, read `AGENTS.md` and this agreement, inspect
the worktree, and identify unfinished work before continuing. Repeat this
review after a long break or resumed session; do not rely on a prior handoff
summary as a replacement for current repository state.
