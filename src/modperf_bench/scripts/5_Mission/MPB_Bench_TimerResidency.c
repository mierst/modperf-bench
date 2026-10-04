// ============================================================================
// MPB_TimerLoad -- ScriptCallQueue load. MPB_NativeTimerLoad below is a
// separate, never-due native Timer treatment; these mechanisms are not priced
// interchangeably.
//
// WHAT IS BEING MEASURED
//   Period 0 CallLater: due/firing queue entries with empty callback bodies.
//   Long-period CallLater: dormant queue entries, separately named by runner.
//   Both include queue mechanics rather than native Timer residency alone.
//   B2: the same K registrations at longer periods. A period of 10/100/1000 ms
//       fires far less often than every frame, so whatever cost survives at
//       long periods is residency (the queue walking the entry and finding it
//       not due), and the difference against period 0 is firing cost.
//
// HOW K LIVE TIMERS COME FROM ONE METHOD
//   Calling the SAME function reference K times is an engine-dependent
//   multiplicity premise. The counted one-versus-many prerequisite must
//   demonstrate it on the current build before queue-dependent rows run.
//
// TEARDOWN
//   ScriptCallQueue.Remove(fn) drops entries for that function+instance. It is
//   called repeatedly rather than once: if Remove clears every matching entry
//   the extra calls are no-ops, and if it only clears one they are necessary.
//   Either way the recovery window is the real check -- if frame time does not
//   return to baseline, the run is reported INVALID_A_PRIME rather than
//   quietly contributing a wrong constant.
// ============================================================================

class MPB_TimerLoad
{
    int  m_ActiveCount;
    int  m_CountedActive;
    int  m_PeriodMs;
    int  m_FireCount;

    void MPB_TimerLoad()
    {
        m_ActiveCount = 0;
        m_CountedActive = 0;
        m_PeriodMs = 0;
        m_FireCount = 0;
    }

    // The unit under test. Deliberately empty: this bench prices the queue
    // entry itself, not the work a real mod would do inside one.
    void NoOp()
    {
    }

    // A counted variant. Not used for the measured runs -- the increment
    // itself would be part of what gets measured -- but it is what makes the
    // self-check possible: the whole amplification premise is that K
    // registrations of one method produce K queue entries, and that has to be
    // demonstrated on the box under test rather than assumed. If this fires
    // once per frame instead of K times per frame, every K-divided per-unit
    // number in the results is wrong by a factor of K, and the run says so.
    void CountedNoOp()
    {
        m_FireCount++;
    }

    void ApplyCounted(int count, int periodMs)
    {
        RemoveCounted();

        m_FireCount = 0;
        ScriptCallQueue countedQueue = GetGame().GetCallQueue(CALL_CATEGORY_GAMEPLAY);
        int countedIndex;
        for (countedIndex = 0; countedIndex < count; countedIndex++)
        {
            countedQueue.CallLater(this.CountedNoOp, periodMs, true);
        }
        m_CountedActive = count;
    }

    void RemoveCounted()
    {
        if (m_CountedActive <= 0)
        {
            m_CountedActive = 0;
            return;
        }
        ScriptCallQueue removeQueue = GetGame().GetCallQueue(CALL_CATEGORY_GAMEPLAY);
        int removeIndex;
        for (removeIndex = 0; removeIndex < m_CountedActive; removeIndex++)
        {
            removeQueue.Remove(this.CountedNoOp);
        }
        m_CountedActive = 0;
    }

    int FireCount()
    {
        return m_FireCount;
    }

    void Apply(int count, int periodMs)
    {
        Remove();

        m_PeriodMs = periodMs;
        m_FireCount = 0;

        ScriptCallQueue queue = GetGame().GetCallQueue(CALL_CATEGORY_GAMEPLAY);
        int registered;
        for (registered = 0; registered < count; registered++)
        {
            queue.CallLater(this.NoOp, periodMs, true);
        }
        m_ActiveCount = count;
    }

    void Remove()
    {
        if (m_ActiveCount <= 0)
        {
            m_ActiveCount = 0;
            return;
        }

        ScriptCallQueue queue = GetGame().GetCallQueue(CALL_CATEGORY_GAMEPLAY);
        int attempt;
        for (attempt = 0; attempt < m_ActiveCount; attempt++)
        {
            queue.Remove(this.NoOp);
        }
        queue.Remove(this.CountedNoOp);
        m_ActiveCount = 0;
    }

    int ActiveCount()
    {
        return m_ActiveCount;
    }

    int PeriodMs()
    {
        return m_PeriodMs;
    }
}

// A native Timer is one retained object, not one CallLater registration.
// The callback counter proves the dormancy condition throughout observation.
// A fire invalidates residency rather than silently turning it into firing
// cost. The runner must verify ActiveCount() equals requested K as well.
class MPB_NativeTimerLoad
{
    static const int MAX_NATIVE_TIMERS = 10000;
    static const float DORMANT_PERIOD_SECONDS = 86400.0;

    ref array<ref Timer> m_NativeTimers;
    int m_NativeFireCount;

    void MPB_NativeTimerLoad()
    {
        m_NativeTimers = new array<ref Timer>();
        m_NativeFireCount = 0;
    }

    void CountedTick()
    {
        m_NativeFireCount++;
    }

    void Apply(int count)
    {
        Remove();
        m_NativeFireCount = 0;
        if (count < 1 || count > MAX_NATIVE_TIMERS)
        {
            return;
        }
        int nativeIndex;
        for (nativeIndex = 0; nativeIndex < count; nativeIndex++)
        {
            Timer nativeTimer = new Timer(CALL_CATEGORY_GAMEPLAY);
            nativeTimer.Run(DORMANT_PERIOD_SECONDS, this, "CountedTick", null, true);
            if (!nativeTimer.IsRunning())
            {
                nativeTimer.Stop();
                Remove();
                return;
            }
            m_NativeTimers.Insert(nativeTimer);
        }
    }

    void Remove()
    {
        int stopIndex;
        for (stopIndex = 0; stopIndex < m_NativeTimers.Count(); stopIndex++)
        {
            Timer stopTimer = m_NativeTimers.Get(stopIndex);
            stopTimer.Stop();
        }
        m_NativeTimers.Clear();
    }

    int ActiveCount()
    {
        int runningCount = 0;
        int runningIndex;
        for (runningIndex = 0; runningIndex < m_NativeTimers.Count(); runningIndex++)
        {
            if (m_NativeTimers.Get(runningIndex).IsRunning())
            {
                runningCount++;
            }
        }
        return runningCount;
    }

    int FireCount()
    {
        return m_NativeFireCount;
    }

    bool IsDormant()
    {
        return m_NativeFireCount == 0;
    }
}
