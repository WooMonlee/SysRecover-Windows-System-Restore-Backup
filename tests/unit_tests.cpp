// 纯逻辑单元测试（零依赖）。覆盖 PLAN §12 第 3 项点名的几块：
//   * sysinfo：注册表原始值 → 可读描述（Win11 家族纠正、版别中文映射、SP、UBR）
//   * exclude：默认排除清单（**含 PIT-056 回归守卫**）+ 云同步目录追加
//   * task   ：restore-task.conf / .json / _zjresy 日志的**契约文本**
//   * zip    ：CRC-32 标准测试向量
// 这些都是把纯逻辑从 Windows 调用里抽出来后才可测的（见各头文件注释）。
#include <cstring>

#include "tiny_test.h"

#include "boot/task.h"
#include "common/sysinfo.h"
#include "common/version.h"
#include "common/zip.h"
#include "wim/exclude.h"

using namespace sysrecover;

// ────────────────────────── sysinfo ──────────────────────────

TEST(sysinfo_win10_iot_ltsc) {
    RawSysInfo r;
    r.productName = L"Windows 10 IoT Enterprise LTSC 2021";  // 英文原值
    r.editionId = L"IoTEnterpriseS";
    r.displayVersion = L"21H2";
    r.currentBuildNumber = L"19044";
    r.hasUbr = true;
    r.ubr = 4046;
    auto d = ComposeSystemDescription(r);
    CHECK_EQ(d.full, std::wstring(L"Windows 10 IoT 企业版 LTSC 21H2 19044.4046"));
    CHECK_EQ(d.shortTag, std::wstring(L"Win10.19044"));
}

TEST(sysinfo_win11_family_from_build) {
    RawSysInfo r;
    r.productName = L"Windows 10 Pro";  // Win11 的 ProductName 仍写 10（历史遗留）
    r.editionId = L"Professional";
    r.currentBuildNumber = L"22631";
    r.hasUbr = true;
    r.ubr = 3155;
    auto d = ComposeSystemDescription(r);
    CHECK_EQ(d.full, std::wstring(L"Windows 11 专业版 22631.3155"));
    CHECK_EQ(d.shortTag, std::wstring(L"Win11.22631"));
}

TEST(sysinfo_win7_sp1) {
    RawSysInfo r;
    r.productName = L"Windows 7 Ultimate";
    r.editionId = L"Ultimate";
    r.csdVersion = L"Service Pack 1";  // Win7 时代
    r.currentBuildNumber = L"7601";
    auto d = ComposeSystemDescription(r);
    CHECK_EQ(d.full, std::wstring(L"Windows 7 旗舰版 SP1 7601"));
    CHECK_EQ(d.shortTag, std::wstring(L"Win7.7601"));
}

TEST(sysinfo_unknown_edition_falls_back) {
    RawSysInfo r;
    r.productName = L"Windows 10 Weird Edition";
    r.editionId = L"WeirdEdition";  // 映射表里没有
    r.currentBuildNumber = L"19045";
    auto d = ComposeSystemDescription(r);
    CHECK(d.full.rfind(L"Windows 10 Weird Edition", 0) == 0);
    CHECK_EQ(d.shortTag, std::wstring(L"Win10.19045"));
}

TEST(sysinfo_ubr_zero_not_appended) {
    RawSysInfo r;
    r.productName = L"Windows 10 Pro";
    r.editionId = L"Professional";
    r.currentBuildNumber = L"19045";
    r.hasUbr = true;
    r.ubr = 0;  // UBR=0 不该拼出 ".0"
    CHECK_EQ(ComposeSystemDescription(r).full,
             std::wstring(L"Windows 10 专业版 19045"));
}

TEST(sysinfo_missing_build_no_shorttag) {
    RawSysInfo r;
    r.productName = L"Windows 10 Pro";
    r.editionId = L"Professional";
    CHECK(ComposeSystemDescription(r).shortTag.empty());
}

TEST(sysinfo_product_falls_back_to_editionid) {
    RawSysInfo r;
    r.editionId = L"Core";  // productName 空 → 用 EditionID 兜底
    r.currentBuildNumber = L"19045";
    auto d = ComposeSystemDescription(r);
    CHECK_CONTAINS(d.full, L"Core");
    CHECK_CONTAINS(d.full, L"19045");
}

TEST(sysinfo_releaseid_used_when_no_displayversion) {
    RawSysInfo r;
    r.productName = L"Windows 10 Enterprise";
    r.editionId = L"Enterprise";
    r.releaseId = L"1909";  // 旧版只有 ReleaseId
    r.currentBuildNumber = L"18363";
    CHECK_CONTAINS(ComposeSystemDescription(r).full, L"1909");
}

// ────────────────────────── exclude ──────────────────────────

TEST(exclude_default_has_essentials) {
    std::string c = DefaultExclusionConfig();
    CHECK_CONTAINS(c, "[ExclusionList]");
    CHECK_CONTAINS(c, "\\pagefile.sys");
    CHECK_CONTAINS(c, "\\hiberfil.sys");
    CHECK_CONTAINS(c, "\\swapfile.sys");
    CHECK_CONTAINS(c, "\\System Volume Information");
}

