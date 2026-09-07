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
//   attachment slots is created, N lookups run inside a SINGLE frame, and the
//   elapsed wall time comes from GetGame().GetTickTime() deltas around the
//   loop. The entity is deleted in the same frame. N is large (100k by
//   default) for two reasons: it takes the total well clear of the float
//   resolution of GetTickTime, and it makes the loop's own overhead a
//   measurable fraction rather than a rounding error.
//
//   That loop overhead is measured, not assumed: an identical-length control
//   loop doing a trivial integer add runs first, and both the raw and the
//   overhead-subtracted per-call figures are reported. The net figure is the
//   honest lower bound -- a script-level add is not free either, so
//   subtracting it can only understate the call, never overstate it.
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
    bool   m_Ok;
    string m_Status;
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
    float  m_TickResolutionSeconds;
    float  m_PerCallNsResolution;
    float  m_ControlSeconds;
    float  m_MeasuredSeconds;
    float  m_PerCallNsRaw;
    float  m_PerCallNsNet;

    void MPB_InventoryLookup()
    {
        m_Ok = false;
        m_Status = "NOT_RUN";
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
        m_TickResolutionSeconds = 0;
        m_PerCallNsResolution = 0;
        m_ControlSeconds = 0;
        m_MeasuredSeconds = 0;
        m_PerCallNsRaw = 0;
        m_PerCallNsNet = 0;
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

        if (repeats < 1)
        {
            repeats = 1;
        }
        m_Repeats = repeats;
        m_TotalCalls = lookups * repeats;

        // The clock's resolution is measured, not assumed. On the test box
        // GetTickTime advances in ~1 ms steps, which is coarser than a whole
        // 100k-call loop -- the first version of this bench timed each repeat
        // separately and reported per-call figures that were pure
        // quantization: a control loop that landed on 0 ms one run and 1 ms
        // the next moved the "net" per-call cost by 50%.
        //
        // So the WHOLE batch is timed as one measurement rather than each
        // repeat, and `repeats` exists to push the batch far enough past the
        // clock step that the step stops mattering. The measured resolution is
        // reported alongside the result so a reader can check that for
        // themselves instead of trusting it.
        m_TickResolutionSeconds = MeasureTickResolution();

        int sink = 0;
        int hits = 0;
        int repeatIndex;
        int controlIndex;
        int measureIndex;
        EntityAI found;

        // Control batch: same iteration count, trivial body.
        float controlStart = GetGame().GetTickTime();
        for (repeatIndex = 0; repeatIndex < repeats; repeatIndex++)
        {
            for (controlIndex = 0; controlIndex < lookups; controlIndex++)
            {
                sink = sink + controlIndex;
            }
        }
        float controlEnd = GetGame().GetTickTime();

        // Measured batch. The result is consumed so the call cannot be treated
        // as dead.
        int measureRepeat;
        float measureStart = GetGame().GetTickTime();
        for (measureRepeat = 0; measureRepeat < repeats; measureRepeat++)
        {
            for (measureIndex = 0; measureIndex < lookups; measureIndex++)
            {
                found = inventory.FindAttachment(m_SlotId);
                if (found)
                {
                    hits++;
                }
            }
        }
        float measureEnd = GetGame().GetTickTime();

        GetGame().ObjectDelete(subject);

        m_Hits = hits;
        m_ControlSeconds = controlEnd - controlStart;
        m_MeasuredSeconds = measureEnd - measureStart;

        if (m_TotalCalls > 0)
        {
            m_PerCallNsRaw = (m_MeasuredSeconds * 1000000000.0) / m_TotalCalls;
            m_PerCallNsNet = ((m_MeasuredSeconds - m_ControlSeconds) * 1000000000.0) / m_TotalCalls;
            // What one clock step is worth per call -- the floor on how
            // precisely this figure can possibly be known.
            m_PerCallNsResolution = (m_TickResolutionSeconds * 1000000000.0) / m_TotalCalls;
        }

        // sink is only here to keep the control loop honest; reference it so
        // the compiler cannot consider it unused.
        if (sink < 0)
        {
            m_Status = "IMPOSSIBLE";
        }

        m_Ok = true;
        m_Status = "MEASURED";
    }

    // Smallest observable step of GetGame().GetTickTime(): spin until the
    // value changes twice and take the second interval (the first is a
    // partial step, since the spin started mid-tick).
    private float MeasureTickResolution()
    {
        float first = GetGame().GetTickTime();
        int spinGuard = 0;
        while (GetGame().GetTickTime() == first && spinGuard < 20000000)
        {
            spinGuard++;
        }
        float second = GetGame().GetTickTime();
        while (GetGame().GetTickTime() == second && spinGuard < 40000000)
        {
            spinGuard++;
        }
        float third = GetGame().GetTickTime();
        return third - second;
    }

    string SummaryText()
    {
        if (!m_Ok)
        {
            return "B3 status=" + m_Status;
        }
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
        text = text + " per_call_ns_net=" + MPB_Fmt.Ns(m_PerCallNsNet);
        text = text + " resolution_ns=" + MPB_Fmt.Ns(m_PerCallNsResolution);
        text = text + " verdict=" + m_Status;
        return text;
    }

    string ToJsonObject()
    {
        string text = "    {\n";
        text = text + "      \"id\": \"B3\",\n";
        text = text + "      \"name\": \"inventory lookup\",\n";
        text = text + "      \"protocol\": \"single_frame_loop\",\n";
        text = text + "      \"status\": " + MPB_Fmt.Quoted(m_Status) + ",\n";
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
        text = text + "      \"clock_step_ms\": " + MPB_Fmt.Ms(m_TickResolutionSeconds * 1000.0) + ",\n";
        text = text + "      \"per_call_ns_resolution\": " + MPB_Fmt.Ns(m_PerCallNsResolution) + ",\n";
        text = text + "      \"hits\": " + m_Hits + ",\n";
        text = text + "      \"control_loop_ms\": " + MPB_Fmt.Ms(m_ControlSeconds * 1000.0) + ",\n";
        text = text + "      \"measured_loop_ms\": " + MPB_Fmt.Ms(m_MeasuredSeconds * 1000.0) + ",\n";
        text = text + "      \"per_call_ns_raw\": " + MPB_Fmt.Ns(m_PerCallNsRaw) + ",\n";
        text = text + "      \"per_call_ns_net\": " + MPB_Fmt.Ns(m_PerCallNsNet) + ",\n";
        text = text + "      \"model_ns\": null,\n";
        text = text + "      \"verdict\": " + MPB_Fmt.Quoted(m_Status) + "\n";
        text = text + "    }";
        return text;
    }
}
