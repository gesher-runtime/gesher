/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class ComponentCallerHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        int checks = com.gesher.server.ComponentCallerFixture.run(false);
        System.out.println("ComponentCaller host all passed: " + checks + " checks");
        System.out.println("Real public caller/controller/provider policy; constructed manifest/process state and controlled native identity. No ART, Binder transport or actual Activity callback claim.");
    }
}
