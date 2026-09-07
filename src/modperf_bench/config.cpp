// ============================================================================
// @modperf-bench -- server-side DayZ script-cost micro-benchmark mod.
//
// Measures what a static script-cost model predicts: timer residency, timer
// firing cost, inventory lookup cost, and the price of an every-frame
// accumulator. Methodology: https://dayz.fyi/modperf
//
// PACKAGING
//   src/modperf_bench  ->  @modperf-bench/addons/modperf_bench.pbo
//   $PBOPREFIX$ = modperf_bench  (must match CfgMods.dir and every files[]
//   entry below, or the engine will not find the scripts).
//
// requiredAddons[] IS DELIBERATELY EMPTY.
//   An unmet requiredAddons entry fires a BLOCKING modal dialog on a Windows
//   dedicated server -- boot stops until somebody clicks OK, and no launch
//   flag or server config key suppresses it. Enforce Script base classes
//   (MissionServer, ...) resolve through the script-module namespace at load
//   time, not through CfgPatches, so no external addon ever needs naming here.
//   Never add one.
//
// 5_Mission ONLY. Everything this mod touches (MissionServer.OnUpdate, the
// gameplay call queue, inventory) is reachable from the Mission module, and a
// single module keeps the load surface as small as the thing being measured.
//
// SERVER-SIDE ONLY, and inert unless armed: the runner does nothing at all
// unless $profile:modperf-bench/config.json exists. Nothing player-shaped is
// read, logged, or written -- there is no player data in this mod's output by
// construction.
// ============================================================================

class CfgPatches
{
    class modperf_bench
    {
        units[] = {};
        weapons[] = {};
        requiredVersion = 0.1;
        requiredAddons[] = {};
    };
};

class CfgMods
{
    class modperf_bench
    {
        dir = "modperf_bench";
        picture = "";
        action = "";
        hideName = 0;
        hidePicture = 0;
        name = "modperf-bench";
        credits = "";
        author = "modperf-bench contributors";
        authorID = "0";
        version = "1.0.0";
        extra = 0;
        type = "mod";
        dependencies[] = { "Mission" };

        class defs
        {
            class missionScriptModule
            {
                value = "";
                files[] = { "modperf_bench/scripts/5_Mission" };
            };
        };
    };
};
