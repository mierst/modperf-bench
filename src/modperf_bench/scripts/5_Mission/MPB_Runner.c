// ============================================================================
// MPB_Runner -- suite orchestrator for @modperf-bench.
//
// ARMING
//   The mod is inert unless $profile:modperf-bench/config.json exists. Mod
//   present and no config file means nothing runs, nothing is written, and
//   nothing is registered on the call queue -- that file IS the arming switch,
//   so a bench build left on a server cannot start degrading frames on its
//   own. These benches deliberately make the server slower; never arm one on a
//   populated server.
//
// PROTOCOL (per windowed run)
//   settle -> A baseline window -> apply load -> B treatment window ->
//   teardown -> A' recovery window.
//
//   A' is not decoration. Frame time on a real box drifts, and a bench that
//   only measures A and B cannot tell a load's cost from that drift. A'
//   repeats the baseline conditions after teardown: if it does not come back
//   to within tolerance of A, the run is marked invalid instead of shipping a
//   constant derived from drift. A' is also the only proof that teardown
//   actually removed the load.
//
//   Per-unit cost is the difference of window MEDIANS divided by K. Medians
//   rather than means because a single scheduler hiccup or an autosave inside
//   a window moves a mean and not a median, and the quantity being estimated
//   is the typical frame, not the total.
//
// AMPLIFICATION
//   The effects are nanoscale per unit, so the load is applied in bulk (K
//   timers, not one) and the windows are long. That is what makes a ~100 ns
//   effect visible in a frame-time distribution.
//
// TIMING SOURCES
//   Frame duration: the `timeslice` parameter of MissionServer.OnUpdate.
//   Wall clock within a frame: GetGame().GetTickTime(), in seconds.
//   No engine profiler is used; it is stubbed on retail server builds.
//
// Methodology background: https://dayz.fyi/modperf
// ============================================================================

class MPB_Config
{
    static const string PATH = "$profile:modperf-bench/config.json";

    bool   m_Present;
    string m_Note;
    string m_HardwareNote;
    float  m_SettleSeconds;
    float  m_WindowSeconds;
    float  m_BlockSeconds;
    float  m_APrimeTolerancePct;
    float  m_ModelNsTimerResidency;
    int    m_B2K;
    int    m_B5Units;
    int    m_B3Lookups;
    int    m_B3Repeats;
    string m_B3Entity;
    string m_B3FallbackEntity;
    string m_B3Slot;
    string m_B3Attachment;
    string m_B3Position;
    ref array<string> m_Benches;
    ref array<int>    m_KValues;
    ref array<int>    m_B2Periods;

    void MPB_Config()
    {
        m_Present = false;
        m_Note = "";
        m_HardwareNote = "";
        m_SettleSeconds = 120.0;
        m_WindowSeconds = 60.0;
        // One frame-time sample covers at least this much wall clock. See
        // MPB_FrameClock for why single frames are not usable as samples on a
        // fast server.
        m_BlockSeconds = 0.01;
        m_APrimeTolerancePct = 10.0;
        // Reference constant for B1 only. Every other bench reports what it
        // measured with no reference number attached -- inventing one would
        // turn "we have not modelled this yet" into a false pass.
        m_ModelNsTimerResidency = 140.0;
        m_B2K = 500;
        // B5 is amplified like every other bench: one accumulator step or one
        // dormant timer is below the noise floor by orders of magnitude.
        m_B5Units = 1000;
        m_B3Lookups = 100000;
        // Repeats of the control/measure pair; the median is reported.
        m_B3Repeats = 5;
        m_B3Entity = "PlateCarrierVest";
        m_B3FallbackEntity = "PlateCarrierVest";
        // Empty slot name means "discover one from the spawned entity".
        // InventorySlots.GetSlotIdFromString resolves against CfgSlots class
        // names, which do not always match the inventorySlot[] spelling a mod
        // author would reach for, so guessing a name is the fragile path.
        m_B3Slot = "";
        m_B3Attachment = "PlateCarrierHolster";
        m_B3Position = "1000 400 1000";

        m_Benches = new array<string>();
        m_KValues = new array<int>();
        m_B2Periods = new array<int>();
    }

