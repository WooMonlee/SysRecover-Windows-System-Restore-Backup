// 纯逻辑单元测试（零依赖）。覆盖 PLAN §12 第 3 项点名的几块：
//   * sysinfo：注册表原始值 → 可读描述（Win11 家族纠正、版别中文映射、SP、UBR）
//   * exclude：默认排除清单（**含 PIT-056 回归守卫**）+ 云同步目录追加
//   * task   ：restore-task.conf / .json / _zjresy 日志的**契约文本**
//   * zip    ：CRC-32 标准测试向量
//   * advice ：建议码/退出码 → "下一步怎么办"（i18n 解耦，PLAN §14 M1）
// 这些都是把纯逻辑从 Windows 调用里抽出来后才可测的（见各头文件注释）。
#include <cstdio>
#include <cstring>

#include <windows.h>

#include "tiny_test.h"

#include "app/advice.h"
#include "app/swapleft.h"
#include "boot/bcd.h"
#include "boot/bcd_parse.h"
#include "boot/bootnames.h"
#include "boot/bootpath.h"
#include "boot/bootself.h"
#include "boot/task.h"
#include "boot/uefi.h"
#include "common/crypt.h"
#include "common/i18n.h"
#include "common/bioskey.h"
#include "common/pathutil.h"
#include "common/refscan.h"
#include "common/relocate.h"
#include "common/rescue_decision.h"
#include "common/sysinfo.h"
#include "common/version.h"
#include "common/zip.h"
#include "common/ea.h"
#include "disk/partscript.h"
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
    CHECK_CONTAINS(c, "\\Users\\*\\AppData\\Local\\Microsoft\\OneDrive*");
    CHECK_CONTAINS(c, "\\ProgramData\\Alibaba\\AlibabaProtectDT\\Logs*");
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
    t.imageSize = 1517536768ull;  // 1019 案：镜像字节数（救援层契约认证比对用）
    t.reparseScan = 0;            // 1019 案 W1：暂存侧已扫描干净
    t.efsScan = 0;                // 1019 案 W2：EFS 预检也干净
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
    CHECK_CONTAINS(c, "image_size=1517536768");  // 1019 案 W1：契约认证（size）
    CHECK_CONTAINS(c, "reparse_scan=0");         // 1019 案 W1：暂存侧扫描干净
    CHECK_CONTAINS(c, "efs_scan=0");             // 1019 案 W2：EFS 预检干净
    CHECK_CONTAINS(c, "esp_index=2");  // 方案 C：ESP 子镜像 index 进契约
    CHECK_CONTAINS(c, "target_offset=105906176");
    CHECK_CONTAINS(c, "target_size=716800000");
    CHECK_CONTAINS(c, "target_disk_serial=QEMU-DRILL-0001");
    CHECK_CONTAINS(c, "repair_boot=0");
    CHECK_CONTAINS(c, "partition_count=3");
    CHECK_CONTAINS(c, "software_dir=D:\\ZJRESTORE");
    CHECK_CONTAINS(c, "keep_old=0");  // PIT-148：默认 false（SampleTask 未设）
}

// PIT-148：keep_old 进 conf（一键重试要复刻"是否备返"）；true=1。
TEST(task_conf_keep_old) {
    RestoreTask t = SampleTask();
    t.keepOld = true;
    CHECK_CONTAINS(BuildTaskConf(t), "keep_old=1");
    CHECK_CONTAINS(BuildRestoreLogText(t, "T"), "keep_old=1");
}

TEST(task_json_escapes_backslashes) {
    std::string j = BuildTaskJson(SampleTask());
    CHECK_CONTAINS(j, "\"schema\":1");
    CHECK_CONTAINS(j, "\"action\":\"restore\"");
    CHECK_CONTAINS(j, "\"repair_boot\":false");
    CHECK_CONTAINS(j, "D:\\\\images\\\\win10.esd");  // JSON 里反斜杠要转义
    CHECK_CONTAINS(j, "\"target_disk\":0");
    CHECK_CONTAINS(j, "\"esp_index\":2");
    CHECK_CONTAINS(j, "\"image_size\":1517536768");  // 1019 案 W1
    CHECK_CONTAINS(j, "\"reparse_scan\":0");         // 1019 案 W1
    CHECK_CONTAINS(j, "\"efs_scan\":0");             // 1019 案 W2
}

// 1019 案 W1/W2：reparse_scan / efs_scan / image_size 是**增量键** —— 默认值
// 必须安全：老调用点（StageReturnOld、未来新入口）不设 → reparse_scan=-1、
// efs_scan=-1（救援层自行扫描）、image_size=0（与任何实际大小都不等 →
// 不会误认证）。W2 关键：老的 0.7.70 版 exe 只写 reparse_scan=0、缺 efs_scan
// → 救援层 fast path 要求两键都 0，缺一即全量扫描（防旧证书漏掉 EFS）。
TEST(task_conf_reparse_defaults) {
    RestoreTask t = SampleTask();
    t.reparseScan = RestoreTask{}.reparseScan;
    t.efsScan = RestoreTask{}.efsScan;
    t.imageSize = RestoreTask{}.imageSize;
    CHECK_EQ(t.reparseScan, -1);
    CHECK_EQ(t.efsScan, -1);
    CHECK_EQ(t.imageSize, 0ull);
    std::string c = BuildTaskConf(t);
    CHECK_CONTAINS(c, "reparse_scan=-1");
    CHECK_CONTAINS(c, "efs_scan=-1");
    CHECK_CONTAINS(c, "image_size=0");
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
    CHECK_CONTAINS(l, "image_size=1517536768");  // 1019 案 W1：日志兜底路径也要有
    CHECK_CONTAINS(l, "reparse_scan=0");         // 1019 案 W1
    CHECK_CONTAINS(l, "efs_scan=0");             // 1019 案 W2：日志兜底路径也要有
    CHECK_CONTAINS(l, "esp_index=2");  // 主发现契约也要有（conf 缺失时的回退）
    CHECK_CONTAINS(l, "repair_boot=0");
    CHECK_CONTAINS(l, "pt_type=gpt");
}

