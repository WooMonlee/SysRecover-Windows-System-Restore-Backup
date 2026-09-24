# SysRecover - MinGW 构建（无 CMake）
# 工具链：x64 = D:\Prog\ProgIDE\mingw64 (GCC 14.2.0, x86_64-w64-mingw32, UCRT)
#         x86 = D:\Prog\ProgIDE\mingw32 (GCC 14.2.0, i686-w64-mingw32,  UCRT)
# 用法（PowerShell）：
#   $env:Path = "D:\Prog\ProgIDE\mingw64\bin;" + $env:Path
#   mingw32-make -f Makefile package        # ★ 双架构发布包 → dist/（启动器 + x86/ + x64/ + 共享）
#   mingw32-make -f Makefile all            # 仅 x64 构建 → dist/x64
#   mingw32-make -f Makefile ARCH=x86 all   # 仅 x86 构建 → dist/x86（需 i686 工具链）
#   mingw32-make -f Makefile check          # 单元测试（纯逻辑，零依赖）
#   mingw32-make -f Makefile clean          # 清理（当前 ARCH）
#
# 架构设计（2026-09-24）：Windows 侧位数**跟随系统**（32 位系统跑 x86、64 位系统跑 x64，
# 主要为备份压缩速度）；发布包根目录放 **x86 启动器**（`SysRecover.exe`/`SysRecoverUI.exe`，
# 无第三方依赖），由它按系统位数调用 `x86\` 或 `x64\` 下的真程序。**Linux 救援层固定 x86_64**
# （与宿主位数无关）。见 docs/08 §0 与 src/launcher/launcher.cpp。

ARCH ?= x64
ifeq ($(ARCH),x86)
  MINGW    = D:/Prog/ProgIDE/mingw32
  CXX      = $(MINGW)/bin/i686-w64-mingw32-g++
  CC       = $(MINGW)/bin/i686-w64-mingw32-gcc
  AR       = $(MINGW)/bin/ar.exe
  WINDRES  = $(MINGW)/bin/windres.exe
  OBJDIR   = build-x86
  DISTDIR  = dist/x86
  WIMLIB   = third_party/wimlib/x86
  UCRT     = third_party/ucrt/x86
else
  CXX      = g++
  CC       = gcc
  AR       = ar
  WINDRES  = windres
  OBJDIR   = build
  DISTDIR  = dist/x64
  WIMLIB   = third_party/wimlib
  UCRT     = third_party/ucrt/x64
endif

# 启动器**始终 x86**（要能在 32 位系统上跑），独立于 ARCH；只用 kernel32/shell32。
MINGW32    ?= D:/Prog/ProgIDE/mingw32
LAUNCH_CXX = $(MINGW32)/bin/i686-w64-mingw32-g++
LAUNCH_OBJ = build/launcher.o
# x86 的对象目录（固定名，供双架构 package 在任意 ARCH 下引用 x86 的清单资源对象）
OBJDIR_X86 = build-x86

# 构建脚本用的 Python（PATH 上的 python 可能是 Microsoft Store 占位符，不可用）
PYTHON   ?= D:/Prog/ProgIDE/Python/Python313/python.exe
# 版本号唯一来源 = src/common/version.h（用 tools/version.py 读写；每个问题修完 --bump）
VERSION  := $(shell $(PYTHON) tools/version.py)
CXXFLAGS = -O2 -std=c++17 -Wall -Wextra -D_WIN32_WINNT=0x0601 -DUNICODE -D_UNICODE
INCLUDES = -Ithird_party/wimlib -Isrc
LDFLAGS  = -static -mconsole
LDLIBS   = -L$(WIMLIB) -l:libwim-15.dll -ladvapi32 -lole32 -lshell32 -luuid

# ---- 应用模块静态库（CLI 与 GUI 共用，避免双份编译 ODR 问题） ----
APP_SRC = src/disk/disk.cpp src/wim/wim.cpp src/wim/exclude.cpp \
      src/common/process.cpp src/common/logger.cpp src/common/progress.cpp \
      src/common/singleton.cpp src/common/sysinfo.cpp src/common/zip.cpp src/boot/bcd.cpp src/boot/grub.cpp \
      src/boot/uefi.cpp src/boot/task.cpp src/boot/bootfix.cpp \
      src/app/safety.cpp \
      src/app/shortcut.cpp src/app/ops.cpp