    bool Load()
    {
        string document = MPB_Json.ReadAll(PATH);
        if (document == "")
        {
            m_Present = false;
            return false;
        }
        m_Present = true;

        m_Note = MPB_Json.GetString(document, "note", "");
        m_HardwareNote = MPB_Json.GetString(document, "hardware_note", "");
        m_SettleSeconds = MPB_Json.GetFloat(document, "settle_seconds", m_SettleSeconds);
        m_WindowSeconds = MPB_Json.GetFloat(document, "window_seconds", m_WindowSeconds);
        m_BlockSeconds = MPB_Json.GetFloat(document, "block_seconds", m_BlockSeconds);
        m_APrimeTolerancePct = MPB_Json.GetFloat(document, "a_prime_tolerance_pct", m_APrimeTolerancePct);
        m_ModelNsTimerResidency = MPB_Json.GetFloat(document, "model_ns_timer_residency", m_ModelNsTimerResidency);
        m_B2K = MPB_Json.GetInt(document, "b2_k", m_B2K);
        m_B5Units = MPB_Json.GetInt(document, "b5_units", m_B5Units);
        m_B3Lookups = MPB_Json.GetInt(document, "b3_lookups", m_B3Lookups);
        m_B3Repeats = MPB_Json.GetInt(document, "b3_repeats", m_B3Repeats);
        m_B3Entity = MPB_Json.GetString(document, "b3_entity", m_B3Entity);
        m_B3FallbackEntity = MPB_Json.GetString(document, "b3_fallback_entity", m_B3FallbackEntity);
        m_B3Slot = MPB_Json.GetString(document, "b3_slot", m_B3Slot);
        m_B3Attachment = MPB_Json.GetString(document, "b3_attachment", m_B3Attachment);
        m_B3Position = MPB_Json.GetString(document, "b3_position", m_B3Position);

        if (!MPB_Json.GetStringArray(document, "benches", m_Benches))
        {
            m_Benches.Insert("B1");
            m_Benches.Insert("B2");
            m_Benches.Insert("B3");
            m_Benches.Insert("B5");
        }
        if (!MPB_Json.GetIntArray(document, "k_values", m_KValues))
        {
            m_KValues.Insert(100);
            m_KValues.Insert(500);
            m_KValues.Insert(1000);
        }
        if (!MPB_Json.GetIntArray(document, "b2_periods_ms", m_B2Periods))
        {
            m_B2Periods.Insert(0);
            m_B2Periods.Insert(10);
            m_B2Periods.Insert(100);
            m_B2Periods.Insert(1000);
        }

        // Floors, not silent corrections -- a zero-length window would make
        // every median meaningless and every verdict confident.
        if (m_SettleSeconds < 1.0)
        {
            m_SettleSeconds = 1.0;
        }
        if (m_WindowSeconds < 1.0)
        {
            m_WindowSeconds = 1.0;
        }
        if (m_BlockSeconds <= 0)
        {
            m_BlockSeconds = 0.01;
        }
        // A block wider than a tenth of the window leaves too few samples for
        // a quantile to mean anything.
        if (m_BlockSeconds > m_WindowSeconds / 10.0)
        {
            m_BlockSeconds = m_WindowSeconds / 10.0;
        }
        return true;
    }

    bool WantsBench(string id)
    {
        int index;
        for (index = 0; index < m_Benches.Count(); index++)
        {
            if (m_Benches.Get(index) == id)
            {
                return true;
            }
        }
        return false;
    }

    string ToJsonObject()
    {
        string text = "  \"config\": {\n";
        text = text + "    \"note\": " + MPB_Fmt.Quoted(m_Note) + ",\n";
        text = text + "    \"hardware_note\": " + MPB_Fmt.Quoted(m_HardwareNote) + ",\n";
        text = text + "    \"settle_seconds\": " + MPB_Fmt.Dec(m_SettleSeconds, 1) + ",\n";
        text = text + "    \"window_seconds\": " + MPB_Fmt.Dec(m_WindowSeconds, 1) + ",\n";
        text = text + "    \"block_seconds\": " + MPB_Fmt.Dec(m_BlockSeconds, 4) + ",\n";
        text = text + "    \"a_prime_tolerance_pct\": " + MPB_Fmt.Dec(m_APrimeTolerancePct, 1) + ",\n";
        text = text + "    \"model_ns_timer_residency\": " + MPB_Fmt.Ns(m_ModelNsTimerResidency) + ",\n";
        text = text + "    \"b2_k\": " + m_B2K + ",\n";
        text = text + "    \"b5_units\": " + m_B5Units + ",\n";
        text = text + "    \"b3_lookups\": " + m_B3Lookups + ",\n";
        text = text + "    \"b3_repeats\": " + m_B3Repeats + "\n";
        text = text + "  },";
        return text;
    }
}

// One executed run: a bench id plus the single load configuration it applied,
// and everything measured about it.
class MPB_Run
{
    // How far past the noise floor a delta must land before it counts as a
    // measurement rather than as noise.
    static const float SIGNIFICANCE_FACTOR = 2.0;

    static const int KIND_TIMER = 1;
    static const int KIND_ACCUMULATOR = 2;
    static const int KIND_SLOW_TIMER = 3;
    static const int KIND_INVENTORY = 4;

    string m_BenchId;
    string m_Variant;
    int    m_Kind;
    int    m_Units;
    int    m_PeriodMs;
    float  m_ModelNs;

    float m_BaseMeanMs;
    float m_BaseMedianMs;
    float m_BaseP95Ms;
    float m_BaseFps;
    int   m_BaseFrames;
    int   m_BaseBlocks;
    float m_BaseStdErrMs;

    float m_TreatMeanMs;
    float m_TreatMedianMs;
    float m_TreatP95Ms;
    float m_TreatFps;
    int   m_TreatFrames;
    int   m_TreatBlocks;

    float m_RecoverMeanMs;
    float m_RecoverMedianMs;
    float m_RecoverP95Ms;
    float m_RecoverFps;
    int   m_RecoverFrames;
    int   m_RecoverBlocks;

    // Instrument settings, carried into the results so a number can be
    // reproduced without also having the config file.
    float m_BlockTargetMs;
    float m_MeanBlockFrames;

