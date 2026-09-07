// ============================================================================
// MPB_FrameClock -- per-frame duration capture and window statistics.
//
// SOURCE OF TRUTH
//   The `timeslice` parameter of MissionServer.OnUpdate, in seconds. That is
//   the engine's own frame delta; no profiler is involved, because the engine
//   profiler is stubbed on retail server builds and cannot be relied on.
//   Wall-clock elapsed within a single frame is measured separately, with
//   GetGame().GetTickTime(), by the benches that need it.
//
// WHY SAMPLES ARE BLOCKS OF FRAMES, NOT SINGLE FRAMES
//   Measured on a 1.29 Windows server: an idle DayZ server runs ~44,000
//   OnUpdate dispatches per second, so a frame lasts ~23 microseconds -- and
//   `timeslice` arrives QUANTIZED. Most individual frames report exactly 0.0
//   and the occasional frame carries the accumulated remainder. A median over
//   raw per-frame values on such a server is 0.0000 ms, and so is p95, which
//   makes every median-based comparison meaningless while the mean stays
//   perfectly sensible. (This was not a guess: the first smoke run reported
//   mean_ms=0.0226 with median_ms=0.0000 across every window.)
//
//   So one SAMPLE is a block of consecutive frames: frames are accumulated
//   until the block covers at least `block target` seconds, and the sample is
//   that block's total divided by its frame count -- a mean frame time over a
//   short, contiguous slice. Quantization averages out inside the block, and
//   the distribution of block means is what median and p95 are taken over.
//
//   The block target is a duration, not a frame count, so it adapts by itself:
//   at 44k fps a 10 ms block holds ~440 frames, while on a server running at
//   20 fps a single frame already exceeds the target and each block is one
//   frame -- exactly the raw per-frame measurement, which is the right
//   behaviour when frames are long enough to measure directly.
//
//   Mean is computed from a running sum over EVERY frame, independent of
//   blocking and of decimation, so it is exact.
//
// WHY THE SAMPLES ARE DECIMATED
//   Even blocked, a long window is a lot of samples, and keeping all of them
//   would itself become a load the bench cannot separate from the load under
//   test. Samples are held under a fixed cap by halving: once the buffer is
//   full, every second sample is dropped and the keep-stride doubles. What
//   remains is a uniform sample of the whole window rather than its first N
//   blocks, so the quantiles stay representative while memory and per-frame
//   cost stay constant.
// ============================================================================

class MPB_FrameClock
{
    static const int MAX_SAMPLES = 20000;
    // A block also closes on this many frames, so a server reporting
    // timeslice 0.0 forever cannot stall the sampler.
    static const int MAX_BLOCK_FRAMES = 65536;

    ref array<float> m_Samples;
    float m_BlockTargetSeconds;

    int   m_KeepStride;
    int   m_StrideCounter;
    int   m_TotalFrames;
    float m_TotalSeconds;
    float m_SumSeconds;
    float m_BlockSeconds;
    int   m_BlockFrames;
    int   m_BlockCount;
    int   m_BlockFramesTotal;
    bool  m_Recording;
    bool  m_Sorted;

    void MPB_FrameClock()
    {
        m_Samples = new array<float>();
        m_BlockTargetSeconds = 0.01;
        Reset();
    }

    // Survives Reset() on purpose: it is a configuration of the instrument,
    // not state of a window.
    void SetBlockTargetSeconds(float seconds)
    {
        if (seconds <= 0)
        {
            return;
        }
        m_BlockTargetSeconds = seconds;
    }

    float BlockTargetSeconds()
    {
        return m_BlockTargetSeconds;
    }

    void Reset()
    {
        m_Samples.Clear();
        m_KeepStride = 1;
        m_StrideCounter = 0;
        m_TotalFrames = 0;
        m_TotalSeconds = 0;
        m_SumSeconds = 0;
        m_BlockSeconds = 0;
        m_BlockFrames = 0;
        m_BlockCount = 0;
        m_BlockFramesTotal = 0;
        m_Recording = false;
        m_Sorted = false;
    }

    void Start()
    {
        Reset();
        m_Recording = true;
    }

    void Stop()
    {
        m_Recording = false;
    }

    bool IsRecording()
    {
        return m_Recording;
    }

