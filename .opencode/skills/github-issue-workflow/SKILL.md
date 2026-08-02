---
name: github-issue-workflow
description: Workflow for implementing or fixing GitHub issues in farre/subtitler — fetch the issue, clarify requirements, post a requirements comment, implement, commit with a closing keyword. Use ONLY when the user explicitly references a GitHub issue by number or URL (e.g. "work on #3", "fix issue 5", a github.com/farre/subtitler/issues/... link).
---

# GitHub issue workflow

The user naming an issue is standing authorization for this entire sequence,
including commenting on the issue and committing. Other GitHub mutations
(closing issues, PRs, labels, ...) still need an explicit request.

## Repository

`farre/subtitler` — `gh` auto-detects it from the `origin` remote when run
inside the working directory.

## Workflow

1. Extract the issue number from the user's message: `#N`, "issue N", or a
   `github.com/farre/subtitler/issues/N` URL.
2. Fetch the issue:

   ```sh
   gh issue view <N>
   ```

   Do NOT use `--comments` — it can silently produce no output at all. The
   plain view shows the comment count; when comments actually need reading,
   use:

   ```sh
   gh issue view <N> --json comments
   ```

3. Fetch repo metadata only when it adds context to the task at hand:
   - `gh label list` — available labels
   - `gh issue list --milestone <name>` — sibling issues in the same milestone
   - `gh release list` / `gh release view <tag>` — release notes
4. If the issue doesn't provide enough to work with, ask the user questions
   until it does.
5. Post the resulting understanding/requirements as a comment on the issue:

   ```sh
   gh issue comment <N> --body "..."
   ```

6. Iterate on the implementation/fix; add tests where applicable.
7. When done, commit. The commit message must end in a paragraph of the
   exact form `Closes #<N>` — GitHub's auto-close keyword. Other phrasings
   ("Closes issue #N") only cross-reference without closing.

## Notes

- `gh issue view*` is pre-allowed in `opencode.json`; other `gh` subcommands
  trigger a permission prompt. That's expected — don't work around it.
