// ============================================================================
// MPB_InventoryLookup -- B3, the per-call cost of GameInventory.FindAttachment.
//
// WHY THIS BENCH EXISTS
//   "Cache the reference instead of looking it up every frame" is cheap advice
//   to give and expensive to justify. This puts a number on the lookup so the
//   recommendation can be priced rather than asserted.
//
// SHAPE OF THE MEASUREMENT
//   No players and no A/B/A' windows are involved. One server-side entity with
//   attachment slots is created, bounded lookups run inside a SINGLE frame, and the
//   elapsed wall time comes from GetGame().GetTickTime() deltas around the
//   loop. The entity is deleted in the same frame. N is large (100k by
//   default) for two reasons: it takes the total well clear of the float
//   resolution of GetTickTime, and it makes the loop's own overhead a
//   measurable fraction rather than a rounding error.
//
//   Equal-sized control/lookup pairs alternate order. The control consumes a
//   cached reference with the same assignment, branch and hit-count sink as
//   the lookup arm. This estimates the increment over cached-reference work;
//   it is not a universal native-call cost or a statistically qualified bound.
//   Raw ordered pair deltas and a conservative clock quantization bound are
//   retained. A negative or unresolved net difference is inconclusive.
//
// ENTITY CHOICE
//   The default is an ordinary ItemBase with attachment slots rather than a
//   survivor. A survivor spawned server-side with no client behind it is a
//   networked character the engine expects to own an identity, and the bench
//   should not be the thing that finds out what happens when it does not. The
//   type, the slot, and the attachment are all config-driven, so a survivor
//   can be measured by editing the config file instead of rebuilding, and the
//   type actually used is recorded in the results.
//
//   A miss (empty slot) and a hit (occupied slot) are not necessarily the same
//   cost, so the bench fills the slot when it can and records whether the
//   lookups were hitting -- a per-call number without that flag is not
//   interpretable.
// ============================================================================

class MPB_InventoryLookup
{
    static const int MAX_LOOKUPS = 1000000;
    static const int MAX_REPEATS = 1000;
    static const int MAX_LOOKUP_CALLS = 10000000;

    bool   m_Ok;
    string m_Status;
    int    m_Replicate;
    string m_RequestedType;
    string m_UsedType;
    bool   m_Substituted;
    string m_SlotName;
    string m_AttachmentType;
    string m_ResolvedSlotName;
    string m_SlotSource;
    int    m_SlotId;
    int    m_SlotCount;
    bool   m_AttachmentCreated;
    bool   m_AttachmentPresent;
    int    m_Lookups;
    int    m_Repeats;
    int    m_TotalCalls;
    int    m_Hits;
    int    m_ControlHits;
    int    m_PairCount;
    int    m_RepeatsPerPair;
    float  m_TickResolutionSeconds;
    float  m_PerCallNsResolution;
    float  m_ControlSeconds;
    float  m_MeasuredSeconds;
    float  m_PerCallNsRaw;
    float  m_PerCallNsNet;
    float  m_QuantizationBoundSeconds;
    ref array<float> m_PairControlSeconds;
    ref array<float> m_PairMeasuredSeconds;
    ref array<float> m_PairDeltaSeconds;

    void MPB_InventoryLookup()
    {
        m_Ok = false;
        m_Status = "NOT_RUN";
        m_Replicate = 0;
        m_RequestedType = "";
        m_UsedType = "";
        m_Substituted = false;
        m_SlotName = "";
        m_AttachmentType = "";
        m_ResolvedSlotName = "";
        m_SlotSource = "";
        m_SlotId = -1;
        m_SlotCount = 0;
        m_AttachmentCreated = false;
        m_AttachmentPresent = false;
        m_Lookups = 0;
        m_Repeats = 0;
        m_TotalCalls = 0;
        m_Hits = 0;
        m_ControlHits = 0;
        m_PairCount = 0;
        m_RepeatsPerPair = 0;
        m_TickResolutionSeconds = 0;
        m_PerCallNsResolution = 0;
        m_ControlSeconds = 0;
        m_MeasuredSeconds = 0;
        m_PerCallNsRaw = 0;
        m_PerCallNsNet = 0;
        m_QuantizationBoundSeconds = 0;
        m_PairControlSeconds = new array<float>();
        m_PairMeasuredSeconds = new array<float>();
        m_PairDeltaSeconds = new array<float>();
    }

