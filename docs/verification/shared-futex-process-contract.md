<!-- SPDX-License-Identifier: Apache-2.0 -->
# Cross-process futex contract

This fixture measures [N29 and T02](https://github.com/gesher-runtime/gesher/issues/56) using the public Linux futex syscall. Two independently launched processes map the same file. A release/acquire handshake establishes that the mapping really is shared. The waker then uses an address different from the waiter's address, tests a wrong bitset, byte offset and private key, and requires the matching wake to report exactly one waiter. The waiter must return from the syscall before its monotonic deadline.

The [Android common kernel key lookup](https://android.googlesource.com/kernel/common/+/refs/heads/android16-6.12/kernel/futex/core.c) keys a file-backed shared futex by backing file, page index and byte offset. The [wake implementation](https://android.googlesource.com/kernel/common/+/refs/heads/android16-6.12/kernel/futex/waitwake.c) also checks the bitset intersection.

## Build and run

Build one PIE per supported guest ABI with the Android NDK. Use `aarch64-linux-android35-clang` or `x86_64-linux-android35-clang` from the NDK compiler directory.

```sh
aarch64-linux-android35-clang -std=c11 -O2 -Wall -Wextra -Werror \
    tests/native/shared_futex_process_contract.c -o shared-futex
adb push shared-futex /data/local/tmp/shared-futex
adb shell 'chmod 755 /data/local/tmp/shared-futex && cd /data/local/tmp &&
    dir=$(mktemp -d ./shared-futex.XXXXXX) &&
    trap '\''rm -rf "$dir"'\'' EXIT &&
    ./shared-futex setup "$dir/word" || exit 2
    ./shared-futex waiter "$dir/word" & waiter=$!
    ./shared-futex waker "$dir/word"; waker_status=$?
    wait "$waiter"; waiter_status=$?
    [ "$waker_status" -eq 0 ] && [ "$waiter_status" -eq 0 ]'
```

On Windows, place the exact same ELF alongside or point it at a build of `gesher-exec.exe`. Use a new file on the runtime drive. Pass its absolute guest path (for example `/Temp/shared-futex.unique/word`, with its parent directory already created). Run the following in two terminals, using the same path and matching ABI in both. The first terminal blocks for up to seven seconds in `waiter`; start the second after `setup PASS` or while waiting for `waiter READY`.

```powershell
# First terminal
& $launcher --elf $elf shared-futex setup $guestPath
if ($LASTEXITCODE -ne 0) { throw 'setup failed' }
& $launcher --elf $elf shared-futex waiter $guestPath
# Second terminal
& $launcher --elf $elf shared-futex waker $guestPath
```

Keep both process exit codes and transcripts, the ELF digest, the Android fingerprint and ABI, and the Windows runtime build and revision. A successful row requires both participants to report `PASS` and exit zero. Exit 1 is a failed contract; exit 2 is a setup or rendezvous error. Delete the fresh Windows file after both processes have exited. The test does not exercise priority inheritance, robust-list owner death, or the pthread process-shared API.
