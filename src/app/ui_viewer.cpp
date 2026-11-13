// Chat picture thumbnails and the full-screen picture viewer (swipe / arrows to browse, pinch / wheel to zoom).
#include <algorithm>
#include <cmath>

#include "emoji_host.h"
#include "image_cache.h"
#include "i18n.h"
#include "media_host.h"
#include "player_host.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "platform.h"
#include "ui.h"
#include "ui_widgets.h"

using namespace ls;
using ui::Icon;

static bool Viewable(const std::string& path) {
    return ImageCache::IsImagePath(path) || (Player().Available() && PlayerHost::IsVideoPath(path));
}

static bool ViewablePath(const Transfer& t) {
    if (!Viewable(t.rel.empty() ? t.name : t.rel)) return false;
    if (t.outgoing) return t.state != TState::Cancelled && FileExists(t.localPath);
    return t.state == TState::Completed && FileExists(t.localPath);
}

// picture for a chat thumbnail: the image itself, or a frame from a video (via the player module)
struct ThumbRef {
    ImTextureID id = 0;
    int w = 0, h = 0;
    bool ready = false, failed = false, video = false;
    double duration = 0;
};
static ThumbRef ThumbFor(const std::string& path) {
    ThumbRef r;
    if (PlayerHost::IsVideoPath(path)) {
        const auto& m = Player().GetMeta(path);
        r.video = true;
        r.duration = m.info.duration;
        r.failed = m.done && !m.ok;
        if (m.tex) {
            r.id = (ImTextureID)(intptr_t)m.tex;
            r.w = m.tw;
            r.h = m.th;
            r.ready = true;
        } else if (m.done && m.ok) {
            r.w = std::max(1, m.info.width);
            r.h = std::max(1, m.info.height);
        }
        return r;
    }
    const ImageTex& t = Images().Get(path, false);
    r.ready = t.state == ImageTex::Ready;
    r.failed = t.state == ImageTex::Failed;
    if (r.ready) {
        r.id = t.Id();
        r.w = t.w;
        r.h = t.h;
    }
    return r;
}

