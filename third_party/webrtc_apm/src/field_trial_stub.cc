// LunaShare: WebRTC field trials are not used; every trial reads as unset.
#include "system_wrappers/include/field_trial.h"
#include <string>
namespace webrtc {
namespace field_trial {
std::string FindFullName(absl::string_view) { return std::string(); }
void InitFieldTrialsFromString(const char*) {}
const char* GetFieldTrialString() { return ""; }
bool FieldTrialsStringIsValid(absl::string_view) { return true; }
std::string MergeFieldTrialsStrings(absl::string_view first, absl::string_view) { return std::string(first); }
}  // namespace field_trial
}  // namespace webrtc
