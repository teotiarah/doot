#!/bin/sh
# check-docs.sh -- consistency gate between the specification and the code.
#
#   1. every relative Markdown link resolves to a file
#   2. every decision reference (01-decisions.md#dNNN) has a matching heading
#   3. every diagnostic code falls inside a documented range (D050)
#   4. every diagnostic code is produced by at least one test (D049)
#   5. no TODO markers in the source tree (D054)

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

  # Must be produced by at least one test. Spec tests are the primary home once
  # the compiler emits diagnostics; until then the base layer's codes are
  # covered by unit tests. Either satisfies the rule.
  enum="$(grep -B0 "\"$code\"" src/base/diag_codes.h | grep -oE 'X\(DIAG_[A-Z0-9_]+' |
    sed 's/^X(//')"
  if [ -z "$enum" ]; then
    fail_with "$code has no enum identifier in src/base/diag_codes.h"
    continue
  fi
  if ! grep -rqlE "(\b$enum\b|$code)" tests/ 2>/dev/null; then
    fail_with "$code ($enum) is not produced by any test (D049)"
  fi
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

if [ "$fail" -eq 0 ]; then
  echo "docs ok: $(echo "$codes" | wc -w | tr -d ' ') diagnostic codes, all documented and tested"
fi
exit "$fail"
