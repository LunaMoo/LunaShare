// Colour emoji: loads the optional emoji module, finds emoji (including sequences) in text and shows each one as a
// single private-use glyph that a custom ImGui font loader draws with the module. Also the emoji picker.
//
// Text in the model always holds real emoji. "Display" text (what ImGui draws, and what the chat input box holds)
// has each emoji replaced by one code point from U+F0000 up; Restore() turns it back. The clipboard hook restores
// whatever ImGui copies, so copied text has real emoji.
#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "lunashare_emoji.h"

class EmojiHost {
public:
    void Load();  // after the Android bridge is set up
    bool Available() const { return mod_ != nullptr; }
    std::string loadInfo;
    void AddToFont(ImFontAtlas* atlas);  // merges the emoji glyphs into the font added last
    void InstallClipboardHook();

    // real text -> display text (cached; returns the input itself when it has no emoji)
    const std::string& Display(const std::string& text);
    std::string Restore(const std::string& display) const;
    // 1..3 when the text is nothing but that many emoji (shown big), else 0
    int EmojiOnly(const std::string& text);

    // chat input box: pass as callback with ImGuiInputTextFlags_CallbackEdit | ImGuiInputTextFlags_CallbackAlways
    static int InputCallback(ImGuiInputTextCallbackData* d);
    int InputCursor() const { return inputCursor_; }  // byte offset of the cursor when the box was last active
    void SetInputCursor(int c) { inputCursor_ = c; }

    // Picker panel (fills the given size). Returns true and sets *picked (display form) when an emoji was chosen.
    bool DrawPicker(ImVec2 size, std::string* picked);
    // recent emoji and skin tone (0 = none, 1..5) as stored in settings
    std::string recent;  // display-independent: real emoji separated by '\n'
    int tone = 0;
    bool settingsChanged = false;

    // one emoji (real text) as display text, for icons and buttons
    std::string Glyph(const std::string& emoji);

private:
    struct Segment {
        size_t start, len;
        bool emoji;
    };
    std::vector<Segment> Segments(const std::string& s) const;
    uint32_t IdFor(const std::string& seq);
    const LseModule* mod_ = nullptr;
    void* lib_ = nullptr;
    LseHost host_{};
    std::vector<std::string> seqs_;  // glyph id -> sequence
    std::unordered_map<std::string, uint32_t> ids_;
    std::unordered_map<std::string, std::string> cache_;
    int inputCursor_ = -1;
    // picker state
    std::string search_;
    int scrollToGroup_ = -1;
    std::vector<std::string> recentList_;
    std::string recentLoaded_;
    friend struct EmojiFontLoader;
};

EmojiHost& Emoji();
