#!/usr/bin/env bash
# Build ooRexx from SVN with its API headers and libraries, and with the fixes
# in patches/oorexx/ (ooRexx bugs that are reported but not yet in trunk; see
# the README there), for building and testing the bridge: an alternative to
# an installed ooRexx, such as the .deb, or the portable builds.  Linux, macOS
# and Windows (Git Bash, in MSVC's developer environment).  Idempotent: skips
# what is already there.  Prints the installation's bin directory on stdout,
# last; everything else goes to stderr.  build.sh finds it through the PATH
# (or OOREXX_HOME=<the installation>).
#   setup-oorexx.sh   (OOREXX_REV, OOREXX_SRC, OOREXX_BUILD, OOREXX_INST;
#                      OOREXX_PATCHES=0 for trunk as it is)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${ORXLUA_WORK:-$(cd "$HERE/../.." && pwd)}   # the directory that contains the clone
REV=${OOREXX_REV:-13269}                          # the revision the patches are made against
SRC=${OOREXX_SRC:-$WORK/oorexx}
BUILD=${OOREXX_BUILD:-$WORK/oorexx-build}
INST=${OOREXX_INST:-$WORK/oorexx-inst}
PATCHES=()
[ "${OOREXX_PATCHES:-1}" = 0 ] || PATCHES=("$HERE"/../patches/oorexx/*.diff)
log() { echo "setup-oorexx.sh: $*" >&2; }

case $(uname -s) in
  Linux)  OS=Linux ;;
  Darwin) OS=macOS ;;
  MINGW*|MSYS*|CYGWIN*) OS=Windows ;;
  *) log "unknown system $(uname -s)"; exit 2 ;;
esac
install_pkg() {   # install_pkg <command> <Debian package> <Homebrew formula> <Chocolatey package>
  command -v "$1" >/dev/null && return
  log "installing $1"
  case $OS in
    Linux)   S=; [ "$(id -u)" = 0 ] || S=sudo
             $S apt-get update -qq >&2 && $S apt-get install -y -qq "$2" >&2 ;;
    macOS)   brew install -q "$3" >&2 ;;
    Windows) choco install -y --no-progress "$4" >&2
             export PATH="$PATH:/c/Program Files/SlikSvn/bin" ;;
  esac
  command -v "$1" >/dev/null || { log "no $1"; exit 2; }
}
NATIVE() { if [ $OS = Windows ]; then cygpath -m "$1"; else echo "$1"; fi; }

# what the installation was built from: the revision and the patches
stamp() {
  echo "r$REV"
  for p in ${PATCHES[@]+"${PATCHES[@]}"}; do echo "$(basename "$p") $(cksum < "$p" | awk '{print $1}')"; done
}
STAMP=$INST/oorexxlua-build.txt
if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$(stamp)" ]; then
  log "$INST is up to date ($(head -1 "$STAMP"), ${#PATCHES[@]} patches)"
else
  if [ ! -d "$SRC" ]; then
    install_pkg svn subversion subversion sliksvn
    log "svn checkout r$REV -> $SRC"
    # a working copy, not an export: ooRexx's build takes its revision from it
    svn checkout -q -r "$REV" https://svn.code.sf.net/p/oorexx/code-0/main/trunk "$SRC" >&2
    for p in ${PATCHES[@]+"${PATCHES[@]}"}; do
      # svn patch, not patch: it knows svn:eol-style (CRLF files on Windows),
      # but reports a hunk it cannot apply only in its output (C, or rejected)
      log "svn patch $(basename "$p")"
      out=$(svn patch "$(NATIVE "$p")" "$(NATIVE "$SRC")" 2>&1); echo "$out" >&2
      if echo "$out" | grep -qE '^C |rejected|^Skipped'; then log "$(basename "$p") does not apply to r$REV"; exit 2; fi
    done
    stamp > "$SRC/oorexxlua-patches.txt"
  elif [ "$(cat "$SRC/oorexxlua-patches.txt" 2>/dev/null)" != "$(stamp)" ]; then
    log "$SRC was checked out for another revision or other patches: remove it (and $BUILD, $INST)"
    exit 2
  fi
  install_pkg cmake cmake cmake cmake
  log "build -> $BUILD, install -> $INST"
  GEN=(); [ $OS = Windows ] && GEN=(-G "NMake Makefiles")
  cmake ${GEN[@]+"${GEN[@]}"} -S "$(NATIVE "$SRC")" -B "$(NATIVE "$BUILD")" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$(NATIVE "$INST")" >&2
  cmake --build "$(NATIVE "$BUILD")" -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)" >&2
  cmake --install "$(NATIVE "$BUILD")" >&2
  if [ $OS = Windows ]; then
    # the Windows install is flat (the programs and DLLs at the top, headers
    # and import libraries in api/): into the layout of the portable builds,
    # which build.sh expects
    mkdir -p "$INST/bin" "$INST/include" "$INST/lib"
    find "$INST" -maxdepth 1 -type f -exec mv {} "$INST/bin/" \;
    mv "$INST"/api/*.h "$INST/include/"; mv "$INST"/api/*.lib "$INST/lib/"
  fi
  stamp > "$STAMP"
fi
[ -f "$INST/include/oorexxapi.h" ] || { log "no $INST/include/oorexxapi.h"; exit 2; }
log "$("$INST/bin/rexx" -v | head -1)"
echo "$INST/bin"
