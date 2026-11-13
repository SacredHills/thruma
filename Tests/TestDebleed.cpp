#include "TestFramework.h"
#include "TestSignals.h"

#include "Debleed.h"

using namespace thruma;

namespace
{
    std::vector<float> runDebleed (const sig::Buf& in, const sig::Buf* ref, double sr, const DebleedSettings& ds,
                                   int block = 512, int* latencyOut = nullptr, const DebleedModel* model = nullptr)
    {
        DebleedProcessor p;
        p.prepare (sr, 1);
        p.setSettings (ds);
        if (model != nullptr) p.adoptModel (*model);
        std::vector<float> out = in;
        for (size_t s = 0; s < in.size(); s += (size_t) block)
        {
            const int n = (int) std::min<size_t> ((size_t) block, in.size() - s);
            float* io[1] = { out.data() + s };
            p.process (io, 1, ref != nullptr ? ref->data() + s : nullptr, n);
        }
        if (latencyOut != nullptr) *latencyOut = p.getLatency();
        // compensate latency so out[i] aligns with in[i]
        const int L = p.getLatency();
        std::vector<float> aligned (out.size(), 0.0f);
        for (size_t i = 0; i + (size_t) L < out.size(); ++i) aligned[i] = out[i + (size_t) L];
        return aligned;
    }

    struct Metrics
    {
        double bleedReductionDb = 0, sdrInDb = 0, sdrOutDb = 0, hitEnergyChangeDb = 0, transientPeakChangeDb = 0,
               ghostEnergyChangeDb = 0, decayEnergyChangeDb = 0;
    };

    Metrics measure (const sig::Session& s, const sig::Buf& in, const sig::Buf& out, int target)
    {
        Metrics m;
        const auto& clean = s.clean[(size_t) target];
        const size_t N = in.size() - (size_t) (0.1 * s.sr);   // ignore the latency tail

        // bleed-only mask: clean target envelope 60 dB below its peak
        float pk = 0.0f;
        for (auto v : clean) pk = std::max (pk, std::abs (v));
        std::vector<char> mask (N, 0);
        float env = 0.0f;
        const float rel = std::exp (-1.0f / (0.05f * (float) s.sr));
        std::vector<float> envBuf (N);
        for (size_t i = 0; i < N; ++i) { env = std::max (std::abs (clean[i]), env * rel); envBuf[i] = env; }
        // look-ahead too: a region is bleed-only if no target energy within +-20 ms
        const auto la = (size_t) (0.02 * s.sr);
        for (size_t i = 0; i < N; ++i)
        {
            const float e = std::max (envBuf[i], envBuf[std::min (N - 1, i + la)]);
            mask[i] = e < pk * 0.001f ? 1 : 0;
        }
        double ein = 0, eout = 0, ec = 0, eerrIn = 0, eerrOut = 0;
        for (size_t i = 0; i < N; ++i)
        {
            if (mask[i]) { ein += (double) in[i] * in[i]; eout += (double) out[i] * out[i]; }
            ec += (double) clean[i] * clean[i];
            eerrIn += (double) (in[i] - clean[i]) * (in[i] - clean[i]);
            eerrOut += (double) (out[i] - clean[i]) * (out[i] - clean[i]);
        }
        m.bleedReductionDb = 10.0 * std::log10 ((ein + 1e-20) / (eout + 1e-20));
        m.sdrInDb = 10.0 * std::log10 (ec / (eerrIn + 1e-20));
        m.sdrOutDb = 10.0 * std::log10 (ec / (eerrOut + 1e-20));

        // target hit windows
        double hin = 0, hout = 0, gin = 0, gout = 0, din = 0, dout = 0, tpi = 0, tpo = 0;
        int ntp = 0;
        for (auto& e : s.eventsOf (target))
        {
            const auto a = (size_t) e.pos, w = (size_t) (0.05 * s.sr), t5 = (size_t) (0.005 * s.sr);
            if (a + w >= N) continue;
            double si = 0, so = 0;
            for (size_t i = a; i < a + w; ++i) { si += (double) clean[i] * clean[i]; so += (double) out[i] * out[i]; }
            double ci = 0;
            for (size_t i = a; i < a + w; ++i) ci += (double) in[i] * in[i];
            if (e.ghost) { gin += ci; gout += so; }
            else
            {
                hin += ci; hout += so;
                float pi = 0, po = 0;
                for (size_t i = a; i < a + t5; ++i) { pi = std::max (pi, std::abs (in[i])); po = std::max (po, std::abs (out[i])); }
                tpi += gainToDb (pi); tpo += gainToDb (po); ++ntp;
            }
            // decay window 50..200 ms, only for hits with no other target hit in that span
            bool isolated = true;
            for (auto& f : s.eventsOf (target)) if (f.pos > e.pos && f.pos < e.pos + (int64_t) (0.2 * s.sr)) isolated = false;
            if (isolated && ! e.ghost && a + (size_t) (0.2 * s.sr) < N)
                for (size_t i = a + w; i < a + (size_t) (0.2 * s.sr); ++i) { din += (double) clean[i] * clean[i]; dout += (double) out[i] * out[i]; }
        }
        m.hitEnergyChangeDb = 10.0 * std::log10 ((hout + 1e-20) / (hin + 1e-20));
        m.ghostEnergyChangeDb = 10.0 * std::log10 ((gout + 1e-20) / (gin + 1e-20));
        m.transientPeakChangeDb = ntp > 0 ? (tpo - tpi) / ntp : 0.0;
        m.decayEnergyChangeDb = 10.0 * std::log10 ((dout + 1e-20) / (din + 1e-20));
        return m;
    }

