// Local test (needs an FFmpeg build): decodes an HEVC Annex B file access unit by access unit through FfDecoder.
//   test_ffdec <dir with libavcodec> <file.hevc> [expected width] [expected height]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

#include "ffdec.h"

using namespace lsv;

// split an HEVC byte stream into access units (new AU at VPS/SPS/PPS/AUD/prefix-SEI or a first slice, after a VCL NAL)
static std::vector<std::pair<size_t, size_t>> SplitHevc(const std::vector<uint8_t>& b) {
    std::vector<size_t> starts;
    for (size_t i = 0; i + 3 < b.size(); i++)
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
            starts.push_back(i > 0 && b[i - 1] == 0 ? i - 1 : i);
            i += 2;
        }
    std::vector<std::pair<size_t, size_t>> aus;
    size_t auStart = starts.empty() ? 0 : starts[0];
    bool haveVcl = false;
    for (size_t k = 0; k < starts.size(); k++) {
        size_t s = starts[k];
        size_t h = s + (b[s + 2] == 1 ? 3 : 4);
        int type = (b[h] >> 1) & 0x3f;
        bool vcl = type < 32;
        bool first = vcl && h + 2 < b.size() && (b[h + 2] & 0x80);
        bool boundary = haveVcl && ((type >= 32 && type <= 35) || type == 39 || first);
        if (boundary) {
            aus.push_back({auStart, s - auStart});
            auStart = s;
            haveVcl = false;
        }
        if (vcl) haveVcl = true;
    }
    if (!starts.empty()) aus.push_back({auStart, b.size() - auStart});
    return aus;
}

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: test_ffdec <libdir> <file.hevc> [w h]\n"); return 2; }
    std::string err;
    if (!FfDecoder::Load({argv[1]}, &err)) { printf("FAIL load: %s\n", err.c_str()); return 1; }
    std::ifstream f(argv[2], std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto aus = SplitHevc(data);
    FfDecoder d;
    if (!d.Open(kHEVC, true, &err)) { printf("FAIL open: %s\n", err.c_str()); return 1; }
    printf("decoder: %s, %zu access units\n", d.Name().c_str(), aus.size());
    int frames = 0, w = 0, h = 0, errors = 0;
    double sum = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (auto& [o, n] : aus)
        if (!d.Decode(&data[o], n, [&](const LsvImage& im) {
                frames++;
                w = im.width;
                h = im.height;
                sum += im.y[(im.height / 2) * im.y_stride + im.width / 2];
            }))
            errors++;
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("%d frames %dx%d, %d errors, %.0f fps, mean centre luma %.0f\n", frames, w, h, errors, frames / sec, frames ? sum / frames : 0);
    bool ok = frames > (int)aus.size() * 9 / 10 && errors == 0;
    if (argc >= 5) ok = ok && w == atoi(argv[3]) && h == atoi(argv[4]);
    printf(ok ? "FFDEC TEST PASSED\n" : "FFDEC TEST FAILED\n");
    return ok ? 0 : 1;
}
