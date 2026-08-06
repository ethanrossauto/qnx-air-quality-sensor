#!/usr/bin/env bash
# The test suite for this project, and it runs on BOTH sides of the mirror pair: the private
# working repo and the public mirror where CI is a thin wrapper around this same file.
#
#   bash tests/run.sh              every group that can run here
#   bash tests/run.sh T1 T5        just those groups
#
# It touches no network and needs no QNX target. Nothing here talks to the CM4.
#
# 🔒 A group that cannot run reports SKIP and is counted in the summary. It is never silently a
# pass. A check that cannot tell "nothing wrong" from "I could not look" is not a check.
#
# ONE FILE, NOT TWO. T1, T4, T5 and T6 are about the code and are identical wherever they run.
# T2 is about the published artifact and means nothing privately, where a tracked CLAUDE.md and
# real local paths are both correct. T3 differs by which doc is meant to teach usage.
#
# ⚠️ PATHS ARE DISCOVERED BY EXTENSION, NEVER HARDCODED BY DIRECTORY, and that is not a style
# choice. This manifest RENAMES trees across the mirror: vss-air-quality/qnx/ privately becomes
# vss/connector/ and vss/catalog/ publicly. A group that said "cppcheck vss/connector" would
# check nothing on the private side and report a pass for it.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1

if [ -f "$ROOT/.mirror-manifest" ]; then SIDE="private"; else SIDE="public"; fi

# The mirror's allowlist, from .mirror-manifest. Anything not matching does not publish.
ALLOWLIST='^(configs/|docs/|driver/|ivi/|ivi-android/|publisher/|scripts/|vss/|tests/|\.github/|README\.md$|SETUP\.md$|LICENSE$|\.gitignore$)'

SHELLCHECK="${SHELLCHECK:-shellcheck}"
CPPCHECK="${CPPCHECK:-cppcheck}"
PASS=0; FAIL=0; SKIPPED=0
FAILED_NAMES=(); SKIPPED_NAMES=()
TMP=""

# Invoked by the trap below, which shellcheck cannot see. Both codes are needed: 0.10 and later
# call this SC2329, while 0.9 reports the same thing as SC2317, and ubuntu-24.04 ships 0.9.
# Suppressing only the code your own shellcheck emits passes locally and reddens CI.
# shellcheck disable=SC2317,SC2329
cleanup() { [ -n "$TMP" ] && rm -rf "$TMP"; }
trap cleanup EXIT

if [ -t 1 ]; then G=$'\033[32m'; R=$'\033[31m'; Y=$'\033[33m'; B=$'\033[1m'; N=$'\033[0m'
else G=""; R=""; Y=""; B=""; N=""; fi

