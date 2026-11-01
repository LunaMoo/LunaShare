// Chat picture thumbnails and the full-screen picture viewer (swipe / arrows to browse, pinch / wheel to zoom).
#include <algorithm>
#include <cmath>

#include "image_cache.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "platform.h"
#include "ui.h"
#include "ui_widgets.h"

using namespace ls;
using ui::Icon;

static bool ViewablePath(const Transfer& t) {
    if (!ImageCache::IsImagePath(t.rel.empty() ? t.name : t.rel)) return false;
    if (t.outgoing) return t.state != TState::Cancelled && FileExists(t.localPath);
    return t.state == TState::Completed && FileExists(t.localPath);
}

// ---------------------------------------------------------------- thumbnails inside a chat card
void App::DrawThumbnails(const std::string& peer, const std::vector<std::shared_ptr<Transfer>>& items, float width) {
    std::vector<std::shared_ptr<Transfer>> imgs;
    for (auto& t : items)
        if (ViewablePath(*t)) imgs.push_back(t);
    if (imgs.empty()) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float rounding = ImGui::GetFontSize() * 0.4f;
    auto clickable = [&](const std::string& path, ImVec2 size) {
        ImGui::PushID(path.c_str());
        bool c = ImGui::InvisibleButton("##thumb", size);
        bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (c) OpenViewer(peer, path, false);
        return hovered;
    };
    if (imgs.size() == 1) {
        const ImageTex& t = Images().Get(imgs[0]->localPath, false);
        float maxH = ImGui::GetFontSize() * (ui::M().mobile ? 16.0f : 13.0f);
        ImVec2 size(width, maxH * 0.6f);
        if (t.state == ImageTex::Ready) {
            float aspect = (float)t.w / (float)t.h;
            size.y = std::min(maxH, width / aspect);
            size.x = size.y * aspect;
            if (size.x > width) { size.x = width; size.y = width / aspect; }
        }
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool hovered = clickable(imgs[0]->localPath, size);
        ImVec2 p1(p.x + size.x, p.y + size.y);
        if (t.state == ImageTex::Ready) dl->AddImageRounded(t.Id(), p, p1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, rounding);
        else dl->AddRectFilled(p, p1, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
        if (t.state == ImageTex::Loading) dl->AddText(ImVec2(p.x + 8, p.y + 8), ui::ColMuted(), "loading...");
        if (t.state == ImageTex::Failed) dl->AddText(ImVec2(p.x + 8, p.y + 8), ui::ColMuted(), "preview not available");
        if (hovered) {
            dl->AddRect(p, p1, ImGui::GetColorU32(ImGuiCol_ButtonHovered), rounding, 0, 2.0f);
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        return;
    }
    // grid of square, center-cropped thumbnails
    int cols = imgs.size() == 2 || imgs.size() == 4 ? 2 : 3;
    float gap = ImGui::GetStyle().ItemSpacing.x * 0.5f;
    float cell = floorf((width - gap * (float)(cols - 1)) / (float)cols);
    int maxShow = cols * (cols == 2 ? 2 : 3);
    int shown = std::min((int)imgs.size(), maxShow);
    for (int i = 0; i < shown; i++) {
        if (i % cols) ImGui::SameLine(0, gap);
        const ImageTex& t = Images().Get(imgs[(size_t)i]->localPath, false);
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool hovered = clickable(imgs[(size_t)i]->localPath, ImVec2(cell, cell));
        ImVec2 p1(p.x + cell, p.y + cell);
        if (t.state == ImageTex::Ready) {
            ImVec2 uv0(0, 0), uv1(1, 1);
            if (t.w > t.h) { float f = (float)t.h / (float)t.w; uv0.x = (1 - f) * 0.5f; uv1.x = 1 - uv0.x; }
            else { float f = (float)t.w / (float)t.h; uv0.y = (1 - f) * 0.5f; uv1.y = 1 - uv0.y; }
            dl->AddImageRounded(t.Id(), p, p1, uv0, uv1, IM_COL32_WHITE, rounding);
        } else {
            dl->AddRectFilled(p, p1, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
        }
        if (i == shown - 1 && (int)imgs.size() > shown) {
            dl->AddRectFilled(p, p1, IM_COL32(0, 0, 0, 140), rounding);
            std::string more = "+" + std::to_string((int)imgs.size() - shown);
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.6f);
            ImVec2 ts = ImGui::CalcTextSize(more.c_str());
            dl->AddText(ImVec2(p.x + (cell - ts.x) * 0.5f, p.y + (cell - ts.y) * 0.5f), IM_COL32_WHITE, more.c_str());
            ImGui::PopFont();
        }
        if (hovered) {
            dl->AddRect(p, p1, ImGui::GetColorU32(ImGuiCol_ButtonHovered), rounding, 0, 2.0f);
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
    }
}

// ---------------------------------------------------------------- viewer
void App::RebuildViewerList() {
    std::string current = viewer.index >= 0 && viewer.index < (int)viewer.paths.size() ? viewer.paths[(size_t)viewer.index] : "";
    viewer.paths.clear();
    viewer.captions.clear();
    std::set<std::string> seen;
    auto add = [&](const std::string& path, const std::string& caption) {
        if (seen.insert(path).second) {
            viewer.paths.push_back(path);
            viewer.captions.push_back(caption);
        }
    };
    if (!viewer.all) {
        auto it = eng.chats.find(viewer.peer);
        if (it != eng.chats.end()) {
            std::string them = eng.PeerName(viewer.peer);
            for (auto& m : it->second.messages) {
                if (m.kind != MsgKind::Files) continue;
                for (auto tid : m.tids) {
                    auto t = eng.transfers.find(tid);
                    if (t == eng.transfers.end() || !ViewablePath(*t->second)) continue;
                    add(t->second->localPath, (m.outgoing ? "Sent to " : "From ") + them + "  -  " + FormatTime(m.ts));
                }
            }
        }
    } else {
        for (auto& h : eng.history) {
            if (!ImageCache::IsImagePath(h.path) || !FileExists(h.path)) continue;
            add(h.path, (h.outgoing ? "Sent to " : "From ") + eng.PeerName(h.peer) + "  -  " + FormatTime(h.ts));
        }
    }
    viewer.index = 0;
    for (size_t i = 0; i < viewer.paths.size(); i++)
        if (viewer.paths[i] == current) viewer.index = (int)i;
}

void App::OpenViewer(const std::string& peer, const std::string& path, bool all) {
    viewer.peer = peer;
    viewer.all = all;
    viewer.paths = {path};
    viewer.index = 0;
    RebuildViewerList();
    if (viewer.paths.empty()) {
        viewer.paths = {path};
        viewer.captions = {""};
    }
    viewer.zoom = 1;
    viewer.pan = ImVec2(0, 0);
    viewer.swipeX = 0;
    viewer.open = true;
    viewer.requestOpen = true;
    viewer.uiAlpha = 1;
}

void App::ViewerGo(int delta) {
    int n = (int)viewer.paths.size();
    int ni = viewer.index + delta;
    if (ni < 0 || ni >= n) {
        viewer.swipeX = 0;
        return;
    }
    viewer.index = ni;
    viewer.zoom = 1;
    viewer.pan = ImVec2(0, 0);
    // slide in from the side we are coming from
    viewer.swipeX = (viewer.areaMax.x - viewer.areaMin.x) * 0.35f * (float)delta;
}

void App::ViewerZoomAt(ImVec2 pt, float factor) {
    float nz = std::clamp(viewer.zoom * factor, 0.5f, 16.0f);
    float f = nz / viewer.zoom;
    ImVec2 c((viewer.areaMin.x + viewer.areaMax.x) * 0.5f, (viewer.areaMin.y + viewer.areaMax.y) * 0.5f);
    ImVec2 r(pt.x - c.x - viewer.pan.x, pt.y - c.y - viewer.pan.y);
    viewer.pan = ImVec2(pt.x - c.x - r.x * f, pt.y - c.y - r.y * f);
    viewer.zoom = nz;
    if (nz <= 1.0f) viewer.pan = ImVec2(0, 0);
}

void App::OnPinch(float dDist, float x, float y) {
    if (!viewer.open) return;
    ViewerZoomAt(ImVec2(x, y), 1.0f + dDist * 3.0f);
}

void App::DrawViewer() {
    if (!viewer.open) return;
    if (viewer.requestOpen) {
        ImGui::OpenPopup("##viewer");
        viewer.requestOpen = false;
    }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(8, 8, 10, 250));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, IM_COL32(0, 0, 0, 200));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    bool vis = ImGui::BeginPopupModal("##viewer", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                               ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    if (!vis) {
        viewer.open = false;
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    bool mobile = ui::M().mobile;
    bool close = false;
    // pick up pictures that finished arriving while the viewer is open
    static int refresh = 0;
    if (++refresh % 30 == 0) RebuildViewerList();
    if (viewer.paths.empty()) { viewer.open = false; ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    int n = (int)viewer.paths.size();
    if (viewer.index >= n) viewer.index = n - 1;
    const std::string& path = viewer.paths[(size_t)viewer.index];
    ImDrawList* dl = ImGui::GetWindowDrawList();

    viewer.areaMin = vp->Pos;
    viewer.areaMax = ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
    ImVec2 areaSize(vp->Size.x, vp->Size.y);
    ImVec2 c(viewer.areaMin.x + areaSize.x * 0.5f, viewer.areaMin.y + areaSize.y * 0.5f);

    // gesture surface (whole screen, toolbar is drawn on top later)
    ImGui::SetCursorScreenPos(viewer.areaMin);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##surface", areaSize, ImGuiButtonFlags_MouseButtonLeft);
    bool active = ImGui::IsItemActive();
    bool hovered = ImGui::IsItemHovered();
    if (hovered && io.MouseWheel != 0) ViewerZoomAt(io.MousePos, powf(1.18f, io.MouseWheel));
    if (active && ImGui::IsMouseDragging(0, 2.0f)) {
        if (viewer.zoom > 1.01f) {
            viewer.pan.x += io.MouseDelta.x;
            viewer.pan.y += io.MouseDelta.y;
        } else {
            viewer.swipeX += io.MouseDelta.x;
        }
    }
    if (ImGui::IsItemDeactivated()) {
        float th = areaSize.x * 0.15f;
        if (viewer.zoom <= 1.01f && viewer.swipeX > th) ViewerGo(-1);
        else if (viewer.zoom <= 1.01f && viewer.swipeX < -th) ViewerGo(1);
        else if (fabsf(ImGui::GetMouseDragDelta(0, 0).x) < 3 && fabsf(ImGui::GetMouseDragDelta(0, 0).y) < 3 && io.MouseClickedLastCount[0] < 2)
            viewer.uiAlpha = viewer.uiAlpha > 0.5f ? 0.0f : 1.0f;  // tap toggles the toolbar
    }
    if (hovered && ImGui::IsMouseDoubleClicked(0)) {
        if (viewer.zoom > 1.01f) { viewer.zoom = 1; viewer.pan = ImVec2(0, 0); }
        else ViewerZoomAt(io.MousePos, 2.5f);
        viewer.uiAlpha = 1.0f;
    }
    if (!active) viewer.swipeX *= powf(0.0005f, io.DeltaTime);  // spring back / slide in
    if (fabsf(viewer.swipeX) < 0.5f) viewer.swipeX = 0;

    // keyboard
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_PageDown) || ImGui::IsKeyPressed(ImGuiKey_Space)) ViewerGo(1);
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_PageUp) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) ViewerGo(-1);
    if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) ViewerZoomAt(c, 1.25f);
    if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) ViewerZoomAt(c, 0.8f);
    if (ImGui::IsKeyPressed(ImGuiKey_0) || ImGui::IsKeyPressed(ImGuiKey_Keypad0)) { viewer.zoom = 1; viewer.pan = ImVec2(0, 0); }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_AppBack)) close = true;

    // ---- image
    const ImageTex& full = Images().Get(path, true);
    const ImageTex& thumb = Images().Get(path, false);
    if (viewer.index + 1 < n) Images().Prefetch(viewer.paths[(size_t)viewer.index + 1], true);
    if (viewer.index > 0) Images().Prefetch(viewer.paths[(size_t)viewer.index - 1], true);
    const ImageTex* show = full.state == ImageTex::Ready ? &full : (thumb.state == ImageTex::Ready ? &thumb : nullptr);
    float fit = 1;
    if (show) {
        fit = std::min(areaSize.x / (float)show->w, areaSize.y / (float)show->h);
        float dw = (float)show->w * fit * viewer.zoom, dh = (float)show->h * fit * viewer.zoom;
        // keep the picture on screen
        float mx = std::max(0.0f, (dw - areaSize.x) * 0.5f), my = std::max(0.0f, (dh - areaSize.y) * 0.5f);
        viewer.pan.x = std::clamp(viewer.pan.x, -mx, mx);
        viewer.pan.y = std::clamp(viewer.pan.y, -my, my);
        ImVec2 ic(c.x + viewer.pan.x + viewer.swipeX, c.y + viewer.pan.y);
        dl->AddImage(show->Id(), ImVec2(ic.x - dw * 0.5f, ic.y - dh * 0.5f), ImVec2(ic.x + dw * 0.5f, ic.y + dh * 0.5f));
        // neighbours peek in while swiping
        if (viewer.swipeX != 0) {
            int ni = viewer.swipeX > 0 ? viewer.index - 1 : viewer.index + 1;
            if (ni >= 0 && ni < n) {
                const ImageTex& nt = Images().Get(viewer.paths[(size_t)ni], false);
                if (nt.state == ImageTex::Ready) {
                    float nf = std::min(areaSize.x / (float)nt.w, areaSize.y / (float)nt.h);
                    float nw = (float)nt.w * nf, nh = (float)nt.h * nf;
                    float ncx = c.x + viewer.swipeX + (viewer.swipeX > 0 ? -areaSize.x : areaSize.x);
                    dl->AddImage(nt.Id(), ImVec2(ncx - nw * 0.5f, c.y - nh * 0.5f), ImVec2(ncx + nw * 0.5f, c.y + nh * 0.5f));
                }
            }
        }
    }
    if (full.state != ImageTex::Ready) {
        const char* msg = full.state == ImageTex::Failed ? "Cannot display this picture" : "Loading...";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(c.x - ts.x * 0.5f, viewer.areaMax.y - ts.y * 3), IM_COL32(220, 220, 220, 255), msg);
    }

    // ---- overlay controls
    float a = viewer.uiAlpha;
    if (a > 0.01f) {
        float bh = mobile ? ui::M().rowH : ImGui::GetFrameHeight() * 1.3f;
        float barH = bh + ImGui::GetStyle().ItemSpacing.y * 2;
        dl->AddRectFilled(viewer.areaMin, ImVec2(viewer.areaMax.x, viewer.areaMin.y + barH), IM_COL32(0, 0, 0, (int)(150 * a)));
        ImGui::SetCursorScreenPos(ImVec2(viewer.areaMin.x + ImGui::GetStyle().ItemSpacing.x, viewer.areaMin.y + ImGui::GetStyle().ItemSpacing.y));
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(40, 40, 46, 200));
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(240, 240, 240, 255));
        if (ui::IconButton("vclose", Icon::Back, "Close (Esc)", bh)) close = true;
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted(BaseName(path).c_str());
        char info[256];
        snprintf(info, sizeof(info), "%d / %d   %s%s", viewer.index + 1, n, viewer.index < (int)viewer.captions.size() ? viewer.captions[(size_t)viewer.index].c_str() : "",
                 full.state == ImageTex::Ready ? ("   " + std::to_string(full.srcW) + "x" + std::to_string(full.srcH)).c_str() : "");
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.75f, 1), "%s", info);
        ImGui::EndGroup();
        // action buttons: top-right on desktop, bottom bar on phones
        int nb = mobile ? 4 : 6;
        if (mobile) {
            float by = viewer.areaMax.y - barH;
            dl->AddRectFilled(ImVec2(viewer.areaMin.x, by), viewer.areaMax, IM_COL32(0, 0, 0, (int)(150 * a)));
            float gap = (areaSize.x - bh * (float)nb) / (float)(nb + 1);
            ImGui::SetCursorScreenPos(ImVec2(viewer.areaMin.x + gap, by + ImGui::GetStyle().ItemSpacing.y));
            if (ui::IconButton("zfit", Icon::Fit, nullptr, bh)) { viewer.zoom = 1; viewer.pan = ImVec2(0, 0); }
            ImGui::SameLine(0, gap);
        } else {
            float right = viewer.areaMax.x - (bh + ImGui::GetStyle().ItemSpacing.x) * (float)nb - ImGui::GetStyle().ItemSpacing.x;
            ImGui::SameLine(std::max(ImGui::GetCursorPosX(), right - ImGui::GetWindowPos().x));
            if (ui::IconButton("zout", Icon::ZoomOut, "Zoom out (-)", bh)) ViewerZoomAt(c, 0.8f);
            ImGui::SameLine();
            if (ui::IconButton("zfit", Icon::Fit, "Fit to screen (0)", bh)) { viewer.zoom = 1; viewer.pan = ImVec2(0, 0); }
            ImGui::SameLine();
            if (ui::IconButton("zin", Icon::ZoomIn, "Zoom in (+)", bh)) ViewerZoomAt(c, 1.25f);
            ImGui::SameLine();
        }
        float sameGap = mobile ? (areaSize.x - bh * (float)nb) / (float)(nb + 1) : -1.0f;
        if (ui::IconButton("vopen", Icon::Open, "Open with default app", bh)) {
            if (Bridge().openFile) Bridge().openFile(path); else OpenWithDefaultApp(path);
        }
        ImGui::SameLine(0, sameGap);
        if (ui::IconButton("vshare", Icon::Share, "Share with another device", bh)) {
            pendingSend = {path};
            openSendTo = true;
            close = true;
        }
        ImGui::SameLine(0, sameGap);
        if (ui::IconButton("vall", viewer.all ? Icon::Users : Icon::Image, viewer.all ? "Showing all shared pictures - tap for this chat only" : "Showing this chat - tap for all shared pictures", bh)) {
            viewer.all = !viewer.all;
            RebuildViewerList();
            if (viewer.paths.empty()) close = true;
        }
        // zoom readout
        char z[32];
        snprintf(z, sizeof(z), "%.0f%%", viewer.zoom * fit * 100.0f);
        ImVec2 zs = ImGui::CalcTextSize(z);
        dl->AddText(ImVec2(viewer.areaMax.x - zs.x - 12, viewer.areaMin.y + barH + 6), IM_COL32(220, 220, 220, (int)(200 * a)), z);
        // prev / next arrows at the edges
        float aw = mobile ? bh : bh * 1.2f;
        if (viewer.index > 0) {
            ImGui::SetCursorScreenPos(ImVec2(viewer.areaMin.x + 8, c.y - aw * 0.5f));
            if (ui::IconButton("vprev", Icon::Back, "Previous (Left)", aw)) ViewerGo(-1);
        }
        if (viewer.index + 1 < n) {
            ImGui::SetCursorScreenPos(ImVec2(viewer.areaMax.x - aw - 8, c.y - aw * 0.5f));
            ImGui::PushID("next");
            bool nx = ImGui::Button("##n", ImVec2(aw, aw));
            ImVec2 mn = ImGui::GetItemRectMin(), mxp = ImGui::GetItemRectMax();
            ImVec2 cc((mn.x + mxp.x) * 0.5f, (mn.y + mxp.y) * 0.5f);
            float h = aw * 0.275f, t = std::max(1.5f, aw * 0.55f * 0.11f) * 1.3f;
            ImVec2 pts[3] = {ImVec2(cc.x - h * 0.35f, cc.y - h * 0.7f), ImVec2(cc.x + h * 0.35f, cc.y), ImVec2(cc.x - h * 0.35f, cc.y + h * 0.7f)};
            dl->AddPolyline(pts, 3, IM_COL32(240, 240, 240, 255), 0, t);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Next (Right)");
            ImGui::PopID();
            if (nx) ViewerGo(1);
        }
        ImGui::PopStyleColor(2);
    }
    if (close) {
        viewer.open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
