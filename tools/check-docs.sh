#!/bin/sh
# check-docs.sh -- consistency gate between the specification and the code.
#
#   1. every relative Markdown link resolves to a file
#   2. every decision reference (01-decisions.md#dNNN) has a matching heading
#   3. every diagnostic code falls inside a documented range (D050)
#   4. every diagnostic code is produced by a spec test (D049, D079)
#   5. no TODO markers in the source tree (D054)
#   6. every well-formedness rule with a registered code has both spec tests (D078)
#   7. every `spec=` code block matches its spec test byte for byte (D080)

set -eu

cd "$(cd "$(dirname "$0")/.." && pwd)"
fail=0

fail_with() {
  echo "check-docs: $1" >&2
  fail=1
}

# ---- 1. relative links resolve ------------------------------------------
broken=""
for f in README.md docs/*.md fuzz/README.md; do
  [ -f "$f" ] || continue
  dir="$(dirname "$f")"
  for link in $(grep -oE '\]\([0-9a-zA-Z/._-]+\.md(#[a-z0-9-]+)?\)' "$f" |
    sed -E 's/^\]\(//; s/\)$//'); do
    target="${link%%#*}"
    [ -f "$dir/$target" ] || broken="$broken $f->$link"
  done
done
[ -z "$broken" ] || fail_with "broken links:$broken"

# ---- 2. decision anchors exist ------------------------------------------
missing=""
for n in $(grep -ohE '01-decisions\.md#d[0-9]+' README.md docs/*.md |
  sed 's/.*#d//' | sort -u); do
  grep -q "^### D$n\$" docs/01-decisions.md || missing="$missing D$n"
done
[ -z "$missing" ] || fail_with "referenced but undefined decisions:$missing"

# ---- 2b. every other anchor resolves to a heading ------------------------
# Step 1 checks that a link's file exists but not that its `#anchor` does, so a
# renamed heading left every reference to it silently dangling. GitHub's slug is
# the heading lowercased with punctuation dropped and each remaining space turned
# into a hyphen -- note "each": an em-dash between two spaces leaves two hyphens,
# which is why the substitution below must not collapse runs.
awk '
  FILENAME ~ /\.md$/ && /^#{1,6} / {
    h = $0
    sub(/^#+ /, "", h)
    h = tolower(h)
    gsub(/`/, "", h)
    gsub(/[^a-z0-9 _-]/, "", h)
    gsub(/ /, "-", h)
    base = FILENAME
    sub(/.*\//, "", base)
    seen[base "#" h] = 1
  }
  { lines[++n] = FILENAME "\t" $0 }
  END {
    for (i = 1; i <= n; i++) {
      split(lines[i], f, "\t")
      s = f[2]
      while (match(s, /\]\([0-9a-zA-Z\/._-]+\.md#[a-zA-Z0-9._-]+\)/)) {
        link = substr(s, RSTART + 2, RLENGTH - 3)
        s = substr(s, RSTART + RLENGTH)
        target = link
        sub(/#.*/, "", target)
        anchor = link
        sub(/[^#]*#/, "", anchor)
        sub(/.*\//, "", target)
        if (!((target "#" anchor) in seen)) {
          print f[1] " -> " target "#" anchor
          bad = 1
        }
      }
    }
    exit bad ? 1 : 0
  }
' README.md docs/*.md fuzz/README.md >&2 || fail_with "broken anchors in Markdown links"

# ---- 3 & 4. diagnostic codes --------------------------------------------
codes="$(grep -oE '"DT[0-9]{4}"' src/base/diag_codes.h | tr -d '"' | sort -u)"
[ -n "$codes" ] || fail_with "no diagnostic codes found in src/base/diag_codes.h"

for code in $codes; do
  num="$(echo "$code" | sed 's/^DT0*//')"
  [ -n "$num" ] || num=0

  # Must fall inside a range documented in docs/06-tooling.md#code-ranges.
  awk -v c="$num" '
    /^\| `DT[0-9]{4}`.*`DT[0-9]{4}` \|/ {
      n = split($0, tok, "`");
      lo = substr(tok[2], 3) + 0;
      hi = substr(tok[4], 3) + 0;
      if (c + 0 >= lo && c + 0 <= hi) { found = 1 }
    }
    END { exit(found ? 0 : 1) }
  ' docs/06-tooling.md ||
    fail_with "$code is outside every range in docs/06-tooling.md#code-ranges"

  enum="$(grep -B0 "\"$code\"" src/base/diag_codes.h | grep -oE 'X\(DIAG_[A-Z0-9_]+' |
    sed 's/^X(//')"
  if [ -z "$enum" ]; then
    fail_with "$code has no enum identifier in src/base/diag_codes.h"
    continue
  fi

  # A code that describes source text is proved by a spec test; a code that
  # describes the driver's own environment is proved by a unit test, because no
  # .do file can elicit it (D079). The second list is closed and reasoned:
  #
  #   DT0002  source file is too large   -- a 64 MB fixture is not committable
  #   DT1001  cannot read the file       -- a spec file is readable by definition
  #   DT1002  cannot write the file      -- needs an induced filesystem failure
  #   DT1003  cannot read the directory  -- likewise
  case " DT0002 DT1001 DT1002 DT1003 " in
  *" $code "*)
    if ! grep -rqlE "\b$enum\b" tests/unit/ 2>/dev/null; then
      fail_with "$code ($enum) is not produced by any unit test (D079)"
    fi
    ;;
  *)
    # -a because a spec file may legitimately contain a NUL byte or invalid
    # UTF-8: two of them exist to produce DT0003 and DT0001, and grep would
    # otherwise report "binary file matches" instead of the directive line.
    if ! grep -rhaE '^// expect-(error|warning):' tests/spec/ 2>/dev/null |
      grep -q "$code"; then
      fail_with "$code is not produced by any spec test (D049, D079)"
    fi
    ;;
  esac
