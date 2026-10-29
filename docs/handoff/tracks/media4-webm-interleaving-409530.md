# Media4 WebM interleaving: bounded CI evidence

Full CI 40953048849 attempt 1 tested source
`42076a8f28c0d23eee5b10ee0acf0844be0dc277` with immutable guard
`0e6787570e85c4a05d58668c050c2555fdc92a5a`: 45 jobs succeeded,
six optional jobs skipped. The isolated external authority was
`NET_MEDIA4_WEBM_CALLER_COMMIT`; the prior manager authority was unchanged.
The source and preflight negative controls and current-run producer identities
were independently audited in `C:/work/gesher-ci-review/media-409530-independent-audit.md`.

Both Windows normal suites (jobs 123743725027 and 123743725061) explicitly
report `0 test(s) failed`. Each exercises eight ABI/caller contexts. Per host,
64 WebM outputs across VP8/Vorbis and VP9/Opus, time-interleaved and
track-blocked arrival, and full and shortened audio were independently read:
128 track sample traces and 64 exact input payload/PTS verdicts, with a single
common origin and one millisecond tolerance. Short cases retained 60 video
samples and 46 Vorbis or 50 Opus packets, including the longer pending tail.
The independent reader/decoder checks fourteen containers per context,
including the original ten, exact video colors, and audio duration/tone.
For every WebM output, seek returned a noninitial sync frame and decoded it;
64 cue traces per host identify the video track and actual sync-block time
in each addressed cluster. This is a finite-size SimpleBlock fixture subset;
seek success alone does not establish that the reader used cues.

Package job 123743725098 has 23 actual installer gates. Both e2e jobs
123745284176 and 123745284190 have ten apps and zero unexpected failures.
Both Windows jobs have an Actions error reporting that the 1029 KiB step
summary exceeded its 1024 KiB upload limit; the full raw logs contain the
actual verdicts and remain available. The compact `gh run view --log` output
omitted their tails; preserved raw logs are `C:/work/gesher-ci-review/media-409530-123743725027.log`
and `C:/work/gesher-ci-review/media-409530-123743725061.log`.

This verifies only the described software muxing slice. It does not close all
Media4, advanced container contracts, hardware, or release requirements.
Earlier runs 40950740771 and 40951056831 failed preflight before builds;
40952429484 failed on a GitHub run-identity API disconnect before source
verification. They are not runtime proofs. No local native compilation or
fixtures were run.
