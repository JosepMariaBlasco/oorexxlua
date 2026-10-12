/* powers.rex -- called by rexx-in-latex.tex (rexx.run("powers.rex", digits)).
 * Rexx typesets a table into the document through ADDRESS LUA: each command
 * is Lua code, run in LuaTeX's own Lua, so tex.sprint puts TeX into the page.
 * Rexx's decimal arithmetic gives exact values that Lua's floats cannot. */
use arg digits
numeric digits digits

address lua
-- RESULT: what the last command returned (here, a field of LuaTeX's status)
'return status.luatex_version'
version = result % 100"."result // 100

call tex "\begin{tabular}{rrr}"
call tex "\toprule $n$ & $2^{3n}$ & $n!$ \\ \midrule"
f = 1
do n = 1 to 30
  f = f * n
  if n // 3 = 0 then call tex n "&" 2**(3 * n) "&" f "\\"
end
call tex "\bottomrule \end{tabular}"
return version

-- One line of TeX into the page.  [==[ ... ]==] is a Lua string that keeps
-- every character as it is, backslashes included.
tex:
  'tex.sprint([==[' arg(1) ']==])'
  return
