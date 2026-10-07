#!/bin/bash
# Constrained reachability proof for the drawing-status local IPC.
#
# Contract: docs/relay-drawing-handoff-contract.md, revision
# plank-drawing-handoff-v1+r3, sections 8.1 to 8.5.
#
# Proves, under the real installed service restrictions, that:
#   A0  a world-accessible filesystem socket IS reachable  (harness sanity gate)
#   A   the withdrawn r1 design (0700 dir + 0600 socket)   FAILS with EACCES
#   B   an abstract AF_UNIX socket IS reachable
#   C   the server's SO_PEERCRED check refuses a third unprivileged peer,
#       with zero bytes received
#   D   the client's SO_PEERCRED check refuses a squatting server whose uid is
#       nonzero and different from the expected server uid
#   D2  a second bind of a held abstract name fails with EADDRINUSE
#   E   the client can stat() a 0700 directory owned by another uid without
#       being able to read inside it
#
# A0 is the credibility gate. If A0 fails, the harness is blocking filesystem
# sockets for an unrelated reason and result A proves nothing: the run is VOID.
#
# Run as root:  sudo bash tests/local-status-reachability-proof.sh
#
# It REFUSES TO RUN unless it can actually apply the full managed restriction set
# and observe CapEff=0000000000003000 (CAP_NET_ADMIN|CAP_NET_RAW only) as uid 0.
# It exits non-zero if the A0 sanity gate fails, if the negative control does not
# fail specifically with EACCES, if any server fails to start or bind, if any
# assertion is not decisively satisfied, or if any resource it created survives
# cleanup.
#
# ISOLATION: not registered with add_test, not in the default ctest set, not
# invoked by any build script. Hand-run only. Do not wire it in.
#
# SAFETY PROPERTIES (contract 8.5):
#   * Every unit name, runtime directory and abstract socket name carries a
#     unique per-invocation run identifier, so two runs or two users cannot
#     collide.
#   * Scratch space is created EXCLUSIVELY (mkdir without -p). A pre-existing
#     directory is never deleted to make room; the run aborts instead.
#   * Overrides are VALIDATED and the run is REFUSED on a bad one. Nothing is
#     silently sanitised.
#   * Cleanup touches only resources this invocation successfully created, and
#     never stops a unit it did not start.
#   * INT and TERM are handled as well as EXIT.
#   * Every privileged step is bounded by a timeout.
#   * Cleanup is verified and a surviving resource makes the run fail.
#
# The unprivileged substitute account is a HARNESS DEVICE ONLY. Production
# resolves the packaged raw Relay service account by name and fails closed; see
# contract sections 8.3 and 8.3a.
#
# Self-test: PROOF_SOURCE_ONLY=1 makes this file define its functions and return
# without creating, starting or deleting anything, so the validation, tracking
# and judging logic can be exercised without root.
#
# Overrides, all validated, none required:
#   PROOF_ABSTRACT_NAME  PROOF_SCRATCH_BASE  PROOF_SERVER_ACCOUNT  PROOF_RUN_ID
#   PROOF_STEP_TIMEOUT
# An overridden PROOF_ABSTRACT_NAME MUST still contain the run identifier, so two
# concurrent runs can never share an abstract socket name. An override that drops
# it is refused, not patched up. PROOF_STEP_TIMEOUT must be a positive integer in
# 5..600 seconds; 0 is refused because `timeout 0` disables the bound entirely.

set -u

# Names that must never be used by a test. The first is the production status
# socket; the second is the live capture interlock (src/capture_lease.hpp:9),
# binding which would steal tablet ownership.
PRODUCTION_SOCKET_NAME=plank-tablet-drawing-status-v1
CAPTURE_LEASE_NAME=plank-tablet-capture-v1
PRODUCTION_ACCOUNT=plank-relay
EXPECT_CAPEFF=0000000000003000
STEP_TIMEOUT=${PROOF_STEP_TIMEOUT:-60}
# Resolved once in the preamble. Every bounded command goes through run_bounded,
# which is also the single seam a self-test can mock, so the ownership and
# fail-closed controls can be exercised without root.
PROOF_TIMEOUT_BIN=${PROOF_TIMEOUT_BIN:-}

run_bounded() { # command...
    if [[ -n ${PROOF_TIMEOUT_BIN:-} ]]; then
        "$PROOF_TIMEOUT_BIN" "$STEP_TIMEOUT" "$@"
    else
        "$@"
    fi
}

PASS=0; FAIL=0; INVALID=0
CLEANUP_VERIFIED=unknown
# Accumulates "cannot determine" reasons raised outside verify_cleanup, e.g. an
# ambiguous launch whose ownership could not be reconciled. Folded in below.
UNVERIFIED_EXTRA=""
LAUNCH_FOREIGN=
declare -a SUMMARY=()
declare -a CREATED_UNITS=()
declare -a CREATED_DIRS=()

# --------------------------------------------------------------- validation
# Each validator prints one reason to stderr and returns 1. Callers REFUSE.

