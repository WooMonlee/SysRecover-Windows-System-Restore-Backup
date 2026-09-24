#pragma once
// 极简 ZIP 写入器（**只用 store 模式、不压缩**，零依赖）—— 导出诊断包用。
//
// 为什么自己写：不想为这点功能引入 zlib（体积 + 许可），也不想依赖
// PowerShell 5+ 的 Compress-Archive（Win7 自带的是 PS 2.0）或 7-Zip（不保证存在）。
// 只实现经典 ZIP 格式的最小集合（单卷、无压缩、无 zip64），日志体量足够。
#include <cstdint>
#include <string>
#include <vector>

namespace sysrecover {

// CRC-32（IEEE 802.3 / ZIP）。公开出来便于单元测试。
uint32_t Crc32(const void* data, size_t n);

class ZipWriter {
public:
    explicit ZipWriter(const std::wstring& zipPath);
    ~ZipWriter();
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    bool ok() const { return ok_; }

    // 从磁盘加一个文件；zipName 是包内路径（用 '/' 分隔，可含中文 → 按 UTF-8 存）
    bool AddFile(const std::wstring& srcPath, const std::string& zipName);
    // 加一段内存内容（例如 diag 文本）
    bool AddData(const void* data, size_t size, const std::string& zipName);
    // 写中央目录（析构里也会兜底调用）
    bool Close();

private:
    bool AddEntry(const std::string& zipName, const void* data, size_t size);

    struct Entry {
        std::string name;
        uint32_t crc = 0;
        uint32_t size = 0;
        uint32_t offset = 0;
        uint16_t dosTime = 0;
        uint16_t dosDate = 0;
    };
    void* fp_ = nullptr;  // HANDLE（头文件里不拉 windows.h）
    bool ok_ = true;
    bool closed_ = false;
    std::vector<Entry> entries_;
};

}  // namespace sysrecover