    float  m_DeltaMs;
    float  m_DriftMs;
    float  m_DriftPct;
    float  m_PerUnitNs;
    float  m_NoiseNs;
    bool   m_APrimeOk;
    string m_Verdict;

    void MPB_Run(string benchId, string variant, int kind, int units, int periodMs, float modelNs)
    {
        m_BenchId = benchId;
        m_Variant = variant;
        m_Kind = kind;
        m_Units = units;
        m_PeriodMs = periodMs;
        m_ModelNs = modelNs;
        m_Verdict = "NOT_RUN";
        m_APrimeOk = false;
    }

    void CaptureBaseline(MPB_FrameClock clock)
    {
        m_BaseMeanMs = clock.MeanMs();
        m_BaseMedianMs = clock.MedianMs();
        m_BaseP95Ms = clock.P95Ms();
        m_BaseFps = clock.MedianFps();
        m_BaseFrames = clock.FrameCount();
        m_BaseBlocks = clock.BlockCount();
        m_BaseStdErrMs = clock.MedianStdErrMs();
        m_BlockTargetMs = clock.BlockTargetSeconds() * 1000.0;
        m_MeanBlockFrames = clock.MeanBlockFrames();
    }

    void CaptureTreatment(MPB_FrameClock clock)
    {
        m_TreatMeanMs = clock.MeanMs();
        m_TreatMedianMs = clock.MedianMs();
        m_TreatP95Ms = clock.P95Ms();
        m_TreatFps = clock.MedianFps();
        m_TreatFrames = clock.FrameCount();
        m_TreatBlocks = clock.BlockCount();
    }

    void CaptureRecovery(MPB_FrameClock clock)
    {
        m_RecoverMeanMs = clock.MeanMs();
        m_RecoverMedianMs = clock.MedianMs();
        m_RecoverP95Ms = clock.P95Ms();
        m_RecoverFps = clock.MedianFps();
        m_RecoverFrames = clock.FrameCount();
        m_RecoverBlocks = clock.BlockCount();
    }

    // All the judgement in one place, so the rule is reviewable rather than
    // scattered through the phase machine.
    void Conclude(float tolerancePct)
    {
        m_DeltaMs = m_TreatMedianMs - m_BaseMedianMs;
        m_DriftMs = m_RecoverMedianMs - m_BaseMedianMs;

        m_DriftPct = 0;
        if (m_BaseMedianMs > 0)
        {
            m_DriftPct = (Math.AbsFloat(m_DriftMs) / m_BaseMedianMs) * 100.0;
        }

        int divisor = m_Units;
        if (divisor <= 0)
        {
            divisor = 1;
        }
        m_PerUnitNs = (m_DeltaMs * 1000000.0) / divisor;

        // The noise floor is the LARGER of two things, because either alone
        // can be fooled:
        //
        //   between-window drift -- how far the box moved between two windows
        //     that carried identical load. On its own it collapses to exactly
        //     zero whenever A and A' happen to land on the same quantized
        //     median, which would then let any difference count as real.
        //
        //   the baseline median's own standard error -- the precision of the
        //     estimate itself. This does not collapse, so it is the floor that
        //     holds when drift is zero.
        float floorMs = Math.AbsFloat(m_DriftMs);
        if (m_BaseStdErrMs > floorMs)
        {
            floorMs = m_BaseStdErrMs;
        }
        m_NoiseNs = (floorMs * 1000000.0) / divisor;

        m_APrimeOk = m_DriftPct <= tolerancePct;
        if (!m_APrimeOk)
        {
            m_Verdict = "INVALID_A_PRIME";
            return;
        }

        // A margin, not a bare comparison. A delta that merely edges past its
        // own noise floor is exactly what a noise floor is bad at
        // distinguishing, and this tool's output is meant to be usable as
        // evidence -- so an effect has to clear the floor by a factor before
        // it is called a measurement at all.
        if (Math.AbsFloat(m_DeltaMs) <= floorMs * SIGNIFICANCE_FACTOR)
        {
            m_Verdict = "BELOW_NOISE";
            return;
        }

        // A load can only add work. A treatment window that measured FASTER
        // than its baseline by more than the noise floor means something other
        // than the load moved the number, so the run cannot be read as a
        // measurement of the load -- and it must not be reported as a model
        // deviation either, which is what would happen if this fell through
        // to the comparison below.
        if (m_DeltaMs < 0)
        {
            m_Verdict = "NEGATIVE_DELTA";
            return;
        }

        if (m_ModelNs > 0)
        {
            float allowed = m_ModelNs * 0.2;
            if (Math.AbsFloat(m_PerUnitNs - m_ModelNs) <= allowed)
            {
                m_Verdict = "WITHIN_20PCT";
            }
            else
            {
                m_Verdict = "DRIFT";
            }
            return;
        }

        m_Verdict = "MEASURED";
    }

