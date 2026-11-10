#include "ui_widgets.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "imgui_internal.h"
#include "qrcodegen.hpp"

namespace ui {

Metrics& M() {
    static Metrics m;
    return m;
}

ImU32 ColAccent() { return ImGui::GetColorU32(ImGuiCol_ButtonActive); }
ImU32 ColOk() { return IM_COL32(64, 190, 110, 255); }
ImU32 ColWarn() { return IM_COL32(230, 175, 50, 255); }
ImU32 ColErr() { return IM_COL32(225, 75, 75, 255); }
ImU32 ColMuted() { return ImGui::GetColorU32(ImGuiCol_TextDisabled); }
ImVec4 MutedText() { return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled); }

void DrawIcon(ImDrawList* dl, Icon icon, ImVec2 c, float s, ImU32 col) {
    float h = s * 0.5f;
    float t = std::max(1.5f, s * 0.11f);  // stroke thickness
    auto P = [&](float x, float y) { return ImVec2(c.x + x * h, c.y + y * h); };
    switch (icon) {
        case Icon::Pause:
            dl->AddRectFilled(P(-0.6f, -0.7f), P(-0.15f, 0.7f), col, 1.5f);
            dl->AddRectFilled(P(0.15f, -0.7f), P(0.6f, 0.7f), col, 1.5f);
            break;
        case Icon::Play:
            dl->AddTriangleFilled(P(-0.5f, -0.75f), P(0.75f, 0.0f), P(-0.5f, 0.75f), col);
            break;
        case Icon::Cancel:
            dl->AddLine(P(-0.55f, -0.55f), P(0.55f, 0.55f), col, t);
            dl->AddLine(P(0.55f, -0.55f), P(-0.55f, 0.55f), col, t);
            break;
        case Icon::Check: {
            ImVec2 pts[3] = {P(-0.65f, 0.0f), P(-0.2f, 0.5f), P(0.7f, -0.55f)};
            dl->AddPolyline(pts, 3, col, 0, t * 1.2f);
            break;
        }
        case Icon::Folder:
            dl->AddRectFilled(P(-0.8f, -0.55f), P(-0.1f, -0.3f), col, 1.0f);
            dl->AddRectFilled(P(-0.8f, -0.4f), P(0.8f, 0.65f), col, 2.0f);
            break;
        case Icon::File: {
            ImVec2 pts[5] = {P(-0.55f, -0.8f), P(0.25f, -0.8f), P(0.6f, -0.45f), P(0.6f, 0.8f), P(-0.55f, 0.8f)};
            dl->AddPolyline(pts, 5, col, ImDrawFlags_Closed, t);
            break;
        }
        case Icon::Retry:
            dl->PathArcTo(c, h * 0.62f, 0.6f, 5.6f, 20);
            dl->PathStroke(col, 0, t);
            dl->AddTriangleFilled(P(0.6f, -0.65f), P(0.85f, 0.05f), P(0.2f, -0.05f), col);
            break;
        case Icon::Loop:  // a rounded loop with an arrow on top and one at the bottom
            dl->AddRect(P(-0.75f, -0.45f), P(0.75f, 0.45f), col, h * 0.35f, 0, t);
            dl->AddTriangleFilled(P(0.05f, -0.75f), P(0.45f, -0.45f), P(0.05f, -0.15f), col);
            dl->AddTriangleFilled(P(-0.05f, 0.75f), P(-0.45f, 0.45f), P(-0.05f, 0.15f), col);
            break;
        case Icon::Shuffle:  // two paths crossing on their way forward, arrows at the right
            dl->AddBezierCubic(P(-0.85f, -0.5f), P(-0.1f, -0.5f), P(0.0f, 0.5f), P(0.5f, 0.5f), col, t);
            dl->AddBezierCubic(P(-0.85f, 0.5f), P(-0.1f, 0.5f), P(0.0f, -0.5f), P(0.5f, -0.5f), col, t);
            dl->AddTriangleFilled(P(0.45f, -0.82f), P(0.9f, -0.5f), P(0.45f, -0.18f), col);
            dl->AddTriangleFilled(P(0.45f, 0.18f), P(0.9f, 0.5f), P(0.45f, 0.82f), col);
            break;
        case Icon::Send:
            dl->AddTriangleFilled(P(-0.8f, -0.75f), P(0.85f, 0.0f), P(-0.8f, 0.75f), col);
            dl->AddTriangleFilled(P(-0.8f, -0.75f), P(-0.35f, 0.0f), P(-0.8f, 0.75f), ImGui::GetColorU32(ImGuiCol_Button));
            break;
        case Icon::Attach:
        case Icon::Plus:
            dl->AddLine(P(0, -0.7f), P(0, 0.7f), col, t * 1.2f);
            dl->AddLine(P(-0.7f, 0), P(0.7f, 0), col, t * 1.2f);
            break;
        case Icon::Back: {
            ImVec2 pts[3] = {P(0.35f, -0.7f), P(-0.35f, 0.0f), P(0.35f, 0.7f)};
            dl->AddPolyline(pts, 3, col, 0, t * 1.3f);
            break;
        }
        case Icon::Up:
            dl->AddLine(P(0, 0.75f), P(0, -0.6f), col, t);
            dl->AddTriangleFilled(P(0, -0.8f), P(0.5f, -0.2f), P(-0.5f, -0.2f), col);
            break;
        case Icon::Down:
            dl->AddLine(P(0, -0.75f), P(0, 0.6f), col, t);
            dl->AddTriangleFilled(P(0, 0.8f), P(-0.5f, 0.2f), P(0.5f, 0.2f), col);
            break;
        case Icon::Trash:
            dl->AddLine(P(-0.7f, -0.55f), P(0.7f, -0.55f), col, t);
            dl->AddLine(P(-0.2f, -0.75f), P(0.2f, -0.75f), col, t);
            dl->AddRect(P(-0.5f, -0.45f), P(0.5f, 0.8f), col, 2.0f, 0, t);
            break;
        case Icon::Copy:
            dl->AddRect(P(-0.7f, -0.7f), P(0.25f, 0.35f), col, 2.0f, 0, t);
            dl->AddRectFilled(P(-0.2f, -0.2f), P(0.75f, 0.8f), col, 2.0f);
            break;
        case Icon::Share:
            dl->AddCircleFilled(P(-0.55f, 0.0f), h * 0.22f, col);
            dl->AddCircleFilled(P(0.55f, -0.55f), h * 0.22f, col);
            dl->AddCircleFilled(P(0.55f, 0.55f), h * 0.22f, col);
            dl->AddLine(P(-0.55f, 0.0f), P(0.55f, -0.55f), col, t);
            dl->AddLine(P(-0.55f, 0.0f), P(0.55f, 0.55f), col, t);
            break;
        case Icon::Bluetooth: {
            ImVec2 pts[6] = {P(-0.45f, -0.4f), P(0.45f, 0.4f), P(0.0f, 0.8f), P(0.0f, -0.8f), P(0.45f, -0.4f), P(-0.45f, 0.4f)};
            dl->AddPolyline(pts, 6, col, 0, t);
            break;
        }
        case Icon::Wifi:
            for (int i = 1; i <= 3; i++) {
                dl->PathArcTo(P(0, 0.6f), h * 0.4f * (float)i, -2.35f, -0.79f, 12);
                dl->PathStroke(col, 0, t);
            }
            dl->AddCircleFilled(P(0, 0.6f), t, col);
            break;
        case Icon::Link:
            dl->AddCircle(P(-0.3f, 0.0f), h * 0.38f, col, 16, t);
            dl->AddCircle(P(0.3f, 0.0f), h * 0.38f, col, 16, t);
            break;
        case Icon::Gear:
            dl->AddCircle(c, h * 0.45f, col, 16, t * 1.3f);
            for (int i = 0; i < 8; i++) {
                float a = (float)i * 3.14159f / 4.0f;
                dl->AddLine(ImVec2(c.x + cosf(a) * h * 0.55f, c.y + sinf(a) * h * 0.55f),
                            ImVec2(c.x + cosf(a) * h * 0.85f, c.y + sinf(a) * h * 0.85f), col, t * 1.4f);
            }
            break;
        case Icon::Users:
            dl->AddCircleFilled(P(0, -0.35f), h * 0.32f, col);
            dl->PathArcTo(P(0, 0.85f), h * 0.7f, 3.4f, 6.0f, 16);
            dl->PathFillConvex(col);
            break;
        case Icon::Library:  // books on a shelf
            dl->AddRectFilled(P(-0.75f, -0.55f), P(-0.4f, 0.65f), col, h * 0.06f);
            dl->AddRectFilled(P(-0.3f, -0.75f), P(0.05f, 0.65f), col, h * 0.06f);
            dl->AddQuadFilled(P(0.2f, -0.5f), P(0.5f, -0.6f), P(0.8f, 0.55f), P(0.5f, 0.65f), col);
            dl->AddLine(P(-0.85f, 0.8f), P(0.85f, 0.8f), col, t);
            break;
        case Icon::History:
            dl->AddCircle(c, h * 0.75f, col, 24, t);
            dl->AddLine(c, P(0, -0.5f), col, t);
            dl->AddLine(c, P(0.4f, 0.2f), col, t);
            break;
        case Icon::Transfers:
            dl->AddLine(P(-0.35f, 0.75f), P(-0.35f, -0.55f), col, t);
            dl->AddTriangleFilled(P(-0.35f, -0.8f), P(0.0f, -0.35f), P(-0.7f, -0.35f), col);
            dl->AddLine(P(0.35f, -0.75f), P(0.35f, 0.55f), col, t);
            dl->AddTriangleFilled(P(0.35f, 0.8f), P(0.7f, 0.35f), P(0.0f, 0.35f), col);
            break;
        case Icon::Edit:
            dl->AddLine(P(-0.6f, 0.6f), P(0.55f, -0.55f), col, t * 1.6f);
            dl->AddTriangleFilled(P(-0.75f, 0.75f), P(-0.7f, 0.35f), P(-0.35f, 0.7f), col);
            break;
        case Icon::Star: {
            ImVec2 pts[10];
            for (int i = 0; i < 10; i++) {
                float a = -1.5708f + (float)i * 3.14159f / 5.0f;
                float r = (i & 1) ? h * 0.38f : h * 0.85f;
                pts[i] = ImVec2(c.x + cosf(a) * r, c.y + sinf(a) * r);
            }
            dl->AddConcavePolyFilled(pts, 10, col);
            break;
        }
        case Icon::ZoomIn:
        case Icon::ZoomOut:
            dl->AddCircle(P(-0.15f, -0.15f), h * 0.55f, col, 20, t);
            dl->AddLine(P(0.25f, 0.25f), P(0.8f, 0.8f), col, t * 1.6f);
            dl->AddLine(P(-0.42f, -0.15f), P(0.12f, -0.15f), col, t);
            if (icon == Icon::ZoomIn) dl->AddLine(P(-0.15f, -0.42f), P(-0.15f, 0.12f), col, t);
            break;
        case Icon::Fit: {
            float a = 0.75f, b = 0.3f;
            ImVec2 c1[3] = {P(-a, -b), P(-a, -a), P(-b, -a)}, c2[3] = {P(b, -a), P(a, -a), P(a, -b)};
            ImVec2 c3[3] = {P(a, b), P(a, a), P(b, a)}, c4[3] = {P(-b, a), P(-a, a), P(-a, b)};
            dl->AddPolyline(c1, 3, col, 0, t);
            dl->AddPolyline(c2, 3, col, 0, t);
            dl->AddPolyline(c3, 3, col, 0, t);
            dl->AddPolyline(c4, 3, col, 0, t);
            break;
        }
        case Icon::Phone:
        case Icon::HangUp: {
            // the classic telephone receiver: a thick curved handle with an ear pad (top left) and a mouth pad
            // (bottom right); hang up = the same receiver lying down
            float rot = icon == Icon::HangUp ? 2.356f : 0.0f;
            auto R = [&](float x, float y) {
                float cs = cosf(rot), sn = sinf(rot);
                return ImVec2(c.x + (x * cs - y * sn) * h, c.y + (x * sn + y * cs) * h);
            };
            const float cx = 0.42f, cy = -0.42f, rad = 1.02f;  // the handle bends around this point
            auto A = [&](float a, float r) { return R(cx + cosf(a) * r, cy + sinf(a) * r); };
            const float a0 = 3.14159f * 0.98f, a1 = 3.14159f * 0.52f;
            for (int i = 0; i <= 16; i++) dl->PathLineTo(A(a0 + (a1 - a0) * (float)i / 16.0f, rad));
            dl->PathStroke(col, 0, h * 0.34f);
            // pads: wide rounded pieces at both ends, pointing in towards the bend
            auto pad = [&](float a) {
                ImVec2 outer = A(a, rad + 0.06f), inner = A(a, rad - 0.42f);
                dl->AddLine(outer, inner, col, h * 0.46f);
                dl->AddCircleFilled(outer, h * 0.23f, col);
                dl->AddCircleFilled(inner, h * 0.23f, col);
            };
            pad(a0 + 0.06f);
            pad(a1 - 0.06f);
            break;
        }
        case Icon::Mic:
        case Icon::MicOff:
            dl->AddRectFilled(P(-0.22f, -0.8f), P(0.22f, 0.15f), col, h * 0.22f);
            dl->PathArcTo(P(0, -0.05f), h * 0.45f, 0.0f, 3.14159f, 12);
            dl->PathStroke(col, 0, t);
            dl->AddLine(P(0, 0.4f), P(0, 0.75f), col, t);
            dl->AddLine(P(-0.3f, 0.75f), P(0.3f, 0.75f), col, t);
            if (icon == Icon::MicOff) dl->AddLine(P(-0.75f, -0.75f), P(0.75f, 0.75f), IM_COL32(230, 70, 70, 255), t * 1.4f);
            break;
        case Icon::Speaker:
            dl->AddRectFilled(P(-0.8f, -0.25f), P(-0.45f, 0.25f), col);
            dl->AddTriangleFilled(P(-0.45f, -0.25f), P(0.0f, -0.65f), P(0.0f, 0.65f), col);
            dl->AddTriangleFilled(P(-0.45f, -0.25f), P(0.0f, 0.65f), P(-0.45f, 0.25f), col);
            dl->PathArcTo(P(0.05f, 0), h * 0.35f, -0.8f, 0.8f, 8);
            dl->PathStroke(col, 0, t);
            dl->PathArcTo(P(0.05f, 0), h * 0.65f, -0.8f, 0.8f, 10);
            dl->PathStroke(col, 0, t);
            break;
        case Icon::Video:
        case Icon::VideoOff:
            // camcorder: body + lens wedge
            dl->AddRectFilled(P(-0.85f, -0.45f), P(0.3f, 0.45f), col, h * 0.15f);
            dl->AddTriangleFilled(P(0.35f, 0.0f), P(0.85f, -0.4f), P(0.85f, 0.4f), col);
            if (icon == Icon::VideoOff) dl->AddLine(P(-0.8f, -0.8f), P(0.8f, 0.8f), IM_COL32(230, 70, 70, 255), t * 1.4f);
            break;
        case Icon::FlipCamera:
            // camera body with two turning arrows
            dl->AddRect(P(-0.7f, -0.35f), P(0.7f, 0.5f), col, h * 0.12f, 0, t);
            dl->AddRectFilled(P(-0.25f, -0.55f), P(0.25f, -0.35f), col);
            dl->PathArcTo(P(0, 0.08f), h * 0.25f, 3.6f, 6.0f, 8);
            dl->PathStroke(col, 0, t);
            dl->AddTriangleFilled(P(0.18f, 0.12f), P(0.38f, 0.12f), P(0.28f, -0.05f), col);
            dl->PathArcTo(P(0, 0.08f), h * 0.25f, 0.46f, 2.86f, 8);
            dl->PathStroke(col, 0, t);
            dl->AddTriangleFilled(P(-0.18f, 0.04f), P(-0.38f, 0.04f), P(-0.28f, 0.21f), col);
            break;
        case Icon::Sliders:
            for (int i = -1; i <= 1; i++) {
                float y = i * 0.55f, k = i == 0 ? 0.3f : i < 0 ? -0.3f : 0.1f;
                dl->AddLine(P(-0.8f, y), P(0.8f, y), col, t);
                dl->AddCircleFilled(P(k, y), h * 0.17f, col);
            }
            break;
        case Icon::Expand:
            dl->AddLine(P(-0.75f, -0.75f), P(-0.25f, -0.75f), col, t);
            dl->AddLine(P(-0.75f, -0.75f), P(-0.75f, -0.25f), col, t);
            dl->AddLine(P(0.75f, 0.75f), P(0.25f, 0.75f), col, t);
            dl->AddLine(P(0.75f, 0.75f), P(0.75f, 0.25f), col, t);
            dl->AddLine(P(0.75f, -0.75f), P(0.25f, -0.75f), col, t);
            dl->AddLine(P(0.75f, -0.75f), P(0.75f, -0.25f), col, t);
            dl->AddLine(P(-0.75f, 0.75f), P(-0.25f, 0.75f), col, t);
            dl->AddLine(P(-0.75f, 0.75f), P(-0.75f, 0.25f), col, t);
            break;
        case Icon::RotateLeft:
        case Icon::RotateRight: {
            float d = icon == Icon::RotateRight ? 1.f : -1.f;
            dl->PathArcTo(P(0, 0.05f), h * 0.6f, d > 0 ? -2.6f : -0.54f, d > 0 ? 1.2f : 3.74f, 14);
            dl->PathStroke(col, 0, t);
            // arrow head at the start of the arc (top)
            ImVec2 tip = P(d * -0.5f, -0.55f);
            dl->AddTriangleFilled(ImVec2(tip.x - d * h * 0.05f, tip.y - h * 0.3f), ImVec2(tip.x + d * h * 0.3f, tip.y + h * 0.02f),
                                  ImVec2(tip.x - d * h * 0.3f, tip.y + h * 0.15f), col);
            break;
        }
        case Icon::Webcam:
            // a monitor with a camera on top
            dl->AddRect(P(-0.8f, -0.35f), P(0.8f, 0.55f), col, h * 0.08f, 0, t);
            dl->AddLine(P(-0.3f, 0.8f), P(0.3f, 0.8f), col, t);
            dl->AddLine(P(0, 0.55f), P(0, 0.8f), col, t);
            dl->AddCircleFilled(P(0, -0.6f), h * 0.2f, col);
            break;
        case Icon::List:
            for (int i = -1; i <= 1; i++) {
                dl->AddRectFilled(P(-0.75f, i * 0.55f - 0.1f), P(-0.5f, i * 0.55f + 0.1f), col);
                dl->AddLine(P(-0.3f, i * 0.55f), P(0.75f, i * 0.55f), col, t);
            }
            break;
        case Icon::Grid:
            for (int y = 0; y < 2; y++)
                for (int x = 0; x < 2; x++)
                    dl->AddRectFilled(P(-0.75f + x * 0.85f, -0.75f + y * 0.85f), P(-0.1f + x * 0.85f, -0.1f + y * 0.85f), col, 1.5f);
            break;
        case Icon::Archive:  // a box with a lid and a zipper down the middle
            dl->AddRect(P(-0.75f, -0.4f), P(0.75f, 0.8f), col, 2.0f, 0, t);
            dl->AddRect(P(-0.85f, -0.75f), P(0.85f, -0.4f), col, 2.0f, 0, t);
            for (float y = -0.25f; y < 0.55f; y += 0.2f) dl->AddLine(P(-0.12f, y), P(0.12f, y), col, t);
            break;
        case Icon::Book:  // an open book
            dl->AddQuad(P(-0.85f, -0.6f), P(-0.02f, -0.45f), P(-0.02f, 0.75f), P(-0.85f, 0.6f), col, t);
            dl->AddQuad(P(0.85f, -0.6f), P(0.02f, -0.45f), P(0.02f, 0.75f), P(0.85f, 0.6f), col, t);
            dl->AddLine(P(-0.65f, -0.25f), P(-0.2f, -0.17f), col, t * 0.8f);
            dl->AddLine(P(0.65f, -0.25f), P(0.2f, -0.17f), col, t * 0.8f);
            break;
        case Icon::Image:
            dl->AddRect(P(-0.8f, -0.65f), P(0.8f, 0.65f), col, 2.0f, 0, t);
            dl->AddTriangleFilled(P(-0.6f, 0.45f), P(-0.15f, -0.15f), P(0.3f, 0.45f), col);
            dl->AddCircleFilled(P(0.4f, -0.25f), h * 0.14f, col);
            break;
        case Icon::Open:
            dl->AddRect(P(-0.7f, -0.5f), P(0.5f, 0.7f), col, 2.0f, 0, t);
            dl->AddLine(P(-0.05f, 0.1f), P(0.75f, -0.7f), col, t);
            dl->AddTriangleFilled(P(0.8f, -0.8f), P(0.75f, -0.25f), P(0.25f, -0.75f), col);
            break;
    }
}

