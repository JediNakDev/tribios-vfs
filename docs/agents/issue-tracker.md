# Issue tracker: GitHub

Issues and specifications for this repository live as GitHub issues.
Use the `gh` CLI for all operations.

## Conventions

- **Create an issue**: `gh issue create --title "..." --body "..."`.
  Use a heredoc for multiline bodies.
- **Read an issue**: `gh issue view <number> --comments`.
  Fetch labels and filter comments with `jq` when needed.
- **List issues**: `gh issue list --state open --json number,title,body,labels,comments --jq '[.[] | {number, title, body, labels: [.labels[].name], comments: [.comments[].body]}]'`.
  Apply appropriate `--label` and `--state` filters.
- **Comment on an issue**: `gh issue comment <number> --body "..."`.
- **Apply or remove labels**: `gh issue edit <number> --add-label "..."` or `gh issue edit <number> --remove-label "..."`.
- **Close an issue**: `gh issue close <number> --comment "..."`.

Infer the repository from `git remote -v`.
The `gh` CLI does this automatically inside the clone.

## Pull requests as a triage surface

**PRs as a request surface: no.**

Set this flag to `yes` if the repository treats external pull requests as feature requests.
The `/triage` skill reads this flag.

When enabled, pull requests use the same labels and states as issues:

- **Read a pull request**: Run `gh pr view <number> --comments` and `gh pr diff <number>`.
- **List external pull requests for triage**: Run `gh pr list --state open --json number,title,body,labels,author,authorAssociation,comments`.
  Keep only `CONTRIBUTOR`, `FIRST_TIME_CONTRIBUTOR`, or `NONE` author associations.
- **Comment, label, or close**: Use `gh pr comment`, `gh pr edit`, or `gh pr close`.

GitHub shares one number space across issues and pull requests.
Resolve an ambiguous reference such as `#42` with `gh pr view 42`, then fall back to `gh issue view 42`.

## Publishing and fetching tickets

When a skill says to publish to the issue tracker, create a GitHub issue.

When a skill says to fetch a ticket, run `gh issue view <number> --comments`.

## Wayfinding operations

The `/wayfinder` skill represents a map as one issue and its tickets as child issues.

- **Map**: Create one issue with the `wayfinder:map` label.
  Its body holds Notes, Decisions so far, and Fog.
- **Child ticket**: Link an issue to the map as a GitHub sub-issue through `gh api`.
  If sub-issues are unavailable, add the child to a task list in the map and place `Part of #<map>` at the top of the child body.
  Apply a `wayfinder:<type>` label, where the type is `research`, `prototype`, `grilling`, or `task`.
- **Blocking**: Prefer GitHub's native issue dependencies.
  Add an edge with `gh api --method POST repos/<owner>/<repo>/issues/<child>/dependencies/blocked_by -F issue_id=<blocker-db-id>`.
  Obtain the numeric database ID with `gh api repos/<owner>/<repo>/issues/<number> --jq .id`.
  If dependencies are unavailable, add `Blocked by: #<number>` to the child body.
- **Frontier query**: List the map's open children and discard assigned tickets or tickets with open blockers.
  The first remaining ticket in map order is the frontier.
- **Claim**: Run `gh issue edit <number> --add-assignee @me` as the session's first write.
- **Resolve**: Comment with the answer, close the ticket, and append a short linked summary to the map's Decisions so far.
