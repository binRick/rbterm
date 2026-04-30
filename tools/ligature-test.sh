#!/usr/bin/env bash
# tools/ligature-test.sh — print every common programmer-font ligature
# pair next to a one-line description, so you can eyeball whether the
# active rbterm font is shaping them as expected.
#
# Usage: ./tools/ligature-test.sh   (run inside any rbterm pane)
#
# Three groups:
#   1. Standard ligatures rbterm enables (liga / clig / calt) — should
#      render as decorated glyphs.
#   2. Path / URL / ellipsis sequences rbterm INTENTIONALLY suppresses
#      (`..`, `../`, `...`, `//`, `://`) — should render as plain
#      ASCII even on ligature fonts.
#   3. JSX-ish slash ligatures (`/>`, `</>`) — also suppressed by
#      rbterm's slash-veto rule. If a user needs these in shell
#      context, the suppression can be narrowed.

set -euo pipefail

printf '%s\n' "Ligature test — what each pair should mean (font: $TERM_PROGRAM)"
printf '%s\n' "------------------------------------------------------------"

printf '%-10s %s\n' \
  '=>'   'fat arrow (lambda / arrow function)' \
  '->'   'thin arrow (return type / pipeline)' \
  '<-'   'left arrow (assignment / channel send)' \
  '<--'  'long left arrow (Haskell do-bind)' \
  '-->'  'long right arrow' \
  '!='   'not equal' \
  '=='   'equal' \
  '==='  'strict equal (JS / triple-equals)' \
  '!=='  'strict not-equal (JS)' \
  '<='   'less-than-or-equal' \
  '>='   'greater-than-or-equal' \
  '<>'   'diamond / not-equal (Haskell, Pascal)' \
  '<|>'  'alternative (Haskell)' \
  '|>'   'pipe forward (F#, OCaml)' \
  '<$>'  'fmap (Haskell)' \
  '<*>'  'apply (Haskell)' \
  '<+>'  'monoid sum (Haskell)' \
  '++'   'increment (C/Java) or list append (Haskell)' \
  '--'   'decrement (C/Java) or comment (Haskell/Lua)' \
  '<<'   'left shift / stream insertion' \
  '>>'   'right shift / sequence (Haskell)' \
  '&&'   'logical AND' \
  '||'   'logical OR' \
  '::'   'scope resolution / cons (Haskell)' \
  ':='   'walrus / Pascal assignment' \
  '?:'   'ternary / Elvis'

printf '\n%s\n' "rbterm intentionally does NOT ligate these (path/URL):"
printf '%s\n' "------------------------------------------------------------"
printf '%-10s %s\n' \
  '..'   'parent dir / range — should be two plain dots' \
  '../'  'parent path — plain dots + slash' \
  '...'  'ellipsis / spread — three plain dots' \
  '//'   'path or comment — two plain slashes' \
  '://'  'URL scheme — plain colon + two slashes' \
  '/>'   'self-closing JSX — plain slash + greater-than' \
  '</>'  'JSX fragment — plain less-than + slash + greater-than'
