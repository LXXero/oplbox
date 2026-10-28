#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Test of tests/mutation.py, the mutation checks' choice of mutants: every mutant when OPLBOX_MUTANTS is unset; in
quick mode (OPLBOX_MUTANTS=changed) none when nothing changed, a changed target file's mutants, the whole set when its
test changed; a mutant whose text is not in its file exactly once caught in both modes; results kept in mutant order.

    mutation_test.py      exit 1 on a failure
"""
import contextlib
import io
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mutation

A, B = "tests/limits.py", "tests/mutation.py"           # files that exist, with text that is in them exactly once
SPECS = [(A, "MAX_PARALLEL = 4"), (A, "MEM_MB = 512"), (B, "def summary(label, total, ran):"),
         (B, "text that is not in the file")]
HARNESS = ("tests/some_test.c", "tests/some_dir/")
failed = 0


def check(ok, what):
    global failed
    print("%s: %s" % ("ok" if ok else "FAIL", what))
    failed += not ok


def plan(mode, paths):
    os.environ.pop("OPLBOX_MUTANTS", None)
    if mode:
        os.environ["OPLBOX_MUTANTS"] = mode
    mutation.changed = lambda: paths
    mutation.SHARED = ()                                       # this file's own change must not decide the cases
    with contextlib.redirect_stdout(io.StringIO()) as said:
        todo, stale = mutation.plan("test", SPECS, HARNESS)
    return todo, stale, said.getvalue()


todo, stale, _ = plan(None, set())
check(todo == [0, 1, 2] and stale == {3}, "unset: every mutant, the stale one reported (%s, %s)" % (todo, stale))
todo, stale, said = plan("changed", set())
check(todo == [] and stale == {3} and "0 of 4 run" in said and "3 skipped" in said,
      "quick, nothing changed: no mutant runs, the stale one still reported (%s, %s, %r)" % (todo, stale, said))
todo, stale, said = plan("changed", {A, "README.md"})
check(todo == [0, 1] and stale == {3} and A in said, "quick, %s changed: its two mutants (%s, %r)" % (A, todo, said))
todo, _, said = plan("changed", {"tests/some_test.c"})
check(todo == [0, 1, 2] and "every mutant runs" in said, "quick, the set's test changed: every mutant (%s)" % todo)
todo, _, _ = plan("changed", {"tests/some_dir/model.c"})
check(todo == [0, 1, 2], "quick, a file in the set's test directory changed: every mutant (%s)" % todo)
todo, _, said = plan("changed", None)
check(todo == [0, 1, 2] and "cannot compare" in said, "quick, git cannot say: every mutant (%s)" % todo)
res = mutation.run([3, 0, 2], lambda n: n * 10)
check(list(res) == [3, 0, 2] and res == {3: 30, 0: 0, 2: 20}, "run: one result per mutant asked for (%s)" % res)
check(mutation.summary("x", 5, 5) == "x: all 5 caught", "summary of a full run")
check("2 skipped (quick mode)" in mutation.summary("x", 5, 3), "summary of a quick run says what was skipped")
check(mutation.summary("x", 5, 0) == "x: none run; 5 skipped (quick mode), not yet checked", "summary of a quick run with none")
os.environ["OPLBOX_MUTANTS"] = "chnaged"
try:
    mutation.quick()
    check(False, "a misspelt OPLBOX_MUTANTS is refused")
except SystemExit:
    check(True, "a misspelt OPLBOX_MUTANTS is refused")
if failed:
    sys.exit("FAIL: %d check(s)" % failed)
print("mutation: all checks passed")