group() { printf '\n%s%s%s\n' "$B" "$1" "$N"; }
ok()    { PASS=$((PASS+1));    printf '  %sPASS%s  %s\n' "$G" "$N" "$1"; }
bad()   { FAIL=$((FAIL+1)); FAILED_NAMES+=("$1")
          printf '  %sFAIL%s  %s\n' "$R" "$N" "$1"
          [ $# -gt 1 ] && [ -n "$2" ] && printf '%s\n' "$2" | sed 's/^/          /'; }
skip()  { SKIPPED=$((SKIPPED+1)); SKIPPED_NAMES+=("$1 ($2)")
          printf '  %sSKIP%s  %s (%s)\n' "$Y" "$N" "$1" "$2"; }

assert_empty() {
  local what="$1" got="$2" hint="${3:-}"
  if [ -z "$got" ]; then ok "$what"; else bad "$what" "${hint:+$hint
}$got"; fi
}

want_group() {
  [ ${#WANTED[@]} -eq 0 ] && return 0
  local g; for g in "${WANTED[@]}"; do [ "$g" = "$1" ] && return 0; done
  return 1
}

tracked() { git -C "$ROOT" ls-files 2>/dev/null; }
is_repo() { git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; }

# Tracked PLUS untracked-not-ignored, so a file written this session is still checked while a
# gitignored one is not: scripts/env.local.sh carries this box's real addresses and is absent
# from a clone by design.
list_files() {
  local rx="$1"
  if is_repo; then
    { tracked; git -C "$ROOT" ls-files --others --exclude-standard; } | sort -u | grep -E "$rx\$"
  else
    find . -path ./.git -prune -o -type f -print 2>/dev/null | sed 's|^\./||' \
      | grep -E "$rx\$" | grep -v '^scripts/env\.local\.sh$' | sort
  fi
}

# ---------------------------------------------------------------------------
# T1  lint
# ---------------------------------------------------------------------------
t1_lint() {
  group "T1  lint"
  local f out broken=""
  local -a sh_list py_list
  mapfile -t sh_list < <(list_files '\.sh')
  mapfile -t py_list < <(list_files '\.py')
  printf '  ....  linting %s shell and %s python file(s)\n' "${#sh_list[@]}" "${#py_list[@]}"

  for f in "${sh_list[@]}"; do bash -n "$f" 2>/dev/null || broken="$broken$f"$'\n'; done
  assert_empty "every shell script parses (bash -n)" "$broken"

  if command -v "$SHELLCHECK" >/dev/null 2>&1; then
    out="$("$SHELLCHECK" -x "${sh_list[@]}" 2>&1)" || true
    assert_empty "shellcheck clean" "$out"
  else
    skip "shellcheck clean" "shellcheck not installed; set SHELLCHECK=<path> to point at one"
  fi

  broken=""
  for f in "${py_list[@]}"; do
    python3 -m py_compile "$f" 2>/dev/null || broken="$broken$f"$'\n'
  done
  assert_empty "every python file compiles" "$broken"

  broken=""
  for f in "${sh_list[@]}"; do
    [ -x "$f" ] || continue
    head -1 "$f" | grep -q '^#!' || broken="$broken$f (executable, no shebang)"$'\n'
  done
  assert_empty "every executable script has a shebang" "$broken"

  assert_empty "no CRLF line endings" \
    "$(list_files '\.(sh|py|c|cpp|h|md|conf|json|yaml|vspec)' | tr '\n' '\0' \
       | xargs -0 grep -lI $'\r$' 2>/dev/null || true)"
}

# ---------------------------------------------------------------------------
# T2  publishing hygiene: Set B, mechanised
#
# The mirror's pre-push hook checks some of this, but it lives in .git/hooks and a fresh clone
# comes back without it. This runs on GitHub, where it cannot be missing.
# ---------------------------------------------------------------------------
t2_hygiene() {
  group "T2  publishing hygiene"
  if [ "$SIDE" = "private" ]; then
    skip "the whole publishing group" "private working repo; these rules describe the published mirror"
    return
  fi
  if ! is_repo; then
    skip "the whole hygiene group" "not a git checkout, so there is no tracked set to audit"
    return
  fi

  local files; files="$( { tracked; git -C "$ROOT" ls-files --others --exclude-standard; } | sort -u )"
  local count; count="$(printf '%s\n' "$files" | grep -c .)"
  if [ "$count" -eq 0 ]; then
    bad "the file list is readable" "git returned nothing. Refusing to report a zero-file scan as clean."
    return
  fi
  printf '  ....  auditing %s file(s), tracked plus untracked-not-ignored\n' "$count"
  local -a flist; mapfile -t flist <<< "$files"

  assert_empty "every tracked file is on the allowlist" \
    "$(printf '%s\n' "$files" | grep -vE "$ALLOWLIST")" "allowlist: $ALLOWLIST"

  assert_empty "no planning docs" \
    "$(printf '%s\n' "$files" | grep -iE '(^|/)(CLAUDE|SESSIONS|SCOPE|STATUS|NOTES)\.md$')"

  assert_empty "no local config" \
    "$(printf '%s\n' "$files" | grep -E '(^|/)env\.local\.sh$')"

  # ⚠️ Plain grep, NOT `git grep`. git grep searches tracked content only, so a file that is
  # untracked-but-about-to-be-committed would be allowlist-checked above and skipped by every
  # content check below.
  scan() { grep -nIE "$1" "${flist[@]}" 2>/dev/null; }

  # The [~/] and [-] brackets stop the patterns from matching the line they are written on.
  assert_empty "no private paths in published content" \
    "$(scan '/home/[a-z]+/|[~/]Claude/[a-z]|CLAUDE[-]MIRROR')" \
    "derive paths from BASH_SOURCE, or put them in scripts/env.local.sh"

  # ⚠️ A COPYRIGHT LINE IS THE ONE PLACE THE NAME BELONGS IN A SOURCE FILE, so those are
  # excluded. Seven files here carry a `Copyright (c) 2026 <maintainer>` line in an SPDX-style
  # header, which is correct and is not what this check is looking for. What it looks for is the
  # name turning up in prose, a comment or runtime output, where it was never meant to be.
  assert_empty "no personal names outside a copyright line" \
    "$(printf '%s\n' "$files" | grep -v '^LICENSE$' | tr '\n' '\0' \
       | xargs -0 grep -nI '[E]than' 2>/dev/null \
       | grep -viE 'copyright|SPDX')"

  # 🔑 THE LONG-DASH RULE IS DELIBERATELY NOT CHECKED IN THIS REPO. Relaxed 2026-08-05, the same
  # call already made for the skryer mirror, so the two behave alike.
  #
  # The house rule governs prose sent under the maintainer's name. This repo is source code plus
  # setup docs, and some of the hits are not prose at all: ivi/index.html uses an em dash as the
  # PLACEHOLDER GLYPH for a reading that has not arrived yet, which is correct typography and
  # would survive any cleanup anyway.
  #
  # ⚠️ REMOVED rather than left as a permanent SKIP, on purpose. The skip list means "could not
  # look", and letting "chose not to" sit in it would blunt the one signal this suite relies on.
  # A decision belongs in a comment; a gap belongs in the summary.

  # 🔒 THERE IS DELIBERATELY NO HARDWARE-TERM CHECK HERE, AND ADDING ONE WOULD BREAK THIS
  # MIRROR'S WHOLE PURPOSE. This demo is meant to be REPRODUCIBLE: its wiring, pin map, I2C
  # address and Teensy sketch are all published on purpose. That embargo belongs to skryer and
  # to skryer alone, which is why the two mirrors' pre-push hooks carry different patterns and
  # why this file says so out loud. See ~/Claude/CLAUDE.md, Set B rule 2.
}

# ---------------------------------------------------------------------------
# T3  docs match the code
# ---------------------------------------------------------------------------
t3_docs() {
  local doc; [ "$SIDE" = "private" ] && doc="CLAUDE.md" || doc="SETUP.md"
  group "T3  docs match the code ($doc)"
  if [ ! -f "$ROOT/$doc" ]; then
    skip "docs match the code" "$doc not present on this side"; return
  fi

  # Forward direction, PUBLIC ONLY: SETUP.md is the usage guide a stranger follows, so it must
  # name every script they could need. The private CLAUDE.md is a rules and state file, not a
  # usage guide, and requiring it to list every script would invent a rule nobody agreed to.
  if [ "$SIDE" = "public" ]; then
    local f missing="" name
    for f in scripts/*.sh; do
      [ -f "$f" ] || continue
      name="$(basename "$f")"
      grep -q "$name" "$ROOT/$doc" || missing="$missing$name"$'\n'
    done
    assert_empty "every script in scripts/ is documented in $doc" "$missing"
  else
    skip "every script is documented" "CLAUDE.md is a rules file here, not the usage guide"
  fi

  # Reverse direction, BOTH SIDES: a doc naming a script that was never written reads as a
  # command that exists. megaplay's CLAUDE.md pointed at a fix-tags.sh for weeks.
  #
  # ⚠️ The name pattern must allow DOTS, or `env.local.sh` is clipped to `local.sh` and reported
  # as a phantom forever. That was this check's first finding, against itself.
  #
  # ⚠️ A gitignored name counts as EXISTING. scripts/env.local.sh is documented precisely
  # because it is deliberately absent from a clone, so looking only at what is on disk would
  # fail this group on every fresh checkout and on CI, which is the opposite of the point.
  # Scripts that belong to somebody else's toolchain and are correctly named in the docs without
  # ever shipping here. qnxsdp-env.sh comes with the QNX SDP and is sourced as `<SDP>/...`.
  local external_scripts='qnxsdp-env\.sh'

  local claimed phantom="" name
  claimed="$(grep -oE '[A-Za-z0-9_.-]+\.sh' "$ROOT/$doc" 2>/dev/null | sort -u)"
  for name in $claimed; do
    find "$ROOT" -name "$name" -not -path '*/.git/*' 2>/dev/null | grep -q . && continue
    grep -qF "$name" "$ROOT/.gitignore" 2>/dev/null && continue
    printf '%s' "$name" | grep -qE "^($external_scripts)$" && continue
    phantom="$phantom$name"$'\n'
  done
  assert_empty "$doc names no script that does not exist" "$phantom"
}

# ---------------------------------------------------------------------------
# T4  parameterisation: no real system shape baked into a tracked file
#
# This is Set G5 and DERIVED cause D1 as a test. The whole reason doa-style DERIVED entries fell
# to zero here is that real addresses moved into a gitignored scripts/env.local.sh and the
# committed scripts kept a placeholder default. Nothing was stopping that from regressing.
# ---------------------------------------------------------------------------
t4_params() {
  group "T4  parameterisation"

  # Every address in a tracked script must arrive through a ${VAR:-default} form, never as a
  # bare literal assignment. The default itself is a documented placeholder, not a real host.
  local bare
  bare="$(grep -nE '^[[:space:]]*[A-Z_]+=["'"'"']?([0-9]{1,3}\.){3}[0-9]{1,3}' scripts/*.sh 2>/dev/null \
          | grep -v 'env\.example\.sh' || true)"
  assert_empty "no address assigned as a bare literal in scripts/" "$bare" \
    "use \${QNX_IP:-<placeholder>} and put the real value in scripts/env.local.sh"

  # env.example.sh is the contract for what a downloader must set. Every variable the scripts
  # read with a default must appear in it, or someone lands on a placeholder they never knew to
  # change.
  local used undeclared="" v
  used="$(grep -ohE '\$\{[A-Z_]+:-' scripts/*.sh 2>/dev/null | grep -oE '[A-Z_]+' | sort -u)"
  for v in $used; do
    grep -qE "^export $v=" scripts/env.example.sh 2>/dev/null || undeclared="$undeclared$v"$'\n'
  done
  assert_empty "every \${VAR:-default} in scripts/ is declared in env.example.sh" "$undeclared"

  # PRIVATE SIDE ONLY, and it is the strongest check in this file: the real values are sitting
  # right there in the gitignored env.local.sh, so we can assert that none of them has reached a
  # file that CROSSES. The public side cannot run this, because it has nothing to compare
  # against, which is exactly why it SKIPs rather than passing.
  #
  # ⚠️ Scoped to the files that publish, read from .mirror-manifest rather than from a second
  # list kept here. A real address in CLAUDE.md, STATUS.md or a raw run log is CORRECT: those
  # are NEVER paths, they are working notes, and flagging them would train everyone to ignore
  # this check. What matters is a real value inside something that leaves.
  #
  # 🔒 It reports the VARIABLE NAME, never the value. This suite's output is public on the
  # mirror's CI, and a failure message that printed the address would publish the very thing it
  # exists to keep back.
  if [ "$SIDE" = "private" ] && [ -f "$ROOT/scripts/env.local.sh" ]; then
    local -a crossing
    mapfile -t crossing < <(
      grep -E '^(VERBATIM|DERIVED)[[:space:]]' "$ROOT/.mirror-manifest" 2>/dev/null \
        | awk '{print $2}' | grep -v '^(none' )
    if [ "${#crossing[@]}" -eq 0 ]; then
      bad "no env.local.sh value reaches a crossing file" \
          "could not read any VERBATIM path out of .mirror-manifest. Refusing to report an empty scan as clean."
      return
    fi
    local leaked="" var val f
    while IFS= read -r line; do
      var="${line%%=*}"; val="${line#*=}"; val="${val%\"}"; val="${val#\"}"
      [ -n "$val" ] || continue
      # The documented placeholders are supposed to be in the tracked files.
      case "$val" in 192.168.1.10|192.168.1.11|8000) continue;; esac
      for f in "${crossing[@]}"; do
        [ -e "$ROOT/$f" ] || continue
        if grep -rIF -- "$val" "$ROOT/$f" >/dev/null 2>&1; then
          leaked="$leaked\$$var reaches $f"$'\n'; break
        fi
      done
    done < <(grep -oE '^(export )?[A-Z_]+=[^ #]+' "$ROOT/scripts/env.local.sh" | sed 's/^export //')
    assert_empty "no env.local.sh value reaches a crossing file" "$leaked" \
      "a real value has been committed into something that publishes; put it behind a \${VAR:-default}"
  else
    skip "no env.local.sh value reaches a crossing file" \
         "no scripts/env.local.sh here, so there is nothing to compare against"
  fi
}

# ---------------------------------------------------------------------------
# T5  the data files parse
#
# The VSS proposal and the QPP catalogue are the artifacts other people consume, and a broken
# one is invisible until someone tries to load it.
# ---------------------------------------------------------------------------
t5_parse() {
  group "T5  data files parse"
  local f broken=""
  local -a json_list yaml_list
  mapfile -t json_list < <(list_files '\.json')
  mapfile -t yaml_list < <(list_files '\.(yaml|vspec)')
  printf '  ....  parsing %s json and %s yaml/vspec file(s)\n' "${#json_list[@]}" "${#yaml_list[@]}"

  for f in "${json_list[@]}"; do
    python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$f" 2>/dev/null \
      || broken="$broken$f"$'\n'
  done
  assert_empty "every .json file loads" "$broken"

  if python3 -c 'import yaml' 2>/dev/null; then
    broken=""
    for f in "${yaml_list[@]}"; do
      python3 -c 'import yaml,sys; yaml.safe_load(open(sys.argv[1]))' "$f" 2>/dev/null \
        || broken="$broken$f"$'\n'
    done
    assert_empty "every .yaml and .vspec file loads" "$broken"
  else
    skip "every .yaml and .vspec file loads" "PyYAML not installed"
  fi

  # The sensor configs are a block format: `begin NAME` / indented `key = value` / `end NAME`.
  # A stray line, or a block left unclosed, is the kind of thing that only shows up on target
  # hours later. Checked structurally rather than by eye.
  broken=""
  for f in configs/*.conf; do
    [ -f "$f" ] || continue
    local stray depth
    stray="$(grep -vE '^[[:space:]]*(#|$)' "$f" \
             | grep -vE '^[[:space:]]*(begin|end)[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*$' \
             | grep -vE '^[[:space:]]*[A-Za-z_][A-Za-z0-9_]*[[:space:]]*=' || true)"
    [ -n "$stray" ] && broken="$broken$f: $stray"$'\n'
    depth=$(( $(grep -cE '^[[:space:]]*begin[[:space:]]' "$f") - $(grep -cE '^[[:space:]]*end[[:space:]]' "$f") ))
    [ "$depth" -ne 0 ] && broken="$broken$f: $depth unclosed begin/end block(s)"$'\n'
  done
  assert_empty "every configs/*.conf is well formed (begin/end blocks, key = value)" "$broken"
}

# ---------------------------------------------------------------------------
# T6  C and C++ static analysis
#
# ⚠️ These sources CANNOT be compiled off-target: they include <hw/i2c.h>,
# <sensor/sensor_api.h>, <devctl.h> and <sys/syspage.h>, which exist only in a QNX SDP. cppcheck
# is used precisely because it does not need to resolve every include, so it says something real
# about code no runner can build.
# ---------------------------------------------------------------------------
t6_cstatic() {
  group "T6  C/C++ static analysis"
  local -a c_list
  mapfile -t c_list < <(list_files '\.(c|cpp)')
  printf '  ....  %s C/C++ source file(s)\n' "${#c_list[@]}"

  if [ "${#c_list[@]}" -eq 0 ]; then
    bad "there is C/C++ to analyse" "found none, which cannot be right in this repo"
    return
  fi
  if ! command -v "$CPPCHECK" >/dev/null 2>&1; then
    skip "cppcheck clean" "cppcheck not installed; set CPPCHECK=<path> or apt install cppcheck"
    return
  fi

  # --error-exitcode makes findings fail the run. Missing includes are EXPECTED here and are
  # suppressed by name rather than by lowering the whole severity floor, so a real defect still
  # lands. --inline-suppr lets a source file justify a finding at the line it happens on.
  #
  # --check-level=exhaustive is deliberate. Without it cppcheck prints an information line on
  # every file saying it limited its branch analysis, which would either fail the group forever
  # or have to be suppressed. Suppressing it would mean the group silently checked less than it
  # claims, so the honest fix is to do the deeper analysis. These files are small.
  local out
  out="$("$CPPCHECK" --quiet --error-exitcode=1 --inline-suppr --check-level=exhaustive \
          --enable=warning,performance,portability \
          --suppress=missingInclude --suppress=missingIncludeSystem \
          --suppress=unmatchedSuppression \
          "${c_list[@]}" 2>&1)" || true
  assert_empty "cppcheck clean (warning, performance, portability)" "$out"
}

# ---------------------------------------------------------------------------
# T7  fresh clone: the shipped tree, somewhere else, with no local config
# ---------------------------------------------------------------------------
t7_fresh_clone() {
  group "T7  fresh clone, no env.local.sh"
  if ! is_repo; then
    skip "the whole fresh-clone group" "not a git checkout, so there is no shipped set to copy"
    return
  fi
  TMP="${TMP:-$(mktemp -d)}"
  local proj="$TMP/fresh/proj" home="$TMP/fresh/home"
  mkdir -p "$proj" "$home"
  # Only what git tracks, so the gitignored env.local.sh cannot travel with it.
  tracked | tar -C "$ROOT" -cf - -T - 2>/dev/null | tar -C "$proj" -xf -

  assert_empty "the tracked tree copies cleanly" \
    "$( [ -n "$(ls -A "$proj" 2>/dev/null)" ] || echo 'nothing was copied' )"

  assert_empty "env.local.sh did not travel with it" \
    "$(find "$proj" -name 'env.local.sh' 2>/dev/null)"

  # The scripts must be sourceable with an empty HOME and nothing configured. They are run with
  # a no-op first argument so nothing tries to reach a target.
  local f out broken=""
  for f in "$proj"/scripts/*.sh; do
    [ -f "$f" ] || continue
    out="$(env -i HOME="$home" PATH="$PATH" bash -n "$f" 2>&1)" || broken="$broken$f: $out"$'\n'
  done
  assert_empty "every shipped script parses under an empty environment" "$broken"

  assert_empty "env.example.sh ships, so a downloader knows what to set" \
    "$( [ -f "$proj/scripts/env.example.sh" ] || echo 'scripts/env.example.sh is missing from the tracked set' )"
}

# ---------------------------------------------------------------------------

WANTED=("$@")
printf '%stest suite%s  (%s, %s side)\n' "$B" "$N" "$ROOT" "$SIDE"

want_group T1 && t1_lint
want_group T2 && t2_hygiene
want_group T3 && t3_docs
want_group T4 && t4_params
want_group T5 && t5_parse
want_group T6 && t6_cstatic
want_group T7 && t7_fresh_clone

printf '\n%s----- summary -----%s\n' "$B" "$N"
printf '  %s%s passed%s, %s%s failed%s, %s%s skipped%s\n' \
  "$G" "$PASS" "$N" "$([ "$FAIL" -gt 0 ] && printf '%s' "$R")" "$FAIL" "$N" \
  "$([ "$SKIPPED" -gt 0 ] && printf '%s' "$Y")" "$SKIPPED" "$N"

if [ "$SKIPPED" -gt 0 ]; then
  printf '\n  not checked on this machine:\n'
  printf '    %s\n' "${SKIPPED_NAMES[@]}"
fi
if [ "$FAIL" -gt 0 ]; then
  printf '\n  %sfailed:%s\n' "$R" "$N"
  printf '    %s\n' "${FAILED_NAMES[@]}"
  exit 1
fi
exit 0
