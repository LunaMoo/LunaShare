# webrtc_apm

WebRTC's audio processing module — the echo canceller (AEC3), noise suppressor, automatic gain
control (AGC2) and high-pass filter used by Chrome — with only the files it needs.

- Source: WebRTC as carried by Telegram's tg_owt (https://github.com/desktop-app/tg_owt,
  commit 2ae9991a7f2278395f9716a2152e61b3cfed7a7a), files copied unchanged.
- Abseil headers from abseil-cpp 20240116.2 (header-only use, no abseil library is built).
- `src/field_trial_stub.cc` is ours: WebRTC field trials are not used, so every trial reads as unset.

Licenses: WebRTC (BSD-3-Clause + PATENTS), Abseil (Apache-2.0), pffft, Ooura FFT, the iSAC FFT,
spl_sqrt_floor and RNN VAD weights — see the LICENSE.* files here.
