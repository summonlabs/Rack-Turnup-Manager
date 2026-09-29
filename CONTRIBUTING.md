# Contributing

Contributions to Rack Turnup Manager are welcome. This document describes what a
change is expected to look like and how it is verified.

## License of contributions

This project is licensed under the Apache License, Version 2.0. By submitting a
contribution you agree that it is your own work and that it is licensed to the
project and to recipients under the terms of that licence, as described in
section 5 of the licence. There is **no contributor licence agreement** to sign,
and no copyright assignment is required: you keep the copyright of what you
write. A `Signed-off-by` line is welcome but not required.

Every new file must carry the same short header the existing files carry:

```
// <one line saying what the file is>.
//
// Copyright <year> <copyright holder>.
// Licensed under the Apache License, Version 2.0.
```

## Building and testing

```
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

Warnings are errors in this repository. The build uses `/W4 /WX /permissive-` on
MSVC and `-Wall -Wextra -Wpedantic -Werror` elsewhere, and a change that adds a
first-party warning is not ready. Debug builds are expected to pass as well,
because some preconditions are checked with assertions that release builds
compile out.

## What a change is expected to include

* **Tests, as proof obligations.** If behaviour changes, a test must show it. If a
  defect is fixed, a test that fails before the fix and passes after it must be
  part of the change.
* **Explicit errors.** New failure modes get a code in the existing taxonomy -
  never a reused code, never a silent default, never "unknown" quietly treated as
  healthy, ready or permitted.
* **Determinism.** The same inputs must produce the same output, including the
  order of reported findings. Nothing may depend on map iteration order, thread
  scheduling, locale or the host clock unless the caller explicitly asked for the
  time.
* **Documentation.** User-visible behaviour belongs in `README.md`; a change to the
  durable format belongs in `docs/DURABLE_FORMAT.md` as well.

Please keep the style of the surrounding code: two-space indentation, one clear
statement per line, comments that explain *why* something is the way it is, and
no commented-out code or unfinished placeholders.

## Commit messages

NaN
imperative subject line, and a body only when the reason for the change is not
obvious from the subject. Describe the change, not the process that produced it.

Do not add AI attribution, `Co-authored-by` trailers, or any other trailer that
misrepresents who wrote the change.

## Reporting a problem

A useful report contains the exact command, the exact output, the version and
build configuration reported by `rack-turnup --version`, and - for a durable
state problem - the state file's `store inspect` output. Please do not attach
state files that contain facility identifiers you are not allowed to share.
