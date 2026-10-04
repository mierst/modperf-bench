// ============================================================================
// MPB_AccumulatorLoad -- B5, the price of the "hook the hottest surface and
// throttle inside it" pattern.
//
// THE PATTERN UNDER TEST
//   A mod that wants to do something once a minute has two ways to arrange it:
//
//     (a) accumulate elapsed time inside an every-frame update hook and act
//         when the accumulator crosses the threshold, or
//     (b) register one repeating call-queue entry with a 60 s period.
//
//   (a) is common because it is easy to write, and it is defended with "the
//   work only runs once a minute". The work does -- the dispatch does not. B5
//   measures the two arrangements against their own baselines. The amplified
//   accumulator includes loop and array access work; it does not independently
//   calibrate virtual hook dispatch or imply a Timer residency constant.
//
// TWO VARIANTS, EACH WITH ITS OWN A/B/A' WINDOWS
//   ACCUMULATOR: a no-op accumulator path is switched on inside the runner's
//                own OnUpdate. Add a float, compare, reset on crossing. That
//                is the whole treatment.
//   SLOW_TIMER:  a separately named repeating 24 h CallLater entry. Its period
//                keeps it dormant in a bounded run; any crossing invalidates
//                that residency condition. It is not a native Timer.
//
//   The honest expectation is that both land under the noise floor of a single
//   window -- one accumulator step and one dormant queue entry are each far
//   below what a frame-time distribution can resolve. That is the result, not
//   a failure of the bench, and it is reported as BELOW_NOISE rather than
//   rounded up into a claim. What makes the pattern expensive in the field is
//   the same cost multiplied across every mod that reaches for it; B1's
//   per-unit constant is what scales, and B5 is the direct check that one unit
//   really is small.
//
//   The noise floor is not assumed either: it is taken from the run's own
//   recovery window, i.e. how far the server drifted between two windows with
//   identical load. An effect smaller than that drift is not an effect this
//   protocol can see. CallLater multiplicity must also pass its prerequisite.
// ============================================================================

class MPB_AccumulatorLoad
{
    static const float THRESHOLD_SECONDS = 60.0;

    bool  m_Active;
    int   m_Units;
    ref array<float> m_Accumulated;
    int   m_Crossings;
    int   m_SlowTimerCount;

    void MPB_AccumulatorLoad()
    {
        m_Active = false;
        m_Units = 0;
        m_Accumulated = new array<float>();
        m_Crossings = 0;
        m_SlowTimerCount = 0;
    }

    // --- variant (a): every-frame accumulator -------------------------------
    //
    // AMPLIFICATION APPLIES HERE TOO. One accumulator step per frame is far
    // below what a frame-time distribution can resolve, and a first pass that
    // measured exactly one produced numbers that were noise wearing a verdict.
    // `units` accumulator paths run per frame, exactly as B1 runs K timers,
    // and the per-unit figure divides by the same number.
    void ApplyAccumulator(int units)
    {
        RemoveAccumulator();
        if (units < 1)
        {
            units = 1;
        }
        m_Units = units;
        int initializeUnit;
        for (initializeUnit = 0; initializeUnit < units; initializeUnit++)
        {
            m_Accumulated.Insert(0);
        }
        m_Crossings = 0;
        m_Active = true;
    }

    void RemoveAccumulator()
    {
        m_Active = false;
        m_Units = 0;
        m_Accumulated.Clear();
    }

    // Called unconditionally from the runner's OnUpdate. The branch on
    // m_Active is itself part of what a real mod would pay, so it stays inside
    // the measured path rather than being hoisted out by the caller.
    void OnFrame(float timeslice)
    {
        if (!m_Active)
        {
            return;
        }
        int step;
        for (step = 0; step < m_Units; step++)
        {
            float elapsed = m_Accumulated.Get(step) + timeslice;
            if (elapsed >= THRESHOLD_SECONDS)
            {
                elapsed = 0;
                m_Crossings++;
            }
            m_Accumulated.Set(step, elapsed);
        }
    }

    // --- variant (b): dormant repeating CallLater entries -----------------

    void SlowTick()
    {
        m_Crossings++;
    }

    // Amplified for the same reason as the accumulator: `units` dormant 24 h
    // entries, so the per-unit figure has something above the noise floor to
    // divide.
    void ApplySlowTimer(int units)
    {
        RemoveSlowTimer();
        if (units < 1)
        {
            units = 1;
        }
        m_Crossings = 0;

        ScriptCallQueue queue = GetGame().GetCallQueue(CALL_CATEGORY_GAMEPLAY);
        int registered;
        for (registered = 0; registered < units; registered++)
        {
            queue.CallLater(this.SlowTick, 86400000, true);
        }
        m_SlowTimerCount = units;
    }

    void RemoveSlowTimer()
    {
        if (m_SlowTimerCount <= 0)
        {
            m_SlowTimerCount = 0;
            return;
        }
        ScriptCallQueue removeQueue = GetGame().GetCallQueue(CALL_CATEGORY_GAMEPLAY);
        int attempt;
        for (attempt = 0; attempt < m_SlowTimerCount; attempt++)
        {
            removeQueue.Remove(this.SlowTick);
        }
        m_SlowTimerCount = 0;
    }

    void RemoveAll()
    {
        RemoveAccumulator();
        RemoveSlowTimer();
    }

    bool IsAccumulatorActive()
    {
        return m_Active;
    }

    int Crossings()
    {
        return m_Crossings;
    }
}
