// SPDX-License-Identifier: GPL-2.0-or-later
// minustwo: the game's render thread waits for the GPU's end-of-pipe label before it reuses a
// command buffer. The game spins on the label without a pause; src/runtime_label_wait.c makes it
// sleep in mtgpu_wait_label (mtgpu.h) instead, and the command processor's writes to guest memory
// (fences, WriteData) wake it here.
#pragma once

#include <cstddef>

namespace MtLabelWait {
/// The command processor wrote `bytes` of guest memory at `address`: wakes a waiter whose label
/// is among them.
void Written(const void* address, std::size_t bytes);
} // namespace MtLabelWait
