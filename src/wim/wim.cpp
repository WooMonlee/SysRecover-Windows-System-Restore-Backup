// WIM 引擎实现：libwim C API 直连。
#include "wim.h"

#include <windows.h>  // WideCharToMultiByte（XML 是 UTF-16LE，见 ImageSize）

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <thread>

#include "wimlib.h"

namespace sysrecover {
namespace {

// wimlib 不允许重名子镜像（否则返回 rc=11 "name already in use"）。
// 追加备份时默认名都是 "Backup"，必须自动加序号，否则 --append 永远失败。
std::wstring UniqueImageName(WIMStruct* w, const std::wstring& desired) {
    std::wstring base = desired.empty() ? L"Backup" : desired;
    struct wimlib_wim_info info = {};
    if (wimlib_get_wim_info(w, &info) != 0)
        return base;
    std::wstring name = base;
    for (int n = 2; n <= 9999; ++n) {
        bool dup = false;
        for (uint32_t i = 1; i <= info.image_count; ++i) {
            const wchar_t* en =
                wimlib_get_image_name(w, static_cast<int>(i));
            if (en && name == en) {
                dup = true;
                break;
            }
        }
        if (!dup)
            break;
        wchar_t buf[48];
        swprintf(buf, 48, L" (%d)", n);
        name = base + buf;
    }
    return name;
}

unsigned ThreadCount() {
    unsigned n = std::thread::hardware_concurrency();
    return n ? n : 4;
}

enum wimlib_compression_type CompressType(const std::string& name,
                                            int& writeFlags) {
    writeFlags = WIMLIB_WRITE_FLAG_CHECK_INTEGRITY;
    if (name == "maximum")
        return WIMLIB_COMPRESSION_TYPE_LZX;
    if (name == "recovery") {
        writeFlags |= WIMLIB_WRITE_FLAG_SOLID;
        return WIMLIB_COMPRESSION_TYPE_LZMS;
    }
    return WIMLIB_COMPRESSION_TYPE_XPRESS;  // fast（默认）
}

struct ProgCtx {
    ProgressFn* fn = nullptr;
};

int Pct(uint64_t done, uint64_t total) {
    if (total == 0)
        return -1;
    int p = static_cast<int>(done * 100 / total);
    return p < 0 ? 0 : (p > 100 ? 100 : p);
}

// 注意：回调可能在工作线程执行，只做轻量计算，不碰 UI/文件（见 AGENTS.md §8）。
enum wimlib_progress_status ProgressThunk(enum wimlib_progress_msg msg,
                                          union wimlib_progress_info* info,
                                          void* ctx) {
    auto* c = static_cast<ProgCtx*>(ctx);
    if (!c || !c->fn || !*c->fn)
        return WIMLIB_PROGRESS_STATUS_CONTINUE;
    int pct = -1;
    std::string stage;
    if (msg == WIMLIB_PROGRESS_MSG_WRITE_STREAMS) {
        pct = Pct(info->write_streams.completed_bytes,
                  info->write_streams.total_bytes);
        stage = "write";
    } else if (msg == WIMLIB_PROGRESS_MSG_EXTRACT_STREAMS) {
        pct = Pct(info->extract.completed_bytes, info->extract.total_bytes);
        stage = "extract";
    } else if (msg == WIMLIB_PROGRESS_MSG_SCAN_BEGIN) {
        pct = 0;
        stage = "scan";
    } else if (msg == WIMLIB_PROGRESS_MSG_SCAN_DENTRY) {
        // 扫描阶段没有"总数"可算百分比，只做节流反馈（每 3000 个文件报一次），
        // 否则大 C 盘扫描时界面会长时间停在 0% 像卡死。
        uint64_t n = info->scan.num_nondirs_scanned;
        if (n == 0 || n % 3000 != 0)
            return WIMLIB_PROGRESS_STATUS_CONTINUE;
        char buf[128];
        snprintf(buf, sizeof(buf), "scan %llu files / %llu MB",
                 (unsigned long long)n,
                 (unsigned long long)(info->scan.num_bytes_scanned >> 20));
        stage = buf;
        pct = 0;
    } else if (msg == WIMLIB_PROGRESS_MSG_SCAN_END) {
        pct = 0;
        stage = "scan done";
    } else {
        return WIMLIB_PROGRESS_STATUS_CONTINUE;
    }
    if (pct < 0)
        return WIMLIB_PROGRESS_STATUS_CONTINUE;
    return (*c->fn)(pct, stage) ? WIMLIB_PROGRESS_STATUS_ABORT
                                : WIMLIB_PROGRESS_STATUS_CONTINUE;
}

struct WimHandle {
    WIMStruct* w = nullptr;
    ~WimHandle() {
        if (w)
            wimlib_free(w);
    }
};

}  // namespace

WimEngine::WimEngine() {
    inited_ = (wimlib_global_init(0) == 0);
}

WimEngine::~WimEngine() {
    if (inited_)
        wimlib_global_cleanup();
}

const wchar_t* WimEngine::ErrorString(int code) {
    return wimlib_get_error_string(
        static_cast<enum wimlib_error_code>(code));
}

int WimEngine::Capture(const std::wstring& source,
                       const std::wstring& imagePath,
                       const std::string& compress, const std::wstring& name,
                       bool snapshot, const std::wstring& configFile,
                       ProgressFn progress) {
    if (!inited_)
        return -1;
    int writeFlags = 0;
    enum wimlib_compression_type ctype = CompressType(compress, writeFlags);
    WimHandle h;
    WIMStruct* raw = nullptr;
    int rc = wimlib_create_new_wim(ctype, &raw);
    if (rc != 0)
        return rc;
    h.w = raw;
    ProgCtx ctx{&progress};
    wimlib_register_progress_function(h.w, ProgressThunk, &ctx);
    // 注意：WIMLIB_ADD_FLAG_NTFS 是"裸卷捕获模式"（源须为未挂载 NTFS 卷），
    // 目录捕获（含 C:/ 热备，配合 SNAPSHOT 做 VSS）必须 flags=0（见 PIT-008）。
    int flags = 0;
    if (snapshot)
        flags |= WIMLIB_ADD_FLAG_SNAPSHOT;
    rc = wimlib_add_image(h.w, source.c_str(), name.c_str(),
                          configFile.empty() ? nullptr : configFile.c_str(),
                          flags);
    if (rc != 0)
        return rc;
    // 原子写：先写 <dest>.tmp，成功后改名。避免备份中途退出/崩溃留下一个
    // "看起来像镜像、其实半截"的文件（WIM 头带 WRITE_IN_PROGRESS），被误拿去
    // 还原 → Linux 侧 apply rc=84 → 黑屏（PIT-057）。
    std::wstring tmp = imagePath + L".tmp";
    DeleteFileW(tmp.c_str());
    rc = wimlib_write(h.w, tmp.c_str(), WIMLIB_ALL_IMAGES, writeFlags,
                      ThreadCount());
    if (rc != 0) {
        DeleteFileW(tmp.c_str());
        return rc;
    }
    if (!MoveFileExW(tmp.c_str(), imagePath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return WIMLIB_ERR_RENAME;
    }
    return 0;
}

int WimEngine::Append(const std::wstring& source,
                      const std::wstring& imagePath,
                      const std::string& compress, const std::wstring& name,
                      bool snapshot, const std::wstring& configFile,
                      ProgressFn progress) {
    if (!inited_)
        return -1;
    WimHandle h;
    WIMStruct* raw = nullptr;
    int rc = wimlib_open_wim(imagePath.c_str(), 0, &raw);
    if (rc != 0)
        return rc;
    h.w = raw;
    ProgCtx ctx{&progress};
    wimlib_register_progress_function(h.w, ProgressThunk, &ctx);
    int flags = 0;  // 同 Capture：目录源不用 NTFS 标志（PIT-008）
    if (snapshot)
        flags |= WIMLIB_ADD_FLAG_SNAPSHOT;
    (void)compress;  // 追加沿用原文件压缩方式
    std::wstring uniqueName = UniqueImageName(h.w, name);
    rc = wimlib_add_image(h.w, source.c_str(), uniqueName.c_str(),
                          configFile.empty() ? nullptr : configFile.c_str(),
                          flags);
    if (rc != 0)
        return rc;
    return wimlib_overwrite(h.w, WIMLIB_WRITE_FLAG_CHECK_INTEGRITY,
                            ThreadCount());
}

int WimEngine::Apply(const std::wstring& imagePath, int index,
                     const std::wstring& target, ProgressFn progress) {
    if (!inited_)
        return -1;
    WimHandle h;
    WIMStruct* raw = nullptr;
    int rc = wimlib_open_wim(imagePath.c_str(), 0, &raw);
    if (rc != 0)
        return rc;
    h.w = raw;
    ProgCtx ctx{&progress};
    wimlib_register_progress_function(h.w, ProgressThunk, &ctx);
    // 注意：WIMLIB_EXTRACT_FLAG_NTFS 是"直写裸卷模式"（需 libntfs-3g），
    // 目录应用必须 flags=0（PIT-008）。
    return wimlib_extract_image(h.w, index, target.c_str(), 0);
}

int WimEngine::Verify(const std::wstring& imagePath) {
    if (!inited_)
        return -1;
    WimHandle h;
    WIMStruct* raw = nullptr;
    int rc = wimlib_open_wim(imagePath.c_str(),
                             WIMLIB_OPEN_FLAG_CHECK_INTEGRITY, &raw);
    if (rc != 0)
        return rc;
    h.w = raw;
    return wimlib_verify_wim(h.w, 0);
}

// 还原暂存前的可用性检查：拒绝"写入未完成"（WIM_HDR_FLAG_WRITE_IN_PROGRESS，
// 上一次备份中途退出/崩溃会留下它）或空镜像，避免把坏镜像暂存进还原任务、
// 重启后 Linux 侧 apply rc=84（WIM_IS_INCOMPLETE）→ 目标分区被格式化却没装上
// 系统 → 开机黑屏（PIT-057）。
int WimEngine::Probe(const std::wstring& imagePath, std::wstring& why) {
    why.clear();
    if (!inited_) {
        why = L"wimlib 初始化失败";
        return -1;
    }
    WimHandle h;
    WIMStruct* raw = nullptr;
    int rc = wimlib_open_wim(imagePath.c_str(),
                             WIMLIB_OPEN_FLAG_CHECK_INTEGRITY, &raw);
    if (rc != 0) {
        why = ErrorString(rc);
        return rc;
    }
    h.w = raw;
    struct wimlib_wim_info info = {};
    rc = wimlib_get_wim_info(h.w, &info);
    if (rc != 0) {
        why = L"读取镜像信息失败";
        return rc;
    }
    if (info.write_in_progress) {
        why = L"上次写入未完成，镜像不完整（很可能是上次备份中途退出/中断）";
        return -1;
    }
    if (info.image_count == 0) {
        why = L"镜像里没有任何子镜像";
        return -1;
    }
    return 0;
}

int WimEngine::ListImages(const std::wstring& imagePath,
                          std::vector<ImageDesc>& out) {
    out.clear();
    if (!inited_)
        return -1;
    WimHandle h;
    WIMStruct* raw = nullptr;
    int rc = wimlib_open_wim(imagePath.c_str(), 0, &raw);
    if (rc != 0)
        return rc;
    h.w = raw;
    struct wimlib_wim_info info = {};
    rc = wimlib_get_wim_info(h.w, &info);
    if (rc != 0)
        return rc;
    for (uint32_t i = 1; i <= info.image_count; ++i) {
        ImageDesc d;
        d.index = static_cast<int>(i);
        const wchar_t* name =
            wimlib_get_image_name(h.w, static_cast<int>(i));
        if (name)
            d.name = name;
        out.push_back(std::move(d));
    }
    return 0;
}

int WimEngine::ImageSize(const std::wstring& imagePath, int index,
                         unsigned long long* bytes) {
    if (!inited_ || !bytes)
        return -1;
    *bytes = 0;
    WimHandle h;
    WIMStruct* raw = nullptr;
    int rc = wimlib_open_wim(imagePath.c_str(), 0, &raw);
    if (rc != 0)
        return rc;
    h.w = raw;
    // 说明：打包的 wimlib.h（1.14）没有 wimlib_get_image_info()，
    // 但每个子镜像的**未压缩**总大小就在 WIM 元数据 XML 的 <TOTALBYTES> 里，
    // 用 wimlib_get_xml_data() 取出来自己解析（顶层也有一个 TOTALBYTES，是整包大小，
    // 所以必须从 <IMAGE INDEX="N"> 之后开始找）。
    void* buf = nullptr;
    size_t n = 0;
    rc = wimlib_get_xml_data(h.w, &buf, &n);
    if (rc != 0 || !buf || n == 0)
        return rc != 0 ? rc : -1;
    // 重要：wimlib 返回的 XML 是 **UTF-16LE**（WIM 元数据的原生编码），不是 UTF-8。
    // 实测（ctypes 直调 libwim-15.dll）：每个字符后跟 0x00。若按窄串直接找标签会
    // **静默失败**（返回 -1 → 预检被跳过，形同虚设），所以这里先统一转成窄串。
    std::string xml;
    if (n >= 2 && static_cast<const char*>(buf)[1] == 0) {
        std::wstring w(static_cast<const wchar_t*>(buf), n / 2);
        int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                       static_cast<int>(w.size()), nullptr, 0,
                                       nullptr, nullptr);
        if (need > 0) {
            xml.resize(static_cast<size_t>(need));
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                static_cast<int>(w.size()), &xml[0], need,
                                nullptr, nullptr);
        }
    } else {
        xml.assign(static_cast<const char*>(buf), n);
    }
    free(buf);  // XML 缓冲区由 wimlib 用 malloc 分配 → 必须用 free()
    // （注意：wimlib_free() 是释放 WIMStruct 的，不是通用释放器）
    if (xml.empty())
        return -1;
    // 定位子镜像段落：<IMAGE INDEX="N" ...> … </IMAGE>
    char tag[64];
    snprintf(tag, sizeof(tag), "<IMAGE INDEX=\"%d\"", index);
    size_t p = xml.find(tag);
    if (p == std::string::npos)
        return -1;
    size_t end = xml.find("</IMAGE>", p);
    std::string seg =
        xml.substr(p, end == std::string::npos ? std::string::npos : end - p);
    auto field = [&seg](const char* open) -> unsigned long long {
        size_t t = seg.find(open);
        if (t == std::string::npos)
            return 0;
        t += strlen(open);
        size_t e = seg.find("</", t);
        if (e == std::string::npos)
            return 0;
        return strtoull(seg.substr(t, e - t).c_str(), nullptr, 10);
    };
    // 真正会占用的空间 = TOTALBYTES − HARDLINKBYTES：
    //   TOTALBYTES 把硬链接**按独立文件**计数，HARDLINKBYTES 是其中"重复"的那部分；
    //   目标分区上硬链接只占一份 → 相减才是真正要写的量。
    //   实测校准（本机测试镜像）：10.16GB − 5.09GB = 5.07GB，与救援层 apply 日志的
    //   "Extracting file data: 5188 MiB"(=5.07GB) **完全一致**。若直接用 TOTALBYTES
    //   会高估近一倍 → 误拒本来装得下的还原。
    unsigned long long total = field("<TOTALBYTES>");
    unsigned long long hard = field("<HARDLINKBYTES>");
    if (total == 0)
        return -1;
    *bytes = (hard && hard < total) ? total - hard : total;
    return 0;
}

}  // namespace sysrecover