bool IconButton(const char* id, Icon icon, const char* tooltip, float size, bool danger, const char* caption) {
    if (size <= 0) size = M().mobile ? M().rowH : ImGui::GetFrameHeight();
    ImGui::PushID(id);
    if (danger) ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.25f, 0.25f, 1.0f));
    bool r = ImGui::Button("##ib", ImVec2(size, size));
    if (danger) ImGui::PopStyleColor();
    ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    ImU32 col = ImGui::GetColorU32(ImGui::GetItemFlags() & ImGuiItemFlags_Disabled ? ImGuiCol_TextDisabled : ImGuiCol_Text);
    float bh = mx.y - mn.y, bw = mx.x - mn.x;
    if (caption && *caption && bh > ImGui::GetFontSize() * 2.6f) {
        ImFont* f = ImGui::GetFont();
        const char* end = strstr(caption, "##");  // (id suffix of a translated label)
        float fs = ImGui::GetFontSize() * 0.78f;
        ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, caption, end);
        if (ts.x > bw * 0.94f) {  // long word: smaller
            fs *= bw * 0.94f / ts.x;
            ts = f->CalcTextSizeA(fs, FLT_MAX, 0, caption, end);
        }
        float iconH = bh - ts.y - bh * 0.12f;
        DrawIcon(ImGui::GetWindowDrawList(), icon, ImVec2((mn.x + mx.x) * 0.5f, mn.y + bh * 0.06f + iconH * 0.5f), std::min(size * 0.48f, iconH * 0.8f), col);
        ImGui::GetWindowDrawList()->AddText(f, fs, ImVec2((mn.x + mx.x - ts.x) * 0.5f, mx.y - ts.y - bh * 0.07f), col, caption, end);
    } else {
        DrawIcon(ImGui::GetWindowDrawList(), icon, ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f), size * 0.55f, col);
    }
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return r;
}

