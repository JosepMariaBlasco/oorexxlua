#!/usr/bin/env bash
# setup-oorexxunit.sh: fetch ooRexx's own test framework (ooRexxUnit) into
# $ORXLUA_WORK/oorexxunit/ (ORXLUA_WORK: the directory that contains the clone),
# at a fixed SVN revision.  It is not part of this repository: it is CPL 1.0
# (ooRexx's licence) and is obtained on demand, as ooRexx.
#
#   OOREXXUNIT_REV  SVN revision of test/trunk (default 13344: the family pins
#                   ooRexx's tests there, its binaries at r13344; CONVENTIONS.md)
#   OOREXXUNIT_DIR  target directory (default $ORXLUA_WORK/oorexxunit)
#
# Files: OOREXXUNIT.CLS (test/trunk/framework), ooTest.frm, worker.rex and
# testOORexx.rex (test/trunk).  Used by tests/run.sh.  Fetched with curl over
# SourceForge's Subversion HTTP interface (the same bytes as `svn export`), so
# no Subversion client is needed.
set -euo pipefail
WORK=${ORXLUA_WORK:-$(cd "$(dirname "$0")/../.." && pwd)}   # the directory that contains the clone
REV=${OOREXXUNIT_REV:-13344}
DIR=${OOREXXUNIT_DIR:-$WORK/oorexxunit}
SVN="https://svn.code.sf.net/p/oorexx/code-0/!svn/bc/$REV/test/trunk"
mkdir -p "$DIR"
get() { curl -sSfL --retry 3 --retry-all-errors -o "$DIR/$(basename "$1")" "$SVN/$1"; }
get framework/OOREXXUNIT.CLS
for f in ooTest.frm worker.rex testOORexx.rex CPLv1.0.txt; do get "$f"; done
echo "ooRexxUnit r$REV in $DIR"
