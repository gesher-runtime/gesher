# BlobStore quota admission

The Android reference is `android-16.0.0_r4`, frameworks/base commit
`45034f0663f960d9ee5fb0a101a4732b71f6e2f4`:
[BlobStoreManagerService.java](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/apex/blobstore/service/java/com/android/server/blob/BlobStoreManagerService.java).

`acquireLeaseInternal` checks the caller's leased-blob count and then the requested
blob size against remaining quota before `addOrReplaceLeasee`. A renewal is still
subject to these admission checks. In `onStateChangedInternal`, the
`STATE_VERIFIED_VALID` branch checks the committed-blob count before finding an
existing blob and replacing its committer contribution. Recommit at the count
limit fails asynchronously and consumes that session.

Admission and accounting have different roles. An accepted renewal or recommit
replaces the existing owner entry, so it charges the blob bytes or committed slot
only once. That replacement does not exempt the operation from Android's limits.
Rejection preserves the earlier lease or contribution and published bytes.

`BlobStoreQuotaContractTest` calls the actual service and asynchronous commit
worker. It covers renewal at the count and remaining-byte limits, successful
renewal below the limits without duplicate charging, and successful/rejected
recommit with session consumption and preservation of earlier contributions.
The fixture seeds bounded metadata to reach 500 leases and 1,000 contributions,
uses a real temporary data file, and writes actual index updates. Its seeded sizes
are an explicit storage-metadata boundary; it does not allocate that many bytes.

The existing Windows/ART `BlobStoreContractTest` includes these controls. The
separate `tools/test_blobstore_quotas.py` runner compiles the production Java
classes and executes them with the existing declared Linux JNI/identity boundary.
`--source-ref` permits the unchanged fixture to run against an earlier service.
These host checks do not establish descriptor revocation, remote Binder, Windows
filesystem behavior or complete product execution; those remain separate gates.
