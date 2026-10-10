-- temporary: texlua on Windows closing its state (removed once found)
io.stdout:setvbuf("no")
print("1 require"); local rexx = require "oorexx"
print("2 interpreter"); local r = rexx.interpreter()
print("3 routine"); rexx.routine("x = .Bye~new('default'); .local~x = x; ::class Bye; ::method init; expose n; use arg n; ::method uninit; expose n; say 'bye' n")()
print("4 table"); t = {}; rexx.routine("use arg t; .local~t = t")(t)
print("5 exit")
os.exit(0, true)
