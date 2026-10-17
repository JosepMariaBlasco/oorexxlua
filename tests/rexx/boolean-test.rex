/*----------------------------------------------------------------------------*/
/*                                                                            */
/* Copyright (c) 2026      Rexx Language Association. All rights reserved.    */
/*                                                                            */
/* This program and the accompanying materials are made available under       */
/* the terms of the Common Public License v1.0 which accompanies this         */
/* distribution. A copy is also available at the following address:           */
/* https://www.oorexx.org/license.html                                        */
/*                                                                            */
/* Redistribution and use in source and binary forms, with or                 */
/* without modification, are permitted provided that the following            */
/* conditions are met:                                                        */
/*                                                                            */
/* Redistributions of source code must retain the above copyright             */
/* notice, this list of conditions and the following disclaimer.              */
/* Redistributions in binary form must reproduce the above copyright          */
/* notice, this list of conditions and the following disclaimer in            */
/* the documentation and/or other materials provided with the distribution.   */
/*                                                                            */
/* Neither the name of Rexx Language Association nor the names                */
/* of its contributors may be used to endorse or promote products             */
/* derived from this software without specific prior written permission.      */
/*                                                                            */
/* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS        */
/* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT          */
/* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS          */
/* FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT   */
/* OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,      */
/* SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED   */
/* TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,        */
/* OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY     */
/* OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING    */
/* NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS         */
/* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.               */
/*                                                                            */
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
/* boolean-test.rex: tests for boolean.cls. Exit code 0 if all pass.          */
/*----------------------------------------------------------------------------*/
pass = 0; fail = 0
t = .Boolean~true; f = .Boolean~false
call ok "isA", t~isA(.Boolean) & f~isA(.Boolean) & \"1"~isA(.Boolean)
call ok "same instance", .Boolean~true == .Boolean~true & (.Boolean~true~identityHash == t~identityHash)
call ok "IF true", iif(t)
call ok "IF false", \iif(f)
call ok "NOT", (\t) == 0 & (\f) == 1 & t~"\" == 0 & f~"\" == 1
call ok "AND/OR/XOR", (t & f) == 0 & (t | f) == 1 & (t && t) == 0
call ok "say/concat", t"" == "1" & ("x" || f) == "x0" & t~string == "1"
call ok "= with strings", t = 1 & f = 0 & t = .true & f \= .true & t <> f
call ok "== with strings", t == "1" & f \== "1"
call ok "= across classes", t = .Json2~true & f = .Json2~false
call ok "string methods", t~length == 1 & f~c2x == "30"
call ok "of", .Boolean~of(3 > 2) == .Boolean~true & .Boolean~of(0)~identityHash == f~identityHash
call ok "of subclass", .Json2~of(1)~isA(.Json2) & .Json2~true~isA(.Boolean)
call ok "subclass has own instances", .Json2~true~identityHash \== t~identityHash
call ok "subclass serializer", .Json2~true~makeJSON == "true" & .Json2~false~makeJSON == "false"
tb = .table~new; tb[t] = "yes"
call ok "table key", tb[t] == "yes" & tb~hasIndex(.Boolean~true)
call ok "new is private", trapNew()
call ok "of rejects 2", trapOf(2)
call ok "datatype O", datatype(t, "O") & datatype(f, "O")
call ok "select when", sel(f) == "f"
call ok "Array makeString", .array~of(t, f, t)~makeString("L", " ") == "1 0 1"
call ok "Array toString", .array~of(t, f)~toString(, ",") == "1,0"
say "all" pass "passed," fail "failed"
exit fail > 0

ok: procedure expose pass fail
  parse arg name, cond
  if cond == 1 then pass = pass + 1; else do; fail = fail + 1; say "FAIL:" name; end
  return
iif: if arg(1) then return 1; return 0
sel: select; when arg(1) then return "t"; otherwise return "f"; end
trapNew: signal on syntax name tn; x = .Boolean~new(1); return 0
tn: return 1
trapOf: signal on syntax name to; x = .Boolean~of(arg(1)); return 0
to: return condition("O")~code == 34.900

::requires "boolean.cls"
::class Json2 subclass Boolean
::method makeJSON unguarded
  if self~value then return "true"; return "false"