APP_OBJS = $(patsubst src/%.cpp,$(OBJDIR)/app/%.o,$(APP_SRC))
APP_LIB  = $(OBJDIR)/libapp.a

# ---- CLI（console） ----
CLI_MAIN = src/cli/main.cpp
CLI_OUT = $(DISTDIR)/SysRecover.exe
# 提权清单（requireAdministrator）：与 GUI 同法，见本文件 GUI 段的说明与 PIT-018。
CLI_RC     = src/cli/SysRecover.rc
CLI_RC_OBJ = $(OBJDIR)/SysRecover_rc.o

# ---- Duilib 静态库（经典 Duilib，MIT；见 AGENTS.md PIT-012；源清单 build/duilib.mk 自动生成） ----
DUI_ROOT  = third_party/duilib-master/DuiLib
DUI_FLAGS = -std=c++17 -O1 -fpermissive -DUNICODE -D_UNICODE -DWIN32 -D_WIN32_WINNT=0x0601 -D_stdcall=__stdcall -DUILIB_STATIC -I$(DUI_ROOT) -I$(DUI_ROOT)/Control -I$(DUI_ROOT)/Core -I$(DUI_ROOT)/Layout -I$(DUI_ROOT)/Utils
CXX_DUI = $(CXX) $(DUI_FLAGS)
CC_DUI  = $(CC) -O1 -DUNICODE -D_UNICODE -DWIN32 -D_WIN32_WINNT=0x0601 -D_stdcall=__stdcall -DUILIB_STATIC -I$(DUI_ROOT)
DUI_LIB = $(OBJDIR)/libduilib.a
include build/duilib.mk

# ---- GUI（windows，经典 Duilib） ----
GUI_SRC = src/gui/main_win.cpp src/gui/main_form.cpp src/gui/ui_skin.cpp src/gui/instance_dlg.cpp src/gui/confirm_dlg.cpp
GUI_OUT = $(DISTDIR)/SysRecoverUI.exe
GUI_INCLUDES = -Isrc -I$(DUI_ROOT) -I$(DUI_ROOT)/Control -I$(DUI_ROOT)/Core -I$(DUI_ROOT)/Layout -I$(DUI_ROOT)/Utils
GUI_FLAGS = -std=c++17 -O1 -fpermissive -DUNICODE -D_UNICODE -DWIN32 -D_WIN32_WINNT=0x0601 -D_stdcall=__stdcall -DUILIB_STATIC
GUI_LDLIBS = -lgdi32 -lcomctl32 -limm32 -lole32 -luuid -lmsimg32 -lshlwapi -luxtheme -ldwmapi -lwinmm -lgdiplus -loleaut32 -ladvapi32 -lshell32

# ---- 提权清单（requireAdministrator）：windres 编成 .o 再链入 GUI ----
# 备份/还原核心功能（枚举分区、写 BCD/引导、wimlib 挂载）全需管理员权限，
# 故让 exe 启动即请求提权：双击时由加载器弹一次 UAC，无需「右键→以管理员身份运行」。
# manifest 方式不改变工作目录（不像 ShellExecute runas 重启进程会把 CWD 变成
# System32），因此 skin\ 与 libwim-15.dll 的相对定位不受影响。
# 只加给 GUI；CLI 保持 asInvoker —— 命令行/脚本调用不该被 UAC 打断，
# 且 src/cli/main.cpp::IsAdmin() 已会自行检测并给出中文提示。
GUI_RC     = src/gui/SysRecoverUI.rc
# 输出到 $(OBJDIR) 根（该目录必然已存在——build/duilib.mk 就在里面并被 include），
# 这样规则里不需要 mkdir，也就不依赖 shell 是 cmd 还是 sh（既有规则里的
# "if not exist ..." 是 cmd 语法，在 sh 下会语法错误，别再仿写）。
GUI_RC_OBJ = $(OBJDIR)/SysRecoverUI_rc.o