    void Sample(float timeslice)
    {
        if (!m_Recording)
        {
            return;
        }

        m_TotalFrames++;
        m_TotalSeconds = m_TotalSeconds + timeslice;
        m_SumSeconds = m_SumSeconds + timeslice;

        m_BlockSeconds = m_BlockSeconds + timeslice;
        m_BlockFrames++;

        if (m_BlockSeconds < m_BlockTargetSeconds && m_BlockFrames < MAX_BLOCK_FRAMES)
        {
            return;
        }

        float blockMean = m_BlockSeconds / m_BlockFrames;
        m_BlockCount++;
        m_BlockFramesTotal = m_BlockFramesTotal + m_BlockFrames;
        m_BlockSeconds = 0;
        m_BlockFrames = 0;

        m_StrideCounter++;
        if (m_StrideCounter < m_KeepStride)
        {
            return;
        }
        m_StrideCounter = 0;

        m_Samples.Insert(blockMean);
        m_Sorted = false;

        if (m_Samples.Count() >= MAX_SAMPLES)
        {
            Halve();
        }
    }

    // Keep every second retained sample, in place, and double the stride.
    private void Halve()
    {
        int sourceCount = m_Samples.Count();
        int writeIndex = 0;
        int readIndex;
        for (readIndex = 0; readIndex < sourceCount; readIndex = readIndex + 2)
        {
            m_Samples.Set(writeIndex, m_Samples.Get(readIndex));
            writeIndex++;
        }
        m_Samples.Resize(writeIndex);
        m_KeepStride = m_KeepStride * 2;
    }

    int FrameCount()
    {
        return m_TotalFrames;
    }

    int BlockCount()
    {
        return m_BlockCount;
    }

    int SampleCount()
    {
        return m_Samples.Count();
    }

    float ElapsedSeconds()
    {
        return m_TotalSeconds;
    }

    // Average frames per block -- the amount of averaging the quantiles are
    // resting on. A reader needs it to judge them.
    float MeanBlockFrames()
    {
        if (m_BlockCount <= 0)
        {
            return 0;
        }
        float framesTotal = m_BlockFramesTotal;
        return framesTotal / m_BlockCount;
    }

    // Exact over all frames in the window, blocking and decimation aside.
    float MeanMs()
    {
        if (m_TotalFrames <= 0)
        {
            return 0;
        }
        return (m_SumSeconds / m_TotalFrames) * 1000.0;
    }

    float MeanFps()
    {
        float mean = MeanMs();
        if (mean <= 0)
        {
            return 0;
        }
        return 1000.0 / mean;
    }

    private void EnsureSorted()
    {
        if (m_Sorted)
        {
            return;
        }
        m_Samples.Sort();
        m_Sorted = true;
    }

    // Quantile over the retained (uniformly decimated) block means.
    float QuantileMs(float fraction)
    {
        int count = m_Samples.Count();
        if (count <= 0)
        {
            return 0;
        }
        EnsureSorted();

        int index = Math.Round(fraction * (count - 1));
        if (index < 0)
        {
            index = 0;
        }
        if (index > count - 1)
        {
            index = count - 1;
        }
        return m_Samples.Get(index) * 1000.0;
    }

    float MedianMs()
    {
        return QuantileMs(0.5);
    }

    float P95Ms()
    {
        return QuantileMs(0.95);
    }

    float MedianFps()
    {
        float median = MedianMs();
        if (median <= 0)
        {
            return 0;
        }
        return 1000.0 / median;
    }

    // Spread of the upper half of the distribution. Used as the scale input
    // to the median's standard error below.
    float SpreadMs()
    {
        return P95Ms() - MedianMs();
    }

    // How precisely this window pins down its own median.
    //
    // This exists because between-window drift alone is not a sufficient noise
    // floor: two windows can land on the SAME median (frame times are
    // quantized), which makes the measured drift exactly zero and would let
    // any difference at all pass as significant. The distribution's own spread
    // divided by the square root of the sample count is the floor that does
    // not collapse. It is an estimate of scale, not a confidence interval --
    // it is used to refuse claims, never to support one.
    float MedianStdErrMs()
    {
        int count = m_Samples.Count();
        if (count <= 1)
        {
            return 0;
        }
        float spread = SpreadMs();
        if (spread <= 0)
        {
            return 0;
        }
        return spread / Math.Sqrt(count);
    }

    string SummaryText()
    {
        // Step-by-step, not one expression: Enforce rejects long concatenation
        // chains with "Formula too complex".
        string text = "frames=" + m_TotalFrames;
        text = text + " blocks=" + m_BlockCount;
        text = text + " kept=" + m_Samples.Count();
        text = text + " mean_ms=" + MPB_Fmt.Ms(MeanMs());
        text = text + " med_ms=" + MPB_Fmt.Ms(MedianMs());
        text = text + " p95_ms=" + MPB_Fmt.Ms(P95Ms());
        text = text + " med_fps=" + MPB_Fmt.Fps(MedianFps());
        return text;
    }
}
