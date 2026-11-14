// 在线账户皮肤/头像获取。
//
// 两条数据源，按账户类型选：
//   1. 账户令牌直连（最准）：Microsoft → /minecraft/profile/skins（带 Bearer）；
//      Yggdrasil  → <server>/sessionserver/session/minecraft/profile/<uuid>（签名纹理）
//   2. 公开渲染服务兜底：Crafatar 按 UUID 出头像，无需令牌，覆盖面最广。
//
// 界面上只用到 8×8 的头部，所以返回的是裁好的头像 data URL，不是整张皮肤。
#pragma once

#include <nlohmann/json.hpp>
#include <string>

#include "core/auth.h"

namespace voxel::skin {

// 取得该账户的头像（data URL）。取不到返回空串。
// 离线账户直接返回空——由调用方回退到内置 Steve / Alex。
std::string OnlineFaceDataUrl(const nlohmann::json& settings, const Account& account);

// 取得整张皮肤 PNG（data URL），用于将来做换肤预览；取不到返回空串。
std::string OnlineSkinDataUrl(const nlohmann::json& settings, const Account& account);

}  // namespace voxel::skin