bool IconTextButton(const char* label, Icon icon, ImVec2 size) {
    float fh = ImGui::GetFrameHeight();
    if (M().mobile && size.y == 0) size.y = M().rowH;
    float ih = (size.y > 0 ? size.y : fh);
    ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
    if (size.x == 0) size.x = ts.x + ih + ImGui::GetStyle().FramePadding.x * 2;
    if (size.y == 0) size.y = fh;
    ImGui::PushID(label);
    bool r = ImGui::Button("##itb", size);
    ImGui::PopID();
    ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float contentW = ih * 0.75f + ts.x;
    float cy = (mn.y + mx.y) * 0.5f;
    ImU32 col = ImGui::GetColorU32(ImGui::GetItemFlags() & ImGuiItemFlags_Disabled ? ImGuiCol_TextDisabled : ImGuiCol_Text);
    if (contentW + ImGui::GetStyle().FramePadding.x * 2 > mx.x - mn.x) {  // too narrow for the text: icon only
        DrawIcon(dl, icon, ImVec2((mn.x + mx.x) * 0.5f, cy), ih * 0.5f, col);
        if (ImGui::IsItemHovered() && !M().mobile && ts.x > 0) ImGui::SetTooltip("%.*s", (int)(ImGui::FindRenderedTextEnd(label) - label), label);
        return r;
    }
    float x0 = mn.x + std::max(ImGui::GetStyle().FramePadding.x, (mx.x - mn.x - contentW) * 0.5f);
    DrawIcon(dl, icon, ImVec2(x0 + ih * 0.25f, cy), ih * 0.5f, col);
    const char* end = ImGui::FindRenderedTextEnd(label);
    dl->AddText(ImVec2(x0 + ih * 0.65f, cy - ts.y * 0.5f), col, label, end);
    return r;
}

