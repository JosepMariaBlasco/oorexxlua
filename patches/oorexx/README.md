# Fixes for ooRexx

Fixes for ooRexx bugs that are reported on SourceForge but are not yet in
trunk, and that the bridge needs: `WITH` in `ADDRESS LUA` runs through
ooRexx's I/O redirector API, and the bridge registers its command
environment while other threads may be running Rexx code. Each one is the
patch attached to its ticket, made against trunk r13269; they apply to
r13271, which the CI builds (it has the fix for
[#2113](https://sourceforge.net/p/oorexx/bugs/2113/), the redirector's
queries, whose patch was here until then).

| Bug | Patch | What it fixes |
|---|---|---|
| [#2111](https://sourceforge.net/p/oorexx/bugs/2111/) | `oorexx-11-addcmdenv-lock.diff` | `AddCommandEnvironment` changes the instance's table of command handlers without the interpreter lock |
| [#2112](https://sourceforge.net/p/oorexx/bugs/2112/) | `oorexx-12-resolve-subcom.diff` | resolving an unknown `ADDRESS` environment writes into an unanchored object with the kernel lock released |

`scripts/setup-oorexx.sh` checks out r13271, applies them (with
`svn patch`) and builds and installs ooRexx with its API headers, on Linux,
macOS and Windows; it prints the installation's `bin` directory:

```bash
PATH=$(oorexxlua/scripts/setup-oorexx.sh):$PATH oorexxlua/scripts/build.sh
```

The CI builds the bridge on Linux with this ooRexx (cached) and tells the tests
so with `ORXLUA_OOREXX_FIXES=1`; elsewhere it uses ooRexx's portable builds.
Run by hand with `oorexx_fixes` (`gh workflow run build.yml -f
oorexx_fixes=true`), every platform builds and uses it.

When a fix is in trunk, its patch goes; when all are, this directory goes,
and the CI goes back to the portable builds everywhere.
