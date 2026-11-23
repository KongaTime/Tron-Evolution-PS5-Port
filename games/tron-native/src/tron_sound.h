// The achievement cue: an original synthesized sound (a rising sweep, a
// detuned saw chord, an echoing square arpeggio), played on an output of its
// own beside the game's audio.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace tron {

// Synthesizes the cue (once) and opens the output.
void PrepareAchievementSound();
// Any thread; restarts the cue.
void PlayAchievementSound();

}  // namespace tron
