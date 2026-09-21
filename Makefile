# SysRecover - MinGW 构建（无 CMake）
# 工具链：D:\Prog\ProgIDE\mingw64 (GCC 14.2.0, x86_64-w64-mingw32)
# 用法（PowerShell）：
#   $env:Path = "D:\Prog\ProgIDE\mingw64\bin;" + $env:Path
#   mingw32-make -f Makefile all       # CLI + GUI 默认目标
#   mingw32-make -f Makefile cli       # 仅 CLI
#   mingw32-make -f Makefile gui       # 仅 GUI（含 duilib 静态库）
#   mingw32-make -f Makefile clean     # 清理
#   mingw32-make -f Makefile package   # 构建 + 部署 bootfiles/皮肤 + version.json

CXX      = g++
CC       = gcc
AR       = ar
# 构建脚本用的 Python（PATH 上的 python 可能是 Microsoft Store 占位符，不可用）
PYTHON   ?= D:/Prog/ProgIDE/Python/Python313/python.exe
# 版本号唯一来源 = src/common/version.h（用 tools/version.py 读写；每个问题修完 --bump）
VERSION  := $(shell $(PYTHON) tools/version.py)
CXXFLAGS = -O2 -std=c++17 -Wall -Wextra -D_WIN32_WINNT=0x0601 -DUNICODE -D_UNICODE
INCLUDES = -Ithird_party/wimlib -Isrc
LDFLAGS  = -static -mconsole
LDLIBS   = -Lthird_party/wimlib -l:libwim-15.dll -ladvapi32 -lole32 -lshell32 -luuid

# ---- 应用模块静态库（CLI 与 GUI 共用，避免双份编译 ODR 问题） ----
APP_SRC = src/disk/disk.cpp src/wim/wim.cpp src/wim/exclude.cpp \
      src/common/process.cpp src/common/logger.cpp src/common/progress.cpp \
      src/common/singleton.cpp src/common/sysinfo.cpp src/boot/bcd.cpp src/boot/grub.cpp \
      src/boot/uefi.cpp src/boot/task.cpp src/boot/bootfix.cpp \
      src/app/safety.cpp \
      src/app/shortcut.cpp src/app/ops.cpp
APP_OBJS = $(patsubst src/%.cpp,build/app/%.o,$(APP_SRC))
APP_LIB  = build/libapp.a

# ---- CLI（console） ----
CLI_MAIN = src/cli/main.cpp
CLI_OUT = dist/SysRecover.exe
# 提权清单（requireAdministrator）：与 GUI 同法，见本文件 GUI 段的说明与 PIT-018。
CLI_RC     = src/cli/SysRecover.rc
CLI_RC_OBJ = build/SysRecover_rc.o

# ---- Duilib 静态库（经典 Duilib，MIT；见 AGENTS.md PIT-012；源清单 build/duilib.mk 自动生成） ----
DUI_ROOT  = third_party/duilib-master/DuiLib
DUI_FLAGS = -std=c++17 -O1 -fpermissive -DUNICODE -D_UNICODE -DWIN32 -D_WIN32_WINNT=0x0601 -D_stdcall=__stdcall -DUILIB_STATIC -I$(DUI_ROOT) -I$(DUI_ROOT)/Control -I$(DUI_ROOT)/Core -I$(DUI_ROOT)/Layout -I$(DUI_ROOT)/Utils
CXX_DUI = $(CXX) $(DUI_FLAGS)
CC_DUI  = $(CC) -O1 -DUNICODE -D_UNICODE -DWIN32 -D_WIN32_WINNT=0x0601 -D_stdcall=__stdcall -DUILIB_STATIC -I$(DUI_ROOT)
DUI_LIB = build/libduilib.a
include build/duilib.mk

# ---- GUI（windows，经典 Duilib） ----
GUI_SRC = src/gui/main_win.cpp src/gui/main_form.cpp src/gui/ui_skin.cpp src/gui/instance_dlg.cpp src/gui/confirm_dlg.cpp
GUI_OUT = dist/SysRecoverUI.exe
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
WINDRES    = windres
GUI_RC     = src/gui/SysRecoverUI.rc
# 输出到 build/ 根（该目录必然已存在——build/duilib.mk 就在里面并被 include），
# 这样规则里不需要 mkdir，也就不依赖 shell 是 cmd 还是 sh（既有规则里的
# "if not exist ..." 是 cmd 语法，在 sh 下会语法错误，别再仿写）。
GUI_RC_OBJ = build/SysRecoverUI_rc.o

