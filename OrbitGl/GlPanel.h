// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once
#include <memory>
#include <string>
#include <vector>

class GlPanel {
 public:
  GlPanel();
  virtual ~GlPanel() = default;

  // Determines if the panel should show debug statistics when the user requests them.
  // This is usually only enabled when Orbit runs in developer mode.
  enum class StatsMode { kEnabled, kDisabled };
  static std::unique_ptr<GlPanel> Create(StatsMode stats_mode, uint32_t font_size);

  virtual void Initialize();
  virtual void Resize(int a_Width, int a_Height);
  virtual void Render(int a_Width, int a_Height);
  virtual void PreRender(){};
  virtual void SetWindowOffset(int a_X, int a_Y) {
    m_WindowOffset[0] = a_X;
    m_WindowOffset[1] = a_Y;
  }
  virtual void SetMainWindowSize(int a_Width, int a_Height) {
    m_MainWindowWidth = a_Width;
    m_MainWindowHeight = a_Height;
  }

  virtual void MouseMoved(int /*a_X*/, int /*a_Y*/, bool /*a_Left*/, bool /*a_Right*/,
                          bool /*a_Middle*/) {}
  virtual void LeftDown(int /*a_X*/, int /*a_Y*/) {}
  virtual void LeftUp() {}
  virtual void RightDown(int /*a_X*/, int /*a_Y*/) {}
  virtual bool RightUp() { return false; }
  virtual void MiddleDown(int /*a_X*/, int /*a_Y*/) {}
  virtual void MiddleUp(int /*a_X*/, int /*a_Y*/) {}
  virtual void LeftDoubleClick() {}
  virtual void MouseWheelMoved(int /*a_X*/, int /*a_Y*/, int /*a_Delta*/, bool /*a_Ctrl*/) {}
  virtual void MouseWheelMovedHorizontally(int a_X, int a_Y, int a_Delta, bool a_Ctrl) {
    MouseWheelMoved(a_X, a_Y, a_Delta, a_Ctrl);
  }
  virtual void CharEvent(unsigned int /*a_Char*/) {}
  virtual void KeyPressed(unsigned int /*a_KeyCode*/, bool /*a_Ctrl*/, bool /*a_Shift*/,
                          bool /*a_Alt*/) {}
  virtual void KeyReleased(unsigned int /*a_KeyCode*/, bool /*a_Ctrl*/, bool /*a_Shift*/,
                           bool /*a_Alt*/) {}
  virtual std::vector<std::string> GetContextMenu() { return std::vector<std::string>(); }
  virtual void OnContextMenu(const std::string& /*a_Action*/, int /*a_MenuIndex*/) {}

  bool GetIsMouseOver() const { return is_mouse_over_; }
  void SetIsMouseOver(bool value) { is_mouse_over_ = value; }

  virtual bool GetNeedsRedraw() const { return m_NeedsRedraw; }
  void NeedsRedraw() { m_NeedsRedraw = true; }

 protected:
  int m_WindowOffset[2];
  int m_MainWindowWidth;
  int m_MainWindowHeight;
  bool m_NeedsRedraw;

  bool is_mouse_over_;
};
