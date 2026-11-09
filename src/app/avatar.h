// Profile pictures: turning a chosen photo into the small square JPEG that is shared with other devices.
#pragma once
#include <string>

// Crops a square out of the picture at `path` and encodes it as a JPEG of kAvatarSide x kAvatarSide.
//   cx, cy: centre of the square in the (EXIF-oriented) picture, 0..1
//   zoom:   1 = the square spans the picture's shorter side, 2 = half of it, ...
// a photo made smaller for sending: at most maxSide pixels on the long side, JPEG (no EXIF: upright, no location)
bool MakeSmallerJpeg(const std::string& path, int maxSide, const std::string& outPath);
bool MakeAvatarJpeg(const std::string& path, float cx, float cy, float zoom, std::string& jpeg, std::string* err);
constexpr int kAvatarSide = 256;