// ── PIT-139「返回旧系统」（action=returnold）：**同一契约的第二个取值**，不升
// contract_version —— 旧救援层不认 action 就照 swap 走，zjswap.sh 缺
// ~new/.zj-done 会 exit 2 fail-closed，绝不会把盘改坏。这里守三件事：
//   ① 三种文本形态都写出 action；② 空 action 回落成 restore（老调用点不改也能用）；
//   ③ engine=swap 且 image_* 被清空（反向交换不需要镜像）。
TEST(task_returnold_action_in_all_builders) {
    RestoreTask t = SampleTask();
    t.action = "returnold";
    t.engine = "swap";
    t.imagePath.clear();
    t.imageRelPath.clear();
    t.imageIndex = 0;
    t.espIndex = 0;
    t.eaIndex = 0;

    CHECK_CONTAINS(BuildTaskConf(t), "action=returnold");
    CHECK_CONTAINS(BuildTaskJson(t), "\"action\":\"returnold\"");
    CHECK_CONTAINS(BuildRestoreLogText(t, "2026-10-13T10:00:00"),
                   "action=returnold");

    // engine 走 swap（A 引擎），且镜像字段确实被清空 —— 反向交换没有镜像。
    std::string c = BuildTaskConf(t);
    CHECK_CONTAINS(c, "engine=swap");
    CHECK_CONTAINS(c, "image_index=0");
    CHECK_CONTAINS(c, "esp_index=0");

    // 契约版本不动（增量键，见 AGENTS §5 / PIT-139）。
    CHECK_CONTAINS(c, "contract_version=1");
    CHECK_EQ(TaskConfGet(c, "action"), std::string("returnold"));
}

