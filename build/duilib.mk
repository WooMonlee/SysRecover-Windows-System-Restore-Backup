# ??????? Duilib MinGW ?????? AGENTS.md PIT-012?
# ???UIFlash/UIWebBrowser?? ATL???? 35 ???

$(OBJDIR)/duilib/f0.o: third_party/duilib-master/DuiLib/Control/UIActiveX.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f1.o: third_party/duilib-master/DuiLib/Control/UIButton.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f2.o: third_party/duilib-master/DuiLib/Control/UICheckBox.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f3.o: third_party/duilib-master/DuiLib/Control/UICombo.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f4.o: third_party/duilib-master/DuiLib/Control/UIDateTime.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f5.o: third_party/duilib-master/DuiLib/Control/UIEdit.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f6.o: third_party/duilib-master/DuiLib/Control/UIGifAnim.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f7.o: third_party/duilib-master/DuiLib/Control/UILabel.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f8.o: third_party/duilib-master/DuiLib/Control/UIList.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f9.o: third_party/duilib-master/DuiLib/Control/UIOption.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f10.o: third_party/duilib-master/DuiLib/Control/UIProgress.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f11.o: third_party/duilib-master/DuiLib/Control/UIRichEdit.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f12.o: third_party/duilib-master/DuiLib/Control/UIScrollBar.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f13.o: third_party/duilib-master/DuiLib/Control/UISlider.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f14.o: third_party/duilib-master/DuiLib/Control/UIText.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f15.o: third_party/duilib-master/DuiLib/Control/UITreeView.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f16.o: third_party/duilib-master/DuiLib/Core/UIBase.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f17.o: third_party/duilib-master/DuiLib/Core/UIContainer.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f18.o: third_party/duilib-master/DuiLib/Core/UIControl.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f19.o: third_party/duilib-master/DuiLib/Core/UIDlgBuilder.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f20.o: third_party/duilib-master/DuiLib/Core/UIManager.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f21.o: third_party/duilib-master/DuiLib/Core/UIMarkup.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f22.o: third_party/duilib-master/DuiLib/Core/UIRender.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f23.o: third_party/duilib-master/DuiLib/Layout/UIChildLayout.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f24.o: third_party/duilib-master/DuiLib/Layout/UIHorizontalLayout.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f25.o: third_party/duilib-master/DuiLib/Layout/UITabLayout.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f26.o: third_party/duilib-master/DuiLib/Layout/UITileLayout.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f27.o: third_party/duilib-master/DuiLib/Layout/UIVerticalLayout.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f28.o: third_party/duilib-master/DuiLib/StdAfx.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f29.o: third_party/duilib-master/DuiLib/UIlib.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f30.o: third_party/duilib-master/DuiLib/Utils/UIDelegate.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f31.o: third_party/duilib-master/DuiLib/Utils/Utils.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f32.o: third_party/duilib-master/DuiLib/Utils/WinImplBase.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f33.o: third_party/duilib-master/DuiLib/Utils/WndShadow.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f34.o: third_party/duilib-master/DuiLib/Utils/XUnzip.cpp
	$(CXX_DUI) -c $< -o $@

$(OBJDIR)/duilib/f35.o: third_party/duilib-master/DuiLib/Utils/stb_image.c
	$(CC_DUI) -c $< -o $@

DUI_OBJS = $(OBJDIR)/duilib/f0.o $(OBJDIR)/duilib/f1.o $(OBJDIR)/duilib/f2.o $(OBJDIR)/duilib/f3.o $(OBJDIR)/duilib/f4.o $(OBJDIR)/duilib/f5.o $(OBJDIR)/duilib/f6.o $(OBJDIR)/duilib/f7.o $(OBJDIR)/duilib/f8.o $(OBJDIR)/duilib/f9.o $(OBJDIR)/duilib/f10.o $(OBJDIR)/duilib/f11.o $(OBJDIR)/duilib/f12.o $(OBJDIR)/duilib/f13.o $(OBJDIR)/duilib/f14.o $(OBJDIR)/duilib/f15.o $(OBJDIR)/duilib/f16.o $(OBJDIR)/duilib/f17.o $(OBJDIR)/duilib/f18.o $(OBJDIR)/duilib/f19.o $(OBJDIR)/duilib/f20.o $(OBJDIR)/duilib/f21.o $(OBJDIR)/duilib/f22.o $(OBJDIR)/duilib/f23.o $(OBJDIR)/duilib/f24.o $(OBJDIR)/duilib/f25.o $(OBJDIR)/duilib/f26.o $(OBJDIR)/duilib/f27.o $(OBJDIR)/duilib/f28.o $(OBJDIR)/duilib/f29.o $(OBJDIR)/duilib/f30.o $(OBJDIR)/duilib/f31.o $(OBJDIR)/duilib/f32.o $(OBJDIR)/duilib/f33.o $(OBJDIR)/duilib/f34.o $(OBJDIR)/duilib/f35.o
