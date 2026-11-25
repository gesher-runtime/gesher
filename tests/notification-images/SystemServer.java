/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;
/** The standalone file-owner fixture has no Android log transport. */
final class SystemServer { static void log(String message) { System.err.println(message); } }
