# SQLite host fixture: declare the unattached drop backend

## Result and scope

This dependent repair restores the existing portable SQLite path fixture on
PR #427's exact author head `b1962c3be9f484da7b1272e2c4f909c5df68a8e6`.
The maintained ASan/UBSan entry passes all **259 existing SQLite checks** and
the new **13 namespace-boundary checks**. Its original unpatched-callback
negative still fails all four required predicates. Six compiled boundary
mutants and two malformed source-declaration controls are rejected.

Only `tools/test_sqlite_paths.py` and the new
`tests/native/sqlite_path_namespace_test.cc` change execution. The production
mapper, drop channel, rooted-storage broker, SQLite bridge/patch, original
SQLite test, workflows and Windows targets remain byte-identical to the author
head. The author branch is not modified.

This is a fixture repair, not approval of the AppContainer PR. The host fixture
has **no attached authenticated Windows drop channel or rooted-storage broker**.
It does not establish their authorization, revocation, handle lifetime, Windows
VFS, AppContainer or ART behavior. Those remain actual Windows runtime gates.

## Observed failure

[Artifact preflight job 138082634530](https://github.com/gesher-runtime/gesher/actions/runs/45285898943/job/138082634530)
ran the existing `tools/test_sqlite_paths.py --fetch --sanitize --negative-control`
entry. It failed to compile the generated `sqlite_path_namespace.h`: the exact
extracted `MapPath` now refers to `drop_mount::kAndroidPrefix` and
`drop_mount::Enabled`, while the portable namespace adapter declares neither.

The job checked out generated merge
`d6364d131e5586e3c3b0cf309cbed7621c1fb885`, with ordered parents
`730d5cb5de2699de6159d209e0686bc1d87d8c65` and the author head above. All eleven
fixture, mapper, declaration and SQLite patch inputs used for this diagnosis
are byte-identical between that failed merge and the author head. The complete
original job log is retained in the evidence archive.

The original local runner also fails at the same undeclared names after
successfully compiling the exact pinned SQLite engine. This is a demonstrated
fixture compile break, not a downloaded-artifact, quota or runtime assertion
failure.

## Boundary supplied by the repair

The generator copies the single `kAndroidPrefix` declaration verbatim from the
actual `gesher/drop_mount.h`. Missing or duplicate declarations fail extraction;
there is no separate hard-coded production prefix in the adapter.

The new `Enabled()` adapter returns false explicitly. The actual production
registry starts with `enabled{false}`; the actual `Enabled()` reads that value,
and `Channel::Attach` publishes true only after its successful worker reply.
No such attachment occurs in this portable fixture. The adapter therefore
describes an unavailable backend; it does not synthesize an admitted channel
or claim that a guest may open a selected host file.

All seven actual mapper/helper function bodies are extracted unchanged. Removing
only the new adapter block reconstructs the complete original generated namespace
byte-for-byte. Existing UTF conversion, `GetFullPathNameW`, font, procfs/device and
rooted-storage seams retain their previous boundaries. Actual database IO,
transactions, URI metadata, ATTACH, VACUUM, WAL, journals, open errors, output
capacity and cleanup still execute the pinned SQLite engine's real Unix VFS.

Before compiling the SQLite caller, the maintained entry now compiles and runs
the new namespace fixture against that exact generated header. It checks the
real prefix, unavailable-backend state, normal and reserved-character spellings,
a prefix lookalike, long-path handling, an ordinary drive path, virtual-device
and malformed-UNC rejection, null/empty inputs, and absence of rooted-storage
admission. These are lexical and supplier-boundary assertions. They do not
simulate or grant a Windows broker permission.

## Validation retained

| Evidence | Actual outcome |
| --- | --- |
| Original maintained runner on the author source | Fails compilation at the same undeclared `drop_mount` references |
| Repaired namespace fixture, ASan/UBSan | 13 checks, zero failures |
| Original complete SQLite corpus through the repaired runner, ASan/UBSan | 259 checks, zero failures |
| Original unpatched callback control | 132 checks, 35 expected failures; guest open, ATTACH, VACUUM and output-truncation predicates all required |
| Existing source-download unit entry | All nine tests pass; pin and finite transport policy unchanged |
| Six separately compiled mapper/adapter mutants | Each fails its designated real namespace assertion |
| Missing and duplicate prefix declarations | Both fail the actual extraction boundary |
| Source and actual-entry proof | 94 identity and contract-evidence checks |
| Independent Media source/namespace replay | 20 identity/source checks; regenerated exact header and all 13 ASan/UBSan checks pass |

The six compiled controls enable the unattached backend, corrupt its prefix,
bypass the backend predicate, omit reserved-character encoding, omit long-path
handling, or admit a virtual device through `DiskPath`. Every mutant compiles;
the failure is its named assertion. The missing/duplicate declaration controls
instead fail extraction as intended.

The original callback's complete stdout and stderr were also retained by an
untimed replay of the exact already-built negative executable. Its digest and
all four required messages are recorded separately. No assertion or threshold
was weakened to make the current entry pass.

The baseline helper was imported before the working helper was edited. Python
then displayed some later working-file snippets in its traceback's line cache.
The archived old helper bytes, exact compiler command, complete generated
translation units and compiler's undeclared-name diagnostic bind the baseline
independently of those displayed snippets. The archived reproducer now points
traceback code locations at its retained helper copy.

## Identities and evidence

The fixture source commit is
`3477578163468ec1a1fd525a4518608009615e66`, tree
`cc8e01423905a40243a6eb67ac1634b24179a086`, sole parent the exact author head.

| Input | SHA-256 |
| --- | --- |
| Current helper | `dfdd91bba9c5dd57ee7ff3a71b07b4226fa97c666f7dddc0cffb8f4444cfd7af` |
| Namespace caller | `339a35d329cace9c59a4f9de62897bf42b0153ef1ab2e7950341d8e550554685` |
| Current complete generated namespace | `ee8ab2b5315fc21f1a6ae8971ed65f92e500e79161881e36cbfc0e776675285b` |
| Original complete generated namespace | `3f672014ebb6d59c2e197ecdb762e91225bfe68fd97953bf9af167b64d06b2ce` |
| Unchanged generated SQLite bridge | `d8af4a6572209183b7def5a2cbf564cf9da6a9e9958e8d2e498c397d4ccca7a9` |

The adjacent JSON contains all parent/failed-merge/current input hashes and the
archive's exact byte size, digest and member count. The archive includes its
own per-member manifest, original complete CI failure, pinned pristine SQLite
sources and their actual patched source, both generated namespaces/bridges,
the maintained helper versions, complete control sources/logs and executable
digests. Compiled binaries and SQLite object files are not duplicated in the
archive; complete generated inputs, commands and their recorded hashes remain.

## Reproduction

On Linux, run the existing entry from this dependent checkout:

```sh
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tools/test_sqlite_paths.py --fetch --sanitize --negative-control
python3 -m unittest discover -s tools/tests -p test_sqlite_source_download.py
```

For an offline supplier replay, extract the archive and replace `--fetch` with
`--sqlite-source /absolute/path/to/extracted/supplier`. The maintained helper
still verifies both original pinned SHA-256 values before patching or compiling.

The archive's `scripts/native_pr427_sqlite_controls.py` accepts `--repo`,
`--output` and `--sanitize`; it regenerates the actual namespace and compiles
the current control and six mutants, then checks the two extraction negatives.
The retained-entry wrapper accepts the same checkout plus `--sqlite-source`,
`--output`, `--sanitize`, and optional `--ref` for the original helper. It only
retains temporary inputs and records existing commands; it does not replace
production mapping, SQLite behavior, compiler flags or acceptance predicates.

Windows/ART, authenticated drop-channel and AppContainer acceptance remain
pending full CI. A queued or successful host-only job does not establish them.