all: cli gui
	@copy /Y $(subst /,\,$(WIMLIB))\libwim-15.dll $(subst /,\,$(DISTDIR))\ >nul
	@if exist $(subst /,\,$(UCRT))\*.dll copy /Y $(subst /,\,$(UCRT))\*.dll $(subst /,\,$(DISTDIR))\ >nul

cli: $(CLI_OUT)

# 独立输出目录（cmd 的 mkdir 会自动建中间目录，故 dist\x64 可直接建）
$(OBJDIR):
	@if not exist $(subst /,\,$(OBJDIR)) mkdir $(subst /,\,$(OBJDIR))

$(DISTDIR):
	@if not exist $(subst /,\,$(DISTDIR)) mkdir $(subst /,\,$(DISTDIR))

$(OBJDIR)/app/%.o: src/%.cpp
	@if not exist $(subst /,\,$(OBJDIR))\app\$(subst /,\,$(dir $*)) mkdir $(subst /,\,$(OBJDIR))\app\$(subst /,\,$(dir $*))
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

# 头文件依赖（-MMD 生成的 .d）：改了 src/common/version.h 等头文件后能自动重编
# 受影响的对象，否则「改了版本号、编译出来还是旧号」（版本号规则的隐形坑）。
-include $(APP_OBJS:.o=.d)

$(APP_LIB): $(APP_OBJS)
	$(AR) rcs $@ $(APP_OBJS)

$(CLI_RC_OBJ): $(CLI_RC) src/cli/SysRecover.manifest | $(OBJDIR)
	$(WINDRES) -I src/cli $< -o $@

