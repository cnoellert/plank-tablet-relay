#!/bin/bash
# Control: a launch REJECTED because the unit name already exists must never
# adopt that unit and must never stop it. Paired with a positive control so a
# "never stopped" result cannot pass because the mock is dead.
set -u
PROOF_SOURCE_ONLY=1
source "$1"
PROOF_TIMEOUT_BIN=""
P=0; F=0
ok(){ P=$((P+1)); echo "  PASS  $1"; }
bad(){ F=$((F+1)); echo "  FAIL  $1"; }
CALLS=$(mktemp)
UNIT_PREFIX=proof-run-abc123
FOREIGN="$UNIT_PREFIX-b"

# --- rejected launch: systemd-run refuses because the name is taken
systemd_run_rejects() { printf 'Failed to start transient service unit: Unit %s.service already exists.\n' "$FOREIGN"; return 1; }
systemd-run() { systemd_run_rejects; }
systemctl() { printf '%s\n' "$*" >> "$CALLS"; return 0; }
unit_exists() { return 0; }   # it DOES exist - but it is not ours

CREATED_UNITS=(); CREATED_DIRS=(); UNVERIFIED_EXTRA=""
launch_unit b /bin/true; rc=$?
[[ $rc -ne 0 ]] && ok "a rejected launch returns non-zero" || bad "rejected launch returned 0"
is_tracked_unit "$FOREIGN" && bad "ADOPTED a foreign unit" || ok "never adopted the foreign unit"
[[ "$UNVERIFIED_EXTRA" == *foreign-unit-name* ]] && ok "run marked unverified (foreign-unit-name)" \
  || bad "run not marked unverified"
cleanup >/dev/null 2>&1
if grep -q "stop $FOREIGN" "$CALLS"; then bad "STOPPED a foreign unit"; else ok "never stopped the foreign unit"; fi
verify_cleanup; vrc=$?
[[ $vrc -ne 0 && "$CLEANUP_VERIFIED" != clean ]] && ok "verification is not clean after a foreign name" \
  || bad "reported clean despite a foreign unit name ($CLEANUP_VERIFIED)"

# --- positive control: an AMBIGUOUS launch (timeout, no 'already exists') DOES
# --- reconcile and IS stopped, proving the mocks and the adopt path both work.
: > "$CALLS"; CREATED_UNITS=(); UNVERIFIED_EXTRA=""
systemd-run() { echo "Job for unit timed out"; return 124; }
launch_unit b /bin/true >/dev/null 2>&1
is_tracked_unit "$FOREIGN" && ok "positive control: an ambiguous launch DOES reconcile" \
  || bad "positive control: ambiguous launch did not reconcile"
cleanup >/dev/null 2>&1
grep -q "stop $FOREIGN" "$CALLS" && ok "positive control: a reconciled unit IS stopped" \
  || bad "positive control: reconciled unit was not stopped"
rm -f "$CALLS"
echo "foreign-unit control: $P passed, $F failed"
[[ $F -eq 0 ]]
