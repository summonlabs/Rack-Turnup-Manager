# Rack Turnup Manager

Rack Turnup Manager is a C++20 library and command line tool that decides whether
a rack in a data center may enter service, and records the authority to do it.
It is the physical-fleet-lifecycle component of the Data Center Control Plane: it
composes generation-stamped evidence that other systems publish, evaluates the
ordered readiness stages of one rack, reports exactly which subsystems are proven
ready and which are unknown, and issues a bounded turnup authorization that is
bound to one exact rack composition. It never talks to hardware, never writes to
another system, and never treats a request as an effect.

* C++20, CMake, no third-party dependencies.
* One static (or shared) library, one command line tool, one durable state file.
* Zero first-party compiler warnings under `/W4 /WX` on MSVC.
* No telemetry. Nothing is transmitted anywhere.

## Systems boundary

### What this repository owns

* **Rack turnup planning.** Binding a turnup plan to a rack composition digest,
  a full set of facility generations, a policy generation and a control epoch.
* **Readiness evaluation.** Reducing generation-stamped evidence to a per-subsystem
  verdict, walking the ordered stage ladder, and aggregating blockers with a
  deterministic precedence.
* **Turnup authority.** Issuing, fencing and consuming a bounded authorization that
  is valid only for the exact composition, revision, epoch and evidence set it was
  issued against.
* **Commissioning coordination.** Recording the post-action observation of the rack
  being active, commissioning against that observation, and the lifecycle that
  follows: draining, decommissioning and re-planning.
* **Durable, crash-consistent state** for all of the above, with a single-writer
  lock, atomic publication and a publication watermark.

### What this repository explicitly does not own

* The **Rack Registry** and the truth of rack identity or membership. This library
  consumes a composition digest; it never maintains the registry.
* **Device commissioning internals** (firmware flashing, burn-in, per-device
  configuration, asset tagging).
* **PDU, UPS and generator control.** Power is observed through published evidence.
* **Airflow and liquid cooling control.** Cooling is observed, never actuated.
* **DFI switch and NIC configuration.** Network attachment, reachability and fabric
  authority are reported by their owners.
* **Workload placement** and anything that runs on the commissioned rack.
* **Physical technician actions.** Recording that an action was observed is not
  performing it.

Where another system owns a fact, this library accepts that fact as an observation
with provenance and a validity window, and decides what the observation proves. A
producer's claim is an input, never an authority.

## Core question

Given the current rack composition and facility generations, is this rack safe and
ready to enter service, which subsystems are proven ready, which are unknown or
failed, and what exact turnup action may be authorized now?

Every command answers part of that question, and `evaluate` answers all of it for
one rack at one explicit authority time.

## Authority, generations and fencing

A turnup decision is only meaningful relative to an exact world. That world is a
`GenerationStamp`: thirteen monotonic generations (rack, composition, topology,
dependency, power, cooling, network, inventory, health, capacity, maintenance,
policy, firmware) plus the rack's composition digest and the control epoch that
issued the plan.

* A **plan** binds to one such world: `PlanBinding{rack, composition digest, stamp,
  epoch}`. Its revision advances by exactly one for every accepted mutation.
* **Evidence** records the world it observed. It contributes to a verdict only when
  its stamp equals the plan binding field for field and its composition digest
  matches - a mismatch is reported per field, never averaged away. Mixed-generation
  evidence therefore cannot be combined into a ready answer.
* An **authorization** binds to the composition digest, the whole stamp, the epoch,
  the policy generation, the plan revision, the digest of the exact verdict it was
  issued from, and the evidence high-water mark it considered.
* Any accepted plan mutation (an evidence import, an activation observation, a
  lifecycle transition) **fences** active authorizations, because the grant no
  longer covers the revision in force. Fencing is monotone: a fence record is
  appended and never removed, and a fenced authorization can never be used again.
* Changing the rack composition supersedes the plan, fences every authorization,
  and advances the rack generation, so the old authority is dead rather than
  inherited.
* Taking control advances the control epoch, fences every active authorization,
  supersedes every active plan, and leaves the rack in `planned`: new authority
  must be created deliberately, under the new epoch.

