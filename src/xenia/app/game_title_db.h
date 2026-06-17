/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_GAME_TITLE_DB_H_
#define XENIA_APP_GAME_TITLE_DB_H_

#include <cstdint>
#include <string>

namespace xe {
namespace app {

// Title metadata from the embedded x360db games.json.
struct GameTitleInfo {
  std::string name;    // Display title, e.g. "007 Legends".
  std::string boxart;  // Cover art URL; may be empty. Not downloaded here.
};

// Looks up title metadata by title id (also resolves alternative ids).
// Returns nullptr when the id is unknown. The pointer is stable for the
// lifetime of the process.
const GameTitleInfo* GetGameTitleInfo(uint32_t title_id);

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_GAME_TITLE_DB_H_
