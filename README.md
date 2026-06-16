# snapshot-previews

Auto-managed branch carrying overlay snapshot diff PNGs referenced from
PR comments posted by `build-and-release.yml` on snapshot test failure.

Each `pr-<n>/run-<id>-attempt-<k>/` folder corresponds to one snapshot
test failure on a PR and contains `old.png` (the committed golden at
the time the test ran) plus `new.png` (the fresh render that failed
comparison).

Folders under a `pr-<n>/` prefix for closed PRs are safe to garbage
collect — no code references them once the PR is merged or closed.

Do not commit anything to this branch by hand.
