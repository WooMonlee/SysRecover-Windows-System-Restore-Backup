# -*- coding: utf-8 -*-
"""Fill empty EN translations in tools/i18n-en.py (PIT-148 one-click retry)."""
import importlib.util
import io
import re
import sys

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8")

spec = importlib.util.spec_from_file_location(
    "i18n_en", r"tools/i18n-en.py"
)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
EN = m.EN

tr = {
    "一键重试": "One-click Retry",
    "不支持的系统镜像": "Unsupported system image",
    "可点「一键重试」重新执行还原任务；或打开日志排查。":
        'Click "One-click Retry" to re-run the restore; or open the logs to investigate.',
    "将按上次任务参数重新执行还原（镜像→目标分区）。\n":
        "The restore will re-run with the previous task parameters (image -> target partition).\n",
    "确认继续？": "Continue?",
    "打开日志": "Open Logs",
    "救援环境缺少必需组件": "Rescue environment is missing required components",
    "新系统交换失败": "New-system swap failed",
    "新系统解压数据不完整": "New-system extract data is incomplete",
    "未找到旧系统数据": "Old-system data not found",
    "正在重试...": "Retrying...",
    "立即重试": "Retry Now",
    "返回旧系统失败": "Return to old system failed",
    "重试失败": "Retry failed",
    "重试失败（详见日志）": "Retry failed (see logs for details)",
    "重试失败：restore-task.conf 缺 image_path，请重新执行还原":
        "Retry failed: restore-task.conf is missing image_path; please run the restore again",
    "重试失败：restore-task.conf 缺 target_offset，请重新执行还原":
        "Retry failed: restore-task.conf is missing target_offset; please run the restore again",
    "重试失败：上次任务不是还原（":
        "Retry failed: the last task was not a restore (",
    "重试失败：按契约 offset/size 找不到目标分区（磁盘表已变化？）":
        "Retry failed: target partition not found by contract offset/size (disk layout changed?)",
    "重试失败：程序目录没有 restore-task.conf，请重新执行还原":
        "Retry failed: restore-task.conf not found next to the program; please run the restore again",
    "重试完成。重启后即可进入恢复的系统。":
        "Retry finished. Reboot to enter the restored system.",
    "重试完成，无需重启": "Retry finished; no reboot needed",
    "重试暂存完成，正在重启...":
        "Retry staged successfully; rebooting now...",
    "（可在图形界面选择「一键重试」，或重新执行 restore）":
        ' (Use "One-click Retry" in the GUI, or run restore again.)',
    "），请重新执行「返回旧系统」":
        '); please run "Return to old system" again',
}

missing = [k for k in tr if k not in EN]
if missing:
    print("MISSING KEYS:")
    for k in missing:
        print("  ", k)
    raise SystemExit(1)

for k, v in tr.items():
    EN[k] = v

path = r"tools/i18n-en.py"
src = open(path, encoding="utf-8").read()
start = src.index("EN = {")
depth = 0
end = None
for i, ch in enumerate(src[start:], start):
    if ch == "{":
        depth += 1
    elif ch == "}":
        depth -= 1
        if depth == 0:
            end = i + 1
            break
if end is None:
    print("ERROR: cannot find end of EN dict")
    raise SystemExit(1)

lines = ["EN = {"]
for k in sorted(EN.keys()):
    lines.append("    %r: %r," % (k, EN[k]))
lines.append("}")
new_block = "\n".join(lines)
src2 = src[:start] + new_block + src[end:]
translated = sum(1 for v in EN.values() if str(v).strip())
total = len(EN)
src2 = re.sub(
    r"# 进度：\d+/\d+ 已译",
    "# 进度：%d/%d 已译" % (translated, total),
    src2,
)
open(path, "w", encoding="utf-8", newline="\n").write(src2)
print("OK: wrote %d keys, %d translated" % (total, translated))
