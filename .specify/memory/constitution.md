<!--
SYNC IMPACT REPORT
==================
Version change: 1.0.0 → 1.1.0
Modified principles: N/A
Added sections:
  - Principle VI: Resiliency & Performance (new)
  - Quality Gates: resiliency gate added (gate 6)
Removed sections: N/A
Templates reviewed:
  - .specify/templates/plan-template.md        ✅ Constitution Check section present; no updates needed
  - .specify/templates/spec-template.md        ✅ No constitution-specific references; no updates needed
  - .specify/templates/tasks-template.md       ✅ Phase structure aligned with principles; no updates needed
  - .specify/templates/checklist-template.md   ✅ Generic; no updates needed
  - .specify/templates/agent-file-template.md  ✅ No constitution references; no updates needed
Deferred TODOs:
  - TODO(RATIFICATION_DATE): Project just initialized; no formal adoption date recorded yet.
    Resolve by setting the date when the team formally adopts this constitution.
  - TODO(PROJECT_DOMAIN): "fre" expansion unknown — no README or source code present.
    Update Principle I rationale once project purpose is documented.
-->

# fre-framework Constitution

## Core Principles

### I. Specification-First

Every feature MUST begin with a written specification (`spec.md`) created via the
`/speckit.specify` workflow before any implementation work starts. The specification
MUST include user stories with acceptance scenarios and measurable success criteria.

**Rationale**: Specs prevent wasted implementation effort, create a shared understanding
across contributors, and provide a reference for code review and testing decisions.
Implementation without a spec is a constitution violation and MUST be rejected in review.

### II. Test-First (NON-NEGOTIABLE)

Tests MUST be written and confirmed failing before the corresponding implementation is
written. The Red → Green → Refactor cycle is strictly enforced. No PR that adds
implementation without accompanying tests (where tests are applicable) MUST be merged.

**Rationale**: Test-first development catches design problems early, guarantees behavior
is specified before coded, and prevents regression debt from accumulating silently.

### III. Minimal Dependencies

The framework MUST keep its dependency footprint as small as possible. Every external
dependency requires explicit justification: what problem it solves that cannot be solved
with reasonable in-house code. Transitive dependencies count toward this surface area.

**Rationale**: Frameworks impose their dependency graph on all consumers. Unnecessary
dependencies increase attack surface, bundle size, version conflict risk, and long-term
maintenance burden.

### IV. Backward Compatibility & Semantic Versioning

The project MUST follow Semantic Versioning (`MAJOR.MINOR.PATCH`):
- PATCH: Bug fixes with no API changes.
- MINOR: New functionality that is backward-compatible.
- MAJOR: Breaking changes to any public API or governance principle.

Breaking changes MUST include a migration guide. Public APIs MUST NOT be removed or
changed incompatibly within the same MAJOR version.

**Rationale**: Consumers of a framework depend on API stability. Surprise breakage
erodes trust and causes cascading failures in downstream projects.

### V. Simplicity



The simplest solution that satisfies the specification MUST be chosen. YAGNI (You Aren't
Gonna Need It) applies at every level: architecture, API design, and implementation.
Complexity MUST be justified in the plan's Complexity Tracking table.

**Rationale**: Accidental complexity is the primary source of maintenance cost and bugs.
The framework's value is in what it enables for consumers, not in its own cleverness.

### VI. Resiliency & Performance

**Latency budget**: The P99 latency as observed by the caller MUST remain under 300ms
for all public operations. Latency budgets MUST be allocated per-layer in the plan and
verified with load tests before merge.

**Synchronous I/O boundary**: Synchronous (blocking) calls are ONLY permitted to
directly-owned data infrastructure (databases, caches, object stores). Calls to any
other service MUST be asynchronous or event-driven. Synchronous inter-service calls
are a constitution violation and MUST be rejected in review.

**Shuffle sharding**: Every resource allocation that touches multiple tenants MUST
apply shuffle sharding to limit blast radius. No single failure domain is permitted
to affect all tenants simultaneously. The sharding strategy MUST be documented in the
feature's `plan.md`.

**Noisy-tenant isolation**: Features that serve multiple tenants MUST include an
explicit strategy for asymmetric load — e.g., per-tenant rate limiting, work queues
with tenant-scoped concurrency caps, or fair-share scheduling. The chosen strategy
MUST be documented and justified in the plan.

**Failure mode analysis**: Before implementation begins, the plan MUST include a
failure mode analysis covering: (a) what can fail, (b) the blast radius of each
failure, and (c) the chosen graceful-degradation strategy (e.g., circuit breaking,
fallback responses, partial results, shed load). Features without a documented
degradation path MUST NOT be merged.

**Rationale**: Distributed systems fail in partial, asymmetric, and cascading ways.
Designing for these conditions after the fact is prohibitively expensive. Shuffle
sharding and noisy-tenant isolation protect all tenants from the worst-case behaviour
of any single tenant. Failure mode analysis ensures degraded states are first-class
design outcomes, not afterthoughts.

## Development Standards

- **Speckit workflow**: All features MUST use the `/speckit.specify` → `/speckit.plan`
  → `/speckit.tasks` → `/speckit.implement` pipeline.
- **Branching**: Feature work MUST happen on a dedicated branch named `###-feature-name`.
- **Commit hygiene**: Each commit MUST represent a single logical change; message format
  is `type: short description` (types: feat, fix, docs, test, refactor, chore).
- **Language/stack**: TODO(PROJECT_DOMAIN): Define once the technology stack is decided.
  Update this section with language version, primary dependencies, and build tooling.
- **Code review**: Every PR MUST be reviewed by at least one other contributor before
  merging. The reviewer MUST verify constitution compliance as part of the review.

## Quality Gates

The following gates MUST pass before any feature branch is merged:

1. **Spec gate**: `spec.md` exists, is approved, and acceptance scenarios are present.
2. **Test gate**: All tests pass; new code has corresponding tests that were written first.
3. **Dependency gate**: No new external dependency was added without documented
   justification in the plan's Technical Context section.
4. **Versioning gate**: If any public API changed, the version bump type is correctly
   identified and the changelog is updated.
5. **Simplicity gate**: Complexity Tracking table in `plan.md` is filled for any
   architecture decision that deviates from the simplest obvious approach.
6. **Resiliency gate**: `plan.md` contains a failure mode analysis; shuffle sharding
   and noisy-tenant strategies are documented; P99 latency budget is defined and load
   tests exist or are explicitly scoped in tasks.

## Governance

This constitution supersedes all other development practices and guidelines for this
project. Where conflict exists between the constitution and any other document, the
constitution takes precedence.

**Amendment procedure**:
1. Open a PR with proposed changes to this file and a draft Sync Impact Report.
2. All active contributors MUST review and approve the amendment.
3. Increment `CONSTITUTION_VERSION` per semantic versioning rules defined in Principle IV.
4. Update `LAST_AMENDED_DATE` to the merge date.
5. Propagate changes to affected templates (checklist in Sync Impact Report).

**Compliance reviews**: Constitution compliance MUST be verified at each PR review.
Violations discovered post-merge MUST be logged as issues and remediated in the next
sprint.

**Runtime guidance**: Use the agent file generated by `/specify update-agent-context`
for up-to-date technology stack and project structure guidance during development.

**Version**: 1.1.0 | **Ratified**: TODO(RATIFICATION_DATE) | **Last Amended**: 2026-03-15
