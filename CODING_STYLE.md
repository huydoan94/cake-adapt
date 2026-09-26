# Reusable Coding and Engineering Style

Use this guide as the default for implementation, refactoring, review, and
testing. Project-specific architecture and repository instructions take
precedence when they are more restrictive.

## Core principles

- Prefer simple, explicit data flow over clever abstractions.
- Use standard-library, operating-system, and established ecosystem APIs before
  writing custom equivalents.
- Add a dependency only when it materially removes complex, error-prone code
  and is available on every supported target.
- Search for an existing equivalent before adding a function.
- Consolidate repeated parsing, conversion, timing, percentage, and bounds
  logic in the module that owns the concept.
- Avoid one-use wrappers, unnecessary layers, and defensive checks for states
  already made impossible by validated internal call paths.
- Retain checks for real external failures: invalid input, allocation failure,
  I/O failure, malformed data, timeouts, and changing system state.
- Make the smallest coherent change that fully solves the problem.

## Architecture and ownership

- Keep the main execution pipeline explicit and easy to trace.
- Give each module one clear responsibility.
- Separate measurement, policy, desired state, and external side effects.
- Keep platform-independent policy free of filesystem, process, socket,
  configuration, and operating-system details so it can be unit-tested with
  synthetic input.
- Keep transport and serialization modules focused on mechanics, not policy.
- Keep orchestration in one place instead of distributing lifecycle decisions
  across low-level modules.
- Keep the source tree flat until a subsystem genuinely needs its own
  directory.
- Make resource ownership explicit: the code that acquires a resource must have
  a clear cleanup path.

## Libraries and dependencies

- Prefer maintained platform APIs and libraries over spawning command-line
  programs or reimplementing their parsers.
- Prefer event subscriptions and callbacks over periodic polling when the
  platform already exposes lifecycle events.
- Confirm target availability, binary-size cost, memory cost, and maintenance
  impact before adding a library.
- Do not add a library for a trivial operation that is already clear and safe.
- Remove dependencies when their final use disappears.

## C language conventions

- Use C11, explicit ownership, and straightforward control flow.
- Keep strict compiler warnings enabled:

```text
-Wall -Wextra -Wpedantic -Wformat=2 -Wshadow -Wconversion -Werror
```

- Disable a warning only for a narrow, documented reason.
- Validate untrusted data at the boundary, then rely on the validated invariant
  inside the program.
- Use types that express units and ranges accurately. Check conversions before
  narrowing.
- Bound every operation that can wait. Never wait forever for I/O, a child
  process, a device, a lock, or a reply.
- Treat optional subsystem failure as degraded operation when useful work can
  continue. Reserve fatal errors for states where continuing would be wrong.
- Keep generated object and dependency files in `build/`, never beside source
  files.

## Naming

Use concise, module-oriented names:

```c
config_load();
controller_update();
tracker_update();
log_message();
```

- Do not blanket-prefix every function, type, or constant with the application
  name.
- Add a prefix only to prevent a concrete collision or ambiguity.
- Keep module-specific helpers local and `static`.
- Put genuinely shared helpers in an existing generic helper module.
- Prefer names that include the unit when it is not obvious, such as
  `timeout_microseconds` or `rate_bits_per_second`.
- Do not repeat the program name in every log message when the logging backend
  already identifies it.

## Constants and defaults

- Put shared semantic string values—names, paths, modes, states, and protocol
  tokens—in a constants header.
- Put built-in application and configuration defaults in dedicated defaults
  files.
- Keep complete diagnostics, format strings, and module-owned schemas beside
  the code that uses them.
- Do not convert every literal into a global constant. Local values that have
  no shared semantic meaning should remain local.
- Tests may use literal expected values when importing the production constant
  would make the test tautological.

## Formatting

- Use four spaces and no tabs in C.
- Keep ordinary declarations on one line.
- Do not impose an arbitrary 80-column limit; wrap for readability.
- Keep a simple condition on one line.
- Format a genuinely compound condition like this:

```c
if (
    result < 0 ||
    operation(context) < 0
) {
    return -1;
}
```

- Keep return-only blocks in normal multiline form; do not compress the entire
  `if` statement onto one line.
- For declarations, definitions, and calls with several substantial arguments,
  put one argument on each line:

```c
int controller_update(
    struct controller *controller,
    const struct controller_input *input,
    struct controller_output *output
)
{
```

```c
result = operation(
    context,
    &input,
    &output
);
```

- Short, obvious calls may stay on one line.
- Comments should explain formulas, invariants, units, ownership, or non-obvious
  platform behavior. Do not narrate obvious syntax.
- Do not reformat unrelated working code during a focused change.

## Configuration and input

- Use the platform's supported configuration library instead of handwritten
  parsing when one is available.
- Keep configuration typed from loading through validation.
- Validate the complete configuration before enabling side effects.
- Check relationships between values, not just each value independently.
- Keep shipped configuration safe, readable, grouped by purpose, and explicit
  about required values.
- Keep optional settings documented with accurate defaults.
- Never commit developer-specific paths, usernames, credentials, or secrets.

## Logging and diagnostics

- Route application logging through one logging module.
- Always expose startup, shutdown, disabled configuration, invalid
  configuration, missing required resources, degraded operation, and failed
  external changes through the normal operational log.
- Keep high-frequency measurement records optional.
- Preserve stable record names, field order, and units when compatibility with
  external analysis tools matters.
- When modifying external state, read it back or otherwise verify the effective
  result when practical.
- Preserve live log-file inode continuity during reset and rotation so existing
  followers remain attached.

## Testing

- Add or update focused tests for every behavioral change.
- Cover normal behavior, boundaries, state transitions, invalid and missing
  samples, counter resets, external resource disappearance, and recovery.
- Keep policy directly testable with synthetic inputs.
- Do not weaken production code to accommodate a host test environment. Add a
  test seam, mock, adapter, dependency, or target integration test instead.
- Use the narrowest useful test command; do not trigger a full toolchain build
  for a host-only change.
- Bound integration-test transfers and waits.
- Before mutating a test system, record the original process, package,
  configuration, file, and kernel state needed for exact restoration.
- Confirm the test exercised the intended path and produced real data; process
  existence alone is not sufficient evidence.
- After testing, stop test-owned processes, remove only test-created data, and
  restore the exact original state.

Before declaring work complete:

1. inspect existing behavior and relevant upstream behavior;
2. make the smallest coherent change;
3. run focused tests;
4. build the affected target when appropriate;
5. run integration tests when runtime or platform behavior changed; and
6. verify the actual process, log, output, and external state.

## Change and release discipline

- Inspect version-control status before editing and preserve unrelated changes.
- Verify a surprising observation once before changing known-working code.
- Keep changes targeted and reviewable; do not mix unrelated refactoring into a
  feature or bug fix.
- During an explicit cleanup pass, remove duplication and unreachable code but
  retain guards for genuine external failures.
- Do not silently change architecture, defaults, compatibility, or public
  behavior.
- Do not commit generated files, machine-specific files, or secrets.
- Do not bump versions, copy artifacts, deploy, publish, or commit unless the
  task explicitly requests it.
- When a version is bumped, update every versioned artifact reference in the
  same change.
- Verify release artifacts and their destinations with checksums.
- Avoid destructive version-control or filesystem commands unless their exact
  scope is requested and verified.