    // Check before multiplying or spawning. The single-frame workload limit
    // counts lookup calls; the control adds the same number of iterations.
    static bool WorkIsBounded(int lookups, int repeats)
    {
        if (lookups < 1 || lookups > MAX_LOOKUPS || repeats < 2 || repeats > MAX_REPEATS)
        {
            return false;
        }
        if (BalancedPairCount(repeats) == 0)
        {
            return false;
        }
        return lookups <= MAX_LOOKUP_CALLS / repeats;
    }

    // Preserve total amplification and equal work for both orderings.
    static int BalancedPairCount(int repeats)
    {
        if (repeats < 2 || repeats % 2 != 0)
        {
            return 0;
        }
        if (repeats % 4 == 0)
        {
            return 4;
        }
        return 2;
    }

    static string DeltaStatus(float netSeconds, float boundSeconds)
    {
        if (boundSeconds <= 0)
        {
            return "CLOCK_UNRESOLVED";
        }
        if (netSeconds < 0)
        {
            return "NEGATIVE_DELTA";
        }
        if (netSeconds <= boundSeconds)
        {
            return "BELOW_RESOLUTION";
        }
        return "MEASURED";
    }

    // Creates the subject entity, or null. Kept separate so Run() does not
    // have to carry two spawn attempts' worth of locals (Enforce scopes every
    // local to the whole method, so name reuse across branches is a trap).
    private EntityAI SpawnSubject(string typeName, vector position)
    {
        if (typeName == "")
        {
            return null;
        }
        Object created = GetGame().CreateObject(typeName, position, false, false, true);
        if (!created)
        {
            return null;
        }
        EntityAI asEntity = EntityAI.Cast(created);
        if (!asEntity || !asEntity.GetInventory())
        {
            GetGame().ObjectDelete(created);
            return null;
        }
        return asEntity;
    }

    // Picks the slot id the timed loop will ask for, in this order:
    //   1. the configured slot name, if CfgSlots knows it;
    //   2. the entity's first OCCUPIED slot -- a hit exercises the path a real
    //      mod is on when it looks up an attachment it expects to find;
    //   3. the entity's first configured slot, occupied or not.
    // Whichever it lands on is recorded, because a lookup that hits and a
    // lookup that misses are not necessarily the same cost and a bare
    // per-call number would hide which one was measured.
    private void ResolveSlot(GameInventory inventory, string slotName, EntityAI attached)
    {
        m_SlotCount = inventory.GetSlotIdCount();
        m_SlotId = InventorySlots.INVALID;

        if (slotName != "")
        {
            int named = InventorySlots.GetSlotIdFromString(slotName);
            if (named != InventorySlots.INVALID)
            {
                m_SlotId = named;
                m_SlotSource = "config_name";
                m_ResolvedSlotName = slotName;
                return;
            }
        }

        // Ask the attachment where it actually landed. This is the only
        // reliable way to get an OCCUPIED slot: GetSlotIdCount enumerates the
        // entity's configured slot list, which on the test box did not include
        // the slot a successfully created attachment ended up in -- so
        // scanning that list finds nothing and the timed loop measures a miss
        // when a hit was available.
        if (attached && attached.GetInventory())
        {
            int attachedSlotId;
            string attachedSlotName;
            if (attached.GetInventory().GetCurrentAttachmentSlotInfo(attachedSlotId, attachedSlotName))
            {
                if (attachedSlotId != InventorySlots.INVALID)
                {
                    m_SlotId = attachedSlotId;
                    m_ResolvedSlotName = attachedSlotName;
                    m_SlotSource = "attachment_slot";
                    return;
                }
            }
        }

        int occupiedIndex;
        for (occupiedIndex = 0; occupiedIndex < m_SlotCount; occupiedIndex++)
        {
            int occupiedCandidate = inventory.GetSlotId(occupiedIndex);
            if (inventory.FindAttachment(occupiedCandidate))
            {
                m_SlotId = occupiedCandidate;
                m_ResolvedSlotName = InventorySlots.GetSlotName(occupiedCandidate);
                m_SlotSource = "auto_occupied";
                return;
            }
        }

        if (m_SlotCount > 0)
        {
            m_SlotId = inventory.GetSlotId(0);
            m_ResolvedSlotName = InventorySlots.GetSlotName(m_SlotId);
            m_SlotSource = "auto_first";
            return;
        }

        m_SlotSource = "none";
    }