    string SummaryText()
    {
        string text = m_BenchId + " " + m_Variant;
        text = text + " base_med_ms=" + MPB_Fmt.Ms(m_BaseMedianMs);
        text = text + " treat_med_ms=" + MPB_Fmt.Ms(m_TreatMedianMs);
        text = text + " rec_med_ms=" + MPB_Fmt.Ms(m_RecoverMedianMs);
        text = text + " base_fps=" + MPB_Fmt.Fps(m_BaseFps);
        text = text + " treat_fps=" + MPB_Fmt.Fps(m_TreatFps);
        text = text + " per_unit_ns=" + MPB_Fmt.Ns(m_PerUnitNs);
        text = text + " noise_ns=" + MPB_Fmt.Ns(m_NoiseNs);
        if (m_ModelNs > 0)
        {
            text = text + " model_ns=" + MPB_Fmt.Ns(m_ModelNs);
        }
        text = text + " a_prime_drift_pct=" + MPB_Fmt.Dec(m_DriftPct, 2);
        text = text + " verdict=" + m_Verdict;
        return text;
    }

    private string WindowJson(string label, int frames, int blocks, float meanMs, float medianMs, float p95Ms, float fps)
    {
        string text = "        \"" + label + "\": { ";
        text = text + "\"frames\": " + frames + ", ";
        text = text + "\"blocks\": " + blocks + ", ";
        text = text + "\"mean_ms\": " + MPB_Fmt.Ms(meanMs) + ", ";
        text = text + "\"median_ms\": " + MPB_Fmt.Ms(medianMs) + ", ";
        text = text + "\"p95_ms\": " + MPB_Fmt.Ms(p95Ms) + ", ";
        text = text + "\"median_fps\": " + MPB_Fmt.Fps(fps);
        text = text + " }";
        return text;
    }

    string ToJsonObject()
    {
        string modelText = "null";
        if (m_ModelNs > 0)
        {
            modelText = MPB_Fmt.Ns(m_ModelNs);
        }

        string text = "    {\n";
        text = text + "      \"id\": " + MPB_Fmt.Quoted(m_BenchId) + ",\n";
        text = text + "      \"variant\": " + MPB_Fmt.Quoted(m_Variant) + ",\n";
        text = text + "      \"protocol\": \"a_b_a_prime\",\n";
        text = text + "      \"units\": " + m_Units + ",\n";
        text = text + "      \"period_ms\": " + m_PeriodMs + ",\n";
        text = text + "      \"block_target_ms\": " + MPB_Fmt.Ms(m_BlockTargetMs) + ",\n";
        text = text + "      \"mean_block_frames\": " + MPB_Fmt.Dec(m_MeanBlockFrames, 1) + ",\n";
        text = text + "      \"windows\": {\n";
        text = text + WindowJson("baseline", m_BaseFrames, m_BaseBlocks, m_BaseMeanMs, m_BaseMedianMs, m_BaseP95Ms, m_BaseFps) + ",\n";
        text = text + WindowJson("treatment", m_TreatFrames, m_TreatBlocks, m_TreatMeanMs, m_TreatMedianMs, m_TreatP95Ms, m_TreatFps) + ",\n";
        text = text + WindowJson("recovery", m_RecoverFrames, m_RecoverBlocks, m_RecoverMeanMs, m_RecoverMedianMs, m_RecoverP95Ms, m_RecoverFps) + "\n";
        text = text + "      },\n";
        text = text + "      \"delta_median_ms\": " + MPB_Fmt.Ms(m_DeltaMs) + ",\n";
        text = text + "      \"baseline_median_stderr_ms\": " + MPB_Fmt.Dec(m_BaseStdErrMs, 6) + ",\n";
        text = text + "      \"a_prime_drift_ms\": " + MPB_Fmt.Ms(m_DriftMs) + ",\n";
        text = text + "      \"a_prime_drift_pct\": " + MPB_Fmt.Dec(m_DriftPct, 2) + ",\n";
        text = text + "      \"a_prime_ok\": " + MPB_Fmt.Bool(m_APrimeOk) + ",\n";
        text = text + "      \"per_unit_ns\": " + MPB_Fmt.Ns(m_PerUnitNs) + ",\n";
        text = text + "      \"noise_ns\": " + MPB_Fmt.Ns(m_NoiseNs) + ",\n";
        text = text + "      \"model_ns\": " + modelText + ",\n";
        text = text + "      \"verdict\": " + MPB_Fmt.Quoted(m_Verdict) + "\n";
        text = text + "    }";
        return text;
    }
}

class MPB_Runner
{
    static const int PHASE_DISARMED       = 0;
    static const int PHASE_SETTLE         = 1;
    static const int PHASE_SELFCHECK_ONE  = 6;
    static const int PHASE_SELFCHECK_MANY = 7;
    static const int PHASE_BASE           = 2;
    static const int PHASE_TREAT          = 3;
    static const int PHASE_RECOVER        = 4;
    static const int PHASE_DONE           = 5;

    // How long each self-check stage runs. Short: it only has to count, not
    // resolve a cost.
    static const float SELFCHECK_SECONDS = 1.0;

    static const string SUITE = "v1";
    static const string MOD_VERSION = "1.0.0";

    ref MPB_Config          m_Config;
    ref MPB_Report          m_Reporter;
    ref MPB_FrameClock      m_Clock;
    ref MPB_TimerLoad       m_TimerLoad;
    ref MPB_AccumulatorLoad m_AccumulatorLoad;
    ref MPB_InventoryLookup m_InventoryBench;
    ref array<ref MPB_Run>  m_Runs;

