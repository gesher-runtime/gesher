# Rotation capability fixtures

These small APKs contain only the existing signing fixture manifest and public
certificates. `generate.py` uses two throwaway RSA keys and discards their
keystores; no private keys are committed.

- `old.apk`: old current signer, v2.
- `rotated-no-data.apk`: verified old-to-new lineage retaining AUTH but denying
  INSTALLED_DATA. A fresh installation is valid; retaining the old install's data
  during an update must be refused.
- `rotated-no-auth.apk`: the same old/new keys, retaining INSTALLED_DATA but
  denying AUTH. The update may retain app data; account ownership must use the
  new signer, without inheriting the old authenticator's authority.

Regenerate with `python generate.py BUILD_TOOLS FRAMEWORK_RES`. Certificate
fingerprints are intentionally read from the verified artifacts by the test;
regeneration does not require preserving any test key.
