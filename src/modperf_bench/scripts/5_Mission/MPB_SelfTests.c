// Engine fixtures; these exercise production clock/config/verdict behavior.
// Opt-in via run_self_tests=1 on an isolated test instance.
class MPB_SelfTests
{
    static bool Run()
    {
        MPB_FrameClock clock = new MPB_FrameClock();
        clock.SetBlockTargetSeconds(0.01);
        clock.Start();
        clock.Sample(0.02);
        clock.Sample(0.01);
        clock.Sample(0.03);
        float originalFirst = clock.m_Samples.Get(0);
        clock.MedianMs();
        bool ordered = clock.m_Samples.Get(0) == originalFirst;
        bool totals = clock.FrameCount() == 3 && Math.AbsFloat(clock.MeanMs() - 20.0) < 0.01;
        MPB_Run run = new MPB_Run("TEST", "recovery", MPB_Run.KIND_TIMER, 10, 0, 0);
        run.m_BaseMedianMs = 1;
        run.m_TreatMedianMs = 2;
        run.m_RecoverMedianMs = 3;
        run.m_BaseFrames = 10;
        run.m_TreatFrames = 10;
        run.m_RecoverFrames = 10;
        run.Conclude(10);
        bool invalidRecovery = run.m_Verdict == "INVALID_A_PRIME";
        MPB_Config config = new MPB_Config();
        bool noMismatchedModel = config.m_ModelNsTimerResidency == 0;
        Print("[modperf-bench] fixture clock-order " + MPB_Fmt.Bool(ordered));
        Print("[modperf-bench] fixture clock-totals " + MPB_Fmt.Bool(totals));
        Print("[modperf-bench] fixture recovery-gate " + MPB_Fmt.Bool(invalidRecovery));
        Print("[modperf-bench] fixture model-default " + MPB_Fmt.Bool(noMismatchedModel));
        return ordered && totals && invalidRecovery && noMismatchedModel;
    }
}