Idempotent replay is handled before ordinary staleness rejection. A mutation may
carry a request identity; a retry of the same intent returns the recorded receipt
with `replayed=true` instead of being refused for a stale revision, while reusing
that identity for a different intent is `request_id_conflict`.

## Lifecycle and state model

The stage ladder is fixed and ordered. A stage is never satisfied before every
stage above it; a stage that would be satisfied but whose predecessor is not is
reported as `pending` and marked as held back by its predecessor.

| # | stage | gated by |
|---|-------|----------|
| 1 | identity_composition_validation | registry composition digest |
| 2 | power_readiness | power evidence |
| 3 | cooling_readiness | cooling evidence |
| 4 | network_readiness | network evidence |
| 5 | hardware_inventory_closure | inventory evidence |
| 6 | health_validation | health evidence |
| 7 | compatibility_closure | compatibility evidence and plan requirements |
| 8 | turnup_authorization | an active, unfenced, unexpired authorization |
| 9 | activation_observation | a post-action observation of the rack |
| 10 | commissioned_state | a commission record naming that observation |

The rack lifecycle is a separate, explicit position: `registered`, `planned`,
`authorized`, `active`, `commissioned`, `draining`, `drained`, `decommissioned`.
Only documented transitions are allowed, and a lifecycle position that claims a
durable fact is rejected by `validate_state` unless the record that proves it is
present. Draining is not decommissioning: `decommission` requires `drained`.

The overall verdict for a rack at an authority time is one of `blocked`,
`not_ready`, `ready_to_authorize`, `authorized`, `active_observed`,
`commissioned`, `fenced_authority`.

### The seven subsystem rules

Each subsystem is evaluated from the newest eligible evidence record for it, never
from an older one and never from a mixture.

* **Identity and composition.** The registry digest must equal the bound
  composition digest and the declared member count must equal the composition.
* **Power.** Available power is not applied power. Insufficient *available* power
  and insufficient *applied* power are different findings, unverified applied power
  is `unknown` rather than ready, and redundant feeds are required only when the
  plan says so.
* **Cooling.** Capacity that exists is not delivery that is proven. Delivery below
  the plan requirement is blocked, and if capacity alone would have sufficed the
  explanation says exactly that. An inlet temperature above the declared maximum is
  blocked.
* **Network.** Attachment is not reachability and reachability is not authority.
  Ports attached, ports proven reachable, the fabric's topology reference and the
  authority verification are separate fields; the observed topology must be the one
  the plan requires.
* **Inventory closure.** Every composition member must be observed exactly once, in
  the slot the composition declares, readable, and no unknown device may appear.
  The primary reason is deterministic: an unknown member outranks a duplicated
  slot, which outranks a displaced member, which outranks an unreadable position,
  which outranks a missing member.
* **Health.** A failing member blocks; a warning degrades; a missing or unknown
  sample is `unknown` (never healthy) unless the plan explicitly does not require
  proof for every member, in which case it degrades.
* **Compatibility.** Requirements are evaluated against what each member *is*
  (observed baseline and traits), not against what the composition intended. A
  mandatory unsatisfied requirement blocks, a mandatory unproven requirement is
  `unknown`, and an advisory requirement degrades.

## Persistence and recovery

The whole service state lives in one versioned, bounded, integrity-checked file
with an explicit commit point: stage, flush, read back and verify, atomically
replace, update the publication watermark, and only then advance durable fencing.
A process that dies at any of those points leaves exactly one readable generation,
and the test suite proves it by terminating real child processes at all seven
points. See [docs/DURABLE_FORMAT.md](docs/DURABLE_FORMAT.md) for the byte layout,
the validation rules and the commit sequence.

* Cross-process exclusion is a real operating-system lock on a separate lock file,
  taken in fail-fast mode. A second writer is refused with `writer_lock_held`;
  readers never take the lock and never observe a torn generation.
* `<state>.watermark` records the newest published generation. A state file older
  than the watermark - a restored backup - is refused with `stale_durable_state`
  instead of being silently inherited.
* Opening a writable store reconciles authority before answering anything: plans
  whose binding no longer matches, authorizations whose epoch, revision or
  composition no longer hold, and racks marked authorized without an active
  authorization are corrected and reported. Recovery never resurrects authority.
* The stage ladder is always derived from stored facts. Nothing derived is
  persisted, so a restart cannot restore a stale verdict.

