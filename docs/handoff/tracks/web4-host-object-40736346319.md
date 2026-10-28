# Same-run host-object and CDP diagnostic 40736346319

Run 40736346319 attempt 1, source `287819077086a6414030c2eff7d9a1130703ab40`, failed both jobs after successful build. Source peer reviewed the exact diagnostic before dispatch. Raw job logs are retained under `C:/work/gesher-ci-review/`.

* Windows 2025 job 122985822118, runtime 155.0.4283.62: the selected controlled sandbox label was associated with default CDP context 1 in session `BB73A8C2C2EC5FD93E4FE62097A76D21`, frame `11019C22D857F24355340356BCE90E7F`, origin `://`. Native frame 5 reported `OK|opaque|ok:5`, and the recorded native queue contains `object=5 attempt=first:opaque retired=False`. The original opaque-denial assertion failed. The CDP-to-native association is controlled fixture-label correlation, not a proven secure COM-to-CDP identity mapping or explanation of engine behavior.
* Windows 2022 job 122985821599, runtime 131.0.2903.86: the selected controlled sandbox context reported `://`; frame 5 reported access denied and no object 5 call appears in the native queue. Frame7 nested-creation subscription raised E_NOINTERFACE; nested registration remained absent and the initial phase timed out. This failure remains required, not a skipped assertion.

Neither runner reached held-navigation, stale-proxy retirement or top-level cross-origin controls. No lifecycle or production security contract is established; the host-object API alone remains unqualified as an opaque-origin guard. Do not retry unchanged source or infer the engine's cause from the correlation.

Plain job log SHA-256: 2022 `3ddd4d031159c335fcfe496837b95b961bacbf7fc325903b02af4ca94ab0f212`; 2025 `88bce4f2cbe3f4edd7ad3b2bcc321ad5da1249547ef2055bb8de5c7177fec008`.