    void Run(string preferredType, string fallbackType, string slotName, string attachmentType, int lookups, int repeats, vector position)
    {
        m_RequestedType = preferredType;
        m_SlotName = slotName;
        m_AttachmentType = attachmentType;
        m_Lookups = lookups;
        m_Repeats = repeats;
        m_Ok = false;
        m_Status = "NOT_RUN";
        m_Hits = 0;
        m_ControlHits = 0;
        m_ControlSeconds = 0;
        m_MeasuredSeconds = 0;
        m_TotalCalls = 0;
        m_PairCount = 0;
        m_RepeatsPerPair = 0;
        m_TickResolutionSeconds = 0;
        m_QuantizationBoundSeconds = 0;
        m_PerCallNsRaw = 0;
        m_PerCallNsNet = 0;
        m_PerCallNsResolution = 0;
        m_UsedType = "";
        m_Substituted = false;
        m_AttachmentCreated = false;
        m_AttachmentPresent = false;
        m_SlotId = -1;
        m_SlotCount = 0;
        m_ResolvedSlotName = "";
        m_SlotSource = "";
        m_PairControlSeconds.Clear();
        m_PairMeasuredSeconds.Clear();
        m_PairDeltaSeconds.Clear();
        if (!WorkIsBounded(lookups, repeats))
        {
            m_Status = "INVALID_WORK";
            return;
        }
        m_TotalCalls = lookups * repeats;
        m_PairCount = BalancedPairCount(repeats);
        m_RepeatsPerPair = repeats / m_PairCount;

        EntityAI subject = SpawnSubject(preferredType, position);
        if (subject)
        {
            m_UsedType = preferredType;
        }
        else
        {
            subject = SpawnSubject(fallbackType, position);
            if (!subject)
            {
                m_Status = "SPAWN_FAILED";
                return;
            }
            m_UsedType = fallbackType;
            m_Substituted = true;
        }

        GameInventory inventory = subject.GetInventory();

        // Try to fill a slot first, by TYPE, letting the engine choose where
        // it goes -- naming a slot and then attaching into it needs both
        // guesses to be right. If that fails, ResolveSlot's second attempt
        // below tries again into the slot it picked.
        EntityAI attached = null;
        if (attachmentType != "")
        {
            attached = inventory.CreateAttachment(attachmentType);
            if (attached)
            {
                m_AttachmentCreated = true;
            }
        }

        ResolveSlot(inventory, slotName, attached);

        if (!m_AttachmentCreated && attachmentType != "" && m_SlotId != InventorySlots.INVALID)
        {
            EntityAI attachedToSlot = inventory.CreateAttachmentEx(attachmentType, m_SlotId);
            if (attachedToSlot)
            {
                m_AttachmentCreated = true;
            }
        }
        if (m_SlotId == InventorySlots.INVALID)
        {
            GetGame().ObjectDelete(subject);
            m_Status = "SLOT_UNKNOWN";
            return;
        }

        // The flag has to describe the slot actually being looked up, not
        // whether an attachment was created somewhere on the entity.
        m_AttachmentPresent = inventory.FindAttachment(m_SlotId) != null;

        // Warm-up: first-touch costs (any lazy slot table build) belong to
        // neither loop.
        int warmIndex;
        EntityAI warmResult;
        for (warmIndex = 0; warmIndex < 1000; warmIndex++)
        {
            warmResult = inventory.FindAttachment(m_SlotId);
        }

        // A bounded probe records observed quantization. If it cannot observe
        // a positive step, do not start the amplified timing batches.
        m_TickResolutionSeconds = MeasureTickResolution();
        if (m_TickResolutionSeconds <= 0)
        {
            GetGame().ObjectDelete(subject);
            m_Status = "CLOCK_UNRESOLVED";
            return;
        }

        EntityAI cachedResult = inventory.FindAttachment(m_SlotId);
        bool invalidClock = false;
        int pairIndex;
        for (pairIndex = 0; pairIndex < m_PairCount; pairIndex++)
        {
            float pairControl;
            float pairMeasured;
            if (pairIndex % 2 == 0)
            {
                pairControl = TimeControlBatch(cachedResult, lookups, m_RepeatsPerPair);
                pairMeasured = TimeLookupBatch(inventory, lookups, m_RepeatsPerPair);
            }
            else
            {
                pairMeasured = TimeLookupBatch(inventory, lookups, m_RepeatsPerPair);
                pairControl = TimeControlBatch(cachedResult, lookups, m_RepeatsPerPair);
            }
            if (pairControl < 0 || pairMeasured < 0)
            {
                invalidClock = true;
            }
            m_ControlSeconds = m_ControlSeconds + pairControl;
            m_MeasuredSeconds = m_MeasuredSeconds + pairMeasured;
            m_PairControlSeconds.Insert(pairControl);
            m_PairMeasuredSeconds.Insert(pairMeasured);
            m_PairDeltaSeconds.Insert(pairMeasured - pairControl);
        }

        GetGame().ObjectDelete(subject);
        // Each measured interval may be off by one clock step; a paired
        // difference by two. Sum these bounds instead of claiming averaging
        // makes deterministic quantization disappear.
        m_QuantizationBoundSeconds = 2 * m_PairCount * m_TickResolutionSeconds;
        m_PerCallNsRaw = (m_MeasuredSeconds * 1000000000.0) / m_TotalCalls;
        m_PerCallNsNet = ((m_MeasuredSeconds - m_ControlSeconds) * 1000000000.0) / m_TotalCalls;
        m_PerCallNsResolution = (m_QuantizationBoundSeconds * 1000000000.0) / m_TotalCalls;
        m_Status = DeltaStatus(m_MeasuredSeconds - m_ControlSeconds, m_QuantizationBoundSeconds);
        if (invalidClock)
        {
            m_Status = "CLOCK_NONMONOTONIC";
        }
        if (m_Hits != m_ControlHits)
        {
            m_Status = "HIT_MISMATCH";
        }
        m_Ok = m_Status == "MEASURED";
    }