    int   m_Phase;
    int   m_RunIndex;
    float m_PhaseSeconds;
    string m_StartedUtc;
    string m_DayzVersion;

    float m_SuiteBaseMedianMs;
    float m_SuiteBaseFps;

    int   m_SelfCheckK;
    int   m_SelfCheckFramesOne;
    int   m_SelfCheckFiresOne;
    float m_SelfCheckSecondsOne;
    int   m_SelfCheckFramesMany;
    int   m_SelfCheckFiresMany;
    float m_SelfCheckSecondsMany;
    float m_SelfCheckRatio;
    float m_QueueTicksPerSecond;
    float m_EngineFramesPerSecond;
    float m_FramesPerQueueTick;
    string m_SelfCheckVerdict;

    void MPB_Runner()
    {
        m_Config = new MPB_Config();
        m_Reporter = new MPB_Report();
        m_Clock = new MPB_FrameClock();
        m_TimerLoad = new MPB_TimerLoad();
        m_AccumulatorLoad = new MPB_AccumulatorLoad();
        m_InventoryBench = new MPB_InventoryLookup();
        m_Runs = new array<ref MPB_Run>();

        m_Phase = PHASE_DISARMED;
        m_RunIndex = -1;
        m_PhaseSeconds = 0;
        m_SuiteBaseMedianMs = 0;
        m_SuiteBaseFps = 0;
        m_SelfCheckK = 0;
        m_SelfCheckFramesOne = 0;
        m_SelfCheckFiresOne = 0;
        m_SelfCheckSecondsOne = 0;
        m_SelfCheckFramesMany = 0;
        m_SelfCheckFiresMany = 0;
        m_SelfCheckSecondsMany = 0;
        m_SelfCheckRatio = 0;
        m_QueueTicksPerSecond = 0;
        m_EngineFramesPerSecond = 0;
        m_FramesPerQueueTick = 0;
        m_SelfCheckVerdict = "NOT_RUN";
    }

    // Called once, on the first server frame. Reads the arming file and, if
    // present, builds the run list and enters the settle phase.
    void MPB_Boot()
    {
        if (!m_Config.Load())
        {
            // Deliberately quiet-but-visible: one line, so an operator who
            // expected a run can tell "mod not loaded" from "mod loaded, not
            // armed" without turning on anything.
            Print(MPB_Report.PREFIX + "inert (no " + MPB_Config.PATH + "); create that file to arm a run");
            m_Phase = PHASE_DISARMED;
            return;
        }

        string version;
        GetGame().GetVersion(version);
        m_DayzVersion = version;
        m_StartedUtc = MPB_Fmt.UtcIso();

        BuildRunList();
        if (m_Runs.Count() == 0)
        {
            m_Reporter.Say("armed but no benches selected -- nothing to do");
            m_Phase = PHASE_DONE;
            return;
        }

        // Assembled step by step: Enforce rejects long concatenation chains
        // with "Formula too complex", and the limit is low enough that a
        // single summary line reaches it.
        string beginLine = "BEGIN suite=" + SUITE;
        beginLine = beginLine + " mod=" + MOD_VERSION;
        beginLine = beginLine + " dayz=" + m_DayzVersion;
        beginLine = beginLine + " runs=" + m_Runs.Count();
        beginLine = beginLine + " settle_s=" + MPB_Fmt.Dec(m_Config.m_SettleSeconds, 1);
        beginLine = beginLine + " window_s=" + MPB_Fmt.Dec(m_Config.m_WindowSeconds, 1);
        m_Reporter.Say(beginLine);
        m_Reporter.Say("WARNING these benches deliberately degrade frame time; never run on a populated server");

        m_Clock.SetBlockTargetSeconds(m_Config.m_BlockSeconds);
        m_Phase = PHASE_SETTLE;
        m_PhaseSeconds = 0;
        // The settle window is also the suite's own free-run reference. It
        // costs nothing to record and it is the number an operator reads first.
        m_Clock.Start();
    }

    private void BuildRunList()
    {
        int index;

        if (m_Config.WantsBench("B1"))
        {
            for (index = 0; index < m_Config.m_KValues.Count(); index++)
            {
                int kValue = m_Config.m_KValues.Get(index);
                m_Runs.Insert(new MPB_Run("B1", "K=" + kValue + " period=0ms", MPB_Run.KIND_TIMER, kValue, 0, m_Config.m_ModelNsTimerResidency));
            }
        }

        if (m_Config.WantsBench("B2"))
        {
            int periodIndex;
            for (periodIndex = 0; periodIndex < m_Config.m_B2Periods.Count(); periodIndex++)
            {
                int periodValue = m_Config.m_B2Periods.Get(periodIndex);
                m_Runs.Insert(new MPB_Run("B2", "K=" + m_Config.m_B2K + " period=" + periodValue + "ms", MPB_Run.KIND_TIMER, m_Config.m_B2K, periodValue, 0));
            }
        }

        if (m_Config.WantsBench("B3"))
        {
            m_Runs.Insert(new MPB_Run("B3", "n=" + m_Config.m_B3Lookups, MPB_Run.KIND_INVENTORY, m_Config.m_B3Lookups, 0, 0));
        }

        if (m_Config.WantsBench("B5"))
        {
            int b5Units = m_Config.m_B5Units;
            m_Runs.Insert(new MPB_Run("B5", "accumulator x" + b5Units, MPB_Run.KIND_ACCUMULATOR, b5Units, 0, 0));
            m_Runs.Insert(new MPB_Run("B5", "slow_timer_60s x" + b5Units, MPB_Run.KIND_SLOW_TIMER, b5Units, 60000, 0));
        }
    }

