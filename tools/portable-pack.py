r"""鎶?dist/ 鎵撳寘鎴愬崟鏂囦欢锛圥IT-201 / PIT-213锛夈€?
**浠?0.9.92 璧凤紝鐪熸鐨勬墦鍖呯敱 exe 鑷繁鍋?*锛坄SysRecover.exe pack-self`锛夛細
瀹瑰櫒杞借嵎鐢?ntdll 鐨?XPRESS-Huffman 鍘嬬缉锛堢害 50MB 鈫?38MB锛夛紝Python 渚т笉鍐嶉噸澶?瀹炵幇涓€浠藉帇缂╁櫒 鈥斺€?涓ゅ瀹炵幇蹇呯劧婕傜Щ銆?
鐢ㄦ硶锛圡akefile 涓嶅彉锛夛細
  python tools/portable-pack.py <stub.exe> <out.exe> <distDir> [releaseDir]

  stub.exe  : 浠呬负鍏煎鏃у懡浠よ淇濈暀锛堝疄闄呯敤 <distDir>\bootfiles\portable-stub.exe锛?  out.exe   : 杈撳嚭璺緞
  distDir   : 瑕佹墦鍖呯殑鐩綍锛? 绋嬪簭鐨?AppDir锛?  releaseDir: 鍙€夌殑鍙戝竷鐩綍锛堟寜鐗堟湰鍙峰鍒朵竴浠斤紝濡?D:\涔濊浆澶ц繕鍘?.9.92.exe锛?"""
import json
import os
import shutil
import subprocess
import sys


def main():
    if len(sys.argv) not in (4, 5):
        print(__doc__)
        return 2
    stub, out, dist = sys.argv[1], sys.argv[2], sys.argv[3]
    release_dir = sys.argv[4] if len(sys.argv) == 5 else None

    exe = os.path.join(dist, 'x64', 'SysRecover.exe')
    if not os.path.exists(exe):
        exe = os.path.join(dist, 'SysRecover.exe')  # 32 浣嶅叆鍙ｏ紙鏃?x64 瀛愮洰褰曟椂锛?    if not os.path.exists(exe):
        print('portable-pack: SysRecover.exe not found under %s' % dist)
        return 1

    out_abs = os.path.abspath(out)
    r = subprocess.run([exe, 'pack-self', '--out', out_abs, '--stub',
                        os.path.abspath(stub)])
    if r.returncode != 0 or not os.path.exists(out_abs):
        print('portable-pack: pack-self failed (rc=%d)' % r.returncode)
        return 1
    size = os.path.getsize(out_abs)
    print('portable-pack: %s (%.1f MB)' % (out, size / 1048576.0))

    if release_dir:
        ver = '0.0.0'
        vj = os.path.join(dist, 'version.json')
        if os.path.exists(vj):
            with open(vj, 'r', encoding='utf-8') as f:
                ver = json.load(f).get('version', ver)
        zh = '\u4e5d\u8f6c\u5927\u8fd8\u539f%s.exe' % ver
        dst_zh = os.path.join(release_dir, zh)
        try:
            shutil.copyfile(out_abs, dst_zh)
            print('portable-pack: released -> %s' % dst_zh)
        except OSError as e:
            print('portable-pack: release copy FAILED: %s' % e)
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