bool BigButton(const char* label, Icon icon, float width) {
    float w = width < 0 ? ImGui::GetContentRegionAvail().x : width;
    float h = M().mobile ? M().rowH : ImGui::GetFrameHeight() * 1.4f;
    return IconTextButton(label, icon, ImVec2(w, h));
}

void ProgressBarText(float frac, const char* overlay, ImVec2 size, ImU32 col) {
    if (col) ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
    ImGui::ProgressBar(frac < 0 ? 0 : (frac > 1 ? 1 : frac), size, overlay);
    if (col) ImGui::PopStyleColor();
}

void StatusDot(ImU32 col, float radius) {
    if (radius <= 0) radius = ImGui::GetFontSize() * 0.3f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight();
    ImGui::Dummy(ImVec2(radius * 2 + 2, h));
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + radius + 1, p.y + h * 0.5f), radius, col);
}

void Badge(const char* text, ImU32 bg) {
    ImVec2 ts = ImGui::CalcTextSize(text);
    float padX = ImGui::GetFontSize() * 0.4f, padY = 1;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight();
    ImGui::Dummy(ImVec2(ts.x + padX * 2, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(p.x, p.y + padY), ImVec2(p.x + ts.x + padX * 2, p.y + h - padY), bg, h * 0.5f);
    dl->AddText(ImVec2(p.x + padX, p.y + (h - ts.y) * 0.5f), IM_COL32(255, 255, 255, 255), text);
}

void HelpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void TouchScroll() {}  // kept for call sites; scrolling is handled globally by TouchScrollUpdate()

// Drag-to-scroll for touch screens, applied once per frame to whatever scrollable area is under the finger
// (child windows, tables, popups, combo lists). Works no matter where the drag starts: on a button, a label or
// empty space. A tap still clicks; once the finger moves vertically past a small threshold the gesture becomes
// a scroll and the pressed widget is released without triggering. After lifting the finger the list keeps
// gliding with decaying speed.
void TouchScrollUpdate(bool enabled) {
    static ImGuiID target = 0;
    static float velocity = 0;
    static bool dragging = false, candidate = false;
    ImGuiContext& g = *GImGui;
    ImGuiIO& io = g.IO;
    if (!enabled) {
        dragging = candidate = false;
        velocity = 0;
        return;
    }
    auto scrollable = [](ImGuiWindow* w) -> ImGuiWindow* {
        while (w) {
            if (w->ScrollMax.y > 0.5f && !(w->Flags & ImGuiWindowFlags_NoScrollWithMouse)) return w;
            if (!(w->Flags & ImGuiWindowFlags_ChildWindow)) break;  // stop at popups / top-level windows
            w = w->ParentWindow;
        }
        return nullptr;
    };
    if (ImGui::IsMouseClicked(0)) {
        ImGuiWindow* w = scrollable(g.HoveredWindow);
        target = w ? w->ID : 0;
        candidate = w != nullptr;
        dragging = false;
        velocity = 0;
    }
    ImGuiWindow* w = target ? ImGui::FindWindowByID(target) : nullptr;
    if (!w || !w->WasActive) {
        target = 0;
        dragging = candidate = false;
        velocity = 0;
        return;
    }
    if (ImGui::IsMouseDown(0) && candidate) {
        float threshold = ImGui::GetFontSize() * 0.5f;
        ImVec2 d = ImGui::GetMouseDragDelta(0, 0.0f);
        if (!dragging && fabsf(d.y) > threshold && fabsf(d.y) > fabsf(d.x) * 1.2f) {
            // Text fields keep drag-to-select; everything else (buttons, rows, empty space) hands over to scrolling.
            bool editingText = g.ActiveId != 0 && g.ActiveId == g.InputTextState.ID;
            if (editingText) {
                candidate = false;
            } else {
                dragging = true;
                if (g.ActiveId) ImGui::ClearActiveID();
                ImGui::SetScrollY(w, w->Scroll.y - d.y);  // catch up with the movement below the threshold
            }
        } else if (dragging) {
            ImGui::SetScrollY(w, w->Scroll.y - io.MouseDelta.y);
            float inst = io.DeltaTime > 0 ? -io.MouseDelta.y / io.DeltaTime : 0;
            velocity = velocity * 0.6f + inst * 0.4f;
        }
        if (dragging) ImGui::SetHoveredID(0);  // no hover highlight on whatever passes under the finger
    } else {
        if (dragging && ImGui::IsMouseReleased(0) && io.MouseDownDurationPrev[0] > 0 && fabsf(io.MouseDelta.y) < 1 && ImGui::GetIO().DeltaTime > 0) {
            // finger stopped before lifting: no fling
            if (fabsf(velocity) < 200) velocity = 0;
        }
        dragging = false;
        candidate = false;
        if (fabsf(velocity) > 20.0f) {
            ImGui::SetScrollY(w, std::clamp(w->Scroll.y + velocity * io.DeltaTime, 0.0f, w->ScrollMax.y));
            velocity *= powf(0.04f, io.DeltaTime);
            if (w->Scroll.y <= 0 || w->Scroll.y >= w->ScrollMax.y) velocity = 0;
        } else {
            velocity = 0;
        }
    }
}

void QrCode(const char* text, float px) {
    using qrcodegen::QrCode;
    static std::string cachedText;
    static std::vector<bool> mods;
    static int n = 0;
    if (cachedText != text) {
        cachedText = text;
        try {
            QrCode qr = QrCode::encodeText(text, QrCode::Ecc::MEDIUM);
            n = qr.getSize();
            mods.assign((size_t)(n * n), false);
            for (int y = 0; y < n; y++)
                for (int x = 0; x < n; x++) mods[(size_t)(y * n + x)] = qr.getModule(x, y);
        } catch (...) {
            n = 0;
        }
    }
    if (n == 0) return;
    int quiet = 3;
    float cell = floorf(px / (float)(n + quiet * 2));
    if (cell < 1) cell = 1;
    float total = cell * (float)(n + quiet * 2);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + total, p.y + total), IM_COL32_WHITE);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (mods[(size_t)(y * n + x)]) {
                ImVec2 a(p.x + (float)(x + quiet) * cell, p.y + (float)(y + quiet) * cell);
                dl->AddRectFilled(a, ImVec2(a.x + cell, a.y + cell), IM_COL32_BLACK);
            }
    ImGui::Dummy(ImVec2(total, total));
}

}  // namespace ui
