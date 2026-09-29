# Durable state format

Rack Turnup Manager stores its whole service state in one file. The format is
versioned, bounded, integrity-checked and self-describing, and it is written so
that a reader either sees one complete generation or refuses the file.

## File layout

```
offset  size  field
0       8     magic, exactly "RTMSTAT1"
8       4     format version, big-endian (currently 1)
12      4     reserved flags; any non-zero value is rejected
16      8     payload length in bytes, big-endian
24      32    SHA-256 of the payload
56      4     CRC-32 of bytes 0..55
60      4     encoding model (currently 1)
64      N     payload
64+N    4     CRC-32 of the payload
68+N    4     CRC-32 of the previous four bytes
```

The file must be exactly `64 + N + 8` bytes. A shorter file is `truncated_state`,
a longer one is `trailing_bytes`. A wrong magic is `corrupt_state`, an unknown
format version or encoding model is `unsupported_format_version`, a non-zero
reserved field is `reserved_bits_set`, and any checksum or digest mismatch is
`integrity_check_failed`.

## Payload layout

All integers are big-endian and fixed width. Booleans are one byte and must be
0 or 1. Text is a `u32` byte count followed by the bytes; the count is checked
against the documented bound for that field *and* against the remaining buffer
before anything is allocated. Collections are a `u32` count followed by that
many elements, checked against the matching bound first.

```
u32   payload model (1), u32 reserved (0)
u64   store epoch, store sequence, incarnation, control epoch, observation sequence
i64   created at, updated at
text  last actor
u32   rack count, then that many rack records
u32   rejection count, then that many rejection records
u64   rejection evictions
```

Each rack record holds its identity, label, site, generation, composition, the
lifecycle position, and then, in this order: plans, evidence, authorizations,
fences, activation records, commission records, idempotency receipts (plus the
eviction count) and provenance records.

### Composition check value

A composition is written together with the digest it claims. On decode the
composition is rebuilt through `RackComposition::create`, which re-validates
every member and recomputes the digest; a mismatch is
`integrity_check_failed`. Trait sets are rebuilt the same way through
`TraitSet::create`.

### Self-describing records

Evidence records, plans, authorizations, activation records, commission records
and idempotency receipts each carry a digest over their own immutable fields.
Decoding recomputes every one of them and refuses a record that does not match,
so a field edited in place is detected even if the surrounding container was
rewritten consistently.

### Identity validation

Non-empty identity text is always re-parsed by its own parser (`RackId::parse`,
`DeviceId::parse`, `Trait::parse`, `Digest::parse` and so on) and the parser's
error is returned unchanged. Empty text decodes to the default-constructed
value of its type, which is what makes legitimately empty optional fields - the
fields a compatibility requirement's kind does not use, the seven unselected
payload slots of a tagged evidence record, an evidence identifier on a rack
registration receipt - round-trip exactly.

Enumerations are validated on decode: an unknown value is `invalid_enum_value`
or `corrupt_state`, never a default. The decoder ends by running
`validate_state`, which checks the cross-record invariants: rack records sorted
and unique, plans ordered by strictly increasing lifetime with at most one
active plan, evidence ordered by strictly increasing observation sequence,
every fence naming an authorization that is fenced, every commission record
naming an activation observation that exists, and a lifecycle position that
claims a durable fact only when the record that proves it is present.

## Commit sequence

`DurableStore::commit` publishes one generation and advances its in-memory
fencing only at the end:

1. check that the caller holds the current fencing (epoch and next sequence)
2. validate the state and encode the payload; refuse anything above the size bound
3. re-read the generation currently on disk and refuse to publish if another
   writer moved it, or if the file is older than the recorded watermark
4. write the staged file (`<state>.staged`) and flush it to the device
5. read the staged file back, verify every checksum and digest, decode it, and
   compare its state digest with the state that was committed
6. replace the state file atomically (a single rename)
7. write the publication watermark atomically
8. adopt the new sequence and epoch

A failure at any step before step 6 leaves the previous generation
authoritative: the staged file is removed and the error is returned. Steps 4 to
7 are the points where `tests/crash_child.cpp` terminates a real process, and
the test suite then reopens the store and proves that exactly one generation is
readable and that the state is structurally valid.

## Publication watermark

`<state>.watermark` records the sequence, epoch and payload digest of the newest
published generation:

```
rtm-watermark-v1
sequence=12
epoch=3
digest=<64 lowercase hexadecimal characters>
```

If the state file's sequence is lower than the watermark's, the file was
replaced by an older copy and every read path refuses it with
`stale_durable_state`; if the sequences are equal the digests must match, or the
read fails with `integrity_check_failed`. A watermark one generation behind is
normal (a process can die between steps 6 and 7) and is accepted.

## Writer lock

`<state>.lock` is a separate file used only for cross-process exclusion. A
writer opens it with read sharing but no write sharing and takes an exclusive
byte-range lock in fail-fast mode, so a second writer is refused immediately
with `writer_lock_held` instead of waiting. Readers never open the lock file.
When a writer dies the operating system releases both the sharing mode and the
lock, which the test suite proves by killing a real child process and taking the
lock from the parent immediately afterwards.

## Recovery

Opening a writable store loads the authoritative generation and reconciles
authority before answering any question: a plan whose binding no longer matches
the rack record is superseded, an authorization whose plan, revision, epoch or
composition no longer holds is fenced, and a rack marked authorized with no
active authorization returns to planned. Every reconciliation is recorded in a
fence or provenance record and reported, so recovery never silently inherits
authority.
