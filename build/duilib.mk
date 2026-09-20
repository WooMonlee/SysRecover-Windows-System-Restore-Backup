# ??????? Duilib MinGW ?????? AGENTS.md PIT-012?
# ???UIFlash/UIWebBrowser?? ATL???? 35 ???

build/duilib/f0.o: third_party/duilib-master/DuiLib/Control/UIActiveX.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f1.o: third_party/duilib-master/DuiLib/Control/UIButton.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f2.o: third_party/duilib-master/DuiLib/Control/UICheckBox.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f3.o: third_party/duilib-master/DuiLib/Control/UICombo.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f4.o: third_party/duilib-master/DuiLib/Control/UIDateTime.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f5.o: third_party/duilib-master/DuiLib/Control/UIEdit.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f6.o: third_party/duilib-master/DuiLib/Control/UIGifAnim.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f7.o: third_party/duilib-master/DuiLib/Control/UILabel.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f8.o: third_party/duilib-master/DuiLib/Control/UIList.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f9.o: third_party/duilib-master/DuiLib/Control/UIOption.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f10.o: third_party/duilib-master/DuiLib/Control/UIProgress.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f11.o: third_party/duilib-master/DuiLib/Control/UIRichEdit.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f12.o: third_party/duilib-master/DuiLib/Control/UIScrollBar.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f13.o: third_party/duilib-master/DuiLib/Control/UISlider.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f14.o: third_party/duilib-master/DuiLib/Control/UIText.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f15.o: third_party/duilib-master/DuiLib/Control/UITreeView.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f16.o: third_party/duilib-master/DuiLib/Core/UIBase.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f17.o: third_party/duilib-master/DuiLib/Core/UIContainer.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f18.o: third_party/duilib-master/DuiLib/Core/UIControl.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f19.o: third_party/duilib-master/DuiLib/Core/UIDlgBuilder.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f20.o: third_party/duilib-master/DuiLib/Core/UIManager.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f21.o: third_party/duilib-master/DuiLib/Core/UIMarkup.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f22.o: third_party/duilib-master/DuiLib/Core/UIRender.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f23.o: third_party/duilib-master/DuiLib/Layout/UIChildLayout.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f24.o: third_party/duilib-master/DuiLib/Layout/UIHorizontalLayout.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f25.o: third_party/duilib-master/DuiLib/Layout/UITabLayout.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f26.o: third_party/duilib-master/DuiLib/Layout/UITileLayout.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f27.o: third_party/duilib-master/DuiLib/Layout/UIVerticalLayout.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f28.o: third_party/duilib-master/DuiLib/StdAfx.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f29.o: third_party/duilib-master/DuiLib/UIlib.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f30.o: third_party/duilib-master/DuiLib/Utils/UIDelegate.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f31.o: third_party/duilib-master/DuiLib/Utils/Utils.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f32.o: third_party/duilib-master/DuiLib/Utils/WinImplBase.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f33.o: third_party/duilib-master/DuiLib/Utils/WndShadow.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f34.o: third_party/duilib-master/DuiLib/Utils/XUnzip.cpp
	$(CXX_DUI) -c $< -o $@

build/duilib/f35.o: third_party/duilib-master/DuiLib/Utils/stb_image.c
	$(CC_DUI) -c $< -o $@

DUI_OBJS = build/duilib/f0.o build/duilib/f1.o build/duilib/f2.o build/duilib/f3.o build/duilib/f4.o build/duilib/f5.o build/duilib/f6.o build/duilib/f7.o build/duilib/f8.o build/duilib/f9.o build/duilib/f10.o build/duilib/f11.o build/duilib/f12.o build/duilib/f13.o build/duilib/f14.o build/duilib/f15.o build/duilib/f16.o build/duilib/f17.o build/duilib/f18.o build/duilib/f19.o build/duilib/f20.o build/duilib/f21.o build/duilib/f22.o build/duilib/f23.o build/duilib/f24.o build/duilib/f25.o build/duilib/f26.o build/duilib/f27.o build/duilib/f28.o build/duilib/f29.o build/duilib/f30.o build/duilib/f31.o build/duilib/f32.o build/duilib/f33.o build/duilib/f34.o build/duilib/f35.o
