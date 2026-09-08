#!/usr/bin/env python3
"""Regenerate plm_k8s/plm_k8s_default_template.c from the shipped template.

plm/k8s falls back to a built-in template when the configured file does not
exist, which means the default has to exist twice: once as a file anyone can
copy and edit (templates/single-job.yaml.tmpl) and once as a C string linked
into the component. Rather than keeping the two in sync by hand, the C copy is
generated from the file by this script and committed alongside it.

    ./contrib/embed-default-template.py

Run it after editing templates/single-job.yaml.tmpl. Reviewers (and
test/run-tests.sh) check it was run by re-running it and diffing.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "templates", "single-job.yaml.tmpl")
DST = os.path.join(ROOT, "plm_k8s", "plm_k8s_default_template.c")

HEADER = '''/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * GENERATED FILE - do not edit.
 *
 * The built-in fallback manifest template, embedded from
 * templates/single-job.yaml.tmpl by contrib/embed-default-template.py.
 * Edit that template and re-run the script; edits made here are lost.
 */

#include "prte_config.h"

#include "plm_k8s_template.h"

'''


def main():
    with open(SRC) as fp:
        lines = fp.read().split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    body = ["const char prte_plm_k8s_default_template[] ="]
    for line in lines:
        escaped = line.replace("\\", "\\\\").replace('"', '\\"')
        body.append('    "%s\\n"' % escaped)
    body[-1] += ";"
    with open(DST, "w") as fp:
        fp.write(HEADER + "\n".join(body) + "\n")
    print("wrote %s (%d template lines)" % (DST, len(lines)))


if __name__ == "__main__":
    sys.exit(main())
