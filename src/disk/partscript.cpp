// 新硬盘初始化的 diskpart 脚本生成（纯逻辑）。见 partscript.h。
#include "partscript.h"

#include <cstdio>

namespace sysrecover {
namespace {

std::string Header(uint32_t diskIndex) {
    char buf[64] = {};
    std::snprintf(buf, sizeof(buf), "select disk %u\n", diskIndex);
    return buf;
}

// 一步 = select disk + 本步命令 + exit。
PartScriptStep MakeStep(const char* stage, uint32_t diskIndex,
                        const std::string& body) {
    PartScriptStep s;
    s.stage = stage;
    s.script = Header(diskIndex) + body + "exit\n";
    return s;
}

}  // namespace

std::vector<PartScriptStep> BuildDiskpartSteps(
    bool gpt, uint32_t diskIndex, unsigned long long espMb,
    unsigned long long msrMb, unsigned long long systemGb, bool createData,
    wchar_t espLetter, wchar_t sysLetter, wchar_t dataLetter) {
    char buf[256] = {};
    std::vector<PartScriptStep> steps;

    // 1) 清旧分区表。单独一步：这一步最慢也最容易失败（盘被占用/只读/离线），
    //    失败时状态栏停在"正在清除旧分区表"，用户一眼知道卡在哪。
    steps.push_back(MakeStep("partinit-clean", diskIndex, "clean\n"));

    // 2) 转换分区表类型。clean 之后盘是 RAW，两种方案都能直接 convert。
    steps.push_back(MakeStep("partinit-convert", diskIndex,
                             gpt ? "convert gpt\n" : "convert mbr\n"));

    if (gpt) {
        // 3) ESP：FAT32 + 卷标 ESP（ESP 找不到时 FindEspDriveByLabel 靠卷标兜底）。
        buf[0] = 0;
        std::snprintf(buf, sizeof(buf),
                      "create partition efi size=%llu\n"
                      "format quick fs=fat32 label=ESP\n",
                      espMb);
        std::string esp = buf;
        if (espLetter)
            esp += std::string("assign letter=") + static_cast<char>(espLetter) +
                   "\n";
        steps.push_back(MakeStep("partinit-esp", diskIndex, esp));

        // 4) MSR：不格式化、不分配盘符。
        std::snprintf(buf, sizeof(buf), "create partition msr size=%llu\n",
                      msrMb);
        steps.push_back(MakeStep("partinit-msr", diskIndex, buf));
    }

    // 5) 系统盘。MBR 必须 active（否则活动分区缺失 → 还原完黑屏）。
    std::snprintf(buf, sizeof(buf),
                  "create partition primary size=%llu\n"
                  "format quick fs=ntfs label=Windows\n",
                  systemGb * 1024ull);
    std::string sys = buf;
    if (!gpt)
        sys += "active\n";
    if (sysLetter)
        sys += std::string("assign letter=") + static_cast<char>(sysLetter) +
               "\n";
    steps.push_back(MakeStep("partinit-system", diskIndex, sys));

    // 6) 数据盘：吃掉剩余空间（createData=false 时不建，见 partinit.h 的小盘规则）。
    if (createData) {
        std::string data =
            "create partition primary\n"
            "format quick fs=ntfs label=Data\n";
        if (dataLetter)
            data += std::string("assign letter=") +
                    static_cast<char>(dataLetter) + "\n";
        steps.push_back(MakeStep("partinit-data", diskIndex, data));
    }
    return steps;
}

std::string JoinDiskpartSteps(const std::vector<PartScriptStep>& steps,
                              uint32_t diskIndex) {
    // 只用于日志：去掉后续步骤重复的 "select disk N" 与 "exit"。
    std::string all = Header(diskIndex);
    const std::string head = Header(diskIndex);
    for (const auto& s : steps) {
        std::string body = s.script;
        if (body.compare(0, head.size(), head) == 0)
            body.erase(0, head.size());
        if (body.size() >= 5 && body.compare(body.size() - 5, 5, "exit\n") == 0)
            body.erase(body.size() - 5);
        all += body;
    }
    all += "exit\n";
    return all;
}

}  // namespace sysrecover