    void MPB_OnUpdate(float timeslice)
    {
        // The accumulator variant's measured path. It is deliberately the
        // first thing in the update, unconditionally called, because the
        // branch it takes when inactive is part of what a real mod pays.
        m_AccumulatorLoad.OnFrame(timeslice);

        if (m_Phase == PHASE_DISARMED || m_Phase == PHASE_DONE)
        {
            return;
        }

        m_Clock.Sample(timeslice);
        m_PhaseSeconds = m_PhaseSeconds + timeslice;

        if (m_Phase == PHASE_SETTLE)
        {
            if (m_PhaseSeconds >= m_Config.m_SettleSeconds)
            {
                m_SuiteBaseMedianMs = m_Clock.MedianMs();
                m_SuiteBaseFps = m_Clock.MedianFps();
                m_Reporter.Say("SETTLED " + m_Clock.SummaryText());
                StartSelfCheckOne();
            }
            return;
        }

        if (m_Phase == PHASE_SELFCHECK_ONE)
        {
            if (m_PhaseSeconds >= SELFCHECK_SECONDS)
            {
                m_SelfCheckFramesOne = m_Clock.FrameCount();
                m_SelfCheckSecondsOne = m_Clock.ElapsedSeconds();
                m_SelfCheckFiresOne = m_TimerLoad.FireCount();
                m_TimerLoad.RemoveCounted();
                StartSelfCheckMany();
            }
            return;
        }

        if (m_Phase == PHASE_SELFCHECK_MANY)
        {
            if (m_PhaseSeconds >= SELFCHECK_SECONDS)
            {
                m_SelfCheckFramesMany = m_Clock.FrameCount();
                m_SelfCheckSecondsMany = m_Clock.ElapsedSeconds();
                m_SelfCheckFiresMany = m_TimerLoad.FireCount();
                m_TimerLoad.RemoveCounted();
                FinishSelfCheck();
                StartNextRun();
            }
            return;
        }

        if (m_PhaseSeconds < m_Config.m_WindowSeconds)
        {
            return;
        }

        if (m_Phase == PHASE_BASE)
        {
            MPB_Run baseRun = m_Runs.Get(m_RunIndex);
            baseRun.CaptureBaseline(m_Clock);
            m_Reporter.Trace(baseRun.m_BenchId + " " + baseRun.m_Variant + " A  " + m_Clock.SummaryText());
            ApplyLoad(baseRun);
            m_Phase = PHASE_TREAT;
            m_PhaseSeconds = 0;
            m_Clock.Start();
            return;
        }

        if (m_Phase == PHASE_TREAT)
        {
            MPB_Run treatRun = m_Runs.Get(m_RunIndex);
            treatRun.CaptureTreatment(m_Clock);
            m_Reporter.Trace(treatRun.m_BenchId + " " + treatRun.m_Variant + " B  " + m_Clock.SummaryText());
            RemoveLoad(treatRun);
            m_Phase = PHASE_RECOVER;
            m_PhaseSeconds = 0;
            m_Clock.Start();
            return;
        }

        if (m_Phase == PHASE_RECOVER)
        {
            MPB_Run recoverRun = m_Runs.Get(m_RunIndex);
            recoverRun.CaptureRecovery(m_Clock);
            m_Reporter.Trace(recoverRun.m_BenchId + " " + recoverRun.m_Variant + " A' " + m_Clock.SummaryText());
            recoverRun.Conclude(m_Config.m_APrimeTolerancePct);
            m_Reporter.Say(recoverRun.SummaryText());
            m_Reporter.AddBenchBlock(recoverRun.ToJsonObject());
            StartNextRun();
            return;
        }
    }

    // Two things have to be true before any per-unit number below can be
    // believed, and neither is safe to assume:
    //
    //   1. K registrations of one method really do become K live queue
    //      entries. If the queue collapsed them to one, every figure divided
    //      by K would be wrong by a factor of K.
    //   2. How often the queue is actually walked. The gameplay call queue is
    //      ticked from the game's simulation update, NOT from every engine
    //      frame -- and on a fast idle server those two rates differ by more
    //      than an order of magnitude. A "cost per frame" for a resident timer
    //      is meaningless without knowing how many frames pass between two
    //      walks of the queue.
    //
    // Both are measured, not assumed, and both go in the results. Stage one
    // runs a SINGLE counted entry: its fire count is the queue's tick count.
    // Stage two runs K of them: the ratio against stage one is the
    // multiplicity, and it should come out at K.
    private void StartSelfCheckOne()
    {
        m_SelfCheckK = 0;
        int scanIndex;
        for (scanIndex = 0; scanIndex < m_Config.m_KValues.Count(); scanIndex++)
        {
            if (m_Config.m_KValues.Get(scanIndex) > m_SelfCheckK)
            {
                m_SelfCheckK = m_Config.m_KValues.Get(scanIndex);
            }
        }
        if (m_SelfCheckK <= 0)
        {
            m_SelfCheckK = 100;
        }

        m_TimerLoad.ApplyCounted(1, 0);
        m_Phase = PHASE_SELFCHECK_ONE;
        m_PhaseSeconds = 0;
        m_Clock.Start();
    }

