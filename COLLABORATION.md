# Collaboration Roles and Workflow

This agreement governs agent roles and handoffs for work in this repository.
`AGENTS.md` and the user's instructions define project constraints and scope.

## Shared roles

- The primary agent owns diagnosis, evidence review, plans, acceptance criteria,
  file ownership, test requirements, final acceptance, and the user-facing
  explanation.
- An implementation agent carries out assigned changes, tests, builds, and VM
  evidence collection.
- A separate review agent reviews the final diff
  for logic gaps, dead code, duplication, and existing helpers or libraries.
  The implementer addresses review findings; the primary agent makes final
  acceptance.

### Claude workflow

For a Claude primary, implementation and review agents use the primary's own
model version (currently Claude Opus 5.5). If that version is unavailable, report
the limitation and use any alternative the user has already authorized. Without
such authorization, wait for direction instead of substituting another model.

### Codex workflow

For a Codex primary, use the current primary model for delegated implementation
and review. Leave model overrides unset so the agents inherit that model; do not
require Claude tools or Claude Opus 5.5. The user's instruction to "make your own
version" authorizes this route. Do not ask again merely because Claude is absent.

For substantial work, use one implementer and one separate bounded reviewer when
delegation is available. Start them with `fork_turns="none"` and a short task
brief. The primary can directly handle small changes, including focused
documentation updates. If delegation is unavailable, implement directly and
perform a separate review of the final diff against the acceptance criteria.
Report that review as self-review; do not claim independent review occurred.

Provider-specific tools and skills apply only when available and relevant. Use
the corresponding Codex tools for filesystem changes, commands and agent
handoffs. Tool differences do not relax project rules, authorization, coding
style or verification requirements. If a required capability is missing, finish
independent authorized work and report the specific remaining limitation.

## Context and usage budget

The primary agent retains the broader project context. Start delegated work as a
fresh subagent (not a fork of the conversation) with a path to its task brief;
do not send the conversation history. Do not have agents independently rediscover the primary agent's diagnosis.
For delegated work, use one implementation agent and one bounded review by
default, with no nested delegation or overlapping audits unless the task needs
them. The provider workflows above define when direct work is appropriate.

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
record the remaining limits. Preserve the VM timeout, log-inode, change
recording and deployment rules in `AGENTS.md`.

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

## Persistent context and authorization

At session start, after a long break, or on resumption after lost context, read
`AGENTS.md`, this agreement and `HANDOFF.md`, inspect the worktree, and identify
unfinished work. Read `CODING_STYLE.md` and the documentation relevant to the
task before editing. During uninterrupted work, reuse established context and
unchanged checks instead of repeatedly rereading the same files. Subagents use
their scoped brief and the supplied policy excerpts.

`AGENTS.md` holds standing project rules; this file holds provider workflows;
`HANDOFF.md` holds the active task, user decisions, scoped approvals, changed
files, evidence, limitations and next steps. Keep the handoff current at a stop
point, scope change or completed milestone. Verify its worktree and environment
claims before relying on them. Do not require the user to reconstruct a chat or
maintain an agent's private memory. A private memory update is a separate action
and requires the user's explicit request.

Development, SDK validation, host-test headers, IntelliSense and VM testing
default to x86. Filogic or arm64 work requires an explicit target-specific
request. The user's router architecture does not change these defaults.

Before a new download, SDK/feed change or installation, check existing approvals
and the proposed dependency, destination and side effects. Continue within an
already authorized scope without asking again. If the scope is new, request
approval before downloading or changing that environment, after completing any
independent work. A build command can download dependencies or modify SDK state;
inspect its effects first. Keep approved local validation dependencies and tool
caches under ignored `build/` unless another destination was explicitly approved.
Record what was changed, where it lives and whether it remains. Approval for
one SDK or dependency does not authorize another SDK, a system installation,
artifact copying or deployment.
