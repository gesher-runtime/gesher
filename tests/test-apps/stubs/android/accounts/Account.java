/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.accounts;

public class Account {
    public final String name;
    public final String type;

    public Account(String name, String type) {
        this.name = name;
        this.type = type;
    }

    @Override
    public boolean equals(Object o) {
        if (this == o) return true;
        if (!(o instanceof Account)) return false;
        Account account = (Account) o;
        return (name != null ? name.equals(account.name) : account.name == null)
                && (type != null ? type.equals(account.type) : account.type == null);
    }

    @Override
    public int hashCode() {
        return (name != null ? name.hashCode() : 0) * 31 + (type != null ? type.hashCode() : 0);
    }
}
