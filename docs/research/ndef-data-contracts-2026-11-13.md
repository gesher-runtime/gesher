# NDEF data contracts on a desktop without NFC hardware

PR #10 originally supplied `NdefMessage` and `NdefRecord` classes whose constructors
discarded their input, getters returned empty fields, and `toByteArray()` returned
an empty array. These data types are transport-independent Android APIs: apps can
parse and serialize NDEF files without an NFC adapter. Returning a null hardware
adapter therefore cannot substitute for their data behavior.

## Source provenance

The following files in `app/java/android/nfc/` are copied **byte for byte**, including
their Apache 2.0 headers, from the real AOSP NFC module implementation at the
project's Android tag `android-16.0.0_r4`. They are not generated SDK compile stubs.

Source directory:
<https://android.googlesource.com/platform/packages/modules/Nfc/+/refs/tags/android-16.0.0_r4/framework/java/android/nfc/>

| File | Source bytes | SHA-256 |
| --- | ---: | --- |
| `NdefMessage.java` | 9,880 | `51c27643a468992bd56bd9c407134339987af562cb4f3c9983e6e222001fc930` |
| `NdefRecord.java` | 43,267 | `1b7c4f24c4f659bfea993234fb7c04570b8a69576d1b7afd7bf5efdfa68d3bbf` |
| `FormatException.java` | 916 | `a53a110de073269d35ada87ed7df3ca61137086dd994ccc5b1f6a6c486f59cd5` |
| `NfcEvent.java` | 2,396 | `6d0ea001c8cd4c7e884a809b75b0e10cabe8af2b1009ce377b219f434f65f0c5` |

The NFC module's `NdefMessageProto` and `NdefRecordProto` references are constants
already supplied by the framework compile input and inlined by javac. The data
classes use existing runtime `Uri`, `Intent`, `Parcelable` and proto APIs; they do
not connect to an NFC service. `NfcAdapter.getDefaultAdapter(context)` still returns
null on Gesher. `Tag` remains available as a callback/parcel type, but cannot be
constructed as a fabricated empty tag: unsupported tag construction and parcel
operations throw explicitly. No NFC tag/controller emulation is claimed.
The pinned `NfcEvent` supplies the public LLCP major/minor fields and decodes
their nibbles; the original placeholder's unrelated combined field is removed.

## Validation

`tools/test_ndef_data_contracts.py` compiles every fixture call site against the
public SDK 36 and the production classes against the framework/core compile inputs.
The fixture checks field retention, copy-returning getters, a separately specified
URI wire vector, MIME/text/external/application helpers, UTF-8, multi-record and
chunked messages, input ownership, short/full payload boundaries, 255-byte type/ID
fields, equality/hash behavior, and malformed input rejection. The AOSP parser's
payload bound is preserved. A negative-control run replaces only the two old data
classes and must fail their input-retention assertion; it retains `FormatException`
so missing exception linkage cannot obscure that original data-loss behavior.

The same `gesher.services.NdefDataContractTest` runs on Windows with no arguments.
That mode loads the real `android_runtime` and additionally verifies native Parcel
round trips for a record and a multi-record message, including trailing sentinels
that detect incomplete parcel consumption. The Linux/JDK run explicitly skips
Parcel transport and reports `parcel=false`; it does not claim NFC hardware,
native Binder/Parcel, or Windows execution.

The local SDK-compiled host run passed **178 checks**. The same fixture with the
two data classes from PR head `8d900ba11b625fdbf1a0b3d89ce7008c22950b5a`
failed at `Record constructor retains TNF`, confirming that the old placeholders
are rejected. `git diff --check` passed and all four imported source hashes were
verified against the downloaded pinned sources. The Windows Parcel branch still
requires execution by the Windows CI gate.