// 空 action 一律回落成 restore：老调用点、外部工具手写的契约都不必改。
TEST(task_action_defaults_to_restore) {
    RestoreTask t = SampleTask();
    t.action.clear();
    CHECK_CONTAINS(BuildTaskConf(t), "action=restore");
    CHECK_CONTAINS(BuildTaskJson(t), "\"action\":\"restore\"");
    CHECK_CONTAINS(BuildRestoreLogText(t, "2026-10-13T10:00:00"),
                   "action=restore");
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
    CHECK_EQ(TaskConfGet(conf, "image_size"), std::string("1517536768"));  // 1019 案
    CHECK_EQ(TaskConfGet(conf, "reparse_scan"), std::string("0"));         // 1019 案
    CHECK_EQ(TaskConfGet(conf, "efs_scan"), std::string("0"));             // 1019 案 W2
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

// 常驻菜单与日常还原临时项必须使用不同 GUID + 不同显示名：
// 首启清理只认临时项，绝不能误删用户显式安装的菜单。
TEST(boot_entry_names_and_guids_distinct) {
    CHECK(wcscmp(RecoveryGuid(), TemporaryGuid()) != 0);
    CHECK(wcscmp(kMenuEntryDesc, kTempEntryDesc) != 0);
    CHECK(wcscmp(kMenuBcdDesc, kTempEntryDesc) != 0);
    CHECK(wcslen(kTempEntryDesc) > 0);
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
    // GPT 盘 → UEFI 链（BIOS 会话 + GPT 暂存由 CheckStagedBootPath 拦截，这里只断言判据）
    CHECK(ShouldUseUefiBoot(true, PartitionStyle::GPT));
    CHECK(ShouldUseUefiBoot(false, PartitionStyle::GPT));
    // 风格未知 → 退回固件类型
    CHECK(ShouldUseUefiBoot(true, PartitionStyle::Unknown));
    CHECK(!ShouldUseUefiBoot(false, PartitionStyle::Unknown));
}

// ── bootpath：暂存会话前置（Q2，2026-10-18）──────────────────────
// 两种必坏组合拒绝、合法组合与 Unknown 放行。
TEST(check_staged_boot_path) {
    using sysrecover::CheckStagedBootPath;
    using sysrecover::PartitionStyle;
    using sysrecover::StagedBootPath;
    // BIOS 会话 + GPT 目标 → 拒
    CHECK(CheckStagedBootPath(false, PartitionStyle::GPT) ==
          StagedBootPath::BiosSessionGptTarget);
    // UEFI 会话 + MBR 目标 → 拒
    CHECK(CheckStagedBootPath(true, PartitionStyle::MBR) ==
          StagedBootPath::UefiSessionMbrTarget);
    // UEFI 会话 + GPT 目标 → 放行
    CHECK(CheckStagedBootPath(true, PartitionStyle::GPT) == StagedBootPath::Ok);
    // BIOS 会话 + MBR 目标 → 放行
    CHECK(CheckStagedBootPath(false, PartitionStyle::MBR) == StagedBootPath::Ok);
    // Unknown 风格 → 放行（两种会话）
    CHECK(CheckStagedBootPath(true, PartitionStyle::Unknown) == StagedBootPath::Ok);
    CHECK(CheckStagedBootPath(false, PartitionStyle::Unknown) == StagedBootPath::Ok);
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
    // Q1 avoidTarget（2026-10-18）：目标盘被跳过
    CHECK_EQ(SelectRelocateDrive({L'C', L'D'}, L'C', L'D'), L'C');
    // avoidTarget 与 systemLetter 同盘：D 不是目标 → 用 D
    CHECK_EQ(SelectRelocateDrive({L'C', L'D'}, L'C', L'C'), L'D');
    // avoidTarget 排除唯一候选 → 0
    CHECK_EQ(SelectRelocateDrive({L'D'}, L'C', L'D'), L'\0');
    // avoidTarget 大小写不敏感
    CHECK_EQ(SelectRelocateDrive({L'C', L'D'}, L'C', L'd'), L'C');
}

// 用户 2026-10-17 规则 ①：日志根优先「已安装的程序目录」——
// `<ProgramFiles>\SysRecover\logs`，但 PE 里 Program Files 可能是离线系统盘
// （还原目标）→ 不能用；装 Windows 的盘与程序目录不同盘时也不能用。
TEST(log_home_install_dir_rule) {
    using sysrecover::UseInstallDirLogHome;
    // 最常见：C: 装 Windows，程序装在 C:\Program Files\SysRecover
    CHECK(UseInstallDirLogHome(L"C:\\Program Files\\SysRecover", L"C:\\Windows",
                              false));
    // 盘符大小写不敏感
    CHECK(UseInstallDirLogHome(L"c:\\Program Files\\SysRecover", L"C:\\Windows",
                              false));
    // Windows 装在 D: → D:\Program Files\SysRecover
    CHECK(UseInstallDirLogHome(L"D:\\Program Files\\SysRecover", L"D:\\Windows",
                              false));
    // 不同盘：D: 的程序目录 + C: 的 Windows → 不用（Program Files 目录不在本系统里）
    CHECK(!UseInstallDirLogHome(L"D:\\Program Files\\SysRecover", L"C:\\Windows",
                               false));
    // PE：winDir 在 RAM（X:），Program Files 可能是离线系统盘 → 一律不用
    CHECK(!UseInstallDirLogHome(L"C:\\Program Files\\SysRecover", L"X:\\Windows",
                               true));
    // 空/非法路径 → 不用
    CHECK(!UseInstallDirLogHome(L"", L"C:\\Windows", false));
    CHECK(!UseInstallDirLogHome(L"Program Files\\SysRecover", L"C:\\Windows",
                               false));
    CHECK(!UseInstallDirLogHome(L"C:\\Program Files\\SysRecover", L"", false));
}

// Q1「先落盘后搬家」触发判据（relocate.h::ShouldSettleHome）：还原只格式化
// 目标分区 → 同盘不同分区的程序不受影响，无需搬迁；只有同盘符才触发。
TEST(settle_home_trigger) {
    using sysrecover::ShouldSettleHome;
    // 同盘符 → 触发（大小写不敏感）
    CHECK(ShouldSettleHome(L"C:\\Program Files\\SysRecover", L'C'));
    CHECK(ShouldSettleHome(L"C:\\SysRecover", L'c'));
    CHECK(ShouldSettleHome(L"c:\\x", L'C'));
    // 异盘 → 不触发
    CHECK(!ShouldSettleHome(L"D:\\SysRecover", L'C'));
    CHECK(!ShouldSettleHome(L"E:\\x", L'c'));
    // targetLetter=0（目标无盘符）→ 不触发
    CHECK(!ShouldSettleHome(L"C:\\x", 0));
    // exeDir 为空 / 无盘符 / 太短 → 不触发
    CHECK(!ShouldSettleHome(L"", L'C'));
    CHECK(!ShouldSettleHome(L"\\x", L'C'));
    CHECK(!ShouldSettleHome(L"C", L'C'));
    CHECK(ShouldSettleHome(L"C:", L'C'));  // 盘符根也算同盘（边界）
}

// ZJRESTORE 残留目录分类（PIT-156）：保守 —— 看得懂的删、看不懂的留。
TEST(zjr_residue_classify) {
    using sysrecover::ClassifyZjrResidue;
    using sysrecover::ZjrFolderKind;
    // 空目录 → Empty（删目录本身）
    CHECK(ClassifyZjrResidue({}) == ZjrFolderKind::Empty);
    // 典型还原残留（EA 载荷）→ Delete
    CHECK(ClassifyZjrResidue({L"ea"}) == ZjrFolderKind::Delete);
    CHECK(ClassifyZjrResidue({L"ea", L"source.txt"}) == ZjrFolderKind::Delete);
    CHECK(ClassifyZjrResidue({L"bootfix", L"logs", L"rescue-build.txt"}) ==
          ZjrFolderKind::Delete);
    // 已装启动菜单（boot/scripts）→ Keep（菜单本体，删不得）
    CHECK(ClassifyZjrResidue({L"boot", L"ea"}) == ZjrFolderKind::Keep);
    CHECK(ClassifyZjrResidue({L"scripts", L"bootfix"}) == ZjrFolderKind::Keep);
    // SettleHome 程序的家（exe）→ Keep（logs\ 正在被写）
    CHECK(ClassifyZjrResidue({L"SysRecover.exe", L"logs"}) ==
          ZjrFolderKind::Keep);
    CHECK(ClassifyZjrResidue({L"SysRecoverUI.exe", L"boot", L"ea"}) ==
          ZjrFolderKind::Keep);
    // 大小写不敏感（磁盘上的真实名字大小写不一）
    CHECK(ClassifyZjrResidue({L"EA", L"SOURCE.TXT"}) == ZjrFolderKind::Delete);
    CHECK(ClassifyZjrResidue({L"BOOT"}) == ZjrFolderKind::Keep);
    // 精确名匹配 —— bootfix/bootfiles 不能命中"boot"（前缀陷阱）
    CHECK(ClassifyZjrResidue({L"bootfix"}) == ZjrFolderKind::Delete);
    CHECK(ClassifyZjrResidue({L"bootfiles"}) == ZjrFolderKind::Keep);
    // 认不出的内容 → 保守保留
    CHECK(ClassifyZjrResidue({L"_zjresy-1.log"}) == ZjrFolderKind::Keep);
    CHECK(ClassifyZjrResidue({L"restore-task.conf", L"mydata"}) ==
          ZjrFolderKind::Keep);
    // 我方信号 + 未知杂物 → 仍删（整个 ZJRESTORE 是还原期重建的，杂物也一并清）
    CHECK(ClassifyZjrResidue({L"ea", L"whatever.bin"}) ==
          ZjrFolderKind::Delete);
}

// 开机快捷启动键识别（bioskey；表移植自 BiosKC，2026-10-18 客户反馈：
// HP 是 F9 而文案写死 F12 → 按品牌/机型给键名，识别不到返回空=中性措辞）。
TEST(bios_key_lookup) {
    using sysrecover::LookupBootMenuKey;
    // 客户机：HP laptop (redacted)（HP 全系 = F9）
    CHECK_EQ(LookupBootMenuKey(L"HP", L"8846", L"HP", L"HP laptop (redacted)"),
             L"F9");
    // 大小写不敏感 + 主板厂商命中（ASUSTEK → F8）
    CHECK_EQ(LookupBootMenuKey(L"ASUSTeK COMPUTER INC.", L"", L"", L""), L"F8");
    // 整机厂商/型号也能命中（主板串为空时）
    CHECK_EQ(LookupBootMenuKey(L"", L"", L"HP", L"HP Pavilion Gaming"), L"F9");
    // Dell 全系 F12、Lenovo F12 or Fn+F12、Samsung F10 or ESC
    CHECK_EQ(LookupBootMenuKey(L"Dell Inc.", L"0XXXXX", L"Dell Inc.",
                               L"Inspiron 3880"),
             L"F12");
    CHECK_EQ(LookupBootMenuKey(L"LENOVO", L"20QF", L"LENOVO", L"ThinkPad X1"),
             L"F12 or Fn+F12");
    CHECK_EQ(LookupBootMenuKey(L"SAMSUNG ELECTRONICS", L"", L"", L""),
             L"F10 or ESC");
    // Surface：表里 boot 为空（用音量键）→ 返回空 = 中性措辞
    CHECK(LookupBootMenuKey(L"Microsoft Corporation", L"Surface", L"", L"")
              .empty());
    // 完全未知 → 空
    CHECK(LookupBootMenuKey(L"", L"", L"", L"").empty());
    CHECK(LookupBootMenuKey(L"FooBar Tech", L"QQQ1234", L"", L"").empty());
    // vendor 回填（展示/诊断用）
    std::wstring ven;
    LookupBootMenuKey(L"ASRock", L"", L"", L"", &ven);
    CHECK_EQ(ven, L"ASRock");
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

// ── PIT-148：A/B 类失败 → 可「一键重试」；C 类（已动盘）不提供 ──
// 分类依据 rescue_decision.h 注释；单测守边界，防止有人往 A 类里塞
// mkntfs/apply（那会把"重试"变成在半成品盘上重来）。
TEST(rescue_step_retryable) {
    using sysrecover::RescueStepRetryable;
    // A/B 类（可一键重试）
    CHECK(RescueStepRetryable("no-task"));
    CHECK(RescueStepRetryable("contract-mismatch"));
    CHECK(RescueStepRetryable("target-not-found"));
    CHECK(RescueStepRetryable("caps-missing"));
    CHECK(RescueStepRetryable("image-not-found"));
    CHECK(RescueStepRetryable("unsupported-image"));
    CHECK(RescueStepRetryable("interrupted"));
    CHECK(RescueStepRetryable("swap-missing"));
    // C 类（不可）
    CHECK(!RescueStepRetryable("mkntfs-failed"));
    CHECK(!RescueStepRetryable("apply-failed"));
    CHECK(!RescueStepRetryable("swap-failed"));
    CHECK(!RescueStepRetryable("returnold-missing"));
    CHECK(!RescueStepRetryable("returnold-failed"));
    // 1019 案 W1/W2：镜像级确定性拒绝（未动盘，但重试同一镜像+引擎必复败）→ 不提供
    CHECK(!RescueStepRetryable("reparse-unsettable"));
    CHECK(!RescueStepRetryable("efs-encrypted"));
    // 未知 step（老回执/新脚本）：fail-closed 不提供一键重试
    CHECK(!RescueStepRetryable(""));
    CHECK(!RescueStepRetryable("done"));
    CHECK(!RescueStepRetryable("esp-failed"));
}

// ────────────── ea（EA 打包 / 组策略文件助手，PIT-122）──────────────
// PIT-135：非微软重解析点判定（bit31=0；微软 tag 均置位）——Linux 侧 apply
// 无法设置非微软 reparse（libntfs-3g bug），备份时须排除。
TEST(ea_non_ms_reparse_tag) {
    CHECK(ea::IsNonMsReparseTag(0x00000001));   // 假想 ISV tag
    CHECK(ea::IsNonMsReparseTag(0x40000002));   // bit30 置位但 bit31=0
    CHECK(!ea::IsNonMsReparseTag(0x80000017));  // WOF（微软）
    CHECK(!ea::IsNonMsReparseTag(0xA0000003));  // junction（微软）
    CHECK(!ea::IsNonMsReparseTag(0xA000000C));  // symlink（微软）
    CHECK(!ea::IsNonMsReparseTag(0x8000001B));  // APPEXECLINK（微软）
    CHECK(!ea::IsNonMsReparseTag(0x00000000));  // 0 = 无 tag，不算
}

// 1019 案 F1/F5：Linux 侧（wimlib NTFS-3G 块模式）写不了的重解析点判定
// （ntfs-3g valid_reparse_data 忠实移植 + 非微软显式并入）。
// blob==nullptr 路径 = 零误报子集（F5 镜像预检只用它在"只读元数据"下定论）。
TEST(ea_unsettable_reparse) {
    const unsigned long kDir = 0x10;
    const unsigned long kReparse = 0x400;
    const unsigned long kRecall = 0x40000;
    // 原始 FSCTL_GET_REPARSE_POINT 缓冲：[tag u32][datalen u16][reserved u16]
    // + payload（微软 tag 的 datalen 不含 8 字节头）。
    auto raw = [](unsigned long tag, const std::vector<unsigned char>& payload) {
        std::vector<unsigned char> b(8, 0);
        for (int i = 0; i < 4; ++i)
            b[i] = (unsigned char)((tag >> (8 * i)) & 0xFF);
        b[4] = (unsigned char)(payload.size() & 0xFF);
        b[5] = (unsigned char)((payload.size() >> 8) & 0xFF);
        b.insert(b.end(), payload.begin(), payload.end());
        return b;
    };

    // 非微软 tag：无论 blob 如何都不可写（PIT-135；wimlib 块模式写不过去）
    CHECK(ea::IsUnsettableReparse(0x00000001UL, kReparse, nullptr, 0));
    {
        auto b = raw(0x00000001UL, {0, 0, 0, 0});
        CHECK(ea::IsUnsettableReparse(0x00000001UL, kReparse, b.data(),
                                      b.size()));
    }

    // 无 blob 子集（F5 预检口径）：junction 落非目录 = 1019 案真凶形态；
    // 其余不确定项一律 false（零误报，宁可不拦）
    CHECK(ea::IsUnsettableReparse(0xA0000003UL, kReparse, nullptr, 0));
    CHECK(!ea::IsUnsettableReparse(0xA0000003UL, kReparse | kDir, nullptr, 0));
    CHECK(!ea::IsUnsettableReparse(0xA000000CUL, kReparse, nullptr, 0));
    CHECK(!ea::IsUnsettableReparse(0x80000017UL, kReparse, nullptr, 0));
    CHECK(!ea::IsUnsettableReparse(0UL, kReparse, nullptr, 0));

    // 有 blob：tag==0 → 拒；长度方程不成立 → 拒
    {
        auto b = raw(0UL, {});
        CHECK(ea::IsUnsettableReparse(0UL, kReparse, b.data(), b.size()));
    }
    {
        auto b = raw(0xA0000003UL, {});  // 实际 8 字节，声称 datalen=8
        b[4] = 8;
        b[5] = 0;
        CHECK(ea::IsUnsettableReparse(0xA0000003UL, kReparse | kDir, b.data(),
                                      b.size()));
    }

    // junction（有 blob）：合法（16B 目录）→ 放行；落非目录/越界/过短 → 拒
    {
        auto ok = raw(0xA0000003UL, {0, 0, 0, 0, 0, 0, 0, 0});
        CHECK(!ea::IsUnsettableReparse(0xA0000003UL, kReparse | kDir,
                                       ok.data(), ok.size()));
        CHECK(ea::IsUnsettableReparse(0xA0000003UL, kReparse, ok.data(),
                                      ok.size()));
        auto shortB = raw(0xA0000003UL, {});
        CHECK(ea::IsUnsettableReparse(0xA0000003UL, kReparse | kDir,
                                      shortB.data(), shortB.size()));
        auto outOfRange = raw(0xA0000003UL, {0, 0, 100, 0, 0, 0, 0, 0});
        CHECK(ea::IsUnsettableReparse(0xA0000003UL, kReparse | kDir,
                                      outOfRange.data(), outOfRange.size()));
    }

    // symlink：20B 合法 → 放行；过短 → 拒
    {
        auto ok = raw(0xA000000CUL, std::vector<unsigned char>(12, 0));
        CHECK(!ea::IsUnsettableReparse(0xA000000CUL, kReparse, ok.data(),
                                       ok.size()));
        auto shortB = raw(0xA000000CUL, {});
        CHECK(ea::IsUnsettableReparse(0xA000000CUL, kReparse, shortB.data(),
                                      shortB.size()));
    }

    // LX symlink（WSL）：datalen>4 且 type==2 才放行
    {
        auto good = raw(0xA000001DUL, {2, 0, 0, 0, 0, 0, 0, 0});
        CHECK(!ea::IsUnsettableReparse(0xA000001DUL, kReparse, good.data(),
                                       good.size()));
        auto small = raw(0xA000001DUL, {});
        CHECK(ea::IsUnsettableReparse(0xA000001DUL, kReparse, small.data(),
                                      small.size()));
        auto wrong = raw(0xA000001DUL, {1, 0, 0, 0, 0, 0, 0, 0});
        CHECK(ea::IsUnsettableReparse(0xA000001DUL, kReparse, wrong.data(),
                                      wrong.size()));
    }

    // AF_UNIX 族：datalen 必须 0 且 RECALL_ON_OPEN 必须在
    {
        auto b = raw(0x80000023UL, {});
        CHECK(!ea::IsUnsettableReparse(0x80000023UL, kReparse | kRecall,
                                       b.data(), b.size()));
        CHECK(ea::IsUnsettableReparse(0x80000023UL, kReparse, b.data(),
                                      b.size()));
        auto withData = raw(0x80000023UL, {1});
        CHECK(ea::IsUnsettableReparse(0x80000023UL, kReparse | kRecall,
                                      withData.data(), withData.size()));
    }

    // 其余微软 tag（WOF 等）→ 放行（不误伤）
    {
        auto b = raw(0x80000017UL, {1, 2, 3});
        CHECK(!ea::IsUnsettableReparse(0x80000017UL, kReparse, b.data(),
                                       b.size()));
    }
}

// PIT-136：reppack 打包/解析往返（内容 + reparse 原始数据二进制安全；截断/坏
// magic 必须拒绝——写回端拿坏包会装出坏文件）。
TEST(ea_repack_roundtrip) {
    std::vector<ea::ReparseEntry> in;
    ea::ReparseEntry a;
    a.relPath =
        L"\\Windows\\ServiceProfiles\\LocalService\\AppData\\Local\\Intel\\"
        L"ipfsrv\\ipfsrv.dptf";
    a.attrs = 0x20;
    a.mtime = 0x01DA123456789ABCull;
    a.content = {0x01, 0x02, 0x03};
    a.reparse = {0xA0, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xDE, 0xAD};
    in.push_back(a);
    ea::ReparseEntry b;
    b.relPath = L"\\x.dat";
    b.reparse = {0x00};
    in.push_back(b);
    auto bytes = ea::BuildRepack(in);
    std::vector<ea::ReparseEntry> out;
    CHECK(ea::ParseRepack(bytes, out));
    CHECK_EQ((int)out.size(), 2);
    CHECK(out[0].relPath == a.relPath);
    CHECK(out[0].attrs == a.attrs);
    CHECK(out[0].mtime == a.mtime);
    CHECK(out[0].content == a.content);
    CHECK(out[0].reparse == a.reparse);
    CHECK(out[1].relPath == b.relPath);
    CHECK(out[1].content.empty());
    auto cut = bytes;
    cut.resize(cut.size() - 3);
    std::vector<ea::ReparseEntry> out2;
    CHECK(!ea::ParseRepack(cut, out2));
    auto bad = bytes;
    bad[0] = 'X';
    std::vector<ea::ReparseEntry> out3;
    CHECK(!ea::ParseRepack(bad, out3));
}

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
    CHECK(IsVolatilePath(L"C:\\Users\\x\\AppData\\Local\\Temp\\"
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

// ────────────────────── swapleft（0.7 A 引擎残留决策表）──────────────────────
// 纯策略 DecideSwapLeftover（src/app/swapleft.cpp，对照 docs/23 §5）：
//   ~new：过期(≥24h)直接删 → 有 .zj-done 复用 → 无标记下次还原重做；
//   ~old：时间**永不**自动删除，只按结构判据（keepOld 标记 > 根残缺 >
//         同盘 ~new 仍在 > 结构完整=交换完成垃圾）。
TEST(swapleft_decision_table) {
    using sysrecover::DecideSwapLeftover;
    using sysrecover::SwapLeftAction;
    using sysrecover::SwapLeftInfo;

    // ---- ~new ----
    SwapLeftInfo n;
    n.isNew = true;
    n.ageHours = 48;
    n.hasMarker = true;  // 过期优先于标记：超期的完整 ~new 也删
    auto v = DecideSwapLeftover(n);
    CHECK(v.action == SwapLeftAction::Delete);
    CHECK_CONTAINS(v.reason, "expired");

    n.ageHours = 24;  // 边界（>=）
    CHECK(DecideSwapLeftover(n).action == SwapLeftAction::Delete);
    n.ageHours = 23;  // 边界内：标记在 → 复用
    CHECK(DecideSwapLeftover(n).action == SwapLeftAction::Reuse);

    n.ageHours = -1;  // 无法判定年龄 → 保守按"刚发生"，绝不因过期删
    CHECK(DecideSwapLeftover(n).action == SwapLeftAction::Reuse);
    n.hasMarker = false;  // 不完整（无 .zj-done）→ 下次还原重做
    v = DecideSwapLeftover(n);
    CHECK(v.action == SwapLeftAction::Redo);
    CHECK_CONTAINS(v.reason, "incomplete");

    n.ageHours = 2;
    n.hasMarker = true;  // 近期完整 → 复用免重解压
    v = DecideSwapLeftover(n);
    CHECK(v.action == SwapLeftAction::Reuse);
    CHECK_CONTAINS(v.reason, "complete");

    // 自定义 staleHours：12h 超过 10h 门槛 → 删
    n.ageHours = 12;
    CHECK(DecideSwapLeftover(n, 10).action == SwapLeftAction::Delete);
    CHECK(DecideSwapLeftover(n, 24).action == SwapLeftAction::Reuse);

    // ---- ~old（时间不作删除依据）----
    SwapLeftInfo o;  // isNew=false 默认
    o.rootIntact = true;
    o.rescueOk = true;
    v = DecideSwapLeftover(o);
    CHECK(v.action == SwapLeftAction::Delete);  // 结构完整 + ~new 已消化 → 交换完成垃圾
    CHECK_CONTAINS(v.reason, "receipt OK");

    o.rescueOk = false;  // 无回执同样删，但理由是"结构判据"
    v = DecideSwapLeftover(o);
    CHECK(v.action == SwapLeftAction::Delete);
    CHECK_CONTAINS(v.reason, "consumed");

    o.ageHours = 1000;  // 再老也不自动删 ~old（时间只对 ~new 生效）
    CHECK(DecideSwapLeftover(o).action == SwapLeftAction::Delete);

    o.siblingNew = true;  // 同盘 ~new 还在 → 交换中断态，问用户
    v = DecideSwapLeftover(o);
    CHECK(v.action == SwapLeftAction::AskUndo);
    CHECK_CONTAINS(v.reason, "sibling");

    o.siblingNew = false;
    o.rootIntact = false;  // 根下无 Windows → 可能是唯一副本，问用户
    v = DecideSwapLeftover(o);
    CHECK(v.action == SwapLeftAction::AskUndo);
    CHECK_CONTAINS(v.reason, "system dir missing");

    o.keepOldMarker = true;  // 备返标记优先级最高：哪怕根也残缺也只问不删
    v = DecideSwapLeftover(o);
    CHECK(v.action == SwapLeftAction::AskUndo);
    CHECK_CONTAINS(v.reason, "keep-old");

    o.rootIntact = true;  // 结构全绿 + keepOld → 仍 AskUndo（用户有意保留）
    CHECK(DecideSwapLeftover(o).action == SwapLeftAction::AskUndo);

    // 决策永不产生空理由（日志/排障可用性）
    CHECK(!v.reason.empty());
}

// ────────────────── partscript（新硬盘分区：分步 diskpart 脚本）──────────────────
// 用户 2026-10-27 规格：分区向导每一步都要在状态栏显示正在做什么。实现把一条
// 大脚本拆成**多次独立 diskpart 调用**（每步之前报一次阶段名）。单测守三条
// 属运维级的事实（真机踩过才写进来）：
//   ① 每一步都必须自带 `select disk N` —— 独立进程没有上下文，少这一行
//      diskpart 会拿"当前磁盘"（默认 disk 0，很可能就是系统盘）；
//   ② 顺序不能乱：clean 在 convert 之前、ESP 在 MSR 之前、系统盘在数据盘之前；
//   ③ MBR 的系统盘必须 `active`（缺了 → 还原完黑屏，PIT-159 实测教训）。
TEST(part_script_steps) {
    // ---- GPT + 数据盘：clean → convert → esp → msr → system → data ----
    auto g = BuildDiskpartSteps(true, 3, 300, 16, 100, true, L'D', L'E', L'F');
    CHECK_EQ(g.size(), static_cast<size_t>(6));
    CHECK_EQ(g[0].stage, std::string("partinit-clean"));
    CHECK_EQ(g[1].stage, std::string("partinit-convert"));
    CHECK_EQ(g[2].stage, std::string("partinit-esp"));
    CHECK_EQ(g[3].stage, std::string("partinit-msr"));
    CHECK_EQ(g[4].stage, std::string("partinit-system"));
    CHECK_EQ(g[5].stage, std::string("partinit-data"));
    for (const auto& s : g) {
        CHECK_CONTAINS(s.script, "select disk 3\n");
        CHECK(s.script.size() >= 5 &&
              s.script.compare(s.script.size() - 5, 5, "exit\n") == 0);
    }
    CHECK_CONTAINS(g[1].script, "convert gpt\n");
    CHECK_CONTAINS(g[2].script, "create partition efi size=300\n");
    CHECK_CONTAINS(g[2].script, "format quick fs=fat32 label=ESP\n");
    CHECK_CONTAINS(g[2].script, "assign letter=D\n");
    CHECK_CONTAINS(g[3].script, "create partition msr size=16\n");
    CHECK_CONTAINS(g[4].script, "create partition primary size=102400\n");
    CHECK_CONTAINS(g[4].script, "format quick fs=ntfs label=Windows\n");
    CHECK_CONTAINS(g[4].script, "assign letter=E\n");
    CHECK_NOT_CONTAINS(g[4].script, "active\n");  // GPT 不能 active
    CHECK_CONTAINS(g[5].script, "assign letter=F\n");
    // MSR：不格式化、不分配盘符（写了 format 会把保留分区弄坏）
    CHECK_NOT_CONTAINS(g[3].script, "format");
    CHECK_NOT_CONTAINS(g[3].script, "assign");

    // ---- MBR + 数据盘：clean → convert → system(active) → data ----
    auto m = BuildDiskpartSteps(false, 0, 300, 16, 60, true, 0, L'C', L'D');
    CHECK_EQ(m.size(), static_cast<size_t>(4));
    CHECK_EQ(m[0].stage, std::string("partinit-clean"));
    CHECK_EQ(m[1].stage, std::string("partinit-convert"));
    CHECK_EQ(m[2].stage, std::string("partinit-system"));
    CHECK_EQ(m[3].stage, std::string("partinit-data"));
    CHECK_CONTAINS(m[1].script, "convert mbr\n");
    CHECK_CONTAINS(m[2].script, "active\n");  // MBR 必须 active
    CHECK_CONTAINS(m[2].script, "assign letter=C\n");
    CHECK_NOT_CONTAINS(m[0].script, "convert");  // 清表与转换是两步

    // ---- 小盘（整盘只做系统盘）：不应出现数据盘这一步 ----
    auto small = BuildDiskpartSteps(false, 0, 300, 16, 60, false, 0, L'C', 0);
    CHECK_EQ(small.size(), static_cast<size_t>(3));
    CHECK_EQ(small.back().stage, std::string("partinit-system"));

    // ---- 盘符传 0 = 不分配（没有空闲盘符时不能写出空的 assign）----
    auto nol = BuildDiskpartSteps(true, 1, 300, 16, 100, true, 0, 0, 0);
    for (const auto& s : nol) CHECK_NOT_CONTAINS(s.script, "assign letter=");
    CHECK_CONTAINS(nol[2].script, "format quick fs=fat32 label=ESP\n");

    // ---- JoinDiskpartSteps：只用于日志留档，不能出现重复 select/exit ----
    const std::string joined = JoinDiskpartSteps(g, 3);
    CHECK_CONTAINS(joined, "select disk 3\n");
    CHECK_CONTAINS(joined, "clean\n");
    CHECK_CONTAINS(joined, "convert gpt\n");
    auto count = [](const std::string& hay, const std::string& needle) {
        size_t n = 0;
        size_t at = 0;
        while ((at = hay.find(needle, at)) != std::string::npos) {
            ++n;
            at += needle.size();
        }
        return n;
    };
    CHECK_EQ(count(joined, "select disk 3"), static_cast<size_t>(1));
    CHECK_EQ(count(joined, "exit"), static_cast<size_t>(1));
    CHECK(joined.find("clean\n") < joined.find("convert gpt\n"));
    CHECK(joined.find("convert gpt\n") < joined.find("create partition efi"));
    CHECK(joined.find("create partition msr") <
          joined.find("create partition primary size="));
}

// ───────────────────── 启动项写后读回校验（需求 4，2026-10-27） ─────────────────
// 用户实测：装了金山毒霸/360 一类底层拦截的机器上，bcdedit / BootNext 会"假成功"
// ——命令返回 0、实际没落盘，重启后照旧进原系统且毫无提示。写后读回校验靠这两个
// 纯逻辑函数，必须守三条：
//   ① 只认 `bootsequence` 这个 **ASCII 元素名**（大小写不敏感），不受标签本地化影响
//      （与 ParseBootmgrTimeout 的"不依赖标签语言"同款纪律，PIT-094）；
//   ② 值必须是 38 字符 `{...}` —— 缺值/残行/别元素的值都不算；
//   ③ GUID 比较大小写不敏感（bcdedit 回显大小写不保证）。
TEST(bcd_parse_bootsequence) {
    const std::string guid = "{12345678-1234-1234-1234-123456789abc}";
    // 英文输出：bootsequence 夹在其它元素中间
    std::string en =
        "Windows Boot Manager\r\n"
        "--------------------\r\n"
        "identifier              {bootmgr}\r\n"
        "device                  partition=\\Device\\HarddiskVolume1\r\n"
        "timeout                 1\r\n"
        "bootsequence            " +
        guid + "\r\n";
    CHECK_EQ(ParseBootsequence(en), guid);

    // 元素名大小写不敏感
    CHECK_EQ(ParseBootsequence("BOOTSEQUENCE   " + guid + "\n"), guid);
    // 行首制表符/多空格
    CHECK_EQ(ParseBootsequence("\t bootsequence\t" + guid + "\r\n"), guid);

    // 本地化标签不影响：伪本地化的"单次启动"不是 ASCII 元素名 → 空
    CHECK(ParseBootsequence("标识符                  {bootmgr}\n"
                            "单次启动                " +
                            guid + "\n")
              .empty());

    // 常态：还原完成/条目已清 → 没有 bootsequence
    CHECK(ParseBootsequence("identifier              {bootmgr}\r\n"
                            "timeout                 1\r\n")
              .empty());
    // 值形态不对：长度不足 / 空值
    CHECK(ParseBootsequence("bootsequence            {12345678-1234}\n").empty());
    CHECK(ParseBootsequence("bootsequence            \n").empty());
    // 别元素以 bootsequence 开头 → 必须按整 token 比较，不能前缀误命中
    CHECK(ParseBootsequence("bootsequenceX           " + guid + "\n").empty());
    // 续行干扰：bcdedit 不会折行，但别让残值骗过校验
    CHECK(ParseBootsequence("bootsequence\r\n" + guid + "\r\n").empty());
}

TEST(same_guid_ascii) {
    const std::wstring want = L"{12345678-1234-1234-1234-123456789ABC}";
    // 大小写混搭都算相同（bcdedit 回显大小写不保证）
    CHECK(SameGuidAscii("{12345678-1234-1234-1234-123456789abc}", want));
    CHECK(SameGuidAscii("{12345678-1234-1234-1234-123456789ABC}", want));
    CHECK(SameGuidAscii("{12345678-1234-1234-1234-123456789AbC}", want));
    // 长度不符：空 / 少一位 / 多一位
    CHECK(!SameGuidAscii("", want));
    CHECK(!SameGuidAscii("{12345678-1234-1234-1234-123456789ab}", want));
    CHECK(!SameGuidAscii("{12345678-1234-1234-1234-123456789abcd}", want));
    // 内容不同
    CHECK(!SameGuidAscii("{12345678-1234-1234-1234-123456789abd}", want));
}

// ── 自足引导（2026-10-27，用户反馈 2）：BCD 模板/占位替换 ────────────

TEST(bcd_patch_placeholder_guids) {
    // UEFI 模板里两处 16B 分区 GUID 占位要靠它替换（ESP 5 次 / 系统卷 10 次）。
    std::vector<unsigned char> buf(64, 0xAA);
    unsigned char from[16];
    unsigned char to[16];
    for (int i = 0; i < 16; ++i) {
        from[i] = (unsigned char)i;
        to[i] = (unsigned char)(15 - i);
    }
    memcpy(buf.data(), from, 16);
    memcpy(buf.data() + 16, from, 16);
    CHECK_EQ(PatchBcdBytes(buf, from, to), 2);
    CHECK(memcmp(buf.data(), to, 16) == 0);
    CHECK(memcmp(buf.data() + 16, to, 16) == 0);
    // 已替换完 -> 再跑一次 0 次（幂等）
    CHECK_EQ(PatchBcdBytes(buf, from, to), 0);
    // from == to -> 0（不误报）
    CHECK_EQ(PatchBcdBytes(buf, to, to), 0);
    // 缓冲不足以容纳一个 GUID -> 不越界、返回 0
    std::vector<unsigned char> tiny(from, from + 8);
    CHECK_EQ(PatchBcdBytes(tiny, from, to), 0);
}

TEST(bcd_file_looks_usable) {
    // 极简 PE 没有 bcdedit：BCD 只能做文件级判定（UTF-16LE 里有 winload 即算有
    // 条目）。这里验证判定本身，不依赖真实 BCD。
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"zj-bcd-unit";
    CreateDirectoryW(dir.c_str(), nullptr);
    auto write = [&](const wchar_t* name, const std::vector<unsigned char>& b) {
        std::wstring p = dir + L"\\" + name;
        HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return std::wstring();
        DWORD wr = 0;
        WriteFile(h, b.data(), (DWORD)b.size(), &wr, nullptr);
        CloseHandle(h);
        return p;
    };
    auto del = [](const std::wstring& p) {
        if (!p.empty()) DeleteFileW(p.c_str());
    };
    auto deldir = [&]() { RemoveDirectoryW(dir.c_str()); };

    std::string detail;
    // 正常：>=4096B 且含 UTF-16LE "winload"
    std::vector<unsigned char> good(8192, 0);
    const char needle[] = "w\0i\0n\0l\0o\0a\0d\0";
    memcpy(good.data() + 100, needle, sizeof(needle) - 1);
    std::wstring p1 = write(L"good.bcd", good);
    CHECK(BcdFileLooksUsable(p1, detail));
    // 纯 ASCII "winload"（不是 UTF-16LE）-> 不可用
    std::vector<unsigned char> narrow(8192, 0);
    memcpy(narrow.data() + 100, "winload", 7);
    std::wstring p2 = write(L"narrow.bcd", narrow);
    CHECK(!BcdFileLooksUsable(p2, detail));
    // 太小（<4096）-> 不可用
    std::vector<unsigned char> small(1024, 0);
    memcpy(small.data() + 100, needle, sizeof(needle) - 1);
    std::wstring p3 = write(L"small.bcd", small);
    CHECK(!BcdFileLooksUsable(p3, detail));
    // 文件不存在 -> 不可用
    CHECK(!BcdFileLooksUsable(dir + L"\\nope.bcd", detail));

    del(p1);
    del(p2);
    del(p3);
    deldir();
}

// ── 镜像加密容器 ZJENC1（docs/30）────────────────────────────────────────
namespace {
std::wstring CryptTempPath(const wchar_t* tag) {
    wchar_t dir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, dir);
    std::wstring p = dir;
    p += L"zjenc-";
    p += tag;
    p += L".bin";
    return p;
}
bool CryptWriteBytes(const std::wstring& path,
                     const std::vector<unsigned char>& v) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(v.data(), 1, v.size(), f) == v.size();
    fclose(f);
    return ok;
}
bool CryptReadBytes(const std::wstring& path, std::vector<unsigned char>& v) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    v.clear();
    unsigned char buf[65536];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        v.insert(v.end(), buf, buf + n);
    fclose(f);
    return true;
}
}  // namespace

// 加密→读元数据→解密→逐字节比对；错密码/篡改必须 fail-closed 且不留明文。
TEST(crypt_zje_roundtrip_meta_and_failclosed) {
    // 3MB + 12345 字节：跨 4 个块，最后一块不整（验证尾块 CTR 处理）
    std::vector<unsigned char> plain((size_t)3 * 1024 * 1024 + 12345);
    for (size_t i = 0; i < plain.size(); ++i)
        plain[i] = (unsigned char)((i * 131u + 7u) & 0xFF);
    const std::wstring src = CryptTempPath(L"src");
    const std::wstring enc = CryptTempPath(L"enc");
    const std::wstring dec = CryptTempPath(L"dec");
    DeleteFileW(src.c_str());
    DeleteFileW(enc.c_str());
    DeleteFileW(dec.c_str());
    CHECK(CryptWriteBytes(src, plain));

    std::vector<sysrecover::ZjeImage> imgs;
    sysrecover::ZjeImage im;
    im.index = 1;
    im.name = L"Backup";
    im.description = L"测试镜像";
    im.sizeBytes = 123456789;
    im.creationTime = 42;
    im.hasWinload = true;
    imgs.push_back(im);

    std::wstring err;
    CHECK_EQ(sysrecover::ZjeEncryptFile(src, enc, L"P@ss word", imgs, {}, err),
             0);
    CHECK(sysrecover::ZjeIsFile(enc));
    CHECK(!sysrecover::ZjeIsFile(src));

    sysrecover::ZjeInfo info;
    std::vector<sysrecover::ZjeImage> got;
    CHECK_EQ(sysrecover::ZjeReadMeta(enc, L"P@ss word", &info, &got, err), 0);
    CHECK_EQ(info.plainSize, (unsigned long long)plain.size());
    CHECK_EQ(got.size(), (size_t)1);
    CHECK_EQ(got[0].name, std::wstring(L"Backup"));
    CHECK_EQ(got[0].description, std::wstring(L"测试镜像"));
    CHECK(got[0].hasWinload);
    CHECK_EQ(got[0].sizeBytes, (unsigned long long)123456789);

    // 错密码：头 MAC 失败，且不产生明文文件
    CHECK(sysrecover::ZjeDecryptFile(enc, dec, L"wrong", {}, err) != 0);
    CHECK(GetFileAttributesW(dec.c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(!sysrecover::ZjeCheckPassword(enc, L"wrong"));
    CHECK(sysrecover::ZjeCheckPassword(enc, L"P@ss word"));

    // 正确密码：逐字节一致
    CHECK_EQ(sysrecover::ZjeDecryptFile(enc, dec, L"P@ss word", {}, err), 0);
    std::vector<unsigned char> back;
    CHECK(CryptReadBytes(dec, back));
    CHECK_EQ(back.size(), plain.size());
    CHECK(back == plain);

    // 篡改正文 1 字节 → 拒绝，且不留半截明文
    {
        HANDLE h = CreateFileW(enc.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER li;
            li.QuadPart = 4096;  // meta 区内（必然被校验覆盖）
            SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
            unsigned char b = 0;
            DWORD got2 = 0, wr = 0;
            if (ReadFile(h, &b, 1, &got2, nullptr) && got2 == 1) {
                b ^= 0x5A;
                SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
                WriteFile(h, &b, 1, &wr, nullptr);
            }
            CloseHandle(h);
        }
        DeleteFileW(dec.c_str());
        CHECK(sysrecover::ZjeDecryptFile(enc, dec, L"P@ss word", {}, err) != 0);
        CHECK(GetFileAttributesW(dec.c_str()) == INVALID_FILE_ATTRIBUTES);
    }

    DeleteFileW(src.c_str());
    DeleteFileW(enc.c_str());
    DeleteFileW(dec.c_str());
}
