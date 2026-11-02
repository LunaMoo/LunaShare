// UI translations. Strings in the code are English; T() looks up the current language's version.
// Translations live in src/app/lang/<code>.inc as {"English", "translation"} pairs (format specifiers kept in order).
#pragma once
#include <string>
#include <vector>

// a string literal (the result is cached by address); "Label##id" keeps the "##id" part untranslated
const char* T(const char* en);
// a string built at run time (not cached)
std::string TS(const std::string& en);
// printf-style with a translated format: TF("%s wants to send", name.c_str())
std::string TF(const char* fmt, ...);
// text the core produced (status lines, call notes in the chat): known English sentences and patterns are translated
std::string TDynamic(const std::string& en);
// ImGui::Combo item lists ("a\0b\0c\0"): each item is translated
const char* TItems(const char* items);

struct Language {
    const char* code;  // "en", "pl"
    const char* name;  // in its own language
};
const std::vector<Language>& Languages();
void SetLanguage(const std::string& code);  // "" = the system's language
std::string CurrentLanguage();               // effective code
int LanguageGeneration();                    // changes whenever the language changes