    void print (const char* label, const Metrics& m)
    {
        std::printf ("    [%s]\n", label);
        tf::metric ("bleed reduction (bleed-only regions)", m.bleedReductionDb, "dB");
        tf::metric ("SDR in -> out", m.sdrInDb, "dB");
        tf::metric ("   ... out", m.sdrOutDb, "dB");
        tf::metric ("target hit energy change (0-50 ms)", m.hitEnergyChangeDb, "dB");
        tf::metric ("transient peak change (0-5 ms)", m.transientPeakChangeDb, "dB");
        tf::metric ("ghost-note energy change", m.ghostEnergyChangeDb, "dB");
        tf::metric ("decay energy vs clean (50-200 ms)", m.decayEnergyChangeDb, "dB");
    }

    const sig::Session& session()
    {
        static const sig::Session s = [] { sig::SessionOptions o; o.seconds = 12.0; o.seed = 3; return sig::makeMetalSession (o); }();
        return s;
    }
}

TEST_CASE ("Debleed: Amount 0 is a perfect (latency-compensated) null")
{
    const auto& s = session();
    DebleedSettings ds;
    ds.amount = 0.0f;
    const auto out = runDebleed (s.mics[sig::Snare], nullptr, s.sr, ds);
    double maxErr = 0.0;
    for (size_t i = 0; i + 10000 < out.size(); ++i)
        maxErr = std::max (maxErr, (double) std::abs (out[i] - s.mics[sig::Snare][i]));
    tf::metric ("max reconstruction error", maxErr);
    CHECK (maxErr < 2e-5);
}

TEST_CASE ("Debleed: blind mode on a snare mic reduces bleed and keeps the drum")
{
    const auto& s = session();
    DebleedSettings ds;
    ds.instrument = Instrument::Snare;
    const auto out = runDebleed (s.mics[sig::Snare], nullptr, s.sr, ds);
    const auto m = measure (s, s.mics[sig::Snare], out, sig::Snare);
    print ("snare, blind", m);
    CHECK (m.bleedReductionDb >= 10.0);
    CHECK (m.sdrOutDb >= m.sdrInDb + 2.0);   // bleed overlapping the hits themselves cannot be removed blind
    CHECK (m.transientPeakChangeDb > -1.0);
    CHECK (std::abs (m.hitEnergyChangeDb) < 1.5);
    CHECK (m.ghostEnergyChangeDb > -4.0);
    CHECK (m.decayEnergyChangeDb > -4.0);
}

TEST_CASE ("Debleed: reference mic improves separation (hi-hat mic as reference for the snare)")
{
    const auto& s = session();
    DebleedSettings ds;
    ds.instrument = Instrument::Snare;
    const auto blind = measure (s, s.mics[sig::Snare], runDebleed (s.mics[sig::Snare], nullptr, s.sr, ds), sig::Snare);
    ds.useReference = true;
    const auto withRef = measure (s, s.mics[sig::Snare], runDebleed (s.mics[sig::Snare], &s.mics[sig::Hat], s.sr, ds), sig::Snare);
    print ("snare, hat reference", withRef);
    CHECK (withRef.bleedReductionDb >= 10.0);
    CHECK (withRef.sdrOutDb >= blind.sdrOutDb - 0.5);
    CHECK (withRef.transientPeakChangeDb > -1.0);
}