## Error model

Every failure is a `TurnupError` with a stable numeric code, a stable machine
name, a one-sentence explanation and structured context (operation, subject,
related, expected, actual, ordered items). Codes are grouped by fault domain:
1xx input, 2xx identity, 3xx authority, 4xx lifecycle, 5xx readiness,
6xx compatibility, 7xx persistence, 8xx limits, 9xx writer. A code is never
reused for a different meaning.

Validation precedence is fixed and independent of map ordering, thread scheduling
or unrelated state, so the same invalid request always resolves to the same
primary error: the request's own identity and format first, then idempotent replay
or conflict, then the rack, then the plan and its revision, then the control epoch,
then the request content, then the generation binding, then the lifecycle and
readiness gates.

Blockers are ordered by a total order - severity, then stage, then subsystem, then
subject text, then code value - and deduplicated, so the primary blocker of a
given state is a function of that state alone.

## Concurrency model

The library is single-threaded by design: one service instance owns one facility
scope and one state file.

* There are no internal threads, no worker pools, no callbacks and no event
  emission. Nothing is called back while state is held, so there is no callback
  under lock, no lock reentrancy, no lock-order inversion, and no join that waits
  on state the joiner is holding.
* The only blocking primitive is the store's writer lock, taken exactly once at
  open time and never re-entered. Opening the same store twice in one process
  fails fast with `writer_lock_held` rather than deadlocking.
* Cross-process exclusion is the operating system's, not the library's: the lock
  is released by the kernel when the holding process dies, which the test suite
  demonstrates with a real terminated child process.
* Publication is a single atomic file replacement, so concurrent readers see one
  whole generation. Readers do not coordinate and cannot block a writer.
* Determinism is a deliberate property: evaluation is a pure function of its
  context, evidence selection sorts by (subsystem, sequence, observation time,
  identity), and no code path consults the host clock unless the caller asks for
  it explicitly.

## Command line usage

The tool is a thin `main` over the library, so everything it does is available to
a program through `rackturnup::run_cli` or directly through `TurnupService`.

```
rack-turnup [global options] <command> [arguments]

global options
  --store <path>       durable state file (default rack_turnup.state)
  --now <rfc3339>      authority time (default: the current time, explicitly)
  --actor <id>         who is asking (default cli)
  --source <ref>       provenance reference for what is being recorded
  --note <text>        free-text note kept with the provenance
  --request-id <id>    idempotency identity for this exact request
  --format text|json   output format (default text)
  --read-only          open the store for reading only

commands
  rack register | rack composition | rack list
  plan create
  evidence power|cooling|network|identity|inventory|health|compatibility|list
  evaluate | blockers | authorize | activate | commission | rollback
  drain begin | drain complete | decommission | take-control | recover
  summary [--rack <id>] | authority list | provenance | rejections
  store inspect | store verify | version | help
```

Exit codes: `0` success, `1` usage error, `2` domain rejection (the payload was
refused by the library, with the error code and context on standard error), `3`
persistence or writer failure.

### A complete turnup cycle

```
rack-turnup --store facility.rtm --now 2026-02-02T02:40:00.000Z \
  rack register --rack rack:r1 --site site:s1 --actor operator \
  --member device=dev:a,unit=1,slot=0,baseline=fw:2026q1,trait=power.ac.208v \
  --member device=dev:b,unit=2,slot=0,baseline=fw:2026q1,trait=power.ac.208v

rack-turnup --store facility.rtm --now 2026-02-02T02:40:00.000Z \
  plan create --rack rack:r1 --actor operator \
  --require-power-mw 10000 --require-cooling-mw 5000 --require-network-ports 2 \
  --require-redundant-feeds --require-topology topo:a \
  --compat kind=trait_on_every_member,trait=power.ac.208v,mandatory=true

rack-turnup --store facility.rtm --now 2026-02-02T02:40:00.020Z \
  evidence power --rack rack:r1 --actor plant --observed-at 2026-02-02T02:40:00.005Z \
  --domain pdu:a --available-mw 20000 --applied-mw 12000 \
  --energized-circuits 2 --redundant --applied-verified

rack-turnup --store facility.rtm --now 2026-02-02T02:40:00.100Z \
  evaluate --rack rack:r1 --at 2026-02-02T02:40:00.100Z

rack-turnup --store facility.rtm --now 2026-02-02T02:40:00.200Z \
  authorize --rack rack:r1 --actor operator --request-id turnup-1

rack-turnup --store facility.rtm --now 2026-02-02T02:40:00.300Z \
  activate --rack rack:r1 --actor operator --attempt at:1 --outcome active \
  --observed-at 2026-02-02T02:40:00.250Z --members 2

rack-turnup --store facility.rtm --now 2026-02-02T02:40:00.400Z \
  commission --rack rack:r1 --actor operator --attempt at:1
```