validate_run_id() {
    local v=$1
    [[ -n $v ]] || { echo "run id is empty" >&2; return 1; }
    [[ ${#v} -ge 6 && ${#v} -le 32 ]] || { echo "run id length must be 6..32" >&2; return 1; }
    [[ $v =~ ^[a-z0-9]+$ ]] || { echo "run id must match ^[a-z0-9]+$" >&2; return 1; }
    return 0
}

validate_abstract_name() {
    local v=$1
    [[ -n $v ]] || { echo "abstract name is empty" >&2; return 1; }
    [[ ${#v} -le 100 ]] || { echo "abstract name longer than 100 characters" >&2; return 1; }
    [[ $v =~ ^[A-Za-z0-9._-]+$ ]] || { echo "abstract name must match ^[A-Za-z0-9._-]+$" >&2; return 1; }
    [[ $v != "$PRODUCTION_SOCKET_NAME" ]] || { echo "refusing the PRODUCTION socket name: $v" >&2; return 1; }
    [[ $v != "$CAPTURE_LEASE_NAME" ]] || { echo "refusing the live CAPTURE LEASE name: $v" >&2; return 1; }
    [[ $v == *proof* ]] || { echo "abstract name must contain 'proof' to mark it test-only: $v" >&2; return 1; }
    # An override that drops the run identifier reintroduces cross-run collision.
    [[ -n ${RUN_ID:-} ]] || { echo "run id is not set; cannot validate the abstract name" >&2; return 1; }
    [[ $v == *"$RUN_ID"* ]] || {
        echo "abstract name must contain the run id ($RUN_ID) to prevent cross-run collision: $v" >&2
        return 1; }
    return 0
}

validate_step_timeout() {
    local v=$1
    [[ -n $v ]] || { echo "step timeout is empty" >&2; return 1; }
    [[ $v =~ ^[0-9]+$ ]] || { echo "step timeout must be a positive integer: $v" >&2; return 1; }
    # `timeout 0` disables the bound completely, so zero is refused outright.
    [[ $v -ge 5 && $v -le 600 ]] || { echo "step timeout must be 5..600 seconds: $v" >&2; return 1; }
    return 0
}

# A scratch base must be an absolute, single-level, non-symlink directory under
# an allowed root. Traversal, symlinks and odd characters are refused outright.
validate_scratch_base() {
    local v=$1
    [[ -n $v ]] || { echo "scratch base is empty" >&2; return 1; }
    [[ $v == /* ]] || { echo "scratch base must be absolute: $v" >&2; return 1; }
    [[ $v != *..* ]] || { echo "refusing path traversal in scratch base: $v" >&2; return 1; }
    [[ $v != */ ]] || { echo "scratch base must not end in a slash: $v" >&2; return 1; }
    [[ $v =~ ^/(run|var/tmp)/[A-Za-z0-9._-]+$ || $v =~ ^/(run|var/tmp)$ ]] || {
        echo "scratch base must be /run, /var/tmp, or one plain level below: $v" >&2; return 1; }
    [[ ! -L $v ]] || { echo "refusing a symlinked scratch base: $v" >&2; return 1; }
    local parent; parent=$(dirname "$v")
    [[ ! -L $parent ]] || { echo "refusing a symlinked parent of the scratch base: $parent" >&2; return 1; }
    [[ -d $v ]] || { echo "scratch base does not exist: $v" >&2; return 1; }
    return 0
}

validate_server_account() {
    local v=$1
    [[ -n $v ]] || { echo "server account is empty" >&2; return 1; }
    [[ $v =~ ^[a-z_][a-z0-9_-]*$ ]] || { echo "server account has invalid characters: $v" >&2; return 1; }
    [[ $v != root ]] || { echo "server account must not be root" >&2; return 1; }
    [[ $v != "$PRODUCTION_ACCOUNT" ]] || {
        echo "refusing the PRODUCTION service account: $v (harness must not use it)" >&2; return 1; }
    local uid
    uid=$(id -u "$v" 2>/dev/null) || { echo "server account does not exist: $v" >&2; return 1; }
    [[ $uid -ne 0 ]] || { echo "server account resolves to uid 0: $v" >&2; return 1; }
    return 0
}

# --------------------------------------------------- created-resource tracking
track_unit() { CREATED_UNITS+=("$1"); }
track_dir() { # dedupe: two phases share one runtime directory name
    local d
    for d in ${CREATED_DIRS+"${CREATED_DIRS[@]}"}; do
        [[ $d == "$1" ]] && return 0
    done
    CREATED_DIRS+=("$1")
}

is_tracked_unit() { # unit
    local u
    for u in ${CREATED_UNITS+"${CREATED_UNITS[@]}"}; do
        [[ $u == "$1" ]] && return 0
    done
    return 1
}

# THE ONLY place in this file that calls `systemctl stop` or `reset-failed`.
# It refuses any unit name not in this invocation's created registry, so a phase
# boundary can never stop a unit that already existed and was not ours.
stop_tracked_unit() { # unit
    local u=$1
    if ! is_tracked_unit "$u"; then
        echo "REFUSING to stop an unowned unit (not created by this run): $u" >&2
        return 1
    fi
    run_bounded systemctl stop "$u" >/dev/null 2>&1
    run_bounded systemctl reset-failed "$u" >/dev/null 2>&1
    return 0
}

# Does a named unit exist? 0 yes, 1 no, 2 CANNOT DETERMINE (query failed).
unit_exists() { # unit
    local out rc
    out=$(run_bounded systemctl list-units --all --no-legend -- "$1" 2>/dev/null)
    rc=$?
    [[ $rc -ne 0 ]] && return 2
    [[ -n ${out//[[:space:]]/} ]] && return 0
    return 1
}

# Bring a possibly-created transient unit under ownership after an AMBIGUOUS
# launch. `timeout systemd-run ...` can fire after systemd already created the
# unit, so a non-zero or timed-out launch does NOT mean no unit exists. If the
# unit is there it is ours, so register it and let the existing
# stop_tracked_unit handle it; there is deliberately no second stop path.
reconcile_unit() { # unit
    local u=$1
    is_tracked_unit "$u" && return 0
    unit_exists "$u"
    case $? in
        0) track_unit "$u"; return 0 ;;
        1) return 0 ;;
        *) UNVERIFIED_EXTRA="$UNVERIFIED_EXTRA reconcile-failed:$u"; return 1 ;;
    esac
}

# Launch a NAMED one-shot transient unit, capture its output, and reconcile
# ownership whether or not the launch completed cleanly.
# Sets LAUNCH_OUT and LAUNCH_RC.
# A launch REJECTED because a unit of that name already exists tells us the
# unit is NOT ours: discovering that it exists is not proof of ownership. Such a
# name must never be adopted and never stopped. Only an AMBIGUOUS launch (a
# timeout or an unexplained failure, where systemd may already have created our
# unit) may reconcile.
launch_rejected_as_existing() { # output
    case $1 in
        *"already exists"*|*"Unit "*" already exists"*) return 0 ;;
        *) return 1 ;;
    esac
}

launch_unit() { # tag command...
    local tag=$1; shift
    local unit="$UNIT_PREFIX-$tag"
    LAUNCH_OUT=$(run_bounded systemd-run --unit="$unit" --wait --pipe --collect "$@" 2>&1)
    LAUNCH_RC=$?
    if [[ $LAUNCH_RC -ne 0 ]] && launch_rejected_as_existing "$LAUNCH_OUT"; then
        # Not ours. Do not track it, do not stop it, and do not claim the run is
        # clean: an unexpected pre-existing name invalidates this invocation.
        UNVERIFIED_EXTRA="$UNVERIFIED_EXTRA foreign-unit-name:$unit"
        LAUNCH_FOREIGN=$unit
        return 1
    fi
    LAUNCH_FOREIGN=
    reconcile_unit "$unit" || true
    return 0
}

# ---- bounded service checks. A failed or timed-out check is CANNOT DETERMINE,
# ---- never "absent". Each pairs a bounded query with a pure decision function.
unit_is_active() { # unit -> echoes state; 0 active, 1 inactive, 2 cannot determine
    local out rc
    out=$(run_bounded systemctl is-active -- "$1" 2>&1); rc=$?
    printf '%s' "$out"
    [[ $rc -eq 0 ]] && return 0
    case ${out//[[:space:]]/} in
        inactive|failed|activating|deactivating|reloading) return 1 ;;
        *) return 2 ;;
    esac
}
decide_is_active() { # rc
    [[ $1 -eq 2 ]] && { echo INVALID-cannot-determine; return 1; }
    [[ $1 -eq 0 ]] && { echo ACTIVE; return 0; }
    echo INACTIVE; return 1
}

unit_journal() { # unit -> sets JOURNAL_OUT; 0 ok, 2 cannot determine
    JOURNAL_OUT=$(run_bounded journalctl -u "$1" --no-pager -o cat 2>/dev/null)
    [[ $? -eq 0 ]] || return 2
    return 0
}
# A journalctl that FAILS must never read as "the server did not report a bind".
decide_server_bound() { # journal_rc journal_out
    [[ $1 -eq 2 ]] && { echo INVALID-journal-unavailable; return 1; }
    grep -q 'SERVER_BOUND' <<<"$2" && { echo BOUND; return 0; }
    echo FAIL-no-bind; return 1
}

unit_main_pid() { # unit -> echoes pid; 0 ok, 2 cannot determine
    local out rc
    out=$(run_bounded systemctl show -p MainPID --value -- "$1" 2>/dev/null); rc=$?
    [[ $rc -eq 0 ]] || return 2
    printf '%s' "${out//[[:space:]]/}"
    return 0
}
# A `systemctl show` that FAILS must never read as "no MainPID".
decide_main_pid() { # rc value
    [[ $1 -eq 2 ]] && { echo INVALID-mainpid-unavailable; return 1; }
    [[ -n $2 && $2 =~ ^[0-9]+$ && $2 -ne 0 ]] && { echo "$2"; return 0; }
    echo INVALID-no-mainpid; return 1
}

# Stop only units this invocation started. Remove only directories it created.
cleanup() {
    local u d
    for u in ${CREATED_UNITS+"${CREATED_UNITS[@]}"}; do
        stop_tracked_unit "$u"
    done
    for d in ${CREATED_DIRS+"${CREATED_DIRS[@]}"}; do
        if [[ -n $d && $d != / && ! -L $d && -d $d ]]; then
            run_bounded rm -rf -- "$d"
        fi
    done
}

# Verify every created resource is gone. FAILS CLOSED: a query that does not
# succeed means "cannot determine", never "clean". Sets CLEANUP_VERIFIED and
# affects the exit status.
verify_cleanup() {
    local u d out rc leftovers="" unverified="$UNVERIFIED_EXTRA"
    for u in ${CREATED_UNITS+"${CREATED_UNITS[@]}"}; do
        out=$(run_bounded systemctl list-units --all --no-legend -- "$u" 2>/dev/null)
        rc=$?
        if [[ $rc -ne 0 ]]; then
            # A failed query produces no output, which is indistinguishable from
            # "gone". Treat it as unverified rather than reporting clean.
            unverified="$unverified unit-query-failed:$u"
        elif [[ -n ${out//[[:space:]]/} ]]; then
            leftovers="$leftovers unit:$u"
        fi
        # rc 0 with empty output: the unit is gone, or `--collect` already
        # collected a one-shot unit. Either way nothing of ours survives.
    done
    for d in ${CREATED_DIRS+"${CREATED_DIRS[@]}"}; do
        if [[ -e $d || -L $d ]]; then
            leftovers="$leftovers dir:$d"
        fi
    done
    if [[ -n $leftovers && -n $unverified ]]; then
        CLEANUP_VERIFIED="LEFTOVERS:$leftovers; UNVERIFIED:$unverified"
        return 1
    fi
    if [[ -n $leftovers ]]; then
        CLEANUP_VERIFIED="LEFTOVERS:$leftovers"
        return 1
    fi
    if [[ -n $unverified ]]; then
        CLEANUP_VERIFIED="UNVERIFIED:$unverified"
        return 1
    fi
    CLEANUP_VERIFIED=clean
    return 0
}

# Any exit after this run created a resource must report cleanup verification.
# An early refusal that created nothing says so plainly.
report_cleanup_verification() { # context
    local units=0 dirs=0
    units=${#CREATED_UNITS[@]}
    dirs=${#CREATED_DIRS[@]}
    if [[ $units -eq 0 && $dirs -eq 0 && -z $UNVERIFIED_EXTRA ]]; then
        echo "cleanup: nothing was created by this run ($1)"
        return 0
    fi
    verify_cleanup
    local rc=$?
    echo "cleanup: $CLEANUP_VERIFIED ($1)"
    return $rc
}

on_signal() {
    echo
    echo "interrupted; cleaning up resources this run created" >&2
    cleanup
    if report_cleanup_verification "interrupted"; then exit 130; fi
    echo "CLEANUP VERIFICATION FAILED on interrupt: $CLEANUP_VERIFIED" >&2
    exit 131
}

# Refuse to continue, cleaning up and reporting verification for anything already
# created. Exit 2 when cleanup verified, 3 when it could not be verified.
refuse() { # message...
    echo "REFUSING TO RUN: $*" >&2
    cleanup
    if report_cleanup_verification "refused before completion"; then exit 2; fi
    echo "CLEANUP VERIFICATION FAILED while refusing: $CLEANUP_VERIFIED" >&2
    exit 3
}

# ------------------------------------------------------------------ reporting
record() { SUMMARY+=("$(printf '%-4s expect=%-30s actual=%-30s %s' "$1" "$2" "$3" "$4")")
           case $4 in PASS) PASS=$((PASS+1));; INVALID) INVALID=$((INVALID+1));; *) FAIL=$((FAIL+1));; esac; }
judge()   { if [[ $2 == "$3" ]]; then record "$1" "$2" "$3" PASS; else record "$1" "$2" "$3" FAIL; fi; }
invalid() { record "$1" "$2" "$3" INVALID; }
banner()  { echo; echo "=================================================================="; echo "$*"; echo "=================================================================="; }

# ------------------------------------------------------- decisive assertions
# Each returns the verdict string so the body and the self-test evaluate the
# SAME logic. A condition that cannot be determined is INVALID, never PASS.

# C requires BOTH the expected exit status AND exactly zero received bytes.
decide_c() { # rc output
    local rc=$1 out=$2
    if [[ $rc -eq 6 ]] && grep -q 'RESULT=CLOSED_WITHOUT_REPLY bytes=0' <<<"$out"; then
        echo PASS; return 0
    fi
    echo FAIL; return 1
}

# D's squatter uid must be determinable, nonzero, and different from the
# expected server uid; otherwise the test proves nothing.
decide_d_squat_uid() { # squat_uid expected_uid
    local squat=$1 expected=$2
    [[ -n $squat ]] || { echo INVALID; return 1; }
    [[ $squat =~ ^[0-9]+$ ]] || { echo INVALID; return 1; }
    if [[ $squat -eq 0 || $squat -eq $expected ]]; then echo INVALID; return 1; fi
    echo PASS; return 0
}

# D requires the expected exit status AND the peerUnverified reason naming both uids.
decide_d() { # rc output squat_uid expected_uid
    local rc=$1 out=$2 squat=$3 expected=$4
    if [[ $rc -eq 4 ]] \
       && grep -q "reason=service.peerUnverified server_uid=$squat expected=$expected" <<<"$out"; then
        echo PASS; return 0
    fi
    echo FAIL; return 1
}

# D2 requires EADDRINUSE specifically, and the probe itself must have run.
decide_d2() { # rc output
    local rc=$1 out=$2
    if [[ $rc -eq 0 ]] && grep -q 'SECOND_BIND=FAILED errno=98' <<<"$out"; then
        echo PASS; return 0
    fi
    echo FAIL; return 1
}

# E requires the probe's own exit status AND both observations.
decide_e() { # rc output expected_uid
    local rc=$1 out=$2 expected=$3
    if [[ $rc -eq 0 ]] \
       && grep -q "STAT=OK uid=$expected mode=700" <<<"$out" \
       && grep -q 'LISTDIR=FAILED errno=EACCES' <<<"$out"; then
        echo PASS; return 0
    fi
    echo FAIL; return 1
}

if [[ ${PROOF_SOURCE_ONLY:-0} == 1 ]]; then
    return 0 2>/dev/null || exit 0
fi

# ----------------------------------------------------------------- preamble
if [[ $(id -u) -ne 0 ]]; then echo "must run as root" >&2; exit 2; fi
for b in systemd-run systemctl python3 timeout id stat; do
    command -v "$b" >/dev/null || { echo "$b is required" >&2; exit 2; }
done
PROOF_TIMEOUT_BIN=$(command -v timeout)

RUN_ID=${PROOF_RUN_ID:-$(head -c 8 /dev/urandom | od -An -tx1 | tr -d ' \n')}
SERVER_ACCOUNT=${PROOF_SERVER_ACCOUNT:-nobody}
SCRATCH_BASE=${PROOF_SCRATCH_BASE:-/run}
ABSTRACT=${PROOF_ABSTRACT_NAME:-plank-tablet-drawing-status-proof-$RUN_ID}

validate_step_timeout "$STEP_TIMEOUT"   || { echo "REFUSING TO RUN: bad step timeout." >&2; exit 2; }
validate_run_id "$RUN_ID"               || { echo "REFUSING TO RUN: bad run id." >&2; exit 2; }
validate_server_account "$SERVER_ACCOUNT" || { echo "REFUSING TO RUN: bad server account." >&2; exit 2; }
validate_scratch_base "$SCRATCH_BASE"   || { echo "REFUSING TO RUN: bad scratch base." >&2; exit 2; }
validate_abstract_name "$ABSTRACT"      || { echo "REFUSING TO RUN: bad abstract socket name." >&2; exit 2; }

SERVER_UID=$(id -u "$SERVER_ACCOUNT")
SCRATCH=$SCRATCH_BASE/plank-handoff-proof-$RUN_ID
RTDIR=plank-handoff-proof-$RUN_ID-rt
RTPATH=/run/$RTDIR
UNIT_PREFIX=plank-handoff-proof-$RUN_ID

trap on_signal INT TERM
trap cleanup EXIT

# Exclusive creation. A pre-existing directory is NEVER deleted to make room.
if ! mkdir -m 0755 "$SCRATCH" 2>/dev/null; then
    refuse "could not exclusively create $SCRATCH (it may already exist);" \
           "this harness never deletes a pre-existing directory to claim it."
fi
track_dir "$SCRATCH"
if [[ -e $RTPATH ]]; then
    refuse "$RTPATH already exists; not reusing or deleting it."
fi

# Restrictions copied verbatim from debian/plank-avp-relay.service:18-35.
MANAGED=(
  --property=User=root
  --property=CapabilityBoundingSet=CAP_NET_ADMIN\ CAP_NET_RAW
  --property=NoNewPrivileges=yes
  --property=ProtectSystem=strict
  --property=ProtectHome=yes
  --property=PrivateTmp=yes
  --property=ProtectKernelTunables=yes
  --property=ProtectKernelModules=yes
  --property=ProtectKernelLogs=yes
  --property=ProtectControlGroups=yes
  --property=RestrictRealtime=yes
  --property=RestrictNamespaces=yes
  --property=RestrictAddressFamilies=AF_UNIX\ AF_BLUETOOTH\ AF_INET\ AF_INET6
  --property=LockPersonality=yes
  --property=DevicePolicy=closed
)
# Restrictions from packaging/plank-tablet-relay.service:8-23, with User
# substituted: the harness MUST NOT create the real service account.
RAW=(
  --property=User="$SERVER_ACCOUNT"
  --property=NoNewPrivileges=true
  --property=ProtectSystem=strict
  --property=ProtectHome=true
  --property=PrivateTmp=true
  --property=ProtectKernelTunables=true
  --property=ProtectControlGroups=true
  --property=UMask=0077
)

# --------------------------------------------------------------- preflight
launch_unit preflight "${MANAGED[@]}" \
    /usr/bin/python3 -c "
import os
st = dict(l.split(':', 1) for l in open('/proc/self/status') if ':' in l)
print('CapEff=%s CapBnd=%s euid=%d' % (st['CapEff'].strip(), st['CapBnd'].strip(), os.geteuid()))
"
preflight=$LAUNCH_OUT
preflight_rc=$LAUNCH_RC
echo "preflight: $preflight"
if [[ $preflight_rc -ne 0 ]]; then
  echo "REFUSING TO RUN: could not start a transient unit with the managed restriction set." >&2
  echo "This host cannot currently reproduce 'root with a restricted capability bounding set'." >&2
  echo "Privileged authentication on the target may simply be outstanding." >&2
  echo "'systemd-run --user' is not a substitute: a user unit runs as an unprivileged" >&2
  echo "uid with no capabilities at all, which is not the condition under test." >&2
  exit 2
fi
grep -q "CapEff=$EXPECT_CAPEFF" <<<"$preflight" || {
  echo "REFUSING TO RUN: expected CapEff=$EXPECT_CAPEFF (CAP_NET_ADMIN|CAP_NET_RAW only)." >&2
  echo "Observed: $preflight" >&2; exit 2; }
grep -q 'euid=0' <<<"$preflight" || { echo "REFUSING TO RUN: client side is not uid 0." >&2; exit 2; }
echo "preflight OK: managed restriction set applies, CapEff=$EXPECT_CAPEFF, euid=0"
echo "run id: $RUN_ID   scratch: $SCRATCH   abstract: $ABSTRACT   server uid: $SERVER_UID"

# ----------------------------------------------------------------- payloads
cat > "$SCRATCH/server.py" <<'PY'
import json, os, socket, struct, sys, time
mode, addr, expect_client = sys.argv[1], sys.argv[2], int(sys.argv[3])
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
if mode == 'abstract':
    s.bind('\0' + addr)
else:
    s.bind(addr); os.chmod(addr, int(sys.argv[4], 8))
s.listen(4)
sys.stderr.write('SERVER_BOUND euid=%d mode=%s addr=%r\n' % (os.geteuid(), mode, addr))
sys.stderr.flush()
s.settimeout(20)
end = time.time() + 20
while time.time() < end:
    try: c, _ = s.accept()
    except socket.timeout: break
    with c:
        _, uid, _ = struct.unpack('3i', c.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize('3i')))
        if uid != expect_client:
            sys.stderr.write('SERVER REFUSED peer uid=%d (expected %d): closing with no reply\n' % (uid, expect_client))
            sys.stderr.flush(); continue
        sys.stderr.write('SERVER ACCEPTED peer uid=%d\n' % uid); sys.stderr.flush()
        c.sendall((json.dumps({'version': 1, 'ok': True, 'supported': True, 'state': 'ready',
            'listener': {'drawingIdentity': 'b4' + '0' * 62,
                         'drawingProtocol': {'name': 'pltr-raw-hid', 'version': 1, 'rawHID': 1, 'linkType': 2},
                         'boundAddress': '0.0.0.0', 'boundPort': 28990}}, separators=(',', ':')) + '\n').encode())
PY

cat > "$SCRATCH/client.py" <<'PY'
import errno, json, os, socket, struct, sys
mode, addr, expect_server = sys.argv[1], sys.argv[2], int(sys.argv[3])
st = dict(l.split(':', 1) for l in open('/proc/self/status') if ':' in l)
sys.stderr.write('CLIENT euid=%d CapEff=%s CapBnd=%s\n' % (os.geteuid(), st['CapEff'].strip(), st['CapBnd'].strip()))
sys.stderr.flush()
c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); c.settimeout(0.75)
try:
    c.connect(('\0' + addr) if mode == 'abstract' else addr)
except OSError as e:
    print('RESULT=CONNECT_FAILED errno=%s(%d) %s' % (errno.errorcode.get(e.errno, '?'), e.errno, e.strerror)); sys.exit(3)
_, uid, _ = struct.unpack('3i', c.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize('3i')))
sys.stderr.write('CLIENT server peer uid=%d (expected %d)\n' % (uid, expect_server)); sys.stderr.flush()
if uid != expect_server:
    print('RESULT=REFUSED_BY_CLIENT reason=service.peerUnverified server_uid=%d expected=%d' % (uid, expect_server)); sys.exit(4)
data = bytearray()
try:
    while not data.endswith(b'\n'):
        part = c.recv(4097 - len(data))
        if not part: break
        data.extend(part)
except socket.timeout:
    print('RESULT=NO_REPLY_TIMEOUT bytes=%d' % len(data)); sys.exit(5)
if not data.endswith(b'\n'):
    print('RESULT=CLOSED_WITHOUT_REPLY bytes=%d' % len(data)); sys.exit(6)
b = json.loads(data)
print('RESULT=OK state=%s boundAddress=%s boundPort=%d' % (b['state'], b['listener']['boundAddress'], b['listener']['boundPort']))
PY

cat > "$SCRATCH/statprobe.py" <<'PY'
import errno, os, sys
t = sys.argv[1]
rc = 0
try:
    st = os.stat(t); print('STAT=OK uid=%d mode=%o' % (st.st_uid, st.st_mode & 0o777))
except OSError as e:
    print('STAT=FAILED errno=%s' % errno.errorcode.get(e.errno, e.errno)); rc = 1
try:
    os.listdir(t); print('LISTDIR=OK')
except OSError as e:
    print('LISTDIR=FAILED errno=%s' % errno.errorcode.get(e.errno, e.errno))
sys.exit(rc)
PY
chmod 644 "$SCRATCH"/*.py

# ------------------------------------------------------------------ helpers
# A server that fails to start or fails to bind is an INVALID proof, never a
# silent pass. Startup output is captured, not discarded.
start_server() { # label unit args...
    local label=$1 unit=$2; shift 2
    local out rc state src
    out=$(run_bounded systemd-run --unit="$unit" --collect "$@" 2>&1); rc=$?
    if [[ $rc -eq 0 ]]; then
        track_unit "$unit"
    else
        # Ambiguous: the unit may exist even though the launch did not report
        # success. Reconcile ownership before declaring anything.
        reconcile_unit "$unit" || true
        echo "  SERVER START FAILED rc=$rc: $out"
        invalid "$label" "server-started" "start-failed(rc=$rc)"
        return 1
    fi
    sleep 1.5
    state=$(unit_is_active "$unit"); rc=$?
    case $(decide_is_active "$rc") in
        ACTIVE) : ;;
        INVALID-cannot-determine)
            echo "  COULD NOT DETERMINE WHETHER THE SERVER IS ACTIVE: ${state:-<no output>}"
            invalid "$label" "server-active" "is-active-unavailable"
            return 1 ;;
        *)
            echo "  SERVER NOT ACTIVE after start: ${state:-<no output>}"
            unit_journal "$unit" && tail -5 <<<"$JOURNAL_OUT" | sed 's/^/    /'
            invalid "$label" "server-active" "not-active"
            return 1 ;;
    esac
    unit_journal "$unit"; rc=$?
    src=$(decide_server_bound "$rc" "${JOURNAL_OUT:-}")
    case $src in
        BOUND) : ;;
        INVALID-journal-unavailable)
            # A failed journal read is NOT evidence that the server did not bind.
            echo "  COULD NOT READ THE SERVER JOURNAL; bind state undetermined"
            invalid "$label" "server-bound" "journal-unavailable"
            return 1 ;;
        *)
            echo "  SERVER DID NOT REPORT A SUCCESSFUL BIND"
            tail -5 <<<"${JOURNAL_OUT:-}" | sed 's/^/    /'
            invalid "$label" "server-bound" "no-bind"
            return 1 ;;
    esac
    return 0
}

run_client() { # tag mode addr expected_uid -> sets CLIENT_OUT, CLIENT_RC
    local tag=$1; shift
    launch_unit "$tag" "${MANAGED[@]}" \
        /usr/bin/python3 "$SCRATCH/client.py" "$1" "$2" "$3"
    CLIENT_OUT=$LAUNCH_OUT
    CLIENT_RC=$LAUNCH_RC
    echo "$CLIENT_OUT" | sed 's/^/  /'
    echo "  exit-status=$CLIENT_RC"
}

banner "A0  harness sanity gate: world-accessible filesystem socket MUST succeed"
# systemd creates RTPATH for the RuntimeDirectory= units, so it is a resource
# this run caused to exist and must be verified gone at the end.
track_dir "$RTPATH"
if start_server A0 "$UNIT_PREFIX-a0" "${RAW[@]}" \
      --property=RuntimeDirectory="$RTDIR" --property=RuntimeDirectoryMode=0755 \
      /usr/bin/python3 "$SCRATCH/server.py" filesystem "$RTPATH/status.sock" 0 0666; then
    ls -ld "$RTPATH" "$RTPATH/status.sock" | sed 's/^/  /'
    run_client a0-client filesystem "$RTPATH/status.sock" "$SERVER_UID"
    if [[ $CLIENT_RC -eq 0 ]] && grep -q 'RESULT=OK' <<<"$CLIENT_OUT"; then
        judge A0 reachable reachable
    else judge A0 reachable "rc=$CLIENT_RC"; fi
fi
stop_tracked_unit "$UNIT_PREFIX-a0" || true

banner "A   NEGATIVE CONTROL: withdrawn r1 design, 0700 dir + 0600 socket, MUST fail EACCES"
A_OK=0
if start_server A "$UNIT_PREFIX-a" "${RAW[@]}" \
      --property=RuntimeDirectory="$RTDIR" --property=RuntimeDirectoryMode=0700 \
      /usr/bin/python3 "$SCRATCH/server.py" filesystem "$RTPATH/status.sock" 0 0600; then
    ls -ld "$RTPATH" "$RTPATH/status.sock" | sed 's/^/  /'
    run_client a-client filesystem "$RTPATH/status.sock" "$SERVER_UID"
    if [[ $CLIENT_RC -eq 3 ]] && grep -q 'errno=EACCES(13)' <<<"$CLIENT_OUT"; then
        judge A EACCES EACCES; A_OK=1
    else judge A EACCES "rc=$CLIENT_RC/other"; fi

    banner "E   expected-uid corroboration: stat() MUST work, listdir() MUST fail EACCES"
    launch_unit e-probe "${MANAGED[@]}" \
        /usr/bin/python3 "$SCRATCH/statprobe.py" "$RTPATH"
    probe=$LAUNCH_OUT
    probe_rc=$LAUNCH_RC
    echo "$probe" | sed 's/^/  /'
    echo "  exit-status=$probe_rc"
    if [[ $(decide_e "$probe_rc" "$probe" "$SERVER_UID") == PASS ]]; then
        judge E stat-ok/list-eacces stat-ok/list-eacces
    else judge E stat-ok/list-eacces "rc=$probe_rc/other"; fi
fi
stop_tracked_unit "$UNIT_PREFIX-a" || true

banner "B   CHOSEN DESIGN: abstract socket with two-way SO_PEERCRED, MUST succeed"
if start_server B "$UNIT_PREFIX-b" "${RAW[@]}" \
      /usr/bin/python3 "$SCRATCH/server.py" abstract "$ABSTRACT" 0; then
    run_client b-client abstract "$ABSTRACT" "$SERVER_UID"
    if [[ $CLIENT_RC -eq 0 ]] && grep -q 'RESULT=OK' <<<"$CLIENT_OUT"; then
        judge B reachable reachable
    else judge B reachable "rc=$CLIENT_RC"; fi
fi
stop_tracked_unit "$UNIT_PREFIX-b" || true

banner "C   server peer check: a third unprivileged peer MUST be refused with ZERO bytes"
if start_server C "$UNIT_PREFIX-c" "${RAW[@]}" \
      /usr/bin/python3 "$SCRATCH/server.py" abstract "$ABSTRACT" 0; then
    launch_unit c-client --property=User="$SERVER_ACCOUNT" \
        /usr/bin/python3 "$SCRATCH/client.py" abstract "$ABSTRACT" "$SERVER_UID"
    c_out=$LAUNCH_OUT
    c_rc=$LAUNCH_RC
    echo "$c_out" | sed 's/^/  /'
    echo "  exit-status=$c_rc"
    # Decisive: BOTH the expected exit status AND exactly zero received bytes.
    if [[ $(decide_c "$c_rc" "$c_out") == PASS ]]; then
        judge C "rc6+bytes=0" "rc6+bytes=0"
    else
        bytes=$(sed -n 's/.*CLOSED_WITHOUT_REPLY bytes=\([0-9]*\).*/\1/p' <<<"$c_out")
        judge C "rc6+bytes=0" "rc=$c_rc+bytes=${bytes:-none}"
    fi
    echo "--- server journal ---"
    if unit_journal "$UNIT_PREFIX-c"; then
        grep -E 'SERVER' <<<"$JOURNAL_OUT" | sed 's/^/  /'
    else
        echo "  COULD NOT READ THE SERVER JOURNAL"
        invalid C "server-journal" "journal-unavailable"
    fi
fi
stop_tracked_unit "$UNIT_PREFIX-c" || true

banner "D   client peer check: a squatting server on a DIFFERENT NONZERO uid MUST be refused"
if start_server D "$UNIT_PREFIX-d" --property=DynamicUser=yes \
      /usr/bin/python3 "$SCRATCH/server.py" abstract "$ABSTRACT" 0; then
    pid_raw=$(unit_main_pid "$UNIT_PREFIX-d"); pid_rc=$?
    squat_pid=$(decide_main_pid "$pid_rc" "$pid_raw")
    if [[ $squat_pid == INVALID-mainpid-unavailable ]]; then
        # A failed `systemctl show` is NOT evidence that there is no MainPID.
        echo "  COULD NOT READ THE SQUATTER MainPID; ownership undetermined"
        invalid D "squat-uid-distinct" "mainpid-unavailable"
    elif [[ $squat_pid == INVALID-no-mainpid ]]; then
        echo "  squatter reported no MainPID"
        invalid D "squat-uid-distinct" "no-mainpid"
    else
    squat_uid=$(stat -c %u "/proc/$squat_pid" 2>/dev/null)
    echo "  squatter pid=$squat_pid uid=${squat_uid:-unknown}"
    # The squatter must genuinely differ from the expected uid and not be root,
    # otherwise this test proves nothing.
    if [[ $(decide_d_squat_uid "${squat_uid:-}" "$SERVER_UID") != PASS ]]; then
        invalid D "squat-uid-distinct" "uid=${squat_uid:-unknown} unusable"
    else
        run_client d-client abstract "$ABSTRACT" "$SERVER_UID"
        if [[ $(decide_d "$CLIENT_RC" "$CLIENT_OUT" "$squat_uid" "$SERVER_UID") == PASS ]]; then
            judge D "rc4+peerUnverified" "rc4+peerUnverified"
        else judge D "rc4+peerUnverified" "rc=$CLIENT_RC/other"; fi

        echo "--- D2  a second bind of the held abstract name MUST fail EADDRINUSE ---"
        launch_unit d2-probe "${RAW[@]}" \
            /usr/bin/python3 -c "
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    s.bind('\0$ABSTRACT'); print('SECOND_BIND=OK')
except OSError as e: print('SECOND_BIND=FAILED errno=%d %s' % (e.errno, e.strerror))"
        d2_out=$LAUNCH_OUT
        d2_rc=$LAUNCH_RC
        echo "$d2_out" | sed 's/^/  /'
        echo "  exit-status=$d2_rc"
        if [[ $(decide_d2 "$d2_rc" "$d2_out") == PASS ]]; then
            judge D2 EADDRINUSE EADDRINUSE
        else judge D2 EADDRINUSE "rc=$d2_rc/$(grep -o 'SECOND_BIND=[A-Z]*' <<<"$d2_out" | head -1)"; fi
    fi
    fi
fi
stop_tracked_unit "$UNIT_PREFIX-d" || true

# ------------------------------------------------------------------ teardown
cleanup
verify_cleanup; cleanup_rc=$?

banner "SUMMARY"
printf '%s\n' ${SUMMARY+"${SUMMARY[@]}"}
echo "cleanup: $CLEANUP_VERIFIED"
echo

status=0
if printf '%s\n' ${SUMMARY+"${SUMMARY[@]}"} | grep -q '^A0 .*FAIL'; then
  echo "A0 FAILED: the harness blocked a world-accessible filesystem socket for some"
  echo "unrelated reason, so result A proves nothing. RUN IS VOID."
  status=1
fi
if [[ $A_OK -ne 1 ]]; then
  echo "NEGATIVE CONTROL FAILED: the withdrawn r1 design did not fail with EACCES."
  echo "Either the harness is not constrained or the r1 conclusion needs revisiting."
  echo "RUN IS VOID."
  status=1
fi
if [[ $INVALID -ne 0 ]]; then
  echo "INVALID PROOF: $INVALID step(s) could not be evaluated (server startup or"
  echo "an undetermined precondition). This is not a pass."
  status=1
fi
if [[ $FAIL -ne 0 ]]; then
  echo "RESULT: $PASS passed, $FAIL FAILED, $INVALID invalid"
  status=1
fi
if [[ $cleanup_rc -ne 0 ]]; then
  echo "CLEANUP VERIFICATION FAILED: $CLEANUP_VERIFIED"
  echo "A resource this run created survived. Remove it by hand before rerunning."
  status=1
fi
if [[ $status -eq 0 ]]; then
  echo "RESULT: all $PASS checks passed; cleanup verified"
  echo "REMAINING GATE: repeat unchanged on the Ubuntu Relay target before promotion."
fi
exit $status
