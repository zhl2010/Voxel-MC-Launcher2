// 哈希与编码：SHA1（文件校验）、MD5（离线 UUID）、Base64（皮肤 data URL）。
//
// 全部走 Windows CNG（bcrypt.dll），不引第三方库：启动器只需要这两个算法，
// 为它们塞一个 OpenSSL 进去不值当。
#pragma once

#include <string>

namespace voxel::crypto {

std::string Sha1Hex(const std::string& data);
std::string Sha1FileHex(const std::wstring& path);
std::string Md5Hex(const std::string& data);

// 离线账户 UUID：等价于 Java 的 UUID.nameUUIDFromBytes(("OfflinePlayer:" + name).getBytes())
// 即 MD5 后置 version=3 / variant=10 的 RFC-4122 字符串。
std::string OfflineUuid(const std::string& playerName);

std::string Base64Encode(const std::string& bytes);
std::string Base64Decode(const std::string& text);

// DPAPI（CryptProtectData，当前用户作用域）：账号令牌落盘前的加解密。
// 密文只能由同一 Windows 账户解开，别的用户或拷走的文件都拿不到明文。
bool ProtectData(const std::string& plain, std::string& out);
bool UnprotectData(const std::string& blob, std::string& out);

}  // namespace voxel::crypto
