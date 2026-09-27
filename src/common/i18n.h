#pragma once
// ── i18n：多语言运行时（PLAN §14 M2）────────────────────────────────────
//
// 设计：**源语言 = 中文** —— 源码字面量本身就是译文的 key（键即原文）：
//   · 中文界面（zh）不查表，直接用源文本（天然回退，永远不会“掉 key”）；
//   · 其它语言从 <root>\lang\<tag>.lang（主词典）与 <root>\lang\en.lang（通用回退）
//     载入 UTF-8 `key=value` 表；查不到 → 回退源文本（局部回退，不崩界面）。
//   · 皮肤 XML 的 text 属性在加载时按同一张词典翻译（见 LoadSkinXml）。
//
// 词典文件（lang/<tag>.lang）：
//   · UTF-8，可带 BOM；每行 `key=value`（**首个未转义的 `=`** 分隔）；`#` 行 = 注释；
//   · key/value 内的换行/制表/反斜杠/等号写成 \n \r \t \\ \=（装载时还原）——
//     等号必须转义：`备份失败(rc=%d): ` 这类词条的**键**本身含 `=`，
//     不转义会被解析器从中间截断。生成端见 tools/i18n-wrap.py::lang_escape。
//
// 红线（AGENTS 国际化纪律）：只翻译面向用户的字符串；日志（Log*/history/progress）、
// 跨层契约（restore-task.conf/_zjresy*.log/progress.json）、救援层屏幕一律不翻。
#include <string>

namespace sysrecover {

// 进程启动时调一次（CLI: main 解析 --lang 之后；GUI: WinMain 尽早处）。
// forced 为空 →环境变量 SYSRECOVER_LANG → GetUserDefaultUILanguage
// （主语言 0x04 = 中文族 → "zh-CN" 源语言，否则 "en"）。
void InitI18n(const char* forced);

const char* LangTag();   // "zh-CN" / "en" / "ja"…（进程内稳定，不会悬空）
bool IsSourceLang();     // true = 中文界面（无需翻译）

// 取译文。窄串 = UTF-8，宽串 = UTF-16。返回内部存储的指针 ——
// 在下一次 InitI18n/ClearI18n 之前稳定（std::unordered_map 节点地址不受 rehash 影响）。
const char* Tr(const char* src);
const wchar_t* Tr(const wchar_t* src);

// 皮肤 XML：读 <root>\skin\<skinFileName>（UTF-8），按词典把引号包裹的属性值
// 翻成宽字符 XML 串（以 '<' 开头 —— Duilib CDialogBuilder 走内存解析分支，
// 见 third_party/duilib-master/DuiLib/Core/UIDlgBuilder.cpp:17）。
// 读失败返回空串（调用方回退原文件名，让 Duilib 自己报它熟悉的错）。
std::wstring LoadSkinXml(const wchar_t* skinFileName);

// 测试/工具钩子：清空词典；按 .lang 语法直接装载一段文本（覆盖已有键）。
void ClearI18n();
void LoadLangText(const std::string& langText);

}  // namespace sysrecover
