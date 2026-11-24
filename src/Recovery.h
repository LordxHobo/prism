// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <string>

// Crash recovery. Prism notes that it's running (running-<pid>.txt in its
// data folder) and removes that note when it closes normally, so a note
// left behind by a process that's gone means the last run crashed or was
// killed. The tabs it had open (session.txt, saved a second after every
// change) are then kept aside in session-recovery.txt, and Prism offers to
// reopen them, unless it reopens last time's tabs anyway.
//
// If Prism itself crashes, a crash report (a minidump) goes to the crashes
// folder in Prism's data folder; the newest five are kept.
namespace recovery {

// Early at startup, before any window opens (and so before the session is
// saved again). Returns true when the last run didn't close normally.
bool start();

// When Prism closes normally (all windows closed).
void finish();

// The tabs kept aside after a crash: how many, and the session file to
// reopen them from. 0 when there's nothing to offer.
int recoverableTabs();
std::wstring recoveryFile();

// The offer was answered (or its window closed): forget the kept tabs.
void discard();

// Writes a crash report when Prism crashes. Call once, early.
void installCrashHandler();

} // namespace recovery