// ⭐ PIT-056 回归守卫：注册表事务日志**绝不能**被排除 ——
// 热备时 hive 常是"脏"的，开机要靠 .LOG1/.LOG2 恢复一致；排除 = 还原后黑屏。
TEST(exclude_must_not_exclude_registry_logs) {
    std::string c = DefaultExclusionConfig();
    CHECK_NOT_CONTAINS(c, ".LOG1");
    CHECK_NOT_CONTAINS(c, ".LOG2");
    CHECK_NOT_CONTAINS(c, "regtrans-ms");
    CHECK_NOT_CONTAINS(c, "NTUSER.DAT");
    CHECK_NOT_CONTAINS(c, "TM.blf");
}

TEST(exclude_cloud_appended_once) {
    std::vector<std::wstring> folders = {L"OneDrive", L"OneDrive", L"Dropbox"};
    std::string c = BuildExclusionContent(folders);
    CHECK_CONTAINS(c, "\\OneDrive\\*");
    CHECK_CONTAINS(c, "\\Dropbox\\*");
    size_t first = c.find("\\OneDrive\\*");
    CHECK(c.find("\\OneDrive\\*", first + 1) == std::string::npos);  // 只一次
}

TEST(exclude_empty_equals_default) {
    CHECK_EQ(BuildExclusionContent({}), std::string(DefaultExclusionConfig()));
}

TEST(exclude_cloud_names_nonempty) {
    CHECK(!CloudFolderNames().empty());
}

// ────────────────────────── task 契约文本 ──────────────────────────

namespace {
RestoreTask SampleTask() {
    RestoreTask t;
    t.imagePath = L"D:\\images\\win10.esd";
    t.imageRelPath = L"images/win10.esd";
    t.imageIndex = 2;
    t.targetOffset = 105906176ull;
    t.targetSize = 716800000ull;
    t.targetDiskSerial = L"QEMU-DRILL-0001";
    t.targetDisk = 0;
    t.targetPart = 2;
    t.ptType = "gpt";
    t.repairBoot = false;
    t.partCount = 3;
    t.targetDiskName = L"INTEL SSDSCKGF240A5H";
    t.targetDiskSize = 240057409536ull;
    t.targetFs = L"NTFS";
    t.targetVolLabel = L"Windows";
    t.softwarePath = L"D:\\ZJRESTORE\\SysRecover.exe";
    t.softwareDir = L"D:\\ZJRESTORE";
    return t;
}
}  // namespace

TEST(task_conf_has_required_keys) {
    std::string c = BuildTaskConf(SampleTask());
    CHECK_CONTAINS(c, "action=restore");
    CHECK_CONTAINS(c, "pt_type=gpt");
    CHECK_CONTAINS(c, "image_path=D:\\images\\win10.esd");
    CHECK_CONTAINS(c, "image_rel_path=images/win10.esd");
    CHECK_CONTAINS(c, "image_index=2");
    CHECK_CONTAINS(c, "target_offset=105906176");
    CHECK_CONTAINS(c, "target_size=716800000");
    CHECK_CONTAINS(c, "target_disk_serial=QEMU-DRILL-0001");
    CHECK_CONTAINS(c, "repair_boot=0");
    CHECK_CONTAINS(c, "partition_count=3");
    CHECK_CONTAINS(c, "software_dir=D:\\ZJRESTORE");
}

TEST(task_json_escapes_backslashes) {
    std::string j = BuildTaskJson(SampleTask());
    CHECK_CONTAINS(j, "\"schema\":1");
    CHECK_CONTAINS(j, "\"action\":\"restore\"");
    CHECK_CONTAINS(j, "\"repair_boot\":false");
    CHECK_CONTAINS(j, "D:\\\\images\\\\win10.esd");  // JSON 里反斜杠要转义
    CHECK_CONTAINS(j, "\"target_disk\":0");
}

TEST(task_restore_log_contract) {
    std::string l = BuildRestoreLogText(SampleTask(), "2026-09-24T18:00:00");
    CHECK_CONTAINS(l, "action=restore");
    CHECK_CONTAINS(l, "log_time=2026-09-24T18:00:00");
    CHECK_CONTAINS(l, std::string("software_version=") + SYSRECOVER_VERSION);
    CHECK_CONTAINS(l, "target_part_offset=105906176");
    CHECK_CONTAINS(l, "target_part_size=716800000");
    CHECK_CONTAINS(l, "target_fs=NTFS");
    CHECK_CONTAINS(l, "target_vol_label=Windows");
    CHECK_CONTAINS(l, "image_index=2");
    CHECK_CONTAINS(l, "repair_boot=0");
    CHECK_CONTAINS(l, "pt_type=gpt");
}

// ────────────────────────── zip CRC-32 ──────────────────────────

TEST(zip_crc32_standard_vectors) {
    CHECK_EQ(Crc32("", 0), 0u);
    CHECK_EQ(Crc32("123456789", 9), 0xCBF43926u);  // 标准校验值
    const char* fox = "The quick brown fox jumps over the lazy dog";
    CHECK_EQ(Crc32(fox, std::strlen(fox)), 0x414FA339u);
}