    private void StartSelfCheckMany()
    {
        m_TimerLoad.ApplyCounted(m_SelfCheckK, 0);
        m_Phase = PHASE_SELFCHECK_MANY;
        m_PhaseSeconds = 0;
        m_Clock.Start();
    }

    private void FinishSelfCheck()
    {
        if (m_SelfCheckSecondsOne > 0)
        {
            float fires = m_SelfCheckFiresOne;
            float frames = m_SelfCheckFramesOne;
            m_QueueTicksPerSecond = fires / m_SelfCheckSecondsOne;
            m_EngineFramesPerSecond = frames / m_SelfCheckSecondsOne;
        }
        if (m_SelfCheckFiresOne > 0)
        {
            float framesOne = m_SelfCheckFramesOne;
            float firesOne = m_SelfCheckFiresOne;
            m_FramesPerQueueTick = framesOne / firesOne;
            float firesMany = m_SelfCheckFiresMany;
            // Rate-normalised: the two stages are separate windows, so compare
            // fires per queue tick rather than raw counts.
            float ticksMany = firesOne * (m_SelfCheckSecondsMany / m_SelfCheckSecondsOne);
            if (ticksMany > 0)
            {
                m_SelfCheckRatio = firesMany / ticksMany;
            }
        }

        float expected = m_SelfCheckK;
        if (m_SelfCheckFiresOne <= 0)
        {
            m_SelfCheckVerdict = "NO_FIRES";
        }
        else if (m_SelfCheckRatio >= expected * 0.8 && m_SelfCheckRatio <= expected * 1.25)
        {
            m_SelfCheckVerdict = "K_ENTRIES_CONFIRMED";
        }
        else if (m_SelfCheckRatio <= 1.5)
        {
            // The queue kept one entry however many times it was asked.
            m_SelfCheckVerdict = "DEDUPLICATED";
        }
        else
        {
            m_SelfCheckVerdict = "UNEXPECTED_RATIO";
        }

        string line = "SELFCHECK timer_multiplicity k=" + m_SelfCheckK;
        line = line + " fires_1=" + m_SelfCheckFiresOne;
        line = line + " fires_k=" + m_SelfCheckFiresMany;
        line = line + " ratio=" + MPB_Fmt.Dec(m_SelfCheckRatio, 1);
        line = line + " verdict=" + m_SelfCheckVerdict;
        m_Reporter.Say(line);

        string rateLine = "SELFCHECK queue_rate ticks_per_s=" + MPB_Fmt.Dec(m_QueueTicksPerSecond, 1);
        rateLine = rateLine + " engine_frames_per_s=" + MPB_Fmt.Dec(m_EngineFramesPerSecond, 1);
        rateLine = rateLine + " frames_per_tick=" + MPB_Fmt.Dec(m_FramesPerQueueTick, 2);
        rateLine = rateLine + " note=gameplay_call_queue_is_ticked_from_the_simulation_update_not_every_engine_frame";
        m_Reporter.Say(rateLine);
    }

    private void StartNextRun()
    {
        m_RunIndex++;
        if (m_RunIndex >= m_Runs.Count())
        {
            Finish();
            return;
        }

        MPB_Run run = m_Runs.Get(m_RunIndex);

        if (run.m_Kind == MPB_Run.KIND_INVENTORY)
        {
            // Not a windowed bench: the whole measurement happens inside this
            // one frame, so it neither needs nor can use A/B/A'.
            string inventoryLine = "RUN " + (m_RunIndex + 1);
            inventoryLine = inventoryLine + "/" + m_Runs.Count();
            inventoryLine = inventoryLine + " " + run.m_BenchId;
            inventoryLine = inventoryLine + " " + run.m_Variant;
            m_Reporter.Trace(inventoryLine);
            RunInventoryBench();
            run.m_Verdict = m_InventoryBench.m_Status;
            m_Reporter.Say(m_InventoryBench.SummaryText());
            m_Reporter.AddBenchBlock(m_InventoryBench.ToJsonObject());
            StartNextRun();
            return;
        }

        string runLine = "RUN " + (m_RunIndex + 1);
        runLine = runLine + "/" + m_Runs.Count();
        runLine = runLine + " " + run.m_BenchId;
        runLine = runLine + " " + run.m_Variant;
        m_Reporter.Trace(runLine);
        m_Phase = PHASE_BASE;
        m_PhaseSeconds = 0;
        m_Clock.Start();
    }

    private void RunInventoryBench()
    {
        vector position = m_Config.m_B3Position.ToVector();
        m_InventoryBench.Run(m_Config.m_B3Entity, m_Config.m_B3FallbackEntity, m_Config.m_B3Slot, m_Config.m_B3Attachment, m_Config.m_B3Lookups, m_Config.m_B3Repeats, position);
    }