all: cli gui

cli: $(CLI_OUT)

build/app/%.o: src/%.cpp
	@if not exist build\app\$(subst /,\,$(dir $*)) mkdir build\app\$(subst /,\,$(dir $*))
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

# 头文件依赖（-MMD 生成的 .d）：改了 src/common/version.h 等头文件后能自动重编
# 受影响的对象，否则「改了版本号、编译出来还是旧号」（版本号规则的隐形坑）。
-include $(APP_OBJS:.o=.d)

$(APP_LIB): $(APP_OBJS)
	$(AR) rcs $@ $(APP_OBJS)

$(CLI_RC_OBJ): $(CLI_RC) src/cli/SysRecover.manifest
	$(WINDRES) -I src/cli $< -o $@

$(CLI_OUT): $(CLI_MAIN) $(CLI_RC_OBJ) $(APP_LIB) third_party/wimlib/libwim-15.dll src/common/version.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(CLI_MAIN) $(CLI_RC_OBJ) $(APP_LIB) -o $(CLI_OUT) $(LDFLAGS) $(LDLIBS)

build/duilib:
	@if not exist build\duilib mkdir build\duilib

$(DUI_OBJS): | build/duilib

$(DUI_LIB): $(DUI_OBJS)
	$(AR) rcs $@ $(DUI_OBJS)

gui: $(DUI_LIB) $(APP_LIB) $(GUI_OUT)

$(GUI_RC_OBJ): $(GUI_RC) src/gui/SysRecoverUI.manifest
	$(WINDRES) -I src/gui $< -o $@

$(GUI_OUT): $(GUI_SRC) $(GUI_RC_OBJ) $(DUI_LIB) $(APP_LIB) src/common/version.h
	$(CXX) $(GUI_FLAGS) $(GUI_INCLUDES) $(GUI_SRC) $(GUI_RC_OBJ) $(APP_LIB) $(DUI_LIB) -o $(GUI_OUT) -static -mwindows $(GUI_LDLIBS) -Lthird_party/wimlib -l:libwim-15.dll

clean:
	-del /Q dist\SysRecover.exe dist\SysRecoverUI.exe 2>nul
	-del /Q build\duilib\*.o build\libduilib.a 2>nul
	-del /S /Q build\app\*.o build\libapp.a 2>nul
	-del /Q build\SysRecoverUI_rc.o build\SysRecover_rc.o 2>nul

package: all
	@copy /Y third_party\wimlib\libwim-15.dll dist\ >nul
	@if not exist dist\bootfiles mkdir dist\bootfiles
	@copy /Y bootfiles\grldr dist\bootfiles\ >nul
	@copy /Y bootfiles\grldr.mbr dist\bootfiles\ >nul
	@copy /Y bootfiles\vmlinuz-zjrestore dist\bootfiles\ >nul
	@copy /Y bootfiles\initramfs-zjrestore.cpio.gz dist\bootfiles\ >nul
	@copy /Y bootfiles\zjrestore-lite.sh dist\bootfiles\ >nul
	@if not exist dist\bootfiles\sb mkdir dist\bootfiles\sb
	@copy /Y bootfiles\sb\grub-ubuntu.efi dist\bootfiles\sb\ >nul
	@copy /Y bootfiles\sb\grub.cfg dist\bootfiles\sb\ >nul
	@copy /Y THIRD_PARTY_LICENSES.txt dist\ >nul
	@if exist third_party\ucrt\x64\*.dll copy /Y third_party\ucrt\x64\*.dll dist\ >nul
	@if not exist dist\resources\themes\default\main mkdir dist\resources\themes\default\main
	@copy /Y resources\themes\default\global.xml dist\resources\themes\default\ >nul
	@copy /Y resources\themes\default\main\main.xml dist\resources\themes\default\main\ >nul
	@if not exist dist\skin mkdir dist\skin
	@copy /Y skin\main.xml dist\skin\ >nul
	@copy /Y skin\instance.xml dist\skin\ >nul
	@copy /Y skin\confirm.xml dist\skin\ >nul
	@echo {"name":"SysRecover","version":"$(VERSION)"} > dist\version.json
	@dir dist\SysRecover.exe dist\SysRecoverUI.exe dist\libwim-15.dll

.PHONY: all cli gui clean package
