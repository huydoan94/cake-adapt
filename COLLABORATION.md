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
- GPT-5.6 Sol at medium reasoning effort reviews final diff for logic gaps,
  dead code, duplication, and existing helpers or libraries. The implementer
  addresses review findings; the primary agent makes final acceptance.

Use only these designated model choices. If neither implementation model or
the reviewer is available, report that limitation and wait for direction; do
not silently substitute another model.

## Context and usage budget

The primary agent retains the broader project context. Start delegated work with
`fork_turns="none"` and a path to its task brief; do not send the conversation
history. Do not have agents independently rediscover the primary agent's diagnosis.
Use one implementation agent and one bounded review by default, with no nested
delegation or overlapping audits unless the task needs them.

Create a separate short Markdown brief for each delegated work item in a temporary
directory, for example `/tmp/cake-adapt-tasks/<task>.md`. Use this structure:

```text
Objective and current status
Relevant facts and invariants already established
Files to read; files permitted to change
Applicable policy and coding-style excerpts
Exact implementation or review scope; explicit exclusions
Acceptance criteria and exact checks to run
Existing passing results that can be reused
Expected handoff: changed files, results, blockers
```

Supply the relevant policy excerpts and necessary callers or interfaces in the
brief. Subagents read that brief and the named files, then expand reading only
to resolve a specific missing fact. They do not reread all Markdown files,
the whole codebase or historical logs by default. A reviewer receives the diff,
brief and implementation results, and checks the affected logic and callers.
If policy or context is missing, ask the primary agent for the specific item.

Verification is proportional to the change and its concrete risks. The primary
agent selects the checks; subagents run those checks without expanding into a
full test campaign. Reuse passing results while their inputs remain unchanged.
Keep existing safety rules and required runtime validation for new behavior.
Run a simple useful runtime check before building a large acceptance harness.

### VM run discipline

Before touching a VM, the primary writes a bounded run plan: the question to
answer, cases and repetition count, required metadata, acceptance criteria,
deadline, cleanup, and stopping condition. Review the harness locally first:
compile with strict warnings, check shell syntax and failure cleanup, and check
that assertions follow the kernel's actual contract rather than assumptions
such as one-at-a-time LRU eviction. Reuse known VM capabilities and working
wrappers instead of rediscovering them on each attempt.

Use at most one short preflight for a new harness, then one planned acceptance
batch combining compatible checks. For performance comparisons, capture object
identity, CPU affinity, JIT/interpreter mode, actual program run counts, sample
loss, and the meaning of memory counters from the first run. Isolate background
traffic before requiring exact packet counts. Select repetitions in advance
for a stated measurement need; do not add runs simply to collect omitted metadata.

A rerun requires a specific failed check, changed input, or unresolved result
that matters to acceptance. Diagnose and fix harness failures locally where
possible, retain their evidence, and rerun only the affected cases. After a
second harness or environment failure, stop VM execution and reassess the plan
before another attempt. Never weaken a correct check merely to obtain a pass.

Close out and review each planned batch before expanding into another test
campaign. A material regression is a diagnosis point, not permission for more
soak tests. State the result and the next decision plainly. Do not require
exhaustive scheduler interleavings as an open-ended acceptance gate: identify
the concurrency invariant, use a bounded targeted test and source review, and
record the remaining limits. Preserve all existing VM restoration, timeout,
log-inode, and deployment restrictions.

Keep reports short: outcome, changed files, checks, remaining blockers. Preserve
only the raw evidence needed to substantiate a claim. Detailed reports, exhaustive
profiling and broad before/after studies are performed when requested or needed
to resolve a concrete risk. Avoid large tool-output dumps, repeated polling and
duplicated reviews.

When an authorized command or subagent is running and there is no independent
work to do, wait using the available completion/mailbox mechanism. Resume on
completion, a relevant update, user input or the required timeout. Both primary
and delegated agents follow this rule. Do not repeatedly list agent status,
probe VM processes or reread unchanged logs just to fill the wait.

For VM jobs, keep a bounded foreground SSH command attached when practical so
its exit reports completion. For a necessary detached job, record its PID,
output path and exit status, then use a bounded process/event wait. Retain
required deadlines and occasional progress updates; do not run a busy polling
loop. Waiting avoids repeated model work, but does not make agent execution,
tool outputs or required updates free. Automatic resumption after the session
ends requires a supported trigger; do not promise one merely because a process
was started in the background.

Requests such as "plow through all plans", "review the whole codebase", "check
everything" or "give detailed evidence" apply to that specific task. They do
not become standing instructions for later tasks unless the user explicitly
says so. "Continue" resumes the active task within its existing scope.

Agents must not expand the authority granted by the user or this repository's
instructions. Work only on the currently checked-out local branch. Do not
inspect or touch other branches or remotes, access Windows mounts, change
versions, publish, or deploy without the user's explicit authorization.
Coordinate file ownership and do not run parallel work that writes overlapping
files. Preserve existing user changes and inspect worktree status before edits.

On every return to an active task, the primary agent reads `AGENTS.md` and this
agreement, inspects the worktree and identifies unfinished work. Repeat after
a long break or resumed session. Subagents use their scoped task brief and the
applicable policy excerpts supplied by the primary agent.