    private float TimeControlBatch(EntityAI cachedResult, int lookups, int repeats)
    {
        int controlHits = 0;
        int controlRepeat;
        int controlIndex;
        EntityAI controlFound;
        float controlStart = GetGame().GetTickTime();
        for (controlRepeat = 0; controlRepeat < repeats; controlRepeat++)
        {
            for (controlIndex = 0; controlIndex < lookups; controlIndex++)
            {
                controlFound = cachedResult;
                if (controlFound)
                {
                    controlHits++;
                }
            }
        }
        float controlEnd = GetGame().GetTickTime();
        m_ControlHits = m_ControlHits + controlHits;
        return controlEnd - controlStart;
    }

    private float TimeLookupBatch(GameInventory inventory, int lookups, int repeats)
    {
        int lookupHits = 0;
        int lookupRepeat;
        int lookupIndex;
        EntityAI lookupFound;
        float lookupStart = GetGame().GetTickTime();
        for (lookupRepeat = 0; lookupRepeat < repeats; lookupRepeat++)
        {
            for (lookupIndex = 0; lookupIndex < lookups; lookupIndex++)
            {
                lookupFound = inventory.FindAttachment(m_SlotId);
                if (lookupFound)
                {
                    lookupHits++;
                }
            }
        }
        float lookupEnd = GetGame().GetTickTime();
        m_Hits = m_Hits + lookupHits;
        return lookupEnd - lookupStart;
    }

    // Observe eight complete steps after discarding the first partial step.
    // The largest observed step conservatively includes float quantization
    // and sampling gaps. This is clock evidence, not an uncertainty interval
    // for environmental noise. The probe is bounded even if the clock stalls.
    private float MeasureTickResolution()
    {
        float previous = GetGame().GetTickTime();
        float observedStep = 0;
        int transitions = 0;
        int spinGuard = 0;
        while (transitions < 9 && spinGuard < 2000000)
        {
            spinGuard++;
            float current = GetGame().GetTickTime();
            if (current < previous)
            {
                return 0;
            }
            if (current > previous)
            {
                float elapsedStep = current - previous;
                if (transitions > 0 && elapsedStep > observedStep)
                {
                    observedStep = elapsedStep;
                }
                previous = current;
                transitions++;
            }
        }
        if (transitions < 9)
        {
            return 0;
        }
        return observedStep;
    }

