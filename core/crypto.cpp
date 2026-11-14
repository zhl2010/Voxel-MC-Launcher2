#include "crypto.h"

#include <windows.h>
#include <bcrypt.h>
#include <fstream>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace voxel::crypto {
namespace {

std::string ToHex(const unsigned char* bytes, size_t size) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(digits[bytes[i] >> 4]);
        out.push_back(digits[bytes[i] & 0x0F]);
    }
    return out;
}

// 通用 CNG 摘要：algo 用 BCRYPT_SHA1_ALGORITHM / BCRYPT_MD5_ALGORITHM。
bool Digest(const wchar_t* algo, const unsigned char* data, size_t size,
            std::vector<unsigned char>& out) {
    BCRYPT_ALG_HANDLE provider = nullptr;
    if (BCryptOpenAlgorithmProvider(&provider, algo, nullptr, 0) != 0) return false;

    DWORD hashSize = 0;
    DWORD written = 0;
    bool ok = BCryptGetProperty(provider, BCRYPT_HASH_LENGTH,
                                reinterpret_cast<PUCHAR>(&hashSize),
                                sizeof(hashSize), &written, 0) == 0;

    BCRYPT_HASH_HANDLE hash = nullptr;
    if (ok) {
        ok = BCryptCreateHash(provider, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    }
    if (ok) {
        ok = BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) == 0;
    }
    if (ok) {
        out.resize(hashSize);
        ok = BCryptFinishHash(hash, out.data(), hashSize, 0) == 0;
    }
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(provider, 0);
    return ok;
}

}  // namespace

std::string Sha1Hex(const std::string& data) {
    std::vector<unsigned char> digest;
    if (!Digest(BCRYPT_SHA1_ALGORITHM,
                reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest)) {
        return std::string();
    }
    return ToHex(digest.data(), digest.size());
}

// 分块流式哈希：客户端 jar 有 26MB，assets 里单个文件不大但总量上千，
// 一次性读进内存既慢又占堆。
std::string Sha1FileHex(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::string();

    BCRYPT_ALG_HANDLE provider = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD hashSize = 20;
    DWORD written = 0;
    std::vector<unsigned char> digest;

    if (BCryptOpenAlgorithmProvider(&provider, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0) return std::string();
    BCryptGetProperty(provider, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize),
                      sizeof(hashSize), &written, 0);
    if (BCryptCreateHash(provider, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<char> buffer(64 * 1024);
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize got = in.gcount();
            if (got <= 0) break;
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                           static_cast<ULONG>(got), 0);
        }
        digest.resize(hashSize);
        BCryptFinishHash(hash, digest.data(), hashSize, 0);
    }
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(provider, 0);
    if (digest.empty()) return std::string();
    return ToHex(digest.data(), digest.size());
}

std::string Md5Hex(const std::string& data) {
    std::vector<unsigned char> digest;
    if (!Digest(BCRYPT_MD5_ALGORITHM,
                reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest)) {
        return std::string();
    }
    return ToHex(digest.data(), digest.size());
}

std::string OfflineUuid(const std::string& playerName) {
    std::vector<unsigned char> digest;
    const std::string seeded = "OfflinePlayer:" + playerName;
    if (!Digest(BCRYPT_MD5_ALGORITHM,
                reinterpret_cast<const unsigned char*>(seeded.data()), seeded.size(), digest) ||
        digest.size() < 16) {
        return std::string();
    }
    // version 3
    digest[6] = static_cast<unsigned char>((digest[6] & 0x0F) | 0x30);
    // variant 10xx (IETF)
    digest[8] = static_cast<unsigned char>((digest[8] & 0x3F) | 0x80);

    const std::string hex = ToHex(digest.data(), 16);
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" +
           hex.substr(16, 4) + "-" + hex.substr(20, 12);
}

std::string Base64Encode(const std::string& bytes) {
    static const char* table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    size_t index = 0;
    while (index + 2 < bytes.size()) {
        const unsigned int chunk = (static_cast<unsigned char>(bytes[index]) << 16) |
                                   (static_cast<unsigned char>(bytes[index + 1]) << 8) |
                                   static_cast<unsigned char>(bytes[index + 2]);
        out.push_back(table[(chunk >> 18) & 0x3F]);
        out.push_back(table[(chunk >> 12) & 0x3F]);
        out.push_back(table[(chunk >> 6) & 0x3F]);
        out.push_back(table[chunk & 0x3F]);
        index += 3;
    }
    const size_t rest = bytes.size() - index;
    if (rest == 1) {
        const unsigned int chunk = static_cast<unsigned char>(bytes[index]) << 16;
        out.push_back(table[(chunk >> 18) & 0x3F]);
        out.push_back(table[(chunk >> 12) & 0x3F]);
        out.append("==");
    } else if (rest == 2) {
        const unsigned int chunk = (static_cast<unsigned char>(bytes[index]) << 16) |
                                   (static_cast<unsigned char>(bytes[index + 1]) << 8);
        out.push_back(table[(chunk >> 18) & 0x3F]);
        out.push_back(table[(chunk >> 12) & 0x3F]);
        out.push_back(table[(chunk >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

std::string Base64Decode(const std::string& text) {
    static const std::string table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string clean;
    clean.reserve(text.size());
    for (char ch : text) {
        if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') continue;
        clean.push_back(ch);
    }

    std::string out;
    out.reserve(clean.size() / 4 * 3);
    unsigned int buffer = 0;
    int bits = 0;
    for (char ch : clean) {
        if (ch == '=') break;
        const size_t index = table.find(ch);
        if (index == std::string::npos) return std::string();
        buffer = (buffer << 6) | static_cast<unsigned int>(index);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

}  // namespace voxel::crypto
