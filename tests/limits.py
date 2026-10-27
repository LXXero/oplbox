# SPDX-License-Identifier: GPL-3.0-only
"""Resource caps for the host test scripts that start many processes (mutation checks). A mutant can loop and keep
allocating; two builds running it at once, one process per CPU core with no memory limit, ran a host out of memory on
2026-10-26. So: at most MAX_PARALLEL at a time, and each process gets an address-space limit.

    limits.workers()                        how many to run at once: min(4, cpu_count)
    limits.run(cmd, mem_mb=..., **kw)       subprocess.run with RLIMIT_AS = mem_mb; a process killed by the limit shows as a
                                            failed run (non-zero exit or signal), which a mutation check counts as caught
"""
import os
import resource
import subprocess

MAX_PARALLEL = 4
MEM_MB = 512


def workers():
    return max(1, min(MAX_PARALLEL, os.cpu_count() or 1))


def run(cmd, mem_mb=MEM_MB, **kw):
    limit = mem_mb * 1024 * 1024

    def cap():
        resource.setrlimit(resource.RLIMIT_AS, (limit, limit))
    return subprocess.run(cmd, preexec_fn=cap, **kw)