TEST_CASE ("Debleed: tom mic with -10 dB snare bleed (after Analyze)")
{
    // Toms play only in the final fill; Analyze learns the tom's fingerprint and level from the take
    // so the bleed hits before it are not mistaken for toms.
    const auto& s = session();
    const auto a = analyseDebleed (s.mics[sig::Tom].data(), (int) s.length, {}, s.sr, Instrument::Tom3, 0.5f);
    DebleedSettings ds;
    ds.instrument = Instrument::Tom3;
    const auto out = runDebleed (s.mics[sig::Tom], nullptr, s.sr, ds, 512, nullptr, &a.model);
    const auto m = measure (s, s.mics[sig::Tom], out, sig::Tom);
    print ("tom, blind + analysed model", m);
    CHECK (m.bleedReductionDb >= 10.0);
    CHECK (m.transientPeakChangeDb > -1.0);
    CHECK (std::abs (m.hitEnergyChangeDb) < 1.5);
}

TEST_CASE ("Debleed: phase coherence - processed hits stay time-aligned with the source")
{
    const auto& s = session();
    DebleedSettings ds;
    ds.instrument = Instrument::Snare;
    const auto out = runDebleed (s.mics[sig::Snare], nullptr, s.sr, ds);
    const auto& clean = s.clean[sig::Snare];
    int maxLagErr = 0;
    for (auto& e : s.eventsOf (sig::Snare, false))
    {
        const auto a = (size_t) e.pos, w = (size_t) (0.03 * s.sr);
        if (a + w + 64 >= out.size()) continue;
        double best = -1e30;
        int bestLag = 0;
        for (int lag = -32; lag <= 32; ++lag)
        {
            double c = 0;
            for (size_t i = a; i < a + w; ++i) c += (double) out[(size_t) ((int64_t) i + lag)] * clean[i];
            if (c > best) { best = c; bestLag = lag; }
        }
        maxLagErr = std::max (maxLagErr, std::abs (bestLag));
    }
    tf::metric ("max cross-correlation lag vs clean source", maxLagErr, "samples");
    CHECK (maxLagErr == 0);
}

TEST_CASE ("Debleed: block-size independent")
{
    const auto& s = session();
    DebleedSettings ds;
    ds.instrument = Instrument::Snare;
    const auto a = runDebleed (s.mics[sig::Snare], nullptr, s.sr, ds, 64);
    const auto b = runDebleed (s.mics[sig::Snare], nullptr, s.sr, ds, 1999);
    double maxDiff = 0.0;
    for (size_t i = 0; i < a.size(); ++i) maxDiff = std::max (maxDiff, (double) std::abs (a[i] - b[i]));
    CHECK (maxDiff == 0.0);
}

TEST_CASE ("Debleed: analysis detects the right reference mic and learns decay times")
{
    const auto& s = session();
    sig::Rng r (99);
    sig::Buf noise (s.mics[0].size());
    for (auto& v : noise) v = r.uni() * 0.1f;
    const std::vector<const float*> cands { s.mics[sig::Kick].data(), s.mics[sig::Hat].data(), noise.data() };
    const auto a = analyseDebleed (s.mics[sig::Snare].data(), (int) s.length, cands, s.sr, Instrument::Snare, 0.5f);
    for (size_t i = 0; i < a.referenceScores.size(); ++i)
        tf::metric (("reference score #" + std::to_string (i)).c_str(), a.referenceScores[i]);
    tf::metric ("hits found", a.numHits);
    tf::metric ("bleed level vs hits", a.bleedLevelDb, "dB");
    CHECK (a.bestReference == 1);
    CHECK (a.referenceScores[2] < 0.05f);
    CHECK (a.model.hasTransfer);
    // decay of the synthetic snare body (~60-120 ms time constants)
    const int k200 = (int) (200.0 * a.model.fftSize / s.sr);
    tf::metric ("learned decay tau @200 Hz", a.model.decayTau[(size_t) k200], "s");
    CHECK (a.model.decayTau[(size_t) k200] > 0.03f && a.model.decayTau[(size_t) k200] < 0.4f);

    // processing with the analysed model must be at least as good as without
    DebleedSettings ds;
    ds.instrument = Instrument::Snare;
    ds.useReference = true;
    const auto withModel = measure (s, s.mics[sig::Snare], runDebleed (s.mics[sig::Snare], &s.mics[sig::Hat], s.sr, ds, 512, nullptr, &a.model), sig::Snare);
    print ("snare, hat reference + analysed model", withModel);
    CHECK (withModel.bleedReductionDb >= 10.0);
    CHECK (withModel.transientPeakChangeDb > -1.0);
}
