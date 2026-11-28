# Built-in Java library provider fixture

`ProviderContracts` uses the shipped `android.test.base.jar` and
`org.apache.http.legacy.jar` in two independent parent-first ART loaders.
It checks boot delegation, sibling visibility, the defining classes behind
sample members, and their behavior. It runs in a standalone ART process. An
installed app's package-manager assignments and shared-library dependency graph
need separate evidence.

Build the fixture with a JDK and Android 36 SDK/build tools.

```bash
runtime_out=/absolute/path/to/provider-test
sdk_jar="$ANDROID_HOME/platforms/android-36/android.jar"
mkdir -p "$runtime_out/classes" "$runtime_out/dex"
javac --release 8 -classpath "$sdk_jar" -d "$runtime_out/classes" \
  tests/java-api/ProviderContracts.java
"$ANDROID_HOME/build-tools/36.0.0/d8" --lib "$sdk_jar" --min-api 36 \
  --output "$runtime_out/dex" "$runtime_out/classes/gesher/ProviderContracts.class"
cp tests/java-api/run-provider-contracts.ps1 "$runtime_out/"
```

On Windows, run the script alongside the generated `dex` directory. Supply a
Gesher runtime distribution with both library archives. The script uses a
temporary Android root and records the SHA-256 of each DEX input in the log.

```powershell
pwsh -NoProfile -File .\run-provider-contracts.ps1 -RuntimeDir C:\gesher\dist
```
