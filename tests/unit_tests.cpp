// 纯逻辑单元测试（零依赖）。覆盖 PLAN §12 第 3 项点名的几块：
//   * sysinfo：注册表原始值 → 可读描述（Win11 家族纠正、版别中文映射、SP、UBR）
//   * exclude：默认排除清单（**含 PIT-056 回归守卫**）+ 云同步目录追加
//   * task   ：restore-task.conf / .json / _zjresy 日志的**契约文本**
//   * zip    ：CRC-32 标准测试向量
//   * advice ：建议码/退出码 → "下一步怎么办"（i18n 解耦，PLAN §14 M1）
// 这些都是把纯逻辑从 Windows 调用里抽出来后才可测的（见各头文件注释）。
#include <cstring>

#include "tiny_test.h"

#include "app/advice.h"
#include "boot/task.h"
#include "common/i18n.h"
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

// ───────────────── advice（建议码，i18n 前置解耦 PLAN §14 M1） ─────────────────

TEST(advice_key_is_ascii_and_stable) {
    // key 是日志 / 单测 / 将来 i18n 取词的唯一标识 —— 必须纯 ASCII 且永不改名
    ErrAdvice codes[] = {ADV_NONE, ADV_BITLOCKER, ADV_SPACE, ADV_INCOMPLETE,
                         ADV_IMAGE_IN_TARGET, ADV_ADMIN, ADV_VSS};
    const char* expect[] = {"none", "bitlocker", "space", "incomplete",
                            "image_in_target", "admin", "vss"};
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        std::string k = AdviceKey(codes[i]);
        CHECK_EQ(k, std::string(expect[i]));
        for (char c : k)
            CHECK(c > 0 && c < 128);  // 纯 ASCII（含中文 = 有人把 key 翻译了）
    }
}

TEST(advice_text_per_code) {
    // 每个建议码都要给出"下一步怎么办"；ADV_VSS 例外（消息自带处理步骤，
    // 叠加建议反而误导 —— PIT-087）。⚠️ M2 把文案改为 i18n 取词时，
    // 这里的"非空/为空"断言不变，只改取词来源。
    CHECK(!ErrorAdvice(1, ADV_BITLOCKER).empty());
    CHECK(!ErrorAdvice(1, ADV_SPACE).empty());
    CHECK(!ErrorAdvice(1, ADV_INCOMPLETE).empty());
    CHECK(!ErrorAdvice(4, ADV_IMAGE_IN_TARGET).empty());
    CHECK(!ErrorAdvice(1, ADV_ADMIN).empty());
    CHECK(ErrorAdvice(1, ADV_VSS).empty());   // 消息自带 services.msc / sc 指引
    CHECK(ErrorAdvice(1, ADV_NONE).empty());
    CHECK(ErrorAdvice(0, ADV_NONE).empty());
}

TEST(advice_by_exit_code) {
    // rc 判定与界面语言无关（原关键词版的 concurrent/正在被修改 分支已废弃）。
    // ⚠️ M2 本地化后若改为按 i18n 取词，这里的中文子串断言需同步改成取词对比。
    CHECK_CONTAINS(ErrorAdvice(88, ADV_NONE), "持续变化");  // 并发修改建议
    CHECK_CONTAINS(ErrorAdvice(6, ADV_NONE), "取消");
    CHECK(ErrorAdvice(89, ADV_NONE).empty());  // wimlib 快照失败：无建议（与旧行为一致）
    CHECK(ErrorAdvice(5, ADV_NONE).empty());   // 校验失败：无建议
}

// ── i18n：多语言运行时（PLAN §14 M2/M3）────────────────────────────────
// 用 LoadLangText **直接注入**词典文本：测试二进制跑在 build/ 下，exe 旁边
// 不一定有 lang/ 目录，靠文件的用例会随摆放位置翻车。每个用例收尾都
// InitI18n("zh") 复位 —— 上面 advice 的中文子串断言依赖源语言。

TEST(i18n_source_lang) {
    InitI18n("zh");
    CHECK(IsSourceLang());
    CHECK_EQ(std::string(LangTag()), std::string("zh-CN"));
    // 源语言不查表：键即原文，窄串与宽串都原样回源
    CHECK_EQ(std::string(Tr("九转还原")), std::string("九转还原"));
    CHECK_EQ(std::wstring(Tr(L"九转还原")), std::wstring(L"九转还原"));
    InitI18n("zh");  // 复位
}

TEST(i18n_lookup_and_fallback) {
    ClearI18n();
    LoadLangText("九转还原=SysRecover\nbackup=Backup\n");
    // 窄串直查、宽串走 W2U→词典→U2W
    CHECK_EQ(std::string(Tr("九转还原")), std::string("SysRecover"));
    CHECK_EQ(std::wstring(Tr(L"九转还原")), std::wstring(L"SysRecover"));
    CHECK_EQ(std::wstring(Tr(L"backup")), std::wstring(L"Backup"));
    // 未收录的键 → 回源（局部回退，不崩界面）
    CHECK_EQ(std::string(Tr("一个没有登记的键")), std::string("一个没有登记的键"));
    CHECK_EQ(std::wstring(Tr(L"一个没有登记的键")),
             std::wstring(L"一个没有登记的键"));
    CHECK_EQ(std::string(Tr(static_cast<const char*>(nullptr))),
             std::string(""));
    InitI18n("zh");  // 复位
}

TEST(i18n_escaping_and_equals_split) {
    ClearI18n();
    // .lang 格式：首遇**未转义**的 '=' 才是分隔符；\= \n \t \\ 装载时还原。
    // 这正是 `备份失败(rc=%d): ` 那类键的坑 —— 键本身含 '='，不转义会把键
    // 从中间截断（回归用例，见下）。
    LoadLangText("# 这行是注释，必须整行跳过\n"
                 "noequals\n"          // 没有 '=' → 跳过
                 "=orphan\n"           // 空键（eq==0）→ 跳过
                 "a\\=b=c\\=d\n"
                 "multi=line1\\nline2\n"
                 "tab=x\\ty\n"
                 "bs=C:\\\\dir\n"
                 "eqv=k=v\n"
                 "备份失败(rc\\=%d): =Backup failed (rc\\=%d): \n");
    CHECK_EQ(std::string(Tr("a=b")), std::string("c=d"));
    CHECK_EQ(std::string(Tr("multi")), std::string("line1\nline2"));
    CHECK_EQ(std::string(Tr("tab")), std::string("x\ty"));
    CHECK_EQ(std::string(Tr("bs")), std::string("C:\\dir"));
    CHECK_EQ(std::string(Tr("eqv")), std::string("k=v"));
    // 含 '=' 的键必须完整命中（否则界面会显示半截中文 + printf 参数错位）
    CHECK_EQ(std::string(Tr("备份失败(rc=%d): ")),
             std::string("Backup failed (rc=%d): "));
    // 注释行/无 '=' 行/空键行都不该产生词条
    CHECK_EQ(std::string(Tr("# 这行是注释，必须整行跳过")),
             std::string("# 这行是注释，必须整行跳过"));
    CHECK_EQ(std::string(Tr("noequals")), std::string("noequals"));
    CHECK_EQ(std::string(Tr("orphan")), std::string("orphan"));
    InitI18n("zh");  // 复位
}

TEST(i18n_skin_xml_missing_file) {
    // 读不到皮肤 → 返回空串，调用方回退原文件名让 Duilib 自己报错
    CHECK(LoadSkinXml(L"__definitely_missing__.xml").empty());
    CHECK(LoadSkinXml(nullptr).empty());
    InitI18n("zh");  // 复位
}
