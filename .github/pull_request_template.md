<!-- Keep it short. Sacrifice grammar for concision. Every line here exists because a PR once lacked it. -->

## What

Closes #

## Evidence

- Build: `Result: Succeeded` (paste the line; a worktree build adds `-NoHotReloadFromIDE`)
- Tests: `N test(s) run, N failed, N crashed.` (paste the line from `Run-AirsideTests.ps1`; crashed must be 0)
- Architecture check: `Check-Architecture.ps1` PASS (it runs first inside the test script)

## Refactor contract (delete this section if no code moved)

- `UE_LOG` count before/after: `N` -> `N` (a log line is a feature; see CLAUDE.md "Diagnosing")
- Comment lines before/after in moved files: `N` -> `N` (WHY comments travel with their code)
- Every `UFUNCTION` and interface virtual still reachable: yes / list what changed
- Seams introduced (delegate, forwarder, interface) and the test that fails if each is unwired:

## Runtime

Per CLAUDE.md "Claiming a fix": `Result: Succeeded` proves plausibility only.

- Verified in PIE / editor: yes (what you saw) / no - **builds, unverified at runtime**
- What the user should check (log line, key, visible behaviour):

🤖 Generated with [Claude Code](https://claude.com/claude-code)