    string SummaryText()
    {
        // Built in steps rather than as one expression: Enforce rejects a
        // long concatenation chain with "Formula too complex", and the limit
        // is low enough that a summary line hits it.
        string text = "B3 entity=" + m_UsedType;
        text = text + " slot=" + m_ResolvedSlotName;
        text = text + " slot_src=" + m_SlotSource;
        text = text + " hit=" + MPB_Fmt.Bool(m_AttachmentPresent);
        text = text + " calls=" + m_TotalCalls;
        text = text + " loop_ms=" + MPB_Fmt.Ms(m_MeasuredSeconds * 1000.0);
        text = text + " ctrl_ms=" + MPB_Fmt.Ms(m_ControlSeconds * 1000.0);
        text = text + " clock_step_ms=" + MPB_Fmt.Ms(m_TickResolutionSeconds * 1000.0);
        text = text + " per_call_ns_raw=" + MPB_Fmt.Ns(m_PerCallNsRaw);
        if (m_Ok)
        {
            text = text + " per_call_ns_net=" + MPB_Fmt.Ns(m_PerCallNsNet);
        }
        else
        {
            text = text + " per_call_ns_net=inconclusive";
        }
        text = text + " resolution_ns=" + MPB_Fmt.Ns(m_PerCallNsResolution);
        text = text + " verdict=" + m_Status;
        return text;
    }

