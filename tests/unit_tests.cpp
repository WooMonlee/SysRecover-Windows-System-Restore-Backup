// 纯逻辑单元测试（零依赖）。覆盖 PLAN §12 第 3 项点名的几块：
//   * sysinfo：注册表原始值 → 可读描述（Win11 家族纠正、版别中文映射、SP、UBR）
//   * exclude：默认排除清单（**含 PIT-056 回归守卫**）+ 云同步目录追加
//   * task   ：restore-task.conf / .json / _zjresy 日志的**契约文本**
//   * zip    ：CRC-32 标准测试向量
//   * advice ：建议码/退出码 → "下一步怎么办"（i18n 解耦，PLAN §14 M1）
// 这些都是把纯逻辑从 Windows 调用里抽出来后才可测的（见各头文件注释）。
#include <cstring>

#include <windows.h>

#include "tiny_test.h"

#include "app/advice.h"
#include "boot/bcd.h"
#include "boot/bcd_parse.h"
#include "boot/bootpath.h"
#include "boot/task.h"
#include "boot/uefi.h"
#include "common/i18n.h"
#include "common/pathutil.h"
#include "common/refscan.h"
#include "common/relocate.h"
#include "common/rescue_decision.h"
#include "common/sysinfo.h"
#include "common/version.h"
#include "common/zip.h"
#include "common/ea.h"
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
    t.espIndex = 2;  // 方案 C：镜像里含名为 ESP 的子镜像（index 2）
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
    CHECK_CONTAINS(c, "esp_index=2");  // 方案 C：ESP 子镜像 index 进契约
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
    CHECK_CONTAINS(j, "\"esp_index\":2");
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
    CHECK_CONTAINS(l, "esp_index=2");  // 主发现契约也要有（conf 缺失时的回退）
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

// ── pathutil：盘符根归一 + 目标目录（2026-09-28 用户 CLI `--source C:` rc=47）──
// Win32 里 `C:` 的语义是“该盘的**当前目录**”≠根，且没有 wimlib 要的尾斜杠：
// 判成非盘符根 → 不走 VSS 热备 → 扫活动系统撞上被占用的文件 → rc=47。

TEST(pathutil_volume_root_normalization) {
    CHECK(IsVolumeRoot(L"C:"));
    CHECK(IsVolumeRoot(L"c:\\"));
    CHECK(IsVolumeRoot(L" C:/ "));  // 允许空白
    CHECK(!IsVolumeRoot(L"C:\\Windows"));
    CHECK(!IsVolumeRoot(L"D:\\backup\\a.wim"));
    CHECK(!IsVolumeRoot(L""));
    CHECK(!IsVolumeRoot(L"backup"));
    // 三种写法统一成 `C:/`（大写盘符 + 尾斜杠）
    CHECK_EQ(NormalizeVolumeRoot(L"c:"), std::wstring(L"C:/"));
    CHECK_EQ(NormalizeVolumeRoot(L"c:\\"), std::wstring(L"C:/"));
    CHECK_EQ(NormalizeVolumeRoot(L"C:/"), std::wstring(L"C:/"));
    CHECK_EQ(NormalizeVolumeRoot(L" c: "), std::wstring(L"C:/"));
    // 非盘符根不改用户的路径
    CHECK_EQ(NormalizeVolumeRoot(L"D:\\backup\\a.wim"),
             std::wstring(L"D:\\backup\\a.wim"));
}

TEST(pathutil_parent_dir) {
    CHECK_EQ(ParentDir(L"D:\\backup\\a.wim"), std::wstring(L"D:\\backup"));
    CHECK_EQ(ParentDir(L"D:\\a.wim"), std::wstring(L"D:\\"));  // 盘符根带分隔符
    CHECK_EQ(ParentDir(L"a.wim"), std::wstring());
    CHECK_EQ(ParentDir(L""), std::wstring());
}

