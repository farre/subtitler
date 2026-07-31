---
name: github-issue-context
description: Gather context from GitHub issues and repo metadata (labels, milestones, releases) in farre/subtitler. Use ONLY when the user explicitly references a GitHub issue by number or URL (e.g. "work on #3", "fix issue 5", a github.com/farre/subtitler/issues/... link).
---

# GitHub issue context

Gather context from GitHub when the user explicitly references an issue.

## Repository

`farre/subtitler` — `gh` auto-detects it from the `origin` remote when run
inside the working directory.

## Steps

1. Extract the issue number from the user's message: `#N`, "issue N", or a
   `github.com/farre/subtitler/issues/N` URL.
2. Fetch the issue, always with comments:

   ```sh
   gh issue view <N> --comments
   ```

3. Fetch repo metadata only when it adds context to the task at hand:
   - `gh label list` — available labels
   - `gh issue list --milestone <name>` — sibling issues in the same milestone
   - `gh release list` / `gh release view <tag>` — release notes
4. Summarize the gathered context (goal, constraints from comments, relevant
   labels/milestone) before starting the actual work.

## Boundaries

- Read-only. Never run `gh` commands that mutate GitHub state
  (`gh issue create/close/comment/edit`, `gh pr *`, ...) unless the user
  explicitly asks.
- `gh issue view*` is pre-allowed in `opencode.json`; other `gh` subcommands
  trigger a permission prompt. That's expected — don't work around it.
