# 02 — Fork & branch setup

## Done

Working repo: `/Users/abhakash/PersonalWS/rpcs3`

Remotes (changed):
```
origin    https://github.com/hsakaminusone/rpcs3.git   (fetch + push)   ← now the working fork
upstream  https://github.com/RPCS3/rpcs3.git           (fetch + push)   ← was "origin"
```

Actions taken:
1. `git remote rename origin upstream`
2. `git remote add origin https://github.com/hsakaminusone/rpcs3.git`
3. `git fetch origin --prune` then `git fetch upstream --prune`
4. Fast-forwarded `master` `4d5def588 → 17543d273`, set `master` to track `origin/master`
5. Created working branch **`kosmickrisp-integration`** off `master` (`17543d273`)

State:
- `origin/master` (fork) == `upstream/master` == `17543d273` — fork is in sync, 0 ahead / 0 behind.
- Branch `kosmickrisp-integration` currently has **no commits of its own** (docs added on top).

Fork facts: public, `fork: true`, parent `RPCS3/rpcs3`, default branch `master`, only branch = `master`.

## Not done yet (intentionally — no code changes)

- Nothing pushed to the fork.
- Branch not opened as a PR.

## Suggested conventions

- Keep `master` fast-forwarded from `upstream/master`; never commit on it.
- Do integration work on `kosmickrisp-integration` (or split: `kk-icd-support`, `kk-ci`, …).
- Push to `origin` (fork) only; open PRs against `RPCS3/rpcs3` from the fork if upstreaming.