TEST(pathutil_make_dir_tree) {
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring base = std::wstring(tmp) + L"sr-pathutil-test";
    std::wstring leaf = base + L"\\a\\b\\c";
    CHECK(MakeDirTree(leaf));           // 递归建到三层
    CHECK(MakeDirTree(leaf));           // 已存在 → 幂等
    // 同名**文件**（不是目录）→ 必须失败，不能被当成“目录已就绪”
    std::wstring f = base + L"\\a\\file.bin";
    HANDLE h = CreateFileW(f.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    CloseHandle(h);
    CHECK(!MakeDirTree(f));
    // 收尾清理（删不掉就留着，%TEMP% 下无害）
    DeleteFileW(f.c_str());
    RemoveDirectoryW((base + L"\\a\\b\\c").c_str());
    RemoveDirectoryW((base + L"\\a\\b").c_str());
    RemoveDirectoryW((base + L"\\a").c_str());
    RemoveDirectoryW(base.c_str());
}

// ── task：restore-task.conf 读取（GUI 判断"菜单项绑定了哪个镜像"）──────
TEST(task_conf_get) {
    std::string conf = BuildTaskConf(SampleTask());
    // 契约里写过的键要能取回来（SampleTask 的 imageIndex = 2）
    CHECK_EQ(TaskConfGet(conf, "image_index"), std::string("2"));
    CHECK_EQ(TaskConfGet(conf, "esp_index"), std::string("2"));
    CHECK_EQ(TaskConfGet(conf, "image_path"),
             std::string("D:\\images\\win10.esd"));
    CHECK(!TaskConfGet(conf, "target_offset").empty());
    CHECK_EQ(TaskConfGet(conf, "contract_version"), std::string("1"));
    // 不存在的键 → 空串
    CHECK_EQ(TaskConfGet(conf, "no_such_key"), std::string(""));
    // 注释/空行跳过；行尾 \r 去掉；值里的空格保留（路径可能带空格）
    std::string t = "# c\r\n\r\na=1\r\nb= x y z \nc#not-a-comment=9\n";
    CHECK_EQ(TaskConfGet(t, "a"), std::string("1"));
    CHECK_EQ(TaskConfGet(t, "b"), std::string(" x y z "));  // 值不做 trim
    CHECK_EQ(TaskConfGet(t, "c"), std::string(""));
    // 前缀相同的键不能误命中
    std::string t2 = "target=GUID\ntarget_size=100\ntarget_offset=200\n";
    CHECK_EQ(TaskConfGet(t2, "target"), std::string("GUID"));
    CHECK_EQ(TaskConfGet(t2, "target_size"), std::string("100"));
    CHECK_EQ(TaskConfGet(t2, "target_offset"), std::string("200"));
}

// ── bcd：{bootmgr} timeout 解析（PIT-094：常驻菜单可被选中的前提）──────
TEST(bcd_parse_bootmgr_timeout) {
    // 实机 bcdedit /enum {bootmgr} 输出形态（元素名不依赖语言，值行两 token）
    std::string s =
        "Windows Boot Manager\r\n"
        "--------------------\r\n"
        "identifier              {bootmgr}\r\n"
        "device                  partition=C:\r\n"
        "path                    \\bootmgr\r\n"
        "description             Windows Boot Manager\r\n"
        "locale                  zh-CN\r\n"
        "default                 {current}\r\n"
        "displayorder            {current}\r\n"
        "                        {12345678-1234-1234-1234-123456789abc}\r\n"
        "toolsdisplayorder       {memdiag}\r\n"
        "timeout                 30\r\n";
    CHECK_EQ(ParseBootmgrTimeout(s), 30);
    // 标签本地化/其它写法：只要「两个 token + 第二个纯数字」就认
    CHECK_EQ(ParseBootmgrTimeout("\xe8\xb6\x85\xe6\x97\xb6\t\t5\n"), 5);
    CHECK_EQ(ParseBootmgrTimeout("timeout 1"), 1);       // 无行尾换行
    CHECK_EQ(ParseBootmgrTimeout("timeout  0\r\n"), 0);  // CR + 多空格
    CHECK_EQ(ParseBootmgrTimeout("locale zh-CN\ntimeout 10\n"), 10);
    // 认不出来 → -1
    CHECK_EQ(ParseBootmgrTimeout(""), -1);
    CHECK_EQ(ParseBootmgrTimeout("identifier {bootmgr}\n"), -1);
    CHECK_EQ(ParseBootmgrTimeout("timeout abc\n"), -1);
    // 第二 token 是数字但不是 timeout 量级（>1 天）→ 跳过继续找
    CHECK_EQ(ParseBootmgrTimeout("foo 9999999\ntimeout 7\n"), 7);
}

// ── bootpath：引导链选择（2026-09-29 用户实测回归）──────────────
// 关键用例：**UEFI 固件 + MBR 盘**必须走 BIOS/GRUB4DOS 链 —— 这类机器（CSM/Legacy
// 装的 Windows）没有 ESP，按固件类型选 UEFI 分支会直接报"未找到 ESP 分区"。
TEST(boot_path_choice) {
    using sysrecover::PartitionStyle;
    using sysrecover::ShouldUseUefiBoot;
    // UEFI 固件 + MBR 盘 → BIOS 链（本轮修的正是这条）
    CHECK(!ShouldUseUefiBoot(true, PartitionStyle::MBR));
    // BIOS 固件 + MBR 盘 → BIOS 链
    CHECK(!ShouldUseUefiBoot(false, PartitionStyle::MBR));
    // GPT 盘 → UEFI 链（BIOS 固件 + GPT 由 safety.cpp 预拒，这里只断言判据）
    CHECK(ShouldUseUefiBoot(true, PartitionStyle::GPT));
    CHECK(ShouldUseUefiBoot(false, PartitionStyle::GPT));
    // 风格未知 → 退回固件类型
    CHECK(ShouldUseUefiBoot(true, PartitionStyle::Unknown));
    CHECK(!ShouldUseUefiBoot(false, PartitionStyle::Unknown));
}

// ── bcd_parse：`bcdedit /enum` 输出断言（docs/15 · P3）────────────
// 用途：bcdboot 会静默失败，所以"写没写成"只能靠 BCD 产物断言。这些用例用的是
// 真实 bcdedit 输出的形状（含本轮实验室里造出来的各种坏状态）。
TEST(bcd_enum_shows_os_entry) {
    std::string why;
    // ① 健康：displayorder 有 {default}，且有 winload 条目
    CHECK(BcdEnumShowsOsEntry(
        "\xef\xbb\xbf"  // BOM 也不能绊倒解析
        "Windows Boot Manager\n--------------------\n"
        "identifier              {bootmgr}\n"
        "displayorder            {default}\n"
        "                        {18c8dcd6-bbd4-11f1-9f3c-54ee759ba870}\n"
        "timeout                 30\n\n"
        "Windows Boot Loader\n-------------------\n"
        "identifier              {default}\n"
        "path                    \\Windows\\system32\\winload.efi\n",
        &why));
    // ② 空 displayorder（实验室：删掉 displayorder 后 bootmgr 直接引导默认项）
    why.clear();
    CHECK(!BcdEnumShowsOsEntry(
        "identifier              {bootmgr}\n"
        "timeout                 30\n"
        "Windows Boot Loader\n"
        "path                    \\Windows\\system32\\winload.efi\n",
        &why));
    CHECK(!why.empty());
    // ③ 有 displayorder 但条目不存在（只剩 memdiag）→ 没有 winload
    why.clear();
    CHECK(!BcdEnumShowsOsEntry(
        "identifier              {bootmgr}\n"
        "displayorder            {deadbeef-0000-0000-0000-000000000000}\n"
        "Windows Memory Tester\n"
        "path                    \\EFI\\Microsoft\\Boot\\memtest.efi\n",
        &why));
    // ④ 模板态：连 displayorder 都没有
    why.clear();
    CHECK(!BcdEnumShowsOsEntry(
        "Windows Boot Manager\n"
        "identifier              {bootmgr}\n"
        "default                 {default}\n"
        "Windows Setup\n"
        "identifier              {default}\n",
        &why));
    // ⑤ 空输出 / 只有噪声
    CHECK(!BcdEnumShowsOsEntry("", nullptr));
    why.clear();
    CHECK(!BcdEnumShowsOsEntry("The boot configuration data store could not be opened.\n",
                               &why));
    CHECK(!why.empty());
    // ⑥ 字段名被本地化（中文标签）也要认出来
    CHECK(BcdEnumShowsOsEntry(
        "Windows \xe5\x90\xaf\xe5\x8a\xa8\xe7\xae\xa1\xe7\x90\x86\xe5\x99\xa8\n"
        "\xe6\x98\xbe\xe7\xa4\xba\xe9\xa1\xba\xe5\xba\x8f            {default}\n"
        "Windows Boot Loader\n"
        "path                    \\Windows\\system32\\winload.efi\n",
        nullptr));
    // ⑦ 行中出现同名子串（不是字段名）不能误判
    why.clear();
    CHECK(!BcdEnumShowsOsEntry(
        "description             my displayorder {fake} note\n"
        "Windows Boot Loader\n"
        "path                    \\Windows\\system32\\winload.efi\n",
        &why));
}

// ────────────────────── relocate（运行目录搬迁目的地选择） ──────────────────────
// 用户 2026-10-03 规则：系统分区之外的第一顺序可写固定分区；没有则退回系统分区。

TEST(relocate_drive_choice) {
    // 系统在 C: → 选第一块非系统盘
    CHECK_EQ(SelectRelocateDrive({L'C', L'D', L'E'}, L'C'), L'D');
    // 系统在 D: → 用 C:
    CHECK_EQ(SelectRelocateDrive({L'C', L'D'}, L'D'), L'C');
    // 只有系统盘 → 退回系统盘（单分区机器）
    CHECK_EQ(SelectRelocateDrive({L'C'}, L'C'), L'C');
    // 无候选 → 0
    CHECK_EQ(SelectRelocateDrive({}, L'C'), L'\0');
    // 大小写不敏感
    CHECK_EQ(SelectRelocateDrive({L'c'}, L'c'), L'C');
    // 候选顺序即优先级（C 被排除时取 D，即便 E 也在列表里）
    CHECK_EQ(SelectRelocateDrive({L'D', L'E'}, L'C'), L'D');
}

// ────────────────────── rescue_decision（回执/待执行标记判定，PIT-115） ────
// 用户 2026-10-05 规格："救援层是否真的跑过"必须能从记录中判定：
//   标记（pending）比所有回执新 / 压根没回执 → 任务从未执行；
//   有 FAILED 回执且标记不更新            → 上次还原失败；
//   其余（只有 OK、标记时间相等）          → 无事可报。
TEST(rescue_report_decision) {
    using sysrecover::DecideRescueReport;
    using sysrecover::RescueReport;
    using sysrecover::RescueRunTimes;

    // ① 什么都没有 → 不报
    RescueRunTimes t{};
    CHECK(DecideRescueReport(t) == RescueReport::None);

    // ② 只有待执行标记（救援从未跑/从未写回执）→ 报"未执行"
    t.pendingTime = "2026-10-05 10:00:00";
    CHECK(DecideRescueReport(t) == RescueReport::TaskNotExecuted);

    // ③ 标记 + 更旧的回执（旧任务的 OK/FAILED 不算数）→ 仍报"未执行"
    t.latestStatusTime = "2026-10-05 09:00:00";
    CHECK(DecideRescueReport(t) == RescueReport::TaskNotExecuted);

    // ④ 标记 + 更新的 OK 回执 → 已执行、无失败 → 不报
    t.latestStatusTime = "2026-10-05 11:00:00";
    CHECK(DecideRescueReport(t) == RescueReport::None);

    // ⑤ 同上但最新回执是 FAILED → 报"上次还原失败"
    t.latestFailedTime = "2026-10-05 11:00:00";
    CHECK(DecideRescueReport(t) == RescueReport::RestoreFailed);

    // ⑥ 标记又更新（又暂存了一次、还没跑）→ 回到"未执行"
    t.pendingTime = "2026-10-05 12:00:00";
    CHECK(DecideRescueReport(t) == RescueReport::TaskNotExecuted);

    // ⑦ 无标记 + FAILED 回执（老路径，0.6.29 之前就有的主场景）→ 报失败
    t = RescueRunTimes{};
    t.latestFailedTime = "2026-10-05 11:00:00";
    CHECK(DecideRescueReport(t) == RescueReport::RestoreFailed);

    // ⑧ 标记与回执**时间相等** → 视为已执行（防同秒竞态误报）
    t.pendingTime = "2026-10-05 11:00:00";
    CHECK(DecideRescueReport(t) == RescueReport::RestoreFailed);

    // ⑨ 只有 OK 回执、无失败、无标记 → 不报
    t = RescueRunTimes{};
    t.latestStatusTime = "2026-10-05 11:00:00";
    CHECK(DecideRescueReport(t) == RescueReport::None);
}

// ────────────── ea（EA 打包 / 组策略文件助手，PIT-122）──────────────
TEST(ea_pack_roundtrip) {
    std::vector<ea::FileEntry> in;
    ea::FileEntry a;
    a.relPath = L"Windows\\System32\\drivers\\foo.sys";
    ea::EaValue v1;
    v1.name = "ZJTEST";
    v1.value = {'1'};
    ea::EaValue v2;
    v2.name = "BIN";
    v2.value = {0x00, 0x01, 0xFF, 0x7F, 0x80};
    a.eas.push_back(v1);
    a.eas.push_back(v2);
    in.push_back(a);
    ea::FileEntry b;
    b.relPath = L"Program Files\\中文 目录\\x.dll";
    ea::EaValue v3;
    v3.name = "EMPTY";
    b.eas.push_back(v3);
    in.push_back(b);

    auto bytes = ea::BuildPack(in);
    std::vector<ea::FileEntry> out;
    std::string why;
    CHECK(ea::ParsePack(bytes.data(), bytes.size(), out, why));
    CHECK_EQ(out.size(), (size_t)2);
    CHECK_EQ(out[0].relPath, in[0].relPath);
    CHECK_EQ(out[0].eas.size(), (size_t)2);
    CHECK_EQ(out[0].eas[0].name, std::string("ZJTEST"));
    CHECK(out[0].eas[0].value.size() == 1 && out[0].eas[0].value[0] == '1');
    CHECK_EQ(out[0].eas[1].value.size(), (size_t)5);
    CHECK_EQ(out[0].eas[1].value[4], (unsigned char)0x80);
    CHECK_EQ(out[1].relPath, in[1].relPath);
    CHECK_EQ(out[1].eas[0].value.size(), (size_t)0);
}

TEST(ea_pack_reject) {
    std::vector<ea::FileEntry> out;
    std::string why;
    auto empty = ea::BuildPack({});
    CHECK(ea::ParsePack(empty.data(), empty.size(), out, why));
    CHECK_EQ(out.size(), (size_t)0);
    auto bad = empty;
    bad[0] = 'X';
    CHECK(!ea::ParsePack(bad.data(), bad.size(), out, why));
    std::vector<ea::FileEntry> one;
    ea::FileEntry f;
    f.relPath = L"a";
    ea::EaValue v;
    v.name = "N";
    v.value = {1, 2, 3};
    f.eas.push_back(v);
    one.push_back(f);
    auto full = ea::BuildPack(one);
    CHECK(!ea::ParsePack(full.data(), full.size() - 2, out, why));
}

TEST(ea_pol_fragment_strip) {
    auto frag = ea::BuildSyncPolFragment();
    CHECK(frag.size() > 16);
    CHECK(memcmp(frag.data(), "PReg", 4) == 0);
    bool removed = false;
    auto stripped = ea::StripOurPolRecords(frag, &removed);
    CHECK(removed);
    CHECK_EQ(stripped.size(), (size_t)8);
    std::vector<unsigned char> junk = {1, 2, 3};
    bool r2 = false;
    auto same = ea::StripOurPolRecords(junk, &r2);
    CHECK(!r2);
    CHECK_EQ(same.size(), (size_t)3);
}

TEST(ea_set_ea_buffer_layout) {
    std::vector<ea::EaValue> eas;
    ea::EaValue e1;
    e1.name = "AA";
    e1.value = {1, 2};
    ea::EaValue e2;
    e2.name = "BBB";
    e2.value = {3, 4, 5, 6, 7};
    eas.push_back(e1);
    eas.push_back(e2);
    auto buf = ea::BuildSetEaBuffer(eas);
    // 第一条: 8+2+1+2=13 → 对齐 16；第二条: 8+3+1+5=17 → 对齐 20
    CHECK_EQ(buf.size(), (size_t)(16 + 20));
    CHECK_EQ(*(unsigned long*)&buf[0], (unsigned long)16);
    CHECK_EQ(buf[5], (unsigned char)2);
    CHECK_EQ(buf[8], (unsigned char)'A');
    CHECK_EQ(*(unsigned long*)&buf[16], (unsigned long)0);
    CHECK_EQ(buf[16 + 5], (unsigned char)3);
    CHECK_EQ(buf[16 + 8 + 4], (unsigned char)3);
}

TEST(ea_scripts_ini_remove) {
    std::string ini =
        "[Startup]\r\n0cmdLine=zj-ea-restore.cmd\r\n0Parameters=\r\n"
        "1cmdLine=other.cmd\r\n1Parameters=\r\n";
    CHECK(ea::RemoveScriptEntry(ini, "zj-ea-restore.cmd"));
    CHECK(ini.find("zj-ea-restore") == std::string::npos);
    CHECK(ini.find("other.cmd") != std::string::npos);
    std::string ini2 =
        "[Startup]\r\n0CMDLINE=ZJ-EA-RESTORE.CMD\r\n0PARAMETERS=\r\n";
    CHECK(ea::RemoveScriptEntry(ini2, "zj-ea-restore.cmd"));
    CHECK(ini2.find("RESTORE") == std::string::npos);
    std::string ini3 = "[Startup]\r\n0cmdLine=other.cmd\r\n";
    CHECK(!ea::RemoveScriptEntry(ini3, "zj-ea-restore.cmd"));
}

TEST(ea_gpt_bump) {
    std::string g = "[General]\r\nVersion=65537\r\n";
    CHECK(ea::BumpGptIniVersion(g));
    CHECK(g.find("Version=65538\r\n") != std::string::npos);
    std::string g2 = "[General]\r\n";
    CHECK(!ea::BumpGptIniVersion(g2));
}

// ────────────── uefi 软项判定（PIT-126：精简系统容错）──────────────
// ESP 缺软项时：源也缺 → 精简镜像的既成事实（放行）；源有 → bcdboot 写一半（fail）。
TEST(esp_soft_item_verdict) {
    using sysrecover::EspSoftItemVerdict;
    using sysrecover::SoftItemVerdict;
    CHECK(EspSoftItemVerdict(true, true) == SoftItemVerdict::Ok);
    CHECK(EspSoftItemVerdict(true, false) == SoftItemVerdict::Ok);
    CHECK(EspSoftItemVerdict(false, true) == SoftItemVerdict::Fail);
    CHECK(EspSoftItemVerdict(false, false) == SoftItemVerdict::SkipSourceMissing);
}

// ────────────── 悬空引用扫描（PIT-132：Temp 注册组件还原后必坏）──────────────
TEST(refscan_volatile_path) {
    using sysrecover::refscan::IsVolatilePath;
    // 正例：备份排除清单里的易失目录（含子目录 / 大小写 / `/` / \??\ 前缀）
    CHECK(IsVolatilePath(L"C:\\Users\\Administrator\\AppData\\Local\\Temp\\"
                         L"DoubaoPortableTemp\\doubao_ext\\1.49.10\\shellext.dll"));
    CHECK(IsVolatilePath(L"c:\\users\\x\\appdata\\local\\temp\\a\\b.dll"));
    CHECK(IsVolatilePath(L"C:/Users/x/AppData/Local/Temp/a/b.dll"));
    CHECK(IsVolatilePath(L"\\??\\C:\\Users\\x\\AppData\\Local\\Temp\\a.exe"));
    CHECK(IsVolatilePath(L"C:\\Windows\\Temp\\setup\\x.dll"));
    CHECK(IsVolatilePath(L"C:\\Windows\\CbsTemp\\x.exe"));
    CHECK(IsVolatilePath(L"C:\\Windows\\winsxs\\InstallTemp\\x.dll"));
    CHECK(IsVolatilePath(
        L"C:\\Users\\x\\AppData\\Local\\Microsoft\\Windows\\INetCache\\a.dll"));
    // 负例：程序目录 / AppData 其它子目录 / 根级 Temp / 仅含 temp 字样
    CHECK(!IsVolatilePath(L"C:\\Program Files\\App\\shellext.dll"));
    CHECK(!IsVolatilePath(
        L"C:\\Users\\x\\AppData\\Local\\Microsoft\\Edge\\a.dll"));
    CHECK(!IsVolatilePath(L"C:\\Temp\\a.dll"));
    CHECK(!IsVolatilePath(L"C:\\Users\\x\\AppData\\Local\\Tempest\\a.dll"));
    CHECK(!IsVolatilePath(L""));
}

TEST(refscan_extract_exe) {
    using sysrecover::refscan::ExtractExecutable;
    CHECK_EQ(ExtractExecutable(L"\"C:\\a b\\x.exe\" /arg"),
             std::wstring(L"C:\\a b\\x.exe"));
    CHECK_EQ(ExtractExecutable(L"C:\\a\\x.exe /arg"),
             std::wstring(L"C:\\a\\x.exe"));
    CHECK_EQ(ExtractExecutable(L"  C:\\a b\\x.exe /arg"),
             std::wstring(L"C:\\a b\\x.exe"));
    CHECK_EQ(ExtractExecutable(L"%TEMP%\\x\\a.exe"),
             std::wstring(L"%TEMP%\\x\\a.exe"));
    // 无 .exe：截到首个空格（记录既有语义）
    CHECK_EQ(ExtractExecutable(L"C:\\a b\\run"), std::wstring(L"C:\\a"));
    CHECK_EQ(ExtractExecutable(L""), std::wstring());
}

TEST(refscan_task_commands) {
    using sysrecover::refscan::ExtractTaskCommands;
    std::wstring xml =
        L"<?xml version=\"1.0\"?><Task><Actions><Exec><Command>"
        L"C:\\Users\\x\\AppData\\Local\\Temp\\a\\up.exe</Command>"
        L"<Arguments>--silent</Arguments></Exec>"
        L"<Exec><Command>C:\\Program Files\\b\\b.exe</Command></Exec>"
        L"</Actions></Task>";
    auto cmds = ExtractTaskCommands(xml);
    CHECK_EQ(cmds.size(), (size_t)2);
    CHECK_EQ(cmds[0],
             std::wstring(L"C:\\Users\\x\\AppData\\Local\\Temp\\a\\up.exe"));
    CHECK_EQ(cmds[1], std::wstring(L"C:\\Program Files\\b\\b.exe"));
    CHECK_EQ(ExtractTaskCommands(L"no commands here").size(), (size_t)0);
}
