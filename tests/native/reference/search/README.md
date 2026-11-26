# Android search-tree reference

These three unmodified files are selected by Bionic libc/Android.bp at the exact Android 16 r4 commit in manifest.json. They are test-only reference suppliers. The OpenBSD tsearch/tfind sources are public domain; Android tdestroy.cpp is Apache-2.0. Original notices are retained.

The reference is an unbalanced tree; Gesher uses AVL balancing. Compare the public lookup, deletion, parent-return, callback, key-ownership and destruction contracts. Tree shape and comparator call counts may differ. Compiling these sources for Linux does not constitute execution on Android, ART or Windows.
