# Setup-mediated drawing enrollment v1

This candidate replaces the Client's tablet-specific ExpressKey ceremony with
explicit approval in authenticated Relay Setup. Private app identities remain
separate. Existing drawing approvals and the drawing wire protocol are retained.
The frozen `plank-drawing-handoff-v1+r3` contract and fixtures are unchanged.

## Journey and trust

1. Setup stops its preview and hands its existing public drawing descriptor to
   PLANK. A known drawing identity follows the existing registered-Relay flow.
2. For a new identity, PLANK creates a random 128-bit enrollment request and a
   pending public receipt in the dedicated shared Keychain group. It sends the
   request, its public key and the unchanged descriptor to Setup by app link.
3. Setup requires that exact pending receipt. The user chooses **Allow PLANK**.
   Setup verifies its selected management identity with its existing pin and
   obtains fresh authenticated status showing the requested drawing identity.
   Setup then prepares one enrollment grant through that authenticated session.
4. The root managed service submits a bounded mutation to the raw daemon. Only
   the currently authenticated owner can do this; enrollment/bootstrap sessions
   cannot delegate drawing approval. The raw daemon checks the requested drawing
   identity against its own identity and retains the exact Client key and nonce.
5. Setup marks the protected public receipt approved and opens PLANK. PLANK
   requires that exact approved receipt before connecting. App links alone are
   not approval: another app cannot write the shared Keychain group without an
   appropriate signing entitlement. Only the two apps are configured for it.
6. PLANK proves possession of its private key using a separate Noise IK domain,
   verifies the drawing identity, and confirms enrollment. The raw daemon adds
   that proven Client to its existing allowlist and acknowledges under Noise.
   Only then does PLANK save its drawing pin and register/select the Relay.

Both apps must be signed for the same shared Keychain access group. Its signing
prefix comes from the build, not a hardcoded team identifier. The original
app-local group is first in each entitlement list, preserving existing private
identity storage. The shared group contains public registration receipts only.

## Root-only local mutation

The separate abstract UNIX socket is `plank-tablet-drawing-enrollment-v1`.
Production accepts peer uid 0 only; the managed service resolves `plank-relay`
explicitly and checks the server's `SO_PEERCRED` uid before sending. Test uid
substitutions exist only in the isolated test constructor. The public status
socket remains read-only. No unit restriction, capture lease or tablet node is
changed by enrollment.

All integers below are bytes unless otherwise stated. The request is 86 bytes:

| Field | Bytes |
| --- | --- |
| `PLEN` followed by version byte 1 | 5 |
| action: 1 prepare, 2 cancel | 1 |
| request id | 16 |
| Client public identity | 32 |
| target drawing public identity | 32 |

The reply is 22 bytes: the same five-byte prefix, one status byte and the exact
16-byte request id. Status 0 is accepted; 1 invalid; 2 target mismatch; 3 missing,
used or canceled; 4 capacity exhausted. The managed exchange has one 750 ms
deadline across connect, send and all partial reads. The raw service bounds four
local connections individually to 250 ms.

Grants exist only in RAM: at most 16, each with a 120-second lifetime. States are
pending, claimed and consumed/canceled. A wrong Client cannot claim a grant.
Claiming burns that attempt even if proof later fails. Duplicate ids are refused
through the grant lifetime, including consumed/canceled tombstones. Expired
slots may be reused only by another authenticated prepare; the apps always use
a fresh random id. Restart loses all provisional grants, not prior approvals.

## Dedicated network proof

This enrollment slice uses an advertised TCP route on the existing raw listener.
It does not change drawing transport selection or advertise Bluetooth in the
frozen handoff. Final registered-Relay Bluetooth selection is a separate slice.

The dedicated prefix is `PLEN` plus version byte 1. Noise uses prologue
`PLANK-DRAWING-ENROLLMENT/1`, distinct from drawing, with the drawing identity as
the responder static key. Records use a two-byte little-endian length:

- Client first Noise record: 112 bytes, including the encrypted 16-byte request id.
- Server second Noise record: 48 bytes with no payload.
- Client encrypted confirmation: 37 bytes; plaintext is the five-byte prefix
  followed by the exact request id.
- Server encrypted committed acknowledgment: the same format.

The server's entire proof has one 10-second deadline, including partial reads and
writes. Existing listener classification is separately bounded to five seconds.
While awaiting confirmation it services local cancellation and process stop.
There is no capture worker, drawing data or desktop session during the proof.
PLANK retries alternate routes only before sending a proof. Ambiguous attempts
are not replayed against another address; start a fresh Setup approval instead.

## Cancellation and qualification

Cancel before confirmation, wrong identity, expiry or replay never creates an
allowlist entry. A cancel after the server's durable commit cannot undo that
approval; a lost acknowledgment is an ambiguous commit, not proof of rollback.
PLANK still saves no local pin on a canceled or incomplete exchange. Previously
saved pins and selection are retained. Pending receipts are removed on cancel,
completion or the Client's deadline; expired receipts cannot authorize enrollment.

Focused checks exercise the real Linux socket/Noise exchange, authorization
binding, cancellation, replay, wrong target and explicit test-peer refusal. The
managed checks cover owner-only dispatch, strict envelopes, peer verification
and one absolute deadline. Shared app checks cover exact links, receipt state,
expiry and binding; original handoff and picker checks remain active.

Signed builds and source checks do not establish live Keychain sharing or user
acceptance. Qualification needs first-time **Use in PLANK → Continue in Relay
Setup → Allow PLANK**, drawing and reconnect without ExpressKeys, plus cancel
without changing the existing selection. Preserve existing pairing and pins;
do not reset the working installation to manufacture a first-time test.
