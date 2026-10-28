# SPDX-License-Identifier: GPL-3.0-only
"""What the mutation checks (tests/*_mutants.py) share: which mutants to run, and running them at most
limits.workers() at a time, each in its own directory, with the results kept in mutant order.

Two modes, chosen by OPLBOX_MUTANTS:

    unset (the default)   every mutant. This is what a merge and anyone plainly running tests/run_host.sh get.
    changed (quick mode)  for build rounds: only the mutants whose target file differs from the base, and every
                          mutant of a set whose test, model or mutant script changed. The base is OPLBOX_MUTANTS_BASE
                          (default main); "differs" is git diff <base>...HEAD plus uncommitted and untracked files.
                          The full set still has to run before the work is merged.

In both modes every mutant's text is checked against its file (it must be there exactly once): that check is cheap,
and a mutant whose code moved is a failure whatever changed.

    mutation.plan(label, specs, harness)    specs: one (repo path, text) per mutant; harness: repo paths of the set's
                                            test, models and script ("dir/" for a whole directory).
                                            -> (indices to run, set of indices whose text is not there exactly once)
    mutation.run(todo, one)                 one(n) for each n in todo, in parallel -> {n: one(n)}
    mutation.summary(label, total, ran)     "<label>: all N caught", or in quick mode how many ran and were skipped
"""
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import limits

ROOT = Path(__file__).resolve().parent.parent
SHARED = ("tests/mutation.py", "tests/limits.py")          # a change here re-runs every set


def quick():
    mode = os.environ.get("OPLBOX_MUTANTS", "")
    if mode not in ("", "changed"):
        sys.exit("OPLBOX_MUTANTS=%r: use 'changed' (quick mode) or leave it unset (every mutant)" % mode)
    return mode == "changed"


def changed():
    """Repo paths that differ from the base (committed since the merge base, uncommitted, untracked); None if git can't say."""
    base = os.environ.get("OPLBOX_MUTANTS_BASE", "main")
    paths = set()
    for cmd in (["diff", "--name-only", base + "...HEAD"], ["diff", "--name-only", "HEAD"],
                ["ls-files", "--others", "--exclude-standard"]):
        r = subprocess.run(["git", *cmd], cwd=ROOT, capture_output=True, text=True)
        if r.returncode:
            return None
        paths.update(p for p in r.stdout.splitlines() if p)
    return paths


def _hit(paths, wanted):
    return [w for w in wanted if (w in paths if not w.endswith("/") else any(p.startswith(w) for p in paths))]


def plan(label, specs, harness):
    stale = {n for n, (path, text) in enumerate(specs) if (ROOT / path).read_text().count(text) != 1}
    every = [n for n in range(len(specs)) if n not in stale]
    if not quick():
        return every, stale
    paths = changed()
    if paths is None:
        print("%s: quick mode, but git cannot compare with %s: running every mutant"
              % (label, os.environ.get("OPLBOX_MUTANTS_BASE", "main")))
        return every, stale
    why = _hit(paths, (*SHARED, *harness))
    if why:
        print("%s: quick mode: %s changed, so every mutant runs" % (label, ", ".join(why)))
        return every, stale
    todo = [n for n in every if specs[n][0] in paths]
    files = sorted({specs[n][0] for n in todo})
    print("%s: quick mode (OPLBOX_MUTANTS=changed): %d of %d run%s; %d skipped, the full set runs at merge "
          "(tests/run_host.sh with OPLBOX_MUTANTS unset)"
          % (label, len(todo), len(specs), " (" + ", ".join(files) + " changed)" if files else "",
             len(specs) - len(todo) - len(stale)))
    return todo, stale


def run(todo, one):
    with ThreadPoolExecutor(limits.workers()) as ex:
        return dict(zip(todo, ex.map(one, todo)))


def summary(label, total, ran):
    if ran == total:
        return "%s: all %d caught" % (label, total)
    if not ran:
        return "%s: none run; %d skipped (quick mode), not yet checked" % (label, total)
    return "%s: all %d run caught; %d skipped (quick mode), not yet checked" % (label, ran, total - ran)
