/* SPDX-License-Identifier: Apache-2.0 */
package org.gesher.contracts.storage.plugin;

public final class Plugin {
    private static int calls;

    public static int next() {
        return ++calls;
    }
}
