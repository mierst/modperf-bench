// ============================================================================
// MPB_TimerLoad -- the load side of B1 (timer residency) and B2 (firing cost
// vs residency).
//
// WHAT IS BEING MEASURED
//   B1: what does merely HAVING a repeating call-queue entry cost per frame,
//       before it does any work? Registered with period 0 and repeat=true, so
//       the entry is due every frame, and the callback body is empty. The
//       delta between the baseline and treatment windows, divided by K, is
//       the per-timer per-frame cost.
//   B2: the same K registrations at longer periods. A period of 10/100/1000 ms
//       fires far less often than every frame, so whatever cost survives at
//       long periods is residency (the queue walking the entry and finding it
//       not due), and the difference against period 0 is firing cost.
//
// HOW K LIVE TIMERS COME FROM ONE METHOD
//   ScriptCallQueue.CallLater registers a call, not a subscription: calling it
//   K times with the SAME function reference adds K distinct queue entries.
//   That is deliberate here -- it gives K identical, independently-walked
//   entries whose only content is a return, which is exactly the unit the
//   model prices.
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
