// Small reusable Dear ImGui widgets: vector icons, touch scrolling, badges, QR codes.
#pragma once
#include <string>

#include "imgui.h"

namespace ui {

enum class Icon { Pause, Play, Cancel, Check, Folder, File, Retry, Send, Attach, Back, Up, Down, Trash, Copy, Share, Bluetooth, Wifi, Link, Gear, Users, History, Transfers, Edit, Star, Open, Plus, ZoomIn, ZoomOut, Fit, Image, List, Grid, Phone, HangUp, Mic, MicOff, Speaker, Video, VideoOff, FlipCamera, Sliders, Expand, RotateLeft, RotateRight, Webcam, Library, Loop, Shuffle, Archive, Book, Lock, Info, Palette, Bell };

// Global UI metrics (set every frame by the app).
struct Metrics {
    float scale = 1.0f;     // DPI * user scale
    bool mobile = false;
    float rowH = 0;          // standard big button height on mobile
};
Metrics& M();

void DrawIcon(ImDrawList* dl, Icon icon, ImVec2 center, float size, ImU32 col);
// Square button with a vector icon (tooltip on hover). size 0 = frame height.
// caption: a short word under the icon (call buttons on phones, so nobody has to guess what they do)
bool IconButton(const char* id, Icon icon, const char* tooltip = nullptr, float size = 0, bool danger = false, const char* caption = nullptr);
// Button with icon + text label.
bool IconTextButton(const char* label, Icon icon, ImVec2 size = ImVec2(0, 0));
// Large full-width button for mobile layouts.
bool BigButton(const char* label, Icon icon, float width = -1);

void ProgressBarText(float frac, const char* overlay, ImVec2 size, ImU32 col = 0);
void StatusDot(ImU32 col, float radius = 0);
void Badge(const char* text, ImU32 bg);
void HelpMarker(const char* text);
// ImGui::Checkbox whose label wraps onto more lines when it doesn't fit the width (phones, long translations)
bool Checkbox(const char* label, bool* v);

// Legacy per-window hook (no-op).
void TouchScroll();
// Global drag-to-scroll with inertia for touch screens. Call once per frame after all windows were submitted.
void TouchScrollUpdate(bool enabled);

// Draws a QR code for `text`, `px` pixels wide.
void QrCode(const char* text, float px);

ImU32 ColAccent();
ImU32 ColAccentText();  // the accent as a colour for text and icons on the window's background
ImU32 ColOnAccent();    // text on the accent
// a button (or several) in the accent colour: the main action, or the choice that is on. on = false pushes nothing
// that shows (so Push / Pop can stay unconditional).
void PushAccent(bool on = true);
void PopAccent();
ImU32 ColOk();
ImU32 ColWarn();
ImU32 ColErr();
ImU32 ColMuted();
ImVec4 MutedText();

}  // namespace ui