static std::string Clock(double s) {
    int t = (int)(s + 0.5);
    char b[32];
    if (t >= 3600) snprintf(b, sizeof(b), "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else snprintf(b, sizeof(b), "%d:%02d", t / 60, t % 60);
    return b;
}

// play triangle + length on top of a video thumbnail
static void VideoBadge(ImDrawList* dl, ImVec2 p, ImVec2 p1, double duration) {
    ImVec2 c((p.x + p1.x) * 0.5f, (p.y + p1.y) * 0.5f);
    float r = std::min(p1.x - p.x, p1.y - p.y) * 0.16f;
    r = std::clamp(r, 12.0f, ImGui::GetFontSize() * 2.2f);
    dl->AddCircleFilled(c, r, IM_COL32(0, 0, 0, 150));
    dl->AddTriangleFilled(ImVec2(c.x - r * 0.35f, c.y - r * 0.5f), ImVec2(c.x + r * 0.55f, c.y), ImVec2(c.x - r * 0.35f, c.y + r * 0.5f), IM_COL32_WHITE);
    if (duration > 0) {
        std::string d = Clock(duration);
        ImVec2 ts = ImGui::CalcTextSize(d.c_str());
        ImVec2 a(p1.x - ts.x - 10, p1.y - ts.y - 6);
        dl->AddRectFilled(ImVec2(a.x - 4, a.y - 2), ImVec2(p1.x - 6, p1.y - 4), IM_COL32(0, 0, 0, 160), 4);
        dl->AddText(a, IM_COL32_WHITE, d.c_str());
    }
}

// ---------------------------------------------------------------- audio files: inline player rows
void App::DrawAudioItems(const std::vector<std::shared_ptr<Transfer>>& items, float width) {
    if (!Player().Available()) return;
    for (auto& t : items) {
        const std::string& path = t->localPath;
        bool ready = t->outgoing ? t->state != TState::Cancelled : t->state == TState::Completed;
        if (!PlayerHost::IsAudioPath(path) || !ready || !FileExists(path)) continue;
        ImGui::PushID(path.c_str());
        const auto& meta = Player().GetMeta(path);
        bool cur = Player().IsCurrent(path);
        bool playing = cur && Player().Playing();
        float h = ui::M().mobile ? ui::M().rowH : ImGui::GetFrameHeight() * 1.4f;
        if (ui::IconButton("play", playing ? Icon::Pause : Icon::Play, playing ? T("Pause") : T("Play"), h)) Player().Toggle(path);
        ImGui::SameLine();
        double dur = cur && Player().Duration() > 0 ? Player().Duration() : meta.info.duration;
        double pos = cur ? Player().Position() : 0;
        float barW = std::max(40.0f, width - h * 2 - ImGui::GetStyle().ItemSpacing.x * 2);
        ImGui::BeginGroup();
        // progress bar you can tap / drag to seek
        ImVec2 p = ImGui::GetCursorScreenPos();
        float bh = h * 0.45f;
        ImGui::InvisibleButton("seek", ImVec2(barW, bh));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float frac = dur > 0 ? (float)std::clamp(pos / dur, 0.0, 1.0) : 0;
        if ((ImGui::IsItemActive() || ImGui::IsItemClicked()) && dur > 0) {
            float f = std::clamp((ImGui::GetIO().MousePos.x - p.x) / barW, 0.0f, 1.0f);
            if (!cur) Player().Play(path);
            Player().Seek(f * dur);
            frac = f;
        }
        float cy = p.y + bh * 0.5f;
        dl->AddRectFilled(ImVec2(p.x, cy - 3), ImVec2(p.x + barW, cy + 3), ImGui::GetColorU32(ImGuiCol_Text, 0.25f), 3);
        dl->AddRectFilled(ImVec2(p.x, cy - 3), ImVec2(p.x + barW * frac, cy + 3), ImGui::GetColorU32(ImGuiCol_SliderGrabActive), 3);
        dl->AddCircleFilled(ImVec2(p.x + barW * frac, cy), 6, ImGui::GetColorU32(ImGuiCol_SliderGrabActive));
        std::string label = (cur ? Clock(pos) + " / " : "") + (dur > 0 ? Clock(dur) : std::string("--:--")) + "   " + (BaseName(path).rfind("Voice message", 0) == 0 ? std::string(T("Voice message")) : BaseName(path));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + barW);
        ImGui::TextDisabled("%s", label.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        ImGui::SameLine();
        bool lp = eng.settings.playerLoop;
        ui::PushAccent(lp);
        if (ui::IconButton("loop", Icon::Loop, lp ? T("Loop: on") : T("Loop: off"), h * 0.8f)) ToggleLoop();
        ui::PopAccent();
        ImGui::BeginGroup();
        if (meta.done && !meta.ok) ImGui::TextDisabled("%s", T("can't play this file here"));
        ImGui::EndGroup();
        ImGui::PopID();
    }
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
        ThumbRef t = ThumbFor(imgs[0]->localPath);
        float maxH = ImGui::GetFontSize() * (ui::M().mobile ? 16.0f : 13.0f);
        ImVec2 size(width, maxH * 0.6f);
        if (t.w > 0 && t.h > 0) {
            float aspect = (float)t.w / (float)t.h;
            size.y = std::min(maxH, width / aspect);
            size.x = size.y * aspect;
            if (size.x > width) { size.x = width; size.y = width / aspect; }
        }
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool hovered = clickable(imgs[0]->localPath, size);
        ImVec2 p1(p.x + size.x, p.y + size.y);
        if (t.ready) dl->AddImageRounded(t.id, p, p1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, rounding);
        else dl->AddRectFilled(p, p1, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
        if (!t.ready && !t.failed && !t.video) dl->AddText(ImVec2(p.x + 8, p.y + 8), ui::ColMuted(), "loading...");
        if (t.failed) dl->AddText(ImVec2(p.x + 8, p.y + 8), ui::ColMuted(), t.video ? T("can't play this video here") : T("preview not available"));
        if (t.video && !t.failed) VideoBadge(dl, p, p1, t.duration);
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
        ThumbRef t = ThumbFor(imgs[(size_t)i]->localPath);
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool hovered = clickable(imgs[(size_t)i]->localPath, ImVec2(cell, cell));
        ImVec2 p1(p.x + cell, p.y + cell);
        if (t.ready) {
            ImVec2 uv0(0, 0), uv1(1, 1);
            if (t.w > t.h) { float f = (float)t.h / (float)t.w; uv0.x = (1 - f) * 0.5f; uv1.x = 1 - uv0.x; }
            else { float f = (float)t.w / (float)t.h; uv0.y = (1 - f) * 0.5f; uv1.y = 1 - uv0.y; }
            dl->AddImageRounded(t.id, p, p1, uv0, uv1, IM_COL32_WHITE, rounding);
        } else {
            dl->AddRectFilled(p, p1, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
        }
        if (t.video) VideoBadge(dl, p, p1, t.duration);
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
void App::OpenViewerList(const std::vector<std::string>& paths, int index, const std::vector<std::string>& captions) {
    if (paths.empty()) return;
    viewer.fixed = true;
    viewer.canDelete = false;
    viewer.comic.clear();
    viewer.rtl = false;
    viewer.together = false;
    viewer.slideshow = false;
    viewer.peer.clear();
    viewer.paths = paths;
    viewer.captions = captions;
    viewer.captions.resize(paths.size());
    viewer.index = std::clamp(index, 0, (int)paths.size() - 1);
    viewer.zoom = 1;
    viewer.pan = ImVec2(0, 0);
    viewer.swipeX = 0;
    viewer.open = true;
    viewer.requestOpen = true;
    viewer.uiAlpha = 1;
    if (getenv("LUNASHARE_SLIDESHOW")) viewer.slideshow = true;  // (dev: smoothness checks)
}

void App::RebuildViewerList() {
    if (viewer.fixed) return;
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
                    add(t->second->localPath, TF(m.outgoing ? "Sent to %s" : "From %s", them.c_str()) + "  -  " + FormatTime(m.ts));
                }
            }
        }
    } else {
        for (auto& h : eng.history) {
            if (!Viewable(h.path) || !FileExists(h.path)) continue;
            add(h.path, TF(h.outgoing ? "Sent to %s" : "From %s", eng.PeerName(h.peer).c_str()) + "  -  " + FormatTime(h.ts));
        }
    }
    viewer.index = 0;
    for (size_t i = 0; i < viewer.paths.size(); i++)
        if (viewer.paths[i] == current) viewer.index = (int)i;
}