$(CLI_OUT): $(CLI_MAIN) $(CLI_RC_OBJ) $(APP_LIB) $(WIMLIB)/libwim-15.dll src/common/version.h | $(DISTDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(CLI_MAIN) $(CLI_RC_OBJ) $(APP_LIB) -o $(CLI_OUT) $(LDFLAGS) $(LDLIBS)

$(OBJDIR)/duilib:
	@if not exist $(subst /,\,$(OBJDIR))\duilib mkdir $(subst /,\,$(OBJDIR))\duilib

$(DUI_OBJS): | $(OBJDIR)/duilib

$(DUI_LIB): $(DUI_OBJS)
	$(AR) rcs $@ $(DUI_OBJS)

gui: $(DUI_LIB) $(APP_LIB) $(GUI_OUT)

$(GUI_RC_OBJ): $(GUI_RC) src/gui/SysRecoverUI.manifest | $(OBJDIR)
	$(WINDRES) -I src/gui $< -o $@

$(GUI_OUT): $(GUI_SRC) $(GUI_RC_OBJ) $(DUI_LIB) $(APP_LIB) src/common/version.h | $(DISTDIR)
	$(CXX) $(GUI_FLAGS) $(GUI_INCLUDES) $(GUI_SRC) $(GUI_RC_OBJ) $(APP_LIB) $(DUI_LIB) -o $(GUI_OUT) -static -mwindows $(GUI_LDLIBS) -L$(WIMLIB) -l:libwim-15.dll

clean:
	-del /Q $(subst /,\,$(DISTDIR))\SysRecover.exe $(subst /,\,$(DISTDIR))\SysRecoverUI.exe 2>nul
	-del /S /Q $(subst /,\,$(OBJDIR)) 2>nul

# ---- 单元测试（零依赖，纯逻辑；不链 duilib/wimlib，跑得快） ----
# 只编译"被抽出来的纯逻辑"单元（见各头文件注释里的 refactor 说明）。
TEST_SRC   = tests/tiny_test.cpp tests/unit_tests.cpp tests/main.cpp
TEST_UNITS = src/common/sysinfo.cpp src/wim/exclude.cpp src/boot/task.cpp src/common/zip.cpp
TEST_BIN   = $(OBJDIR)/tests.exe

check: $(TEST_BIN)
	$(TEST_BIN)
	$(PYTHON) tests/test_version.py

$(TEST_BIN): $(TEST_SRC) $(TEST_UNITS) src/common/version.h | $(OBJDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(TEST_SRC) $(TEST_UNITS) -o $(TEST_BIN) -static -mconsole -ladvapi32 -lole32 -luuid

# ---- ★ 双架构发布包：启动器 + x86/ + x64/ + 与位数无关的共享资源 ----
# 需要**两套工具链**：x64 走 PATH 上的 g++（请先把 mingw64\bin 加到 PATH），x86/启动器走绝对路径。
package:
	@echo === [1/4] 构建 x64 ===
	$(MAKE) -f Makefile ARCH=x64 all
	@echo === [2/4] 构建 x86 ===
	$(MAKE) -f Makefile ARCH=x86 all
	@echo === [3/4] 构建启动器（x86，通吃）===
	$(LAUNCH_CXX) -O2 -std=c++17 -Wall -Wextra -D_WIN32_WINNT=0x0601 -DUNICODE -D_UNICODE -c src/launcher/launcher.cpp -o $(LAUNCH_OBJ)
# 启动器**必须**嵌 requireAdministrator 清单：真程序是 requireAdministrator，非提权进程用
# CreateProcess 启动它会失败(ERROR_ELEVATION_REQUIRED=740)（只有 ShellExecute runas 才会自动提权）。
# 复用既有的清单资源对象（x86 构建已产出）：CLI=SysRecover_rc.o、GUI=SysRecoverUI_rc.o。
	$(LAUNCH_CXX) $(LAUNCH_OBJ) $(OBJDIR_X86)/SysRecover_rc.o   -o dist/SysRecover.exe   -static -s -mconsole -lshell32
	$(LAUNCH_CXX) $(LAUNCH_OBJ) $(OBJDIR_X86)/SysRecoverUI_rc.o -o dist/SysRecoverUI.exe -static -s -mwindows -lshell32
	@echo === [4/4] 组装 dist/ ===
	@if not exist dist\bootfiles mkdir dist\bootfiles
	@copy /Y bootfiles\grldr dist\bootfiles\ >nul
	@copy /Y bootfiles\grldr.mbr dist\bootfiles\ >nul
	@copy /Y bootfiles\vmlinuz-zjrestore dist\bootfiles\ >nul
	@copy /Y bootfiles\initramfs-zjrestore.cpio.gz dist\bootfiles\ >nul
	@copy /Y bootfiles\zjrestore-lite.sh dist\bootfiles\ >nul
	@if not exist dist\bootfiles\sb mkdir dist\bootfiles\sb
# package 只增不删 → 换链时旧资产会残留（grub-ubuntu.efi 曾与 grubx64.efi 并存）。
# 注意：这里必须用 make 的 `#` 注释；命令行注释 `::` 在「单独一条 cmd /c」下不是合法命令。
	@if exist dist\bootfiles\sb\grub-ubuntu.efi del /Q dist\bootfiles\sb\grub-ubuntu.efi >nul
	@copy /Y bootfiles\sb\shimx64.efi dist\bootfiles\sb\ >nul
	@copy /Y bootfiles\sb\grubx64.efi dist\bootfiles\sb\ >nul
	@copy /Y bootfiles\sb\grub.cfg dist\bootfiles\sb\ >nul
	@copy /Y THIRD_PARTY_LICENSES.txt dist\ >nul
# 启动器本身也要 UCRT（Win7），故在**根目录**再放一套 x86 UCRT（启动器是 x86）
	@if exist third_party\ucrt\x86\*.dll copy /Y third_party\ucrt\x86\*.dll dist\ >nul
	@if not exist dist\resources\themes\default\main mkdir dist\resources\themes\default\main
	@copy /Y resources\themes\default\global.xml dist\resources\themes\default\ >nul
	@copy /Y resources\themes\default\main\main.xml dist\resources\themes\default\main\ >nul
	@if not exist dist\skin mkdir dist\skin
	@copy /Y skin\main.xml dist\skin\ >nul
	@copy /Y skin\instance.xml dist\skin\ >nul
	@copy /Y skin\confirm.xml dist\skin\ >nul
	@echo {"name":"SysRecover","version":"$(VERSION)","arch":"x86+x64"} > dist\version.json
	@dir dist\SysRecover.exe dist\SysRecoverUI.exe dist\x86\SysRecover.exe dist\x64\SysRecover.exe

.PHONY: all cli gui clean package check
