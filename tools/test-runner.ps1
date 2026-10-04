[CmdletBinding()]
param([switch]$LegacyControl)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$sourcePath = Join-Path $repo 'src\modperf_bench\scripts\5_Mission\MPB_Runner.c'
$source = Get-Content -LiteralPath $sourcePath -Raw
if ($LegacyControl) {
    $source = (& git -c "safe.directory=$repo" -C $repo show 'HEAD:src/modperf_bench/scripts/5_Mission/MPB_Runner.c') -join "`n"
}
function Method([string]$Name) {
    $signature = [regex]::Match($source, '(?:private\s+)?void\s+' + $Name + '\([^)]*\)\s*\{')
    if (-not $signature.Success) { throw "Missing production method $Name" }
    $begin = $signature.Index
    $brace = $source.IndexOf('{', $begin)
    $depth = 1; $cursor = $brace + 1
    while ($depth -gt 0 -and $cursor -lt $source.Length) {
        if ($source[$cursor] -eq '{') { $depth++ }
        if ($source[$cursor] -eq '}') { $depth-- }
        $cursor++
    }
    $method = $source.Substring($begin, $cursor - $begin)
    $method = $method -replace '^private\s+', ''
    $method = 'public ' + $method
    $method = $method -replace 'Math\.AbsFloat', 'Math.Abs'
    $method = $method -replace '\bfloat\b', 'double'
    $method
}
$conclude = Method 'Conclude'
$start = Method 'StartSelfCheckOne'
$finish = Method 'FinishSelfCheck'
$code = @'
using System;
using System.Collections.Generic;
public class Values { public List<int> Data=new List<int>(); public int Count(){return Data.Count;} public int Get(int i){return Data[i];} }
public class Config { public Values m_KValues=new Values(); public int m_B2K=500,m_B5Units=1000; public HashSet<string> Enabled=new HashSet<string>(); public bool WantsBench(string id){return Enabled.Contains(id);} }
public class TimerLoad { public int Count; public void ApplyCounted(int count,int period){Count=count;} }
public class Reporter { public void Say(string text){} }
public class MPB_Fmt { public static string Dec(double value,int places){return value.ToString();} }
public class RunVerdict {
public const double SIGNIFICANCE_FACTOR=2;
public double m_DeltaMs,m_DriftMs,m_DriftPct,m_BaseMedianMs=1,m_TreatMedianMs=1,m_RecoverMedianMs=1,m_PerUnitNs,m_NoiseNs,m_BaseStdErrMs,m_ModelNs;
public int m_Units=10,m_BaseFrames=10,m_TreatFrames=10,m_RecoverFrames=10;
public bool m_APrimeOk; public string m_Verdict;
CONCLUDE
}
public class SelfCheck {
public const int PHASE_SELFCHECK_ONE=6;
public Config m_Config=new Config(); public TimerLoad m_TimerLoad=new TimerLoad(); public Reporter m_Reporter=new Reporter();
public int m_SelfCheckK,m_Phase,m_SelfCheckFiresOne=1000,m_SelfCheckFramesOne=40000,m_SelfCheckFiresMany=1000000;
public double m_PhaseSeconds,m_SelfCheckSecondsOne=1,m_SelfCheckSecondsMany=1,m_QueueTicksPerSecond,m_EngineFramesPerSecond,m_FramesPerQueueTick,m_SelfCheckRatio;
public string m_SelfCheckVerdict;
public void StartClock(){} public Clock m_Clock=new Clock();
START
FINISH
}
public class Clock { public void Start(){} }
public class RunnerFixtures {
public static void Assert(bool valid,string reason){if(!valid)throw new Exception(reason);}
public static void Run(){
var r=new RunVerdict(); r.m_TreatMedianMs=.8; r.Conclude(10); Assert(r.m_Verdict=="REQUIRES_ANALYSIS","valid negative contrast must survive pooling");
r.m_TreatMedianMs=1.001; r.m_BaseStdErrMs=1; r.Conclude(10); Assert(r.m_Verdict=="REQUIRES_ANALYSIS","valid small contrast must survive pooling");
r.m_RecoverMedianMs=2; r.Conclude(10); Assert(r.m_Verdict=="INVALID_A_PRIME","bad recovery must fail");
var s=new SelfCheck(); s.m_Config.m_KValues.Data.Add(1); s.m_Config.Enabled.Add("B2"); s.m_Config.Enabled.Add("B5"); s.StartSelfCheckOne(); Assert(s.m_SelfCheckK==1000,"selfcheck must cover actual B2/B5 amplification");
s.m_Config.Enabled.Clear(); s.StartSelfCheckOne(); Assert(s.m_SelfCheckK>=2,"K=1 cannot prove multiplicity");
s.m_SelfCheckK=1000; s.m_SelfCheckFiresMany=1000; s.FinishSelfCheck(); Assert(s.m_SelfCheckVerdict=="DEDUPLICATED","deduplicating queue must fail");
s.m_SelfCheckFiresMany=1000000; s.FinishSelfCheck(); Assert(s.m_SelfCheckVerdict=="K_ENTRIES_CONFIRMED","distinct entries must pass");
s.m_SelfCheckFiresOne=0; s.FinishSelfCheck(); Assert(s.m_SelfCheckVerdict=="NO_FIRES","no callbacks must fail");
}
}
'@
$code = $code.Replace('CONCLUDE',$conclude).Replace('START',$start).Replace('FINISH',$finish)
Add-Type -TypeDefinition $code
[RunnerFixtures]::Run()
Write-Output 'PASS: production runner signed contrasts, recovery gate and multiplicity decisions (compatible subset; not engine qualification).'