    string ToJsonObject()
    {
        string text = "    {\n";
        text = text + "      \"id\": \"B3\",\n";
        text = text + "      \"name\": \"inventory lookup\",\n";
        text = text + "      \"mechanism\": \"inventory_lookup\",\n";
        text = text + "      \"replicate\": " + m_Replicate + ",\n";
        if (m_AttachmentPresent)
        {
            text = text + "      \"variant\": \"occupied_slot\",\n";
        }
        else
        {
            text = text + "      \"variant\": \"empty_slot\",\n";
        }
        text = text + "      \"protocol\": \"paired_loops\",\n";
        text = text + "      \"protocol_version\": 2,\n";
        text = text + "      \"control\": \"cached_reference_assignment_branch_hit_sink\",\n";
        text = text + "      \"status\": " + MPB_Fmt.Quoted(m_Status) + ",\n";
        if (m_Ok)
        {
            text = text + "      \"validity_reasons\": [],\n";
        }
        else
        {
            text = text + "      \"validity_reasons\": [" + MPB_Fmt.Quoted(m_Status) + "],\n";
        }
        text = text + "      \"requested_entity\": " + MPB_Fmt.Quoted(m_RequestedType) + ",\n";
        text = text + "      \"entity\": " + MPB_Fmt.Quoted(m_UsedType) + ",\n";
        text = text + "      \"entity_substituted\": " + MPB_Fmt.Bool(m_Substituted) + ",\n";
        text = text + "      \"slot_name_requested\": " + MPB_Fmt.Quoted(m_SlotName) + ",\n";
        text = text + "      \"slot_name_resolved\": " + MPB_Fmt.Quoted(m_ResolvedSlotName) + ",\n";
        text = text + "      \"slot_source\": " + MPB_Fmt.Quoted(m_SlotSource) + ",\n";
        text = text + "      \"slot_id\": " + m_SlotId + ",\n";
        text = text + "      \"slot_count\": " + m_SlotCount + ",\n";
        text = text + "      \"attachment_requested\": " + MPB_Fmt.Quoted(m_AttachmentType) + ",\n";
        text = text + "      \"attachment_created\": " + MPB_Fmt.Bool(m_AttachmentCreated) + ",\n";
        text = text + "      \"attachment_present\": " + MPB_Fmt.Bool(m_AttachmentPresent) + ",\n";
        text = text + "      \"lookups\": " + m_Lookups + ",\n";
        text = text + "      \"repeats\": " + m_Repeats + ",\n";
        text = text + "      \"total_calls\": " + m_TotalCalls + ",\n";
        text = text + "      \"pair_count\": " + m_PairCount + ",\n";
        text = text + "      \"repeats_per_pair\": " + m_RepeatsPerPair + ",\n";
        if (m_TickResolutionSeconds > 0)
        {
            text = text + "      \"clock_step_ms\": " + MPB_Fmt.Ms(m_TickResolutionSeconds * 1000.0) + ",\n";
        }
        else
        {
            text = text + "      \"clock_step_ms\": null,\n";
        }
        text = text + "      \"hits\": " + m_Hits + ",\n";
        text = text + "      \"control_hits\": " + m_ControlHits + ",\n";
        if (m_PairDeltaSeconds.Count() > 0)
        {
            text = text + "      \"control_loop_ms\": " + MPB_Fmt.Ms(m_ControlSeconds * 1000.0) + ",\n";
            text = text + "      \"measured_loop_ms\": " + MPB_Fmt.Ms(m_MeasuredSeconds * 1000.0) + ",\n";
            text = text + "      \"per_call_ns_raw\": " + MPB_Fmt.Ns(m_PerCallNsRaw) + ",\n";
            text = text + "      \"per_call_ns_resolution\": " + MPB_Fmt.Ns(m_PerCallNsResolution) + ",\n";
        }
        else
        {
            text = text + "      \"control_loop_ms\": null,\n";
            text = text + "      \"measured_loop_ms\": null,\n";
            text = text + "      \"per_call_ns_raw\": null,\n";
            text = text + "      \"per_call_ns_resolution\": null,\n";
        }
        if (m_Ok)
        {
            text = text + "      \"per_call_ns_net\": " + MPB_Fmt.Ns(m_PerCallNsNet) + ",\n";
        }
        else
        {
            text = text + "      \"per_call_ns_net\": null,\n";
        }
        if (m_PairDeltaSeconds.Count() > 0)
        {
            text = text + "      \"net_ns_diagnostic\": " + MPB_Fmt.Ns(m_PerCallNsNet) + ",\n";
            text = text + "      \"quantization_bound_ms\": " + MPB_Fmt.Ms(m_QuantizationBoundSeconds * 1000.0) + ",\n";
            text = text + "      \"resolution_bound_ns_lower\": " + MPB_Fmt.Ns(m_PerCallNsNet - m_PerCallNsResolution) + ",\n";
            text = text + "      \"resolution_bound_ns_upper\": " + MPB_Fmt.Ns(m_PerCallNsNet + m_PerCallNsResolution) + ",\n";
        }
        else
        {
            text = text + "      \"net_ns_diagnostic\": null,\n";
            text = text + "      \"quantization_bound_ms\": null,\n";
            text = text + "      \"resolution_bound_ns_lower\": null,\n";
            text = text + "      \"resolution_bound_ns_upper\": null,\n";
        }
        text = text + "      \"resolution_bound_kind\": \"observed_clock_quantization_only\",\n";
        text = text + "      \"pairs\": " + PairsJson() + ",\n";
        text = text + "      \"model_ns\": null,\n";
        text = text + "      \"verdict\": " + MPB_Fmt.Quoted(m_Status) + "\n";
        text = text + "    }";
        return text;
    }

    private string PairsJson()
    {
        string pairsText = "[";
        int jsonPair;
        for (jsonPair = 0; jsonPair < m_PairDeltaSeconds.Count(); jsonPair++)
        {
            if (jsonPair > 0)
            {
                pairsText = pairsText + ",";
            }
            string pairText = "{\"index\":" + jsonPair;
            if (jsonPair % 2 == 0)
            {
                pairText = pairText + ",\"order\":\"control_lookup\"";
            }
            else
            {
                pairText = pairText + ",\"order\":\"lookup_control\"";
            }
            pairText = pairText + ",\"calls\":" + (m_Lookups * m_RepeatsPerPair);
            pairText = pairText + ",\"control_ms\":" + MPB_Fmt.Ms(m_PairControlSeconds.Get(jsonPair) * 1000.0);
            pairText = pairText + ",\"lookup_ms\":" + MPB_Fmt.Ms(m_PairMeasuredSeconds.Get(jsonPair) * 1000.0);
            pairText = pairText + ",\"delta_ms\":" + MPB_Fmt.Ms(m_PairDeltaSeconds.Get(jsonPair) * 1000.0);
            pairsText = pairsText + pairText + "}";
        }
        return pairsText + "]";
    }
}