`evaluate` prints the verdict, one line per stage in ladder order, one line per
subsystem with the evidence record that was used, both digests (the evidence set
and the verdict), and the blockers from most severe first:

```
verdict=ready_to_authorize
identity_composition_validation=satisfied
...
turnup_authorization=pending
power=ready evidence=te:2 observed_at=2026-02-02T02:40:00.005Z code=ok
...
evidence_set=9d2c...ed69
verdict_digest=a0bf...e9d7
blocker_count=0
```

A blocked rack says why, in the same deterministic order:

```
rack-turnup --store facility.rtm --now <t> evaluate --rack rack:r1 --at <t>
verdict=blocked
power_readiness=blocked
power=blocked evidence=te:9 observed_at=<t> code=headroom_insufficient
  items: available_milliwatts=40000 applied_milliwatts=4000 required_milliwatts=10000
blockers=1
  severity=blocking
  code=headroom_insufficient
  stage=power_readiness
  subsystem=power
  explanation=power is available but the applied power is below the plan requirement
```

`--format json` emits one object per command with a stable key order, which is what
the tests use to check that text and JSON can never disagree.

## Installation and downstream consumption

```
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
cmake --install build/release --prefix /some/prefix
```

The install provides the headers, the library, the `rack-turnup` tool, the
Apache-2.0 licence and notice, and a CMake package that exports the namespaced
imported target `rack_turnup::rack_turnup`:

```cmake
find_package(rack_turnup 1.0 REQUIRED)
target_link_libraries(my_tool PRIVATE rack_turnup::rack_turnup)
```

An independent consumer that uses exactly this path lives in
`tests/packaging/consumer`; it is configured against an installed prefix, built
out of tree, and executed by the `installed_artifact` test, which also checks that
the package contains the config, version and export files and the headers.

Build options: `RACK_TURNUP_BUILD_SHARED`, `RACK_TURNUP_BUILD_CLI`,
`RACK_TURNUP_BUILD_TESTS`, `RACK_TURNUP_BUILD_BENCHMARKS`,
`RACK_TURNUP_WARNINGS_AS_ERRORS` (on by default), `RACK_TURNUP_ENABLE_ASAN`.

## Validation actually performed

Everything below was run in this repository on Windows 11 with MSVC 19.44
(Visual Studio 2022 Build Tools), CMake 4.3.2 and Ninja 1.13.2, from a shell
initialised by `vcvars64.bat`. `ctest --test-dir build/release --output-on-failure`
runs the whole thing.

| configuration | command | result |
|---|---|---|
| Release | `cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release` | 0 warnings, 0 errors with `/W4 /WX /permissive-` |
| Release tests | `build/release/rtm_tests.exe` | 65 of 65 passed |
| Debug | `cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug` | 0 warnings, 0 errors with `/W4 /WX /permissive-` |
| Debug tests | `build/debug/rtm_tests.exe` | 65 of 65 passed |
| AddressSanitizer | `cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRACK_TURNUP_ENABLE_ASAN=ON` | 0 warnings, 0 errors, `/fsanitize=address` |
| AddressSanitizer tests | `build/asan/rtm_tests.exe` | 65 of 65 passed, no sanitizer report |
| Packaging | `ctest --test-dir build/release` | `rtm_tests` and `installed_artifact` both passed |

### What the suite actually proves

**Unit and primitive tests (14).** SHA-256 against published known-answer vectors
(empty input, `abc`, the 56-byte two-block vector, one million `a`), incremental
hashing equal to one-shot hashing at every split point, digest text form and the
fail-closed zero digest; strict UTF-8 validation against overlong, surrogate,
truncated and out-of-range sequences; control-character rejection; byte-wise
ordering; canonical splitting and joining; strict integer parsing; command-line
argument splitting; RFC 3339 parsing and formatting including leap years, negative
epochs and every rejected shape; freshness boundaries; the error taxonomy; and the
identity parsing policy.