void App::OpenViewer(const std::string& peer, const std::string& path, bool all) {
    viewer.fixed = false;
    viewer.canDelete = false;
    viewer.comic.clear();
    viewer.rtl = false;
    viewer.together = false;
    viewer.slideshow = false;
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
    // slide in from the side we are coming from - from where the pictures are now: the one we leave stays exactly where
    // it was (it is the neighbour now, one width away) and both move together, without a jump
    float w = viewer.areaMax.x - viewer.areaMin.x;
    viewer.swipeX = std::clamp(viewer.swipeX + w * (float)delta * (viewer.rtl ? -1.0f : 1.0f), -w, w);
    busy = true;  // (the next frame right away, not after the idle wait)
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
        if (!viewer.videoPath.empty()) {
            Player().Stop();
            viewer.videoPath.clear();
        }
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    int fwd = viewer.rtl ? -1 : 1;  // (manga: right to left - swipes and arrows the other way)
    bool mobile = ui::M().mobile;
    bool close = false;
    // pick up pictures that finished arriving while the viewer is open
    static int refresh = 0;
    if (++refresh % 30 == 0) RebuildViewerList();
    if (viewer.paths.empty()) { viewer.open = false; ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    int n = (int)viewer.paths.size();
    if (viewer.index >= n) viewer.index = n - 1;
    const std::string path = viewer.paths[(size_t)viewer.index];
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
        if (viewer.zoom <= 1.01f && viewer.swipeX > th) ViewerGo(-fwd);
        else if (viewer.zoom <= 1.01f && viewer.swipeX < -th) ViewerGo(fwd);
        else if (fabsf(ImGui::GetMouseDragDelta(0, 0).x) < 3 && fabsf(ImGui::GetMouseDragDelta(0, 0).y) < 3 && io.MouseClickedLastCount[0] < 2)
            viewer.uiAlpha = viewer.uiAlpha > 0.5f ? 0.0f : 1.0f;  // tap toggles the toolbar
    }
    if (hovered && ImGui::IsMouseDoubleClicked(0)) {
        if (viewer.zoom > 1.01f) { viewer.zoom = 1; viewer.pan = ImVec2(0, 0); }
        else ViewerZoomAt(io.MousePos, 2.5f);
        viewer.uiAlpha = 1.0f;
    }
    if (!active) {
        // spring back / slide in (the slideshow glides more slowly than a swipe)
        viewer.swipeX *= powf(viewer.slideshow ? 0.004f : 0.0005f, std::min(io.DeltaTime, 0.05f));
        if (fabsf(viewer.swipeX) < 0.5f) viewer.swipeX = 0;
        // (no touch keeps frames coming: without this the app idles at a few frames a second and the slide stutters)
        else busy = true;
    }
    // the slideshow: the next one every few seconds (any touch / key stops it)
    if (viewer.slideshow) {
        viewer.slideT += io.DeltaTime;
        if (active || ImGui::IsKeyPressed(ImGuiKey_Escape)) viewer.slideshow = false;
        else if (viewer.slideT > 3.6) busy = true;  // (frames at full rate before the change: its first step isn't late)
        if (viewer.slideshow && viewer.slideT > 4.0) {
            viewer.slideT = 0;
            if (viewer.index + 1 < (int)viewer.paths.size()) ViewerGo(1);
            else viewer.slideshow = false;
        }
    }
    if (fabsf(viewer.swipeX) < 0.5f) viewer.swipeX = 0;

    bool isVideo = PlayerHost::IsVideoPath(path);
    if (!viewer.comic.empty()) ComicTick();  // (where it's read up to)
    // videos play on the movie screen (the same one as the movie player: sound, subtitles, picture, loop, people)
    if (!viewer.videoPath.empty()) {
        Player().Stop();
        viewer.videoPath.clear();
    }
    bool playVideo = false;

    // keyboard (space plays a video, otherwise next picture)
    if (isVideo && (ImGui::IsKeyPressed(ImGuiKey_Space) || ImGui::IsKeyPressed(ImGuiKey_Enter))) playVideo = true;
    else if (ImGui::IsKeyPressed(ImGuiKey_Space)) ViewerGo(1);
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) ViewerGo(fwd);
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) ViewerGo(-fwd);
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) ViewerGo(1);
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) ViewerGo(-1);
    if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) ViewerZoomAt(c, 1.25f);
    if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) ViewerZoomAt(c, 0.8f);
    if (ImGui::IsKeyPressed(ImGuiKey_0) || ImGui::IsKeyPressed(ImGuiKey_Keypad0)) { viewer.zoom = 1; viewer.pan = ImVec2(0, 0); }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_AppBack)) close = true;

    // ---- video
    if (isVideo) {
        ImVec2 off(viewer.swipeX, 0);
        const auto& vt = Media().PlayerVideo();
        if (vt.valid) {
            Media().DrawVideo(dl, vt, ImVec2(viewer.areaMin.x + off.x, viewer.areaMin.y), ImVec2(viewer.areaMax.x + off.x, viewer.areaMax.y), false);
        } else {
            ThumbRef t = ThumbFor(path);
            if (t.ready) {
                float f = std::min(areaSize.x / (float)t.w, areaSize.y / (float)t.h);
                float dw = t.w * f, dh = t.h * f;
                dl->AddImage(t.id, ImVec2(c.x + off.x - dw * 0.5f, c.y - dh * 0.5f), ImVec2(c.x + off.x + dw * 0.5f, c.y + dh * 0.5f));
            }
        }
        // a big play button in the middle
        float r = std::min(areaSize.x, areaSize.y) * 0.09f;
        r = std::max(r, ImGui::GetFontSize() * 1.8f);
        ImVec2 pc(c.x + off.x, c.y);
        ImGui::SetCursorScreenPos(ImVec2(pc.x - r, pc.y - r));
        if (ImGui::InvisibleButton("##vplaybig", ImVec2(r * 2, r * 2))) playVideo = true;
        bool hov = ImGui::IsItemHovered();
        dl->AddCircleFilled(pc, r, IM_COL32(0, 0, 0, hov ? 170 : 130));
        dl->AddCircle(pc, r, IM_COL32(255, 255, 255, 200), 0, 2.5f);
        ui::DrawIcon(dl, Icon::Play, ImVec2(pc.x + r * 0.08f, pc.y), r * 0.9f, IM_COL32_WHITE);
        const auto& meta = Player().GetMeta(path);
        if (meta.done && meta.ok && meta.info.duration > 0) {
            std::string d = Clock(meta.info.duration);
            ImVec2 ts = ImGui::CalcTextSize(d.c_str());
            dl->AddText(ImVec2(pc.x - ts.x * 0.5f, pc.y + r * 1.25f), IM_COL32(230, 230, 230, 255), d.c_str());
        }
        if (playVideo) {
            if (eng.StartWatch("", path)) {
                watchShown = true;
                watchFromViewer = true;  // (leaving the movie comes back here)
                close = true;
            }
        }
    }
    // ---- image
    const ImageTex& full = Images().Get(isVideo ? std::string() : path, true);
    const ImageTex& thumb = Images().Get(isVideo ? std::string() : path, false);
    if (viewer.index + 1 < n && !PlayerHost::IsVideoPath(viewer.paths[(size_t)viewer.index + 1])) Images().Prefetch(viewer.paths[(size_t)viewer.index + 1], true);
    if (viewer.index > 0 && !PlayerHost::IsVideoPath(viewer.paths[(size_t)viewer.index - 1])) Images().Prefetch(viewer.paths[(size_t)viewer.index - 1], true);
    const ImageTex* show = isVideo ? nullptr : full.state == ImageTex::Ready ? &full : (thumb.state == ImageTex::Ready ? &thumb : nullptr);
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
            int ni = viewer.swipeX > 0 ? viewer.index - fwd : viewer.index + fwd;
            if (ni >= 0 && ni < n) {
                // (the sharp picture when it is loaded - the one just left always is - else its smaller copy)
                const ImageTex& nfull = Images().Get(viewer.paths[(size_t)ni], true);
                const ImageTex& nt = nfull.state == ImageTex::Ready ? nfull : Images().Get(viewer.paths[(size_t)ni], false);
                if (nt.state == ImageTex::Ready) {
                    float nf = std::min(areaSize.x / (float)nt.w, areaSize.y / (float)nt.h);
                    float nw = (float)nt.w * nf, nh = (float)nt.h * nf;
                    float ncx = c.x + viewer.swipeX + (viewer.swipeX > 0 ? -areaSize.x : areaSize.x);
                    dl->AddImage(nt.Id(), ImVec2(ncx - nw * 0.5f, c.y - nh * 0.5f), ImVec2(ncx + nw * 0.5f, c.y + nh * 0.5f));
                }
            }
        }
    }
    // neighbours peek in while swiping away from a video too
    if (isVideo && viewer.swipeX != 0) {
        int ni = viewer.swipeX > 0 ? viewer.index - fwd : viewer.index + fwd;
        if (ni >= 0 && ni < n) {
            ThumbRef nt = ThumbFor(viewer.paths[(size_t)ni]);
            if (nt.ready) {
                float nf = std::min(areaSize.x / (float)nt.w, areaSize.y / (float)nt.h);
                float nw = (float)nt.w * nf, nh = (float)nt.h * nf;
                float ncx = c.x + viewer.swipeX + (viewer.swipeX > 0 ? -areaSize.x : areaSize.x);
                dl->AddImage(nt.id, ImVec2(ncx - nw * 0.5f, c.y - nh * 0.5f), ImVec2(ncx + nw * 0.5f, c.y + nh * 0.5f));
            }
        }
    }
    if (!isVideo && full.state != ImageTex::Ready) {
        const char* msg = viewer.together && path.empty() ? T("The page is on its way...")
                        : full.state == ImageTex::Failed ? T("Cannot display this picture") : T("Loading...");
        if (viewer.together && path.empty()) busy = true;  // (it shows the moment it comes)
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
        if (ui::IconButton("vclose", Icon::Back, T("Close (Esc)"), bh)) close = true;
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted(viewer.together ? eng.read.name.c_str() : BaseName(path).c_str());
        if (viewer.together) {  // who's reading
            std::string ppl;
            for (size_t i = 0; i < eng.read.people.size(); i++)
                if (eng.read.devs.size() <= i || eng.read.devs[i] != eng.settings.deviceId)
                    ppl += (ppl.empty() ? "" : ", ") + Emoji().Display(eng.read.people[i]);
            viewer.captions[(size_t)viewer.index] = ppl.empty() ? std::string(T("reading together - waiting for them")) : TF("reading together with %s", ppl.c_str());
        }
        char info[256];
        snprintf(info, sizeof(info), "%d / %d   %s%s", viewer.index + 1, n, viewer.index < (int)viewer.captions.size() ? viewer.captions[(size_t)viewer.index].c_str() : "",
                 full.state == ImageTex::Ready ? ("   " + std::to_string(full.srcW) + "x" + std::to_string(full.srcH)).c_str() : "");
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.75f, 1), "%s", info);
        ImGui::EndGroup();
        // action buttons: top-right on desktop, bottom bar on phones
        bool comicish = !viewer.comic.empty() || viewer.together;
        std::string callKey = CallChatKey();
        bool togetherBtn = !callKey.empty();  // in a call: Together (this comic, or something else) right here
        bool shareRead = !viewer.comic.empty() && !viewer.together && !togetherBtn;  // (a comic of mine: read it with someone)
        int nb = (mobile ? 3 : 5) + (viewer.fixed ? 0 : 1) + 1 + (viewer.canDelete ? 1 : 0) + (comicish ? 1 : 0) + (shareRead ? 1 : 0) + (togetherBtn ? 1 : 0);
        if (mobile) {
            bh = std::min(bh, areaSize.x / ((float)nb + 0.6f));  // (all of them fit across: smaller when there are many)
            float by = viewer.areaMax.y - barH;
            dl->AddRectFilled(ImVec2(viewer.areaMin.x, by), viewer.areaMax, IM_COL32(0, 0, 0, (int)(150 * a)));
            float gap = (areaSize.x - bh * (float)nb) / (float)(nb + 1);
            ImGui::SetCursorScreenPos(ImVec2(viewer.areaMin.x + gap, by + ImGui::GetStyle().ItemSpacing.y));
            if (ui::IconButton("zfit", Icon::Fit, nullptr, bh)) { viewer.zoom = 1; viewer.pan = ImVec2(0, 0); }
            ImGui::SameLine(0, gap);
        } else {
            float right = viewer.areaMax.x - (bh + ImGui::GetStyle().ItemSpacing.x) * (float)nb - ImGui::GetStyle().ItemSpacing.x;
            ImGui::SameLine(std::max(ImGui::GetCursorPosX(), right - ImGui::GetWindowPos().x));
            if (ui::IconButton("zout", Icon::ZoomOut, T("Zoom out (-)"), bh)) ViewerZoomAt(c, 0.8f);
            ImGui::SameLine();
            if (ui::IconButton("zfit", Icon::Fit, T("Fit to screen (0)"), bh)) { viewer.zoom = 1; viewer.pan = ImVec2(0, 0); }
            ImGui::SameLine();
            if (ui::IconButton("zin", Icon::ZoomIn, T("Zoom in (+)"), bh)) ViewerZoomAt(c, 1.25f);
            ImGui::SameLine();
        }
        float sameGap = mobile ? (areaSize.x - bh * (float)nb) / (float)(nb + 1) : -1.0f;
        ImGui::BeginDisabled(path.empty());
        if (ui::IconButton("vopen", Icon::Open, T("Open with default app"), bh)) {
            if (Bridge().openFile) Bridge().openFile(path); else OpenWithDefaultApp(path);
        }
        ImGui::SameLine(0, sameGap);
        if (ui::IconButton("vshare", Icon::Share, T("Share with another device"), bh)) {
            pendingSend = {path};
            openSendTo = true;
            close = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0, sameGap);
        if (ui::IconButton("vslide", viewer.slideshow ? Icon::Pause : Icon::Play, viewer.slideshow ? T("Stop the slideshow") : T("Slideshow"), bh)) {
            viewer.slideshow = !viewer.slideshow;
            viewer.slideT = 0;
            if (viewer.slideshow) viewer.uiAlpha = 0;
        }
        if (shareRead) {
            ImGui::SameLine(0, sameGap);
            ui::PushAccent(!CallChatKey().empty());  // (in a call: the obvious thing to do)
            if (ui::IconButton("vreadwith", Icon::Users, T("Read together with someone"), bh)) readWhoOpen = true;
            ui::PopAccent();
        }
        ReadWhoPopup();
        if (togetherBtn) {
            ImGui::SameLine(0, sameGap);
            ui::PushAccent(TogetherInvited(callKey) || (!viewer.comic.empty() && !viewer.together));
            if (ui::IconButton("vtogether", Icon::Users, T("Together: watch, listen, read"), bh)) OpenTogether(callKey);
            ui::PopAccent();
        }
        DrawTogetherPopup();  // (here while the viewer is open: it's on top of everything)
        if (comicish) {
            ImGui::SameLine(0, sameGap);
            if (viewer.rtl) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(70, 110, 200, 255));
            if (ui::IconButton("vrtl", Icon::Book, viewer.rtl ? T("Pages right to left (manga): on") : T("Pages right to left (manga): off"), bh)) {
                viewer.rtl = !viewer.rtl;
                ComicTick();
            }
            if (viewer.rtl) ImGui::PopStyleColor();
        }
        if (viewer.canDelete) {
            ImGui::SameLine(0, sameGap);
            if (ui::IconButton("vdel", Icon::Trash, T("Delete from this device"), bh, true)) ImGui::OpenPopup("##vdel");
        }
        if (ImGui::BeginPopupModal("##vdel", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(TF("Delete %s from this device? This can't be undone.", BaseName(path).c_str()).c_str());
            if (ImGui::Button(T("Delete"), ImVec2(0, bh * 0.8f))) {
                RemoveFile(path);
                galleryRescanWanted = true;
                size_t at = (size_t)viewer.index;
                viewer.paths.erase(viewer.paths.begin() + (long)at);
                if (at < viewer.captions.size()) viewer.captions.erase(viewer.captions.begin() + (long)at);
                if (viewer.paths.empty()) close = true;
                else if (viewer.index >= (int)viewer.paths.size()) viewer.index = (int)viewer.paths.size() - 1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(T("Cancel"), ImVec2(0, bh * 0.8f))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (!viewer.fixed) ImGui::SameLine(0, sameGap);
        if (!viewer.fixed && ui::IconButton("vall", viewer.all ? Icon::Users : Icon::Image, viewer.all ? T("Showing all shared pictures - tap for this chat only") : T("Showing this chat - tap for all shared pictures"), bh)) {
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
        if (viewer.index - fwd >= 0 && viewer.index - fwd < n) {
            ImGui::SetCursorScreenPos(ImVec2(viewer.areaMin.x + 8, c.y - aw * 0.5f));
            if (ui::IconButton("vprev", Icon::Back, fwd > 0 ? T("Previous (Left)") : T("Next (Left)"), aw)) ViewerGo(-fwd);
        }
        if (viewer.index + fwd >= 0 && viewer.index + fwd < n) {
            ImGui::SetCursorScreenPos(ImVec2(viewer.areaMax.x - aw - 8, c.y - aw * 0.5f));
            ImGui::PushID("next");
            bool nx = ImGui::Button("##n", ImVec2(aw, aw));
            ImVec2 mn = ImGui::GetItemRectMin(), mxp = ImGui::GetItemRectMax();
            ImVec2 cc((mn.x + mxp.x) * 0.5f, (mn.y + mxp.y) * 0.5f);
            float h = aw * 0.275f, t = std::max(1.5f, aw * 0.55f * 0.11f) * 1.3f;
            ImVec2 pts[3] = {ImVec2(cc.x - h * 0.35f, cc.y - h * 0.7f), ImVec2(cc.x + h * 0.35f, cc.y), ImVec2(cc.x - h * 0.35f, cc.y + h * 0.7f)};
            dl->AddPolyline(pts, 3, IM_COL32(240, 240, 240, 255), 0, t);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", fwd > 0 ? T("Next (Right)") : T("Previous (Right)"));
            ImGui::PopID();
            if (nx) ViewerGo(fwd);
        }
        // video: play/pause + seek bar + time, above the bottom bar on phones
        if (isVideo && Player().IsCurrent(path)) {
            float vbh = bh;
            float y = mobile ? viewer.areaMax.y - barH - vbh - ImGui::GetStyle().ItemSpacing.y * 2 : viewer.areaMax.y - vbh - ImGui::GetStyle().ItemSpacing.y * 2;
            dl->AddRectFilled(ImVec2(viewer.areaMin.x, y - ImGui::GetStyle().ItemSpacing.y), ImVec2(viewer.areaMax.x, y + vbh + ImGui::GetStyle().ItemSpacing.y),
                              IM_COL32(0, 0, 0, (int)(130 * a)));
            ImGui::SetCursorScreenPos(ImVec2(viewer.areaMin.x + ImGui::GetStyle().ItemSpacing.x, y));
            bool playing = Player().Playing();
            if (ui::IconButton("vplay", playing ? Icon::Pause : Icon::Play, playing ? T("Pause (Space)") : T("Play (Space)"), vbh)) Player().Toggle(path);
            ImGui::SameLine();
            bool lp = eng.settings.playerLoop;
            if (lp) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(70, 110, 200, 255));
            if (ui::IconButton("vloop", Icon::Loop, lp ? T("Loop: on") : T("Loop: off"), vbh)) ToggleLoop();
            if (lp) ImGui::PopStyleColor();
            ImGui::SameLine();
            double dur = Player().Duration(), pos = Player().Position();
            std::string tl = Clock(pos) + " / " + Clock(dur);
            float tw = ImGui::CalcTextSize("00:00:00 / 00:00:00").x * (dur >= 3600 ? 1.0f : 0.7f);
            float barW = viewer.areaMax.x - ImGui::GetCursorScreenPos().x - tw - ImGui::GetStyle().ItemSpacing.x * 3;
            ImVec2 bp = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("vseek", ImVec2(std::max(20.0f, barW), vbh));
            float frac = dur > 0 ? (float)std::clamp(pos / dur, 0.0, 1.0) : 0;
            if ((ImGui::IsItemActive() || ImGui::IsItemClicked()) && dur > 0) {
                frac = std::clamp((io.MousePos.x - bp.x) / barW, 0.0f, 1.0f);
                Player().Seek(frac * dur);
                viewer.swipeX = 0;
            }
            float cy = bp.y + vbh * 0.5f;
            dl->AddRectFilled(ImVec2(bp.x, cy - 3), ImVec2(bp.x + barW, cy + 3), IM_COL32(255, 255, 255, 70), 3);
            dl->AddRectFilled(ImVec2(bp.x, cy - 3), ImVec2(bp.x + barW * frac, cy + 3), IM_COL32(120, 170, 255, 255), 3);
            dl->AddCircleFilled(ImVec2(bp.x + barW * frac, cy), vbh * 0.18f, IM_COL32(240, 240, 240, 255));
            ImGui::SameLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (vbh - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::TextUnformatted(tl.c_str());
        }
        ImGui::PopStyleColor(2);
    }
    if (viewer.closeRequest) {
        viewer.closeRequest = false;
        close = true;
    }
    if (close) {
        viewer.open = false;
        if (!viewer.videoPath.empty()) {
            Player().Stop();
            viewer.videoPath.clear();
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
