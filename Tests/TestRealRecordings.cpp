// Detection accuracy on real, hand-annotated drum recordings (optional).
//
// Put files in Tests/data as   <name>.<instrument>.wav   +   <name>.<instrument>.txt
// where <instrument> is kick, snare, tom1..tom4, hihat, crash, ride, china or splash, and the
// .txt file lists one onset time in seconds per line (as exported by e.g. Audacity/Sonic Visualiser
// label tracks: only the first number on each line is used). Without data the test is skipped.
#include "TestFramework.h"
#include "TestSignals.h"

#include "Triggers.h"

#include <filesystem>
#include <fstream>
#include <cstring>
#include <sstream>

using namespace thruma;
namespace fs = std::filesystem;

namespace
{
    /** Minimal RIFF/WAVE reader: PCM 16/24/32-bit and IEEE float 32, any channel count (mixed to mono). */
    bool readWav (const fs::path& p, std::vector<float>& out, double& sr)
    {
        std::ifstream f (p, std::ios::binary);
        if (! f) return false;
        auto rd32 = [&f] { uint32_t v = 0; f.read (reinterpret_cast<char*> (&v), 4); return v; };
        auto rd16 = [&f] { uint16_t v = 0; f.read (reinterpret_cast<char*> (&v), 2); return v; };
        char id[4];
        f.read (id, 4); rd32(); char wave[4]; f.read (wave, 4);
        if (std::string (id, 4) != "RIFF" || std::string (wave, 4) != "WAVE") return false;
        uint16_t fmt = 0, ch = 0, bits = 0;
        while (f.read (id, 4))
        {
            const uint32_t size = rd32();
            const std::string chunk (id, 4);
            if (chunk == "fmt ")
            {
                fmt = rd16(); ch = rd16(); sr = rd32(); rd32(); rd16(); bits = rd16();
                f.seekg (size - 16, std::ios::cur);
                if (fmt == 0xFFFE) fmt = (bits == 32) ? 3 : 1;   // WAVE_FORMAT_EXTENSIBLE (assume PCM/float by depth)
            }
            else if (chunk == "data")
            {
                const int bps = bits / 8;
                const size_t frames = size / (size_t) (bps * ch);
                out.assign (frames, 0.0f);
                std::vector<char> buf (size);
                f.read (buf.data(), size);
                for (size_t i = 0; i < frames; ++i)
                {
                    float acc = 0.0f;
                    for (int c = 0; c < ch; ++c)
                    {
                        const char* s = buf.data() + (i * ch + (size_t) c) * (size_t) bps;
                        float v = 0.0f;
                        if (fmt == 3 && bits == 32) std::memcpy (&v, s, 4);
                        else if (bits == 16) { int16_t x; std::memcpy (&x, s, 2); v = x / 32768.0f; }
                        else if (bits == 24) { int32_t x = (uint8_t) s[0] | ((uint8_t) s[1] << 8) | ((int8_t) s[2] << 16); v = x / 8388608.0f; }
                        else if (bits == 32) { int32_t x; std::memcpy (&x, s, 4); v = (float) (x / 2147483648.0); }
                        acc += v;
                    }
                    out[i] = acc / ch;
                }
                return true;
            }
            else f.seekg (size + (size & 1), std::ios::cur);
        }
        return false;
    }

    Instrument instrumentFromTag (const std::string& t)
    {
        static const std::pair<const char*, Instrument> map[] = {
            { "kick", Instrument::Kick }, { "snare", Instrument::Snare }, { "tom1", Instrument::Tom1 }, { "tom2", Instrument::Tom2 },
            { "tom3", Instrument::Tom3 }, { "tom4", Instrument::Tom4 }, { "hihat", Instrument::HiHat }, { "crash", Instrument::Crash },
            { "ride", Instrument::Ride }, { "china", Instrument::China }, { "splash", Instrument::Splash } };
        for (auto& [k, v] : map) if (t == k) return v;
        return Instrument::Count;
    }
}

TEST_CASE ("Real recordings: detection accuracy on annotated takes in Tests/data (optional)")
{
    const fs::path dir (THRUMA_TEST_DATA_DIR);
    int files = 0;
    if (fs::exists (dir))
    {
        for (auto& e : fs::directory_iterator (dir))
        {
            if (e.path().extension() != ".wav") continue;
            const auto stem = e.path().stem().string();            // name.instrument
            const auto dot = stem.rfind ('.');
            if (dot == std::string::npos) continue;
            const auto inst = instrumentFromTag (stem.substr (dot + 1));
            auto labels = e.path(); labels.replace_extension (".txt");
            if (inst == Instrument::Count || ! fs::exists (labels)) continue;

            std::vector<float> audio;
            double sr = 0.0;
            if (! readWav (e.path(), audio, sr)) { CHECK_MSG (false, "unreadable " + e.path().string()); continue; }
            std::vector<int64_t> truth;
            std::ifstream lf (labels);
            std::string line;
            while (std::getline (lf, line))
            {
                std::istringstream ss (line);
                double t;
                if (ss >> t) truth.push_back ((int64_t) std::llround (t * sr));
            }

            DetectorSettings ds;
            ds.instrument = inst;
            const auto trig = analyseTake (audio.data(), (int64_t) audio.size(), sr, ds);
            std::vector<int64_t> det;
            for (auto& t : trig) det.push_back (t.position);
            const auto sc = sig::scoreOnsets (det, truth, sr, 10.0);
            std::printf ("    %-40s P=%.3f R=%.3f F1=%.3f  timing mean=%.2f ms\n", e.path().filename().string().c_str(),
                         sc.precision(), sc.recall(), sc.f1(), sc.meanAbsErrMs);
            CHECK (sc.f1() >= 0.8);
            ++files;
        }
    }
    if (files == 0) std::printf ("    (no annotated recordings in %s - skipped)\n", dir.string().c_str());
}
