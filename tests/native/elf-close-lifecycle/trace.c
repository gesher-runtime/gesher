/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0 */

// The combined observer, mixed-owner and NODELETE probe records 18 events.
static int events[32];
static int count;

void gesher_n06_event_record(int event) {
  if (count < (int)(sizeof(events) / sizeof(events[0]))) {
    events[count++] = event;
  }
}

int gesher_n06_event_count(void) { return count; }
int gesher_n06_event_at(int index) { return events[index]; }