done

# ---- 5. no TODO markers in the source tree (D054) ------------------------
# Scoped to code: documentation legitimately discusses the rule itself.
# This script is excluded from its own scan: it necessarily names the markers it
# looks for.
found_todo="$(find src tests fuzz tools -type f \
  \( -name '*.c' -o -name '*.h' -o -name '*.sh' \) \
  ! -path 'tools/check-docs.sh' -exec \
  grep -nE '\b(TODO|FIXME|XXX|HACK)\b' {} + 2>/dev/null || true)"
if [ -n "$found_todo" ]; then
  echo "$found_todo" >&2
  fail_with "TODO markers in the source tree are forbidden (D054)"
fi
if grep -nE '\b(TODO|FIXME)\b' Makefile >/dev/null 2>&1; then
  fail_with "TODO markers in the Makefile are forbidden (D054)"
fi

# ---- 6. a rule with a registered code has both spec tests (D078) ---------
# The rule-to-code table in docs/03-grammar.md is the source of truth. A rule
# whose codes are all still reserved needs no tests yet, so coverage grows as the
# semantic pass lands instead of the gate demanding tests for absent features.
rules=0
sed -n 's/^| \([0-9][0-9]*\) | `\([a-z_]*\)` | \(.*\) |$/\1 \2 \3/p' docs/03-grammar.md |
  while read -r num slug codes; do
    registered=0
    for c in $(echo "$codes" | grep -oE 'DT[0-9]{4}'); do
      # A bracketed code is a reservation, not a registration, so only look for
      # the ones the compiler can actually emit.
      case "$codes" in
      *"[\`$c\`]"*) continue ;;
      esac
      if grep -q "\"$c\"" src/base/diag_codes.h; then
        registered=1
      fi
    done
    [ "$registered" -eq 1 ] || continue

    padded="$(printf '%02d' "$num")"
    for disposition in ok err; do
      f="tests/spec/rules/rule_${padded}_${slug}_${disposition}.do"
      [ -f "$f" ] || echo "missing $f" >&2
    done
  done >/tmp/rulecheck.$$ 2>/tmp/rulefail.$$ || true
if [ -s /tmp/rulefail.$$ ]; then
  cat /tmp/rulefail.$$ >&2
  fail_with "every well-formedness rule with a registered code needs an accepting and a rejecting spec test (D078)"
fi
rm -f /tmp/rulecheck.$$ /tmp/rulefail.$$
[ "$rules" -eq 0 ] || true

# ---- 7. a `spec=` code block matches its spec test (D080) ----------------
# A claim that the documentation and the implementation agree has to be checked
# against the documentation, not against a copy of it.
for f in README.md docs/*.md; do
  [ -f "$f" ] || continue
  grep -oE '^```do spec=[^ ]+$' "$f" | sed 's/^```do spec=//' | while read -r target; do
    if [ ! -f "$target" ]; then
      echo "$f pins a block to $target, which does not exist" >&2
      exit 1
    fi
    awk -v want="$target" '
      $0 == "```do spec=" want { inblock = 1; next }
      inblock && $0 == "```"   { exit }
      inblock                  { print }
    ' "$f" >"/tmp/block.$$"
    # Strip only the leading directive block: the spec file carries directives
    # the documentation does not.
    awk 'lead && /^\/\// { next } { lead = 0; print }' lead=1 "$target" >"/tmp/spec.$$"
    if ! awk '
      NR == FNR { a[FNR] = $0; n = FNR; next }
      { if ($0 != a[FNR]) { exit 1 } }
      END { exit (FNR == n) ? 0 : 1 }
    ' "/tmp/block.$$" "/tmp/spec.$$"; then
      echo "$f and $target have drifted apart" >&2
      exit 1
    fi
  done || fail_with "a documented program does not match its spec test (D080)"
done
rm -f "/tmp/block.$$" "/tmp/spec.$$"

if [ "$fail" -eq 0 ]; then
  echo "docs ok: $(echo "$codes" | wc -w | tr -d ' ') diagnostic codes, all documented and tested"
fi
exit "$fail"
