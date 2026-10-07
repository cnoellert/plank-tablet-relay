# plank-drawing-handoff-v1 shared fixtures

## Provenance

| | |
| --- | --- |
| Contract revision | `plank-drawing-handoff-v1+r3` (supersedes `+r2` and `+r1` — **implement r3 only**) |
| Canonical contract | `docs/relay-drawing-handoff-contract.md` in `cnoellert/plank-tablet-relay` |
| Contract SHA-256 | `1dce3dd079e26bc9d348d3fbea96e035c743e8ace390171862226c9a043545fc` |
| Canonical fixtures | `tests/fixtures/plank-drawing-handoff-v1` in `cnoellert/plank-tablet-relay` |
| `MANIFEST.json` SHA-256 | `dd1125d8257f11431671fe116335c4e3c798c157207bdd576f3c44e724e95954` |
| Reachability harness | `tests/local-status-reachability-proof.sh`, SHA-256 `b5b3c643b54b65bd223468ed26a191be7a2b7b335c49424e7f11153eeea64fdb` |

Pinned sources for every `file:line` citation:

| Repository | Commit |
| --- | --- |
| `cnoellert/plank-tablet-relay` | `e9e0e1cbe22d42839db3db1b5fd509823bdb51cf` |
| `cnoellert/plank-client` | `1ab229ce4740b6b79ae367bef527d487c9e0827f` |
| `cnoellert/plank-avp-relay` | **`7b477b3c75d91fbde667e5b4df4c46a36ea15219`**, branch `codex/managed-capture-lease-onto-main` |
| `cnoellert/plank-avp-relay` upstream base | `812bae39bfa54c30f06961962e6ab51845cf3752` |
| `cnoellert/plank-avp-relay` pre-rebase provenance | `5a915a7cc817b39c150e32975643ae94f9c25e78` (PR #1 head; history only) |

This directory is mirrored byte-for-byte into:

* `cnoellert/plank-client` — `visionos-native/Tests/Fixtures/plank-drawing-handoff-v1`
* `cnoellert/plank-avp-relay` — `tests/fixtures/plank-drawing-handoff-v1`

Do not edit a mirror. A change to a vector is a contract revision: it is made in
the canonical repository by Workstream A, which remints the revision string and
the hashes, and then re-mirrors.

`README.md` is the only file here that `MANIFEST.json` does not hash, because it
records the manifest's own SHA-256.

## Entry points — read this before writing a parser

There are **three** schemas and they are not interchangeable. The directory names
the entry point so a consumer cannot apply the wrong one.

| Directory | Entry point | Required members |
| --- | --- | --- |
| `local/` | 1 — local listener metadata, raw service to managed service | envelope `version`, `ok`, `supported`, `state`; `listener` when ready, `reason` when unavailable. `listener` is exactly `drawingIdentity`, `drawingProtocol`, `boundAddress`, `boundPort`. |
| `status/` | 2 — managed status descriptor, managed service to Setup | wrapper `supported`, `state`; `descriptor` when ready, `reason` when unavailable. `descriptor` is exactly `version`, `drawingIdentity`, `drawingProtocol`, `routes`. |
| `link/` | 3 — app-link descriptor, Setup to PLANK | exactly `version`, `requestID`, `displayName`, `managementIdentity`, `drawingIdentity`, `drawingProtocol`, `routes`. |
| `conversion/` | the contract §9 conversion: listener metadata in, routes or a reason out |
| `migration/` | the contract §10.9 Keychain migration; not a payload to parse. Each `legacyAccounts[]` entry is `{account, pinnedDrawingIdentity}` — a pinned **public** identity, not secret material. |

Three things that are easy to get wrong:

1. **`requestID`, `displayName` and `managementIdentity` are FORBIDDEN in a status
   descriptor and are never required of one.** Requiring any of them would make
   every honest status response invalid. See `status/invalid/with-request-id.json`,
   `with-display-name.json`, `with-management-identity.json`.
2. **Wildcard `0.0.0.0` and loopback `127.0.0.0/8` are legal in `local/` and
   nowhere else.** They are the truth about the listener. Conversion resolves the
   wildcard and refuses loopback with `listener.loopbackOnly`. They can never
   appear in a descriptor: see `status/invalid/route-wildcard.json` and
   `route-loopback.json`.
3. **A `.url` fixture and its `.json` sibling match semantically, not
   byte-for-byte.** `link/valid/known-identity.json` is 650 bytes pretty-printed;
   the payload decoded from `link/valid/known-identity.url` is 506 bytes of compact
   JSON. **Compare parsed values, never file lengths or raw bytes.** The 4096-byte
   limit applies to the decoded compact payload.

## What r3 changed

| Correction | What it means for an implementer |
| --- | --- |
| **Server authorization is uid 0 only** | The raw service accepts peer uid `0` and nothing else. r2 also allowed the server's own uid "for tests"; that widened the production boundary and contradicted measured result C, where a client running as the server's own account was correctly refused. A test needing another uid **injects** the policy explicitly: not a default, not an environment variable, not a config key, not a flag, not a shipped define. |
| **Resolve the service account by name** | The expected server uid comes from `getpwnam("plank-relay")`, the name the unit declares. r2 resolved it from the owner of `/var/lib/plank-tablet-relay`, which is a directory owner, not a service account. Directory ownership is now **corroboration only**: reject symlinks, reject disagreement, fail closed. Never compile in a uid. |
| **`service.peerUnverified` vs `service.absent`** | A name **bound by the wrong uid** is `service.peerUnverified` — the client's own check fired. Only a name **not bound at all** is `service.absent`. Paired fixtures: `status/valid/unavailable-peer-unverified.json` and `status/valid/unavailable.json`. |
| **Non-object `descriptor`/`listener`** | `status.descriptor.type` and `metadata.envelope.listener.type`, checked **before** any member-level reason. Fixtures: `status/invalid/descriptor-not-object.json`, `-null`, `-array`; `local/invalid/listener-not-object.json`, `-null`, `-array`. |
| **Status duplicates need the original bytes** | The surrounding `TabletSetupStatus` decoder is tolerant by design, and a tolerant decode collapses duplicate members before anything can observe them. Keep the response `Data` and scan it. Decoding first and inspecting the object is not an implementation of this check. |
| **Migration member renamed** | Each legacy account entry names its value **`pinnedDrawingIdentity`**, not `key`. It is a pinned *public* identity; the old name was inaccurate and also tripped the repository publication guard's `generic-api-key` rule. |

## File conventions

* `.url` holds exactly one app link plus a single LF. Hash the file as stored;
  strip the one trailing LF before parsing. No CR anywhere.
* `.json` holds payload bytes exactly as they are to be validated. Several carry
  deliberate formatting — indentation, insignificant white space, a duplicate
  member — because that formatting is the thing under test. **Do not reformat a
  fixture.** Reformatting changes its hash and breaks all three repositories.
* `.bin` holds raw bytes that are deliberately not well-formed UTF-8.

Addresses are reserved documentation ranges only: `192.0.2.0/24`,
`198.51.100.0/24`, `203.0.113.0/24`, `2001:db8::/32`, and the name
`relay.example`, plus generic refused classes. Every identity is the SHA-256 of a
documented ASCII label, recorded under `syntheticIdentities` in `MANIFEST.json`,
so anyone can confirm that no real key material is present. No fixture carries a
private-range address, a credential, a pairing code or a capture.

## Required of every consumer

1. Load the fixture **files**. Do not retype a vector inline. A contract revision
   must break a test, not quietly diverge.
2. At least one test per repository asserts the SHA-256 of `MANIFEST.json` against
   the literal above, so contract drift fails a test.
3. Verify each fixture's SHA-256 from `MANIFEST.json` before using it, so a
   corrupted or locally edited mirror fails loudly.
4. Apply **identical** acceptance and rejection behaviour for every rule. A
   consumer that accepts a fixture the manifest marks as a rejection is
   non-conformant regardless of how reasonable its own behaviour seems.
5. New assertions use `precondition` or an explicit failure check, never bare
   `assert`. C `assert` is removed by `NDEBUG` under CMake `Release` and
   `RelWithDebInfo`; Swift `assert` is removed by `-O`. Both were measured.
6. Record a negative control **per harness**, not one per revision: invert one
   assertion, confirm that harness fails with a non-zero exit, restore it, and
   report the inverted assertion and the observed failure.

## The four r2 rules that are easiest to miss

| Rule | Fixtures |
| --- | --- |
| `encoding.nonCanonical` — a final base64url quantum with nonzero unused pad bits must be refused. A permissive decoder returns the right bytes, so re-encode and compare to the original `d` value. | `link/invalid/encoded-noncanonical-2char.url`, `-3char.url` |
| Duplicate member names must be found by a **raw-byte scan before any dictionary decode**, at every depth, comparing names after unescaping. `json.loads` and `JSONSerialization` both collapse duplicates silently. | `link/invalid/payload-duplicate-member.json`, `-nested.json`, `-escaped.json`, `-escaped-nested.json`, plus the acceptance control `link/valid/sibling-members-not-duplicate.json` |
| `version` is **privileged**: presence, then type, then value, all resolved before the member-set check. | `link/invalid/version-missing-with-unknown-member.json`, `version-unsupported-with-unknown-member.json`, `local/invalid/envelope-version-wrong-with-unknown-member.json` |
| Literal canonicalisation does **not** rely on the platform parser. Python and Swift disagree on leading-zero IPv4 and on IPv4-mapped IPv6 rendering, so contract §7.4a specifies a pure-string dotted-quad grammar and a raw-byte mapped-prefix test. | `link/invalid/route-address-non-canonical-v4.json`, `route-address-mapped.json`, `route-address-non-canonical-v6-case.json`, `route-address-non-canonical-v6-expanded.json` |

## How to load, per test style

**Raw and managed C tests** — a CMake `add_test` executable. Pass the directory in
from CMake and read with `fopen`; never embed fixture bytes in the source.

```cmake
target_compile_definitions(handoff_descriptor_test PRIVATE
    PLANK_HANDOFF_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/plank-drawing-handoff-v1")
```

Register it **above** the `if(CMAKE_SYSTEM_NAME STREQUAL "Linux")` guard so it runs
on macOS, and configure with no `CMAKE_BUILD_TYPE` (or `Debug`).

**Managed Python tests** — `python3 tests/<name>.py`, pass or fail by exit code,
never `python3 -O`. Resolve the directory the way `tests/local_service_test.py`
already does and prefer `unittest` assertions, which cannot be optimized away.

```python
FIXTURES = Path(__file__).resolve().parents[1] / 'tests/fixtures/plank-drawing-handoff-v1'
MANIFEST = json.loads((FIXTURES / 'MANIFEST.json').read_text())
for name, meta in MANIFEST['files'].items():
    self.assertEqual(hashlib.sha256((FIXTURES / name).read_bytes()).hexdigest(),
                     meta['sha256'], name)
```

**Setup Apple tests** — a Swift `@main` executable registered in
`apps/tablet-setup/CMakeLists.txt` inside the existing
`if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")` block, using `precondition`, built by
`scripts/build-tablet-setup.sh macos`, which then runs `ctest -C Debug`.

```cmake
set_tests_properties(handoff-descriptor-tests PROPERTIES ENVIRONMENT
    "PLANK_HANDOFF_FIXTURES=${RELAY_ROOT}/tests/fixtures/plank-drawing-handoff-v1")
```

**Client tests** — a standalone Swift `@main` program in `visionos-native/Tests/`
using `precondition`, resolving the directory relative to `#filePath`.

```bash
swiftc -Onone -DPLANK_TABLET_RELAY \
    visionos-native/Sources/Services/PlankRelayHandoff.swift \
    visionos-native/Tests/PlankRelayHandoffTests.swift \
    -o /tmp/handoff-tests && /tmp/handoff-tests
```

A single-file `@main` test additionally needs `-parse-as-library`.

## State-dependent scenarios

`MANIFEST.json` lists these under `scenarios` with ordered steps:

* `duplicate-request` — accept `link/valid/known-identity.url`, then
  `link/trust/replayed-request.url` must be deduplicated as `requestID.duplicate`.
* `active-desktop-session` — the canonical link delivered during an active PLANK
  session must yield `trust.sessionActive`, declined with an offer to use it after
  disconnect. Never interrupt a stroke.
* `unknown-identity-approval` — `link/trust/unknown-drawing-identity.url` must
  reach PLANK's existing explicit physical drawing-approval flow and must never be
  auto-approved.
* `peer-credential-squat` — an unprivileged local process that binds the abstract
  status name first must be refused by the managed client's `SO_PEERCRED` check,
  producing `status/valid/unavailable-peer-unverified.json`.

## The reachability harness is not part of any suite

`tests/local-status-reachability-proof.sh` is privileged and root-only. It is
**not** registered with `add_test`, **not** in the default ctest set, and **not**
invoked by any build script. Running the ordinary suites never triggers it. It was
executed on a Rocky Linux 9.7 proof host under the r2 revision, as Linux kernel and
systemd behaviour evidence only. **Ubuntu Relay target service qualification is an
outstanding gate**: what is outstanding is privileged authentication on that host,
not any property of the target. The r3 harness has **not** been executed; its
validation, tracking, cleanup-verification and decision logic were exercised
without root via `PROOF_SOURCE_ONLY=1`. See contract §8.5 and §13.

Safety properties worth knowing before anyone runs it:

* Every `systemctl stop` and `reset-failed` goes through **one ownership-checked
  helper** that refuses any unit name not in this invocation's created registry.
  The phase boundaries use it too, so a failed start caused by a pre-existing unit
  of the same name can never lead to stopping someone else's unit.
* Cleanup verification **fails closed**. A unit query that does not succeed is
  `UNVERIFIED`, never `clean`, because a failed query produces no output and is
  otherwise indistinguishable from "gone". `UNVERIFIED` and `LEFTOVERS` are
  distinct states and both exit non-zero. A successful empty query is clean, which
  is how a legitimately collected one-shot `--collect` unit is distinguished from a
  query that simply failed.
* Cleanup and verification commands are bounded, not just the privileged steps.
* `PROOF_STEP_TIMEOUT` is validated to a positive integer in 5..600 seconds. `0`
  is refused outright because `timeout 0` removes the bound entirely.
* An overridden `PROOF_ABSTRACT_NAME` must still contain the run identifier, so
  two concurrent runs cannot share an abstract socket name.
* The systemd-created `RuntimeDirectory` path is tracked and verified gone.
* **Every** transient unit gets an explicit `--unit=` name carrying the run id and
  is tracked. An ambiguous launch — a non-zero or timed-out `systemd-run`, which
  can fire *after* systemd already created the unit — is reconciled into ownership
  and then stopped only through the same helper; a reconciliation that cannot be
  completed is `UNVERIFIED`, not clean.
* The `is-active`, `journalctl` and `MainPID` checks are bounded and
  status-checked. A failed journal read never reads as "the server did not bind",
  and a failed `systemctl show` never reads as "no MainPID"; each surfaces as
  `INVALID` naming which check could not be completed.
* Signal and early-refusal exits report cleanup verification for whatever they
  created, and an unverified outcome is visible and non-zero in those paths too.