**Domain tests (7).** Composition validation and digest determinism (member order
does not change the digest, a label does), duplicate device and duplicate slot
rejection, trait set canonicalisation, inventory closure for every outcome
(closed, missing, unknown member, displaced, unreadable, duplicated slot) and the
fixed precedence of its primary reason, evidence eligibility (expiry, future
observation, generation mismatch, tampering) and the newest-wins rule being
independent of the order records are supplied in.

**Evaluation tests (20).** A complete ready rack reaching `ready_to_authorize`;
each subsystem rule with the doctrine's distinctions made concrete - power
available but not applied, unverified power is unknown, cooling capacity without
proven delivery, network attachment without reachability, the wrong fabric
topology, hardware present but failing, warning health under both policies,
inventory gaps, mandatory and advisory compatibility, missing and mixed-generation
evidence, stale evidence, a composition change fencing authority, the
authorization, activation and commission stages including an expired grant and a
fenced attempt, an activation observed before the authorization, a not-active
observation, blocker ordering being a total order, and verdict-digest determinism.

**Service tests (13).** A full cycle from registration to commissioned through the
service API; stale revisions and unknown racks; idempotent replay returning the
same receipt without advancing the revision, and a conflicting reuse being refused;
an evidence import after an authorization fencing that authorization and
returning the rack to planned; a composition change superseding the plan and
fencing its authorizations; rollback cancelling the grant; the drain, drain
completion and decommission sequence with decommission refused before draining;
a control-epoch takeover fencing everything and superseding every plan; restart
recovering exactly one generation with a new incarnation; a read-only store
refusing every mutation; the rejection journal and the provenance trail; the plan
policy generation being derived from the request and a contradictory stamp still
being refused; and a seeded state machine that performs forty pseudo-random
operations and re-checks the invariants after each one (state validity, at most
one active authorization per rack and it binds the current revision and epoch,
strictly increasing evidence sequences, and a byte-identical encode/decode round
trip).

**Store tests (12).** Commit and reload with an inspection of the published
generation; canonical encoding; malformed and tampered files rejected at every
layer (future format version, non-zero reserved field, wrong magic, flipped
payload byte, wrong declared length, truncation, trailing bytes, empty file); the
watermark refusing a restored older copy through both read paths; single-writer
and reader behaviour in one process; the commit sequence refusing a wrong epoch or
sequence; every abort point being reachable; 300 guaranteed-effective random
mutations of a valid state file with zero accepted; Unicode and long paths
(including a path beyond the classic Windows limit, which is where a real defect
was found and fixed); crash consistency at all seven commit points using real
terminated child processes, with the parent then proving that exactly one
generation is readable, that it is structurally valid, and that the lock died with
the process; a child that commits and is then killed keeping every committed
generation; and cross-process exclusion observed between two real processes.

**Command line tests (3).** A full cycle driven in process through `run_cli` with
text and JSON output; exit codes for usage errors, domain rejections and
persistence failures; and a real child process running the built tool.

**Packaging test (1).** Installs into a private prefix, configures the independent
consumer with `find_package(rack_turnup 1.0 REQUIRED)`, builds it out of tree, runs
it, and checks that it reports a verdict and a verdict digest and that it published
a watermarked generation.

### Defects found by this validation and fixed