    private void ApplyLoad(MPB_Run run)
    {
        if (run.m_Kind == MPB_Run.KIND_TIMER)
        {
            m_TimerLoad.Apply(run.m_Units, run.m_PeriodMs);
            return;
        }
        if (run.m_Kind == MPB_Run.KIND_ACCUMULATOR)
        {
            m_AccumulatorLoad.ApplyAccumulator(run.m_Units);
            return;
        }
        if (run.m_Kind == MPB_Run.KIND_SLOW_TIMER)
        {
            m_AccumulatorLoad.ApplySlowTimer(run.m_Units);
            return;
        }
    }

    private void RemoveLoad(MPB_Run run)
    {
        if (run.m_Kind == MPB_Run.KIND_TIMER)
        {
            m_TimerLoad.Remove();
            return;
        }
        m_AccumulatorLoad.RemoveAll();
    }

    private void Finish()
    {
        // Belt and braces: nothing this mod registered may outlive the suite,
        // or the server it was measuring keeps paying for the measurement.
        m_TimerLoad.Remove();
        m_TimerLoad.RemoveCounted();
        m_AccumulatorLoad.RemoveAll();

        m_Clock.Stop();
        m_Phase = PHASE_DONE;

        string header = BuildHeaderJson();
        string path = m_Reporter.Write(header);
        if (path == "")
        {
            Print(MPB_Report.PREFIX + "END (results file could not be written)");
            return;
        }
        Print(MPB_Report.PREFIX + "END -> " + path);
    }

    private string BuildHeaderJson()
    {
        string text = "  \"schema\": \"modperf-bench/results/1\",\n";
        text = text + "  \"suite\": " + MPB_Fmt.Quoted(SUITE) + ",\n";
        text = text + "  \"mod_version\": " + MPB_Fmt.Quoted(MOD_VERSION) + ",\n";
        text = text + "  \"dayz_version\": " + MPB_Fmt.Quoted(m_DayzVersion) + ",\n";
        text = text + "  \"started_utc\": " + MPB_Fmt.Quoted(m_StartedUtc) + ",\n";
        text = text + "  \"finished_utc\": " + MPB_Fmt.Quoted(MPB_Fmt.UtcIso()) + ",\n";
        text = text + "  \"settle_window\": { \"median_ms\": ";
        text = text + MPB_Fmt.Ms(m_SuiteBaseMedianMs);
        text = text + ", \"median_fps\": ";
        text = text + MPB_Fmt.Fps(m_SuiteBaseFps);
        text = text + " },\n";
        text = text + "  \"self_check\": {\n";
        text = text + "    \"name\": \"timer_multiplicity\",\n";
        text = text + "    \"k\": " + m_SelfCheckK + ",\n";
        text = text + "    \"fires_with_1_entry\": " + m_SelfCheckFiresOne + ",\n";
        text = text + "    \"fires_with_k_entries\": " + m_SelfCheckFiresMany + ",\n";
        text = text + "    \"multiplicity_ratio\": " + MPB_Fmt.Dec(m_SelfCheckRatio, 1) + ",\n";
        text = text + "    \"queue_ticks_per_second\": " + MPB_Fmt.Dec(m_QueueTicksPerSecond, 1) + ",\n";
        text = text + "    \"engine_frames_per_second\": " + MPB_Fmt.Dec(m_EngineFramesPerSecond, 1) + ",\n";
        text = text + "    \"engine_frames_per_queue_tick\": " + MPB_Fmt.Dec(m_FramesPerQueueTick, 2) + ",\n";
        text = text + "    \"note\": \"the gameplay call queue is ticked from the simulation update, not from every engine frame\",\n";
        text = text + "    \"verdict\": " + MPB_Fmt.Quoted(m_SelfCheckVerdict) + "\n";
        text = text + "  },\n";
        text = text + m_Config.ToJsonObject();
        return text;
    }
}

// ----------------------------------------------------------------------------
// Engine attachment point.
//
// One override, on the server mission's update. super is called FIRST and
// unconditionally: every vanilla per-frame job (scheduler, logout handling,
// environment) must run whatever this mod is doing, and a bench that breaks
// the chain it is measuring is measuring a different server.
//
// Every member added here carries the MPB_ prefix. Two mods declaring the same
// member name in modded blocks of one class collide, and the compiler blames
// the earlier-loaded mod's usage site, not the one that added the clash -- so
// the prefix is the only thing keeping this tool from breaking somebody else's
// build in a way that looks like their bug.
// ----------------------------------------------------------------------------
modded class MissionServer
{
    ref MPB_Runner m_MPB_Runner;
    bool m_MPB_Booted;

    override void OnUpdate(float timeslice)
    {
        super.OnUpdate(timeslice);

        if (!m_MPB_Booted)
        {
            m_MPB_Booted = true;
            m_MPB_Runner = new MPB_Runner();
            m_MPB_Runner.MPB_Boot();
        }

        if (m_MPB_Runner)
        {
            m_MPB_Runner.MPB_OnUpdate(timeslice);
        }
    }
}
