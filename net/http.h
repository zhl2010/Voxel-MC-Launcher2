// HTTP 客户端：WinHTTP 封装 + BMCLAPI 镜像路由。
//
// 设计取舍：
//  1. 不引 libcurl：WinHTTP 是系统组件，TLS/代理/重定向都是现成的，省一个二进制依赖。
//  2. 镜像策略复刻旧 Electron 版 network.js：piston-meta 的 JSON 文档优先官方
//     （BMCLAPI 的清单端点会多跳一次且常更慢），大文件/资源/库优先镜像，失败再回落官方。
//  3. 每个候选主机最多试 2 次，最终失败把最后一条 HTTP 错误抛给上层，UI 能显示具体原因。
#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace voxel::net {

// 与 network.js 的 mirrorUrl() 一一对应
std::vector<std::wstring> Candidates(const std::wstring& url, const nlohmann::json& settings);

// 取 JSON / 小文本：先试候选列表，成功返回 2xx 的 body
bool GetText(const std::wstring& url, const nlohmann::json& settings, std::string& out,
             std::wstring* error);

// 下载到文件：写 .part 再原子改名；onChunk 报告新增字节数；cancel 可中断
bool GetFile(const std::wstring& url, const std::wstring& dest, const nlohmann::json& settings,
             std::wstring* error, const std::function<void(uint64_t)>& onChunk,
             const std::atomic<bool>* cancel = nullptr);

// 单次请求（不带镜像/重试），给需要自己控制候选顺序的场景用
bool GetFileOnce(const std::wstring& url, const std::wstring& dest, const std::string& proxy,
                 std::wstring* error, const std::function<void(uint64_t)>& onChunk,
                 const std::atomic<bool>* cancel);

std::wstring MirrorUrl(const std::wstring& url, const nlohmann::json& settings);

}  // namespace voxel::net