* **The extended-length path prefix was malformed.** The `\\?\` prefix was built
  with one backslash instead of two, so paths beyond the classic Windows limit
  failed to open. The long-path test found it; the prefix is now correct for both
  drive-letter and UNC paths.
* **The stage ladder held back stages after an acceptable degraded subsystem.** A
  degraded stage stopped its successor from being satisfied even when policy
  permitted degraded subsystems, which made `ready_to_authorize` false for a rack
  the policy allowed. The ladder now holds a stage back only when the predecessor
  genuinely fails to pass.
* **A degraded subsystem did not block the verdict when policy forbade it.** The
  blocker was correctly reported as blocking, but the overall verdict stayed
  `not_ready` instead of `blocked`. A degraded stage now blocks the verdict when
  policy does not permit degraded subsystems.
* **`inspect` did not check the publication watermark.** `read_only_load` refused a
  rolled-back state file while `inspect` accepted it. Both paths now refuse it.
* **A store that may not create a missing state only said so on first read.**
  `DurableStore::open` with `create_if_missing = false` now fails immediately with
  `no_authoritative_state`.
* **A registered rack could not be replayed idempotently.** `register_rack` checked
  for a duplicate rack before the idempotency replay, so a retry of a request whose
  response was lost was refused with `duplicate_rack_id`. Replay is now resolved
  first, which is what lost-response retry requires.
* **A read-only store reported content errors before it reported that it was
  read-only.** `register_rack` now refuses a read-only store first, like every
  other mutation.
* **Commissioned racks had no current plan.** Commissioning completed the plan, so
  draining and decommissioning could never find a plan to advance and a
  commissioned rack's summary reported no plan. A rack now has a *current* plan:
  the active plan when there is one, otherwise its newest plan.
* **The plan's policy generation had to be repeated inside the stamp.** A caller
  that set the policy but left the stamp's policy field zero was refused. It is now
  derived from the request, while a contradictory non-zero value is still refused.
* **The Debug build caught a test violating a documented precondition.** A unit
  test called `Digest::of` with eight bytes, which the header documents as a
  programming error; the release build's fail-closed path hid it and Debug's
  assertion aborted the run. The test now exercises the documented contract.

## Benchmarks

`rtm_bench` measures **completed** operations only. Durable operations include the
whole commit sequence - stage, flush to the device, read back and verify, atomic
publication, watermark - because that is the cost that matters. Nothing here
measures submission or enqueue latency, and no before/after performance claim is
made.

Measured on Windows 11, MSVC 19.44, Release, with a real filesystem:

| completed operation | input | operations | seconds | operations/second | ms/operation |
|---|---|---|---|---|---|
| durable evidence import | REAL file, 1 rack | 300 | 5.378 | 55.8 | 17.925 |
| readiness evaluation | SYNTHETIC 64 member rack | 2000 | 0.133 | 15091.5 | 0.066 |
| complete turnup cycle | REAL file, 4 member rack | 20 | 4.944 | 4.0 | 247.187 |
| state encode and decode | REAL 20 rack state | 500 | 1.355 | 369.1 | 2.709 |

* **COMPOSITIONS ARE SYNTHETIC.** The racks, members and evidence used as input are
  generated by the benchmark program. They are not real data-center inventory.
* **THE MEASUREMENTS ARE REAL.** The filesystem, the operating system's writer
  lock, the flushes and the timing are real. No durable cost is simulated.
* The durable numbers are dominated by the deliberate verification work: every
  commit flushes the staged file, reads it back, verifies its digests and checksums,
  decodes it, compares its state digest, then replaces the file atomically and
  updates the watermark. Throughput was traded for the guarantee that a published
  generation is the generation that was committed.
* The evaluation number is the pure evaluator on a synthetic 64-member rack with
  complete evidence: evidence selection, seven subsystem rules, the stage ladder,
  blocker aggregation and digest computation, with no I/O.

## Repository layout

```
include/rack_turnup/   public headers (identities, time, digests, model, plan,
                       evidence, evaluation, authorization, service, store, CLI)
src/                   library implementation
app/main.cpp           the rack-turnup entry point
tests/                 the proof obligations
tests/harness.*        a dependency-free test harness, temporary directories and
                       child process support
tests/crash_child.cpp  the process that is terminated at commit points
tests/packaging/       the out-of-tree consumer and the install validation script
bench/bench.cpp        the completed-operation benchmark
docs/                  the durable format specification
cmake/                 the package config template
```

## What this repository does not claim

* No hardware was available. Every statement about power, cooling, network,
  inventory, health and compatibility is a statement about how this library
  *evaluates evidence that others publish*. Plant and hardware behaviour is
  **SYNTHETIC** here; process, filesystem, durability, locking and packaging
  behaviour is **REAL** and was tested on the host.
* Multiprocess behaviour is claimed only where two real processes were used: the
  writer lock, the crash-consistency points and the command line tool. Thread
  behaviour is not claimed, because the library is single-threaded by design.
* No distributed or federated behaviour is claimed.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
