/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/game_title_db.h"

#include <deque>
#include <string_view>
#include <unordered_map>

#include "rapidjson/document.h"

#include "xenia/app/title_id_util.h"
#include "xenia/base/embedded_bundle.h"
#include "xenia/base/logging.h"

#include "embedded_bundle_game_db.h"

namespace xe {
namespace app {

namespace {

struct TitleIndex {
  // Owns the metadata; pointers into it stay valid (deque never reallocates).
  std::deque<GameTitleInfo> entries;
  std::unordered_map<uint32_t, const GameTitleInfo*> by_id;
};

std::string GetMemberString(const rapidjson::Value& entry, const char* key) {
  auto it = entry.FindMember(key);
  if (it == entry.MemberEnd() || !it->value.IsString()) {
    return std::string();
  }
  return std::string(it->value.GetString(), it->value.GetStringLength());
}

const TitleIndex& GetTitleIndex() {
  static const TitleIndex index = []() {
    TitleIndex idx;
    xe::EmbeddedBundle bundle(xe::embedded_bundle_game_db::kBundleData,
                              xe::embedded_bundle_game_db::kBundleSize);
    if (!bundle.ok()) {
      XELOGE("TitleDb: bundle decompress failed");
      return idx;
    }
    bundle.ForEach([&](std::string_view name, std::string_view data) {
      if (name != "games.json") {
        return;
      }
      rapidjson::Document doc;
      doc.Parse(data.data(), data.size());
      if (doc.HasParseError() || !doc.IsArray()) {
        XELOGE("TitleDb: {} parse failed", name);
        return;
      }
      for (const auto& entry : doc.GetArray()) {
        if (!entry.IsObject()) {
          continue;
        }
        auto id_it = entry.FindMember("id");
        if (id_it == entry.MemberEnd() || !id_it->value.IsString()) {
          continue;
        }
        uint32_t title_id = ParseHexTitleId(id_it->value.GetString(),
                                            id_it->value.GetStringLength());
        if (title_id == 0) {
          continue;
        }
        const GameTitleInfo& info = idx.entries.emplace_back(GameTitleInfo{
            GetMemberString(entry, "title"),
            GetMemberString(entry, "boxart"),
        });
        // First writer wins so the primary id is never shadowed by another
        // game's alternative id.
        idx.by_id.emplace(title_id, &info);
        auto alt_it = entry.FindMember("alternative_id");
        if (alt_it != entry.MemberEnd() && alt_it->value.IsArray()) {
          for (const auto& alt : alt_it->value.GetArray()) {
            if (!alt.IsString()) {
              continue;
            }
            uint32_t alt_id =
                ParseHexTitleId(alt.GetString(), alt.GetStringLength());
            if (alt_id != 0) {
              idx.by_id.emplace(alt_id, &info);
            }
          }
        }
      }
    });
    return idx;
  }();
  return index;
}

}  // namespace

const GameTitleInfo* GetGameTitleInfo(uint32_t title_id) {
  if (title_id == 0) {
    return nullptr;
  }
  const auto& idx = GetTitleIndex();
  auto it = idx.by_id.find(title_id);
  if (it == idx.by_id.end()) {
    return nullptr;
  }
  return it->second;
}

}  // namespace app
}  // namespace xe
