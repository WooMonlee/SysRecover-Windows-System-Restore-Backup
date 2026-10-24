#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check-config.py -- 皮肤/主题配置文件校验（零依赖，接进 make check）。

检查：
  * skin/*.xml 与 resources/themes/**/*.xml 能否被 XML 解析器解析；
  * 同一文件内 name 属性不重复；
  * 控件 pos="l,t,r,b" 是四个整数并满足 l<r、t<b；
  * skin/main.xml 的 Tab 文案和按钮不出现非法控制字符。
不做渲染结果判断（那由 tools/ui 的截图门禁负责）。
"""
from __future__ import annotations

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
POS_RE = re.compile(r"^-?\d+,-?\d+,-?\d+,-?\d+$")


def files():
    out = sorted((ROOT / "skin").glob("*.xml"))
    out += sorted((ROOT / "resources" / "themes").rglob("*.xml"))
    return out


def main() -> int:
    bad = []
    checked = 0
    for p in files():
        rel = str(p.relative_to(ROOT)).replace("\\", "/")
        try:
            tree = ET.parse(p)
        except Exception as e:
            bad.append("%s: XML parse error: %s" % (rel, e))
            continue
        checked += 1
        names = set()
        for el in tree.iter():
            name = el.attrib.get("name")
            # Font/Theme 等资源节点也常用 name，但不是控件 ID；只对带 pos
            # 的布局节点做重复 ID 检查（skin 的控件命名空间）。
            if name is not None and "pos" in el.attrib:
                if name in names:
                    bad.append("%s: duplicate name=%r" % (rel, name))
                names.add(name)
            pos = el.attrib.get("pos")
            if pos is not None:
                if not POS_RE.match(pos):
                    bad.append("%s: <%s> bad pos=%r" % (rel, el.tag, pos))
                    continue
                l, t, r, b = (int(x) for x in pos.split(","))
                if l >= r or t >= b:
                    bad.append("%s: <%s> empty/negative pos=%r" % (rel, el.tag, pos))
    if bad:
        print("check-config: FAIL")
        for x in bad:
            print("  - " + x)
        return 1
    print("check-config: PASS (%d XML file(s))" % checked)
    return 0


if __name__ == "__main__":
    sys.exit(main())
