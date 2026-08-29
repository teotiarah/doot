#!/bin/sh
# vendor.sh -- fetch, verify, and install a vendored dependency (D052).
#
# Dependencies are committed to the repository, not fetched at build time: a
# build must work with no network, no package manager, and no upstream still
# online, which is what a ten-year horizon actually requires. This script is run
# by a person updating a dependency, never by the build.
#
# Every dependency is pinned to a version and verified against a SHA-256 recorded
# in vendor/MANIFEST. No local patches: if one becomes unavoidable it lives in
# vendor/patches/ as a standalone file with its reason and upstream issue
# recorded, because an edit in place is invisible at the next update.
#
# usage: tools/vendor.sh <name>      install or re-verify one dependency
#        tools/vendor.sh --verify    check every installed tree against MANIFEST
#        tools/vendor.sh --list      show what is pinned and what is installed

set -eu

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
manifest="vendor/MANIFEST"

die() {
  echo "vendor: $1" >&2
  exit 1
}

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d' ' -f1
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | cut -d' ' -f1
  else
    die "no sha256sum or shasum available; cannot verify a download"
  fi
}

# MANIFEST columns: name version sha256 url archive_member
field() {
  awk -v want="$1" -v col="$2" '
    /^#/ || /^[ \t]*$/ { next }
    $1 == want { print $col; found = 1; exit }
    END { exit(found ? 0 : 1) }
  ' "$manifest"
}

names() {
  awk '/^#/ || /^[ \t]*$/ { next } { print $1 }' "$manifest"
}

cmd_list() {
  printf '%-10s %-12s %s\n' NAME VERSION INSTALLED
  for n in $(names); do
    v="$(field "$n" 2)"
    if [ -f "vendor/$n/.doot-version" ]; then
      installed="$(cat "vendor/$n/.doot-version")"
    else
      installed="-"
    fi
    printf '%-10s %-12s %s\n' "$n" "$v" "$installed"
  done
}

# Drift detection only. A dependency that is pinned but not yet installed is
# reported, not failed: a version installs when the version that needs it is
# built, and the compiler is what notices a genuinely missing dependency.
cmd_verify() {
  fail=0
  for n in $(names); do
    want="$(field "$n" 2)"
    if [ ! -f "vendor/$n/.doot-version" ]; then
      echo "vendor: $n pinned at $want, not installed yet"
      continue
    fi
    have="$(cat "vendor/$n/.doot-version")"
    if [ "$have" != "$want" ]; then
      echo "vendor: $n has drifted -- installed $have, pinned $want" >&2
      fail=1
    else
      echo "vendor: $n $have ok"
    fi
  done
  return "$fail"
}

cmd_install() {
  name="$1"
  version="$(field "$name" 2)" || die "$name is not in $manifest"
  want_sha="$(field "$name" 3)"
  url="$(field "$name" 4)"
  member="$(field "$name" 5)"
  tmp="build/vendor-tmp/$name"
  archive="$tmp/download"

  command -v curl >/dev/null 2>&1 || die "curl is required to fetch a dependency"

  echo "vendor: fetching $name $version"
  rm -rf "$tmp"
  mkdir -p "$tmp"
  curl -fsSL -o "$archive" "$url" || die "download failed: $url"

  got_sha="$(sha256_of "$archive")"
  if [ "$got_sha" != "$want_sha" ]; then
    die "checksum mismatch for $name
  expected $want_sha
       got $got_sha
The pinned version, the URL, or the recorded checksum is wrong. Do not proceed."
  fi
  echo "vendor: checksum ok"

  case "$url" in
  *.zip) (cd "$tmp" && unzip -q download) ;;
  *.tar.gz | *.tgz) (cd "$tmp" && tar xzf download) ;;
  *.tar.bz2 | *.tbz2) (cd "$tmp" && tar xjf download) ;;
  *.tar.xz) (cd "$tmp" && tar xJf download) ;;
  *) die "unsupported archive type for $url" ;;
  esac

  [ -d "$tmp/$member" ] || die "archive does not contain expected directory $member"

  rm -rf "vendor/$name"
  mkdir -p vendor
  mv "$tmp/$member" "vendor/$name"
  echo "$version" >"vendor/$name/.doot-version"
  rm -rf "$tmp"

  if [ -d "vendor/patches/$name" ]; then
    for p in vendor/patches/$name/*.patch; do
      [ -f "$p" ] || continue
      echo "vendor: applying $p"
      patch -p1 -d "vendor/$name" <"$p" || die "patch failed: $p"
    done
  fi

  echo "vendor: installed $name $version into vendor/$name"
  echo "vendor: commit the tree -- dependencies live in git (D052)"
}

[ -f "$manifest" ] || die "missing $manifest"

case "${1:---list}" in
--list) cmd_list ;;
--verify) cmd_verify ;;
-*) die "unknown option $1" ;;
*) cmd_install "$1" ;;
esac
