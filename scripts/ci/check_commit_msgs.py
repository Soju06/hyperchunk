#!/usr/bin/env python3
"""Commit-subject validator for the hyperchunk commit convention.

Grammar (formalized from the repo's actual history; normative text lives in
.github/CONTRIBUTING.md § Commit convention):

    subject    = component *( "+" component ) ":" SP text
    component  = type [ "(" scope ")" ]
    type       = feat | fix | perf | test | bench | viz | brand | docs
               | notes | chore | golden | build | ci
    scope      = [a-z0-9][a-z0-9_./+-]*      (free-form token; recommended
                                              values listed in CONTRIBUTING)
    text       = non-empty; English only (ASCII), enforced forward-only
                 (history up to 2026-08-20 is EN/KR mixed and is not
                 rewritten; CI only feeds this script the PR range)

Compound subjects ("bench(micro)+notes(bench): ...") are legal for a commit
that is genuinely one logic unit spanning two areas — rare by design.

Git-generated revert subjects ('Revert "..."', and 'Reapply "..."' from
reverting a revert, git >= 2.43) are exempt: tool-authored formats, not
convention violations. Merge commits are NOT exempted here — feed this
script `git log --no-merges` output (as CI and CONTRIBUTING both do) so
real merges never reach it; a hand-typed non-merge subject starting with
"Merge ..." is then correctly rejected.

Usage:
    git log --no-merges --format=%s BASE..HEAD | scripts/ci/check_commit_msgs.py --stdin
    scripts/ci/check_commit_msgs.py --self-test

Exit 0 when every subject conforms, 1 otherwise (offenders listed on stderr).
Stdlib only — no pip, no Node.
"""

import re
import sys

TYPES = (
    "feat|fix|perf|test|bench|viz|brand|docs|notes|chore|golden|build|ci"
)
SCOPE = r"[a-z0-9][a-z0-9_./+-]*"
COMPONENT = rf"(?:{TYPES})(?:\({SCOPE}\))?"
SUBJECT_RE = re.compile(rf"^{COMPONENT}(?:\+{COMPONENT})*: \S")

EXEMPT_RE = re.compile(r'^(Revert "|Reapply ")')
ASCII_RE = re.compile(r"^[\x20-\x7E]*$")


def check(subjects):
    # No `if s` guard: an empty subject (git commit --allow-empty-message)
    # is the most nonconforming subject possible and must be flagged.
    bad = [
        s
        for s in subjects
        if not EXEMPT_RE.match(s)
        and not (SUBJECT_RE.match(s) and ASCII_RE.match(s))
    ]
    for s in bad:
        print(f"NONCONFORMING: {s if s else '(empty subject)'}", file=sys.stderr)
    if bad:
        print(
            f"\n{len(bad)} commit subject(s) violate the convention "
            "'type(scope): subject' (English/ASCII only) -- see "
            ".github/CONTRIBUTING.md, Commit convention.",
            file=sys.stderr,
        )
    return not bad


def self_test():
    good = [
        "feat(noise): AVX-512 8-lane cone stream evaluator",
        "fix: off-by-one in zoom cache",
        "perf(simd): 2-way cone-stream interleave (P2-11)",
        "chore(git): untrack ctest Testing/ debris + ignore",
        "brand: v17 banner (ivory/dark) + mark SVG for README hero",
        "bench(micro)+notes(bench): B-4 closure verdict note",
        "test+build: split unit target",
        "ci: build/ctest/no-fma/sanitizer/commitlint workflows",
        "golden: recapture stages bundle (seed1234567890)",
        'Revert "feat(noise): AVX-512 8-lane cone stream evaluator"',
        'Reapply "feat(noise): AVX-512 8-lane cone stream evaluator"',
    ]
    bad = [
        "Fixed the thing",                       # no type prefix
        "feat(Noise): uppercase scope",          # scope must be lowercase
        "wip(light): half-done",                 # type not in the closed set
        "feat(noise):missing space",             # ': ' separator required
        "feat(noise): ",                         # empty text
        "refactor(core): rename",                # legacy type, not formalized
        "brand: v17 배너(ivory/dark)+마크 SVG — README 히어로 장착",  # Korean: English-only since 2026-08-20
        "docs(readme): de-slop pass — remove em dashes",  # non-ASCII punctuation
        "",                                      # empty subject (--allow-empty-message)
        "Merge branch 'main' into feat/x",       # merges are filtered upstream via --no-merges
    ]
    ok = True
    for s in good:
        if not (EXEMPT_RE.match(s) or (SUBJECT_RE.match(s) and ASCII_RE.match(s))):
            print(f"self-test FAIL (should pass): {s}", file=sys.stderr)
            ok = False
    for s in bad:
        if EXEMPT_RE.match(s) or (SUBJECT_RE.match(s) and ASCII_RE.match(s)):
            print(f"self-test FAIL (should fail): {s}", file=sys.stderr)
            ok = False
    print("self-test PASS" if ok else "self-test FAIL")
    return ok


def main():
    if "--self-test" in sys.argv:
        sys.exit(0 if self_test() else 1)
    if "--stdin" in sys.argv:
        subjects = [line.rstrip("\n") for line in sys.stdin]
        sys.exit(0 if check(subjects) else 1)
    print(__doc__, file=sys.stderr)
    sys.exit(2)


if __name__ == "__main__":
    main()
