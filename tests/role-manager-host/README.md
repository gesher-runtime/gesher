# RoleManager admission and callback contracts

This host JVM fixture compiles the actual production RoleManager and the shared
DEX service contract tests. It uses the built app classes, including the real
generated compatibility-change table, and the prepared Android framework classes.
Only Binder identity, Process identity and the client CompatChanges delegate are
substituted explicitly; permission contexts expose separate self/caller decisions.
These boundary classes are confined to this directory and are never packaged in
the application or DEX service fixture.

Run after `app/build.sh`:

```sh
python3 tools/test_role_manager_contracts.py \
  --framework-classes out/framework/classes \
  --app-classes out/app/gesher-app-classes.jar
```

The checks cover all shared admission/unsupported contracts, old/new target user
selection, deferred callbacks while another Binder identity is active, exception
restoration, and local permissions that differ from an incoming caller's grants.
Both privilege borrowing and false denials must fail. This is an executable
production-source contract test, not Windows ART or real Binder IPC execution;
the existing DEX/Windows service tests remain required.
