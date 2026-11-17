# Independent bounded follow-up: PR #87

This record was created locally, not posted as a GitHub review.

Reviewed PR head: c0b2a744dfa1be80eabd22cfb29ea99d20656b23.
Prepared maintainer repair: 503eaf20885481c4f860d9bde611c75a82823448.

The full original production JavaBidirectionalStream.java was reconstructed from
connector-returned content and verified byte-for-byte against Git blob
18c9d3632c36e83880d39546b2a52d2c0768ad77 before compilation. The full file from
the existing maintainer repair was likewise verified against Git blob
c20cd0c774aef3a40812f972145d1e000f82002d.

A separate host harness ran the production class with explicit substitutes for
framework classes, the engine/executor boundary and Http2Transport. Four of 18
checks fail on the original: absent response information is instead a fabricated
zero-status response on disabled HTTP/2, connect failure, cancellation before IO,
and a write callback before response headers. All 18 pass on the prepared repair.
Real response objects remain present after received headers and are retained in
subsequent failure/cancellation/success callbacks. Registration is retired once.

The local experimental two-line repair was superseded by the existing maintainer
commit. Do not apply a second competing patch. That prepared commit also fixes
transport-control/header issues outside this harness's scope.

Limits: no actual HTTP/2 network, TLS, Android device, ART, Windows, full engine
build, race benchmark or sanitizer run in this harness. A host pass here is not
approval of the entire PR and not current-head CI success. No GitHub mutation
was performed by this QA pass.

Reference: Android's external/cronet CronetBidirectionalStream stores response
information when headers arrive and passes its nullable response to failure and
cancellation callbacks. Source and reference URLs are in README.md.
