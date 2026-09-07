// ============================================================================
// MPB_Report -- output side of @modperf-bench.
//
// Three small classes, no inheritance and no cross-file base types (Enforce
// resolves a `class X extends Y` only if Y was already declared, and the file
// order inside a script module folder is filesystem order -- so this mod keeps
// every class standalone on purpose):
//
//   MPB_Fmt     fixed-decimal float -> string, and string sanitising.
//   MPB_Json    minimal hand-rolled JSON reader for the config file.
//   MPB_Report  accumulates the log/result lines and writes the results file.
//
// WHY THE JSON IS HAND-BUILT
//   The engine's JSON serializer reflects over class fields, which makes the
//   emitted shape a side effect of member layout rather than of the schema we
//   promise. Results files are the artifact third parties read, so the schema
//   is written out literally, here, where it can be reviewed.
//
// WHY THE JSON READER IS HAND-ROLLED
//   Same reason in reverse, plus one that matters more: a deserializer that
//   fails silently leaves the mod inert with no way to tell "not armed" from
//   "config rejected". Every key read below has a printed default.
//
// Paths use FORWARD slashes ("$profile:modperf-bench/..."), matching vanilla
// usage. The engine maps that onto the profile directory on both platforms;
// on Windows the files land in <profile>\modperf-bench\.
// ============================================================================

class MPB_Fmt
{
    // Fixed-decimal rendering. Enforce's default float-to-string conversion
    // gives inconsistent precision, and these numbers get parsed downstream,
    // so every emitted float goes through here.
    static string Dec(float value, int places)
    {
        bool negative = false;
        float magnitude = value;
        if (magnitude < 0)
        {
            negative = true;
            magnitude = -magnitude;
        }

        int multiplier = 1;
        int p;
        for (p = 0; p < places; p++)
        {
            multiplier = multiplier * 10;
        }

        // Guard the int conversion: anything past this is nonsense for a
        // frame-time or per-unit-cost number and would silently wrap.
        float scaledFloat = magnitude * multiplier;
        if (scaledFloat > 2000000000.0)
        {
            return "null";
        }

        int scaled = Math.Round(scaledFloat);
        int wholePart = scaled / multiplier;
        int fracPart = scaled - (wholePart * multiplier);

        string fracText = "" + fracPart;
        while (fracText.Length() < places)
        {
            fracText = "0" + fracText;
        }

        string result = "" + wholePart;
        if (places > 0)
        {
            result = result + "." + fracText;
        }
        if (negative)
        {
            result = "-" + result;
        }
        return result;
    }

    static string Ms(float value)
    {
        return Dec(value, 4);
    }

    static string Ns(float value)
    {
        return Dec(value, 1);
    }

    static string Fps(float value)
    {
        return Dec(value, 1);
    }

    // Free-text coming from the operator's config file is echoed into the
    // results JSON. Strip the two characters that could break the document
    // rather than attempting general escaping.
    static string Safe(string text)
    {
        string copy = text;
        copy.Replace("\\", "/");
        copy.Replace("\"", "'");
        return copy;
    }

    static string Quoted(string text)
    {
        return "\"" + Safe(text) + "\"";
    }

    // Enforce renders a bool through string concatenation as 0/1. JSON
    // consumers expect the keywords, so booleans go through here.
    static string Bool(bool value)
    {
        if (value)
        {
            return "true";
        }
        return "false";
    }

    static string Pad2(int value)
    {
        if (value < 10)
        {
            return "0" + value;
        }
        return "" + value;
    }

    // UTC stamp, used for the results filename and the run metadata.
    static string UtcStamp()
    {
        int year;
        int month;
        int day;
        int hour;
        int minute;
        int second;
        GetYearMonthDayUTC(year, month, day);
        GetHourMinuteSecondUTC(hour, minute, second);
        // Assembled step by step: Enforce rejects long concatenation chains
        // with "Formula too complex".
        string stamp = "" + year;
        stamp = stamp + Pad2(month);
        stamp = stamp + Pad2(day);
        stamp = stamp + "-";
        stamp = stamp + Pad2(hour);
        stamp = stamp + Pad2(minute);
        stamp = stamp + Pad2(second);
        return stamp;
    }

    static string UtcIso()
    {
        int isoYear;
        int isoMonth;
        int isoDay;
        int isoHour;
        int isoMinute;
        int isoSecond;
        GetYearMonthDayUTC(isoYear, isoMonth, isoDay);
        GetHourMinuteSecondUTC(isoHour, isoMinute, isoSecond);
        string iso = "" + isoYear;
        iso = iso + "-" + Pad2(isoMonth);
        iso = iso + "-" + Pad2(isoDay);
        iso = iso + "T" + Pad2(isoHour);
        iso = iso + ":" + Pad2(isoMinute);
        iso = iso + ":" + Pad2(isoSecond);
        iso = iso + "Z";
        return iso;
    }
}

class MPB_Json
{
    // Whole file into one string. Line structure is irrelevant to the
    // extractors below, so the config file may be pretty-printed.
    static string ReadAll(string path)
    {
        FileHandle handle = OpenFile(path, FileMode.READ);
        if (handle == 0)
        {
            return "";
        }

        string accumulated = "";
        string line;
        int guard = 0;
        // Hard iteration cap: FGets' end-of-file return value is documented
        // as -1 but an empty trailing line returns 0, and a reader that trusts
        // only one of those spins forever on the wrong build.
        while (guard < 4096)
        {
            if (FGets(handle, line) < 0)
            {
                break;
            }
            accumulated = accumulated + line;
            guard++;
        }
        CloseFile(handle);
        return accumulated;
    }

    // Raw scalar text following "key": , stopping at the first structural
    // character. Not valid for string or array values -- see below.
    static string ScalarOf(string source, string key)
    {
        int keyIndex = source.IndexOf("\"" + key + "\"");
        if (keyIndex < 0)
        {
            return "";
        }
        int colonIndex = source.IndexOfFrom(keyIndex, ":");
        if (colonIndex < 0)
        {
            return "";
        }

        int total = source.Length();
        int start = colonIndex + 1;
        while (start < total && source.Substring(start, 1) == " ")
        {
            start++;
        }

        int end = start;
        string character;
        while (end < total)
        {
            character = source.Substring(end, 1);
            if (character == "," || character == "}" || character == "]")
            {
                break;
            }
            end++;
        }
        if (end <= start)
        {
            return "";
        }
        return source.Substring(start, end - start).Trim();
    }

    static float GetFloat(string source, string key, float fallback)
    {
        string text = ScalarOf(source, key);
        if (text == "")
        {
            return fallback;
        }
        return text.ToFloat();
    }

    static int GetInt(string source, string key, int fallback)
    {
        string text = ScalarOf(source, key);
        if (text == "")
        {
            return fallback;
        }
        return text.ToInt();
    }

    // String values are read quote-to-quote so a comma inside the text is
    // harmless.
    static string GetString(string source, string key, string fallback)
    {
        int keyIndex = source.IndexOf("\"" + key + "\"");
        if (keyIndex < 0)
        {
            return fallback;
        }
        int colonIndex = source.IndexOfFrom(keyIndex, ":");
        if (colonIndex < 0)
        {
            return fallback;
        }
        int openQuote = source.IndexOfFrom(colonIndex, "\"");
        if (openQuote < 0)
        {
            return fallback;
        }
        int closeQuote = source.IndexOfFrom(openQuote + 1, "\"");
        if (closeQuote <= openQuote)
        {
            return fallback;
        }
        return source.Substring(openQuote + 1, closeQuote - openQuote - 1);
    }

    // Body of a [ ... ] value, without the brackets. Nested arrays are not
    // supported and are not in the config schema.
    static string ArrayBodyOf(string source, string key)
    {
        int keyIndex = source.IndexOf("\"" + key + "\"");
        if (keyIndex < 0)
        {
            return "";
        }
        int openBracket = source.IndexOfFrom(keyIndex, "[");
        if (openBracket < 0)
        {
            return "";
        }
        int closeBracket = source.IndexOfFrom(openBracket, "]");
        if (closeBracket <= openBracket)
        {
            return "";
        }
        return source.Substring(openBracket + 1, closeBracket - openBracket - 1);
    }

    static bool GetIntArray(string source, string key, array<int> destination)
    {
        string body = ArrayBodyOf(source, key);
        if (body == "")
        {
            return false;
        }
        destination.Clear();

        string token = "";
        int position = 0;
        int length = body.Length();
        string current;
        while (position <= length)
        {
            if (position == length)
            {
                current = ",";
            }
            else
            {
                current = body.Substring(position, 1);
            }

            if (current == ",")
            {
                string trimmed = token.Trim();
                if (trimmed != "")
                {
                    destination.Insert(trimmed.ToInt());
                }
                token = "";
            }
            else
            {
                token = token + current;
            }
            position++;
        }
        return destination.Count() > 0;
    }

    static bool GetStringArray(string source, string key, array<string> destination)
    {
        string body = ArrayBodyOf(source, key);
        if (body == "")
        {
            return false;
        }
        destination.Clear();

        string token = "";
        int position = 0;
        int length = body.Length();
        string current;
        while (position <= length)
        {
            if (position == length)
            {
                current = ",";
            }
            else
            {
                current = body.Substring(position, 1);
            }

            if (current == ",")
            {
                string trimmed = token.Trim();
                trimmed.Replace("\"", "");
                trimmed = trimmed.Trim();
                if (trimmed != "")
                {
                    destination.Insert(trimmed);
                }
                token = "";
            }
            else
            {
                token = token + current;
            }
            position++;
        }
        return destination.Count() > 0;
    }
}

class MPB_Report
{
    static const string PREFIX = "[modperf-bench] ";

    // Every human-readable summary line is also kept here and mirrored into
    // the results JSON, so the file and the log can never disagree about what
    // the run concluded.
    ref array<string> m_LogLines;
    ref array<string> m_BenchBlocks;

    void MPB_Report()
    {
        m_LogLines = new array<string>();
        m_BenchBlocks = new array<string>();
    }

    // Print to the script log AND retain for the JSON.
    void Say(string message)
    {
        Print(PREFIX + message);
        m_LogLines.Insert(message);
    }

    // Print only -- progress chatter that does not belong in the artifact.
    void Trace(string message)
    {
        Print(PREFIX + message);
    }

    void AddBenchBlock(string jsonObject)
    {
        m_BenchBlocks.Insert(jsonObject);
    }

    static bool EnsureDirectory()
    {
        return MakeDirectory("$profile:modperf-bench");
    }

    // Writes the results document. Returns the path written, or "" on
    // failure. The document is assembled as a plain string list and printed
    // line by line; nothing here reflects over a class.
    string Write(string header)
    {
        EnsureDirectory();

        string stamp = MPB_Fmt.UtcStamp();
        string path = "$profile:modperf-bench/results-" + stamp + ".json";

        FileHandle handle = OpenFile(path, FileMode.WRITE);
        if (handle == 0)
        {
            Print(PREFIX + "ERROR could not open " + path + " for writing");
            return "";
        }

        FPrintln(handle, "{");
        FPrintln(handle, header);

        FPrintln(handle, "  \"benches\": [");
        int benchCount = m_BenchBlocks.Count();
        int benchIndex;
        for (benchIndex = 0; benchIndex < benchCount; benchIndex++)
        {
            string separator = ",";
            if (benchIndex == benchCount - 1)
            {
                separator = "";
            }
            FPrintln(handle, m_BenchBlocks.Get(benchIndex) + separator);
        }
        FPrintln(handle, "  ],");

        FPrintln(handle, "  \"log\": [");
        int logCount = m_LogLines.Count();
        int logIndex;
        for (logIndex = 0; logIndex < logCount; logIndex++)
        {
            string logSeparator = ",";
            if (logIndex == logCount - 1)
            {
                logSeparator = "";
            }
            FPrintln(handle, "    " + MPB_Fmt.Quoted(m_LogLines.Get(logIndex)) + logSeparator);
        }
        FPrintln(handle, "  ]");
        FPrintln(handle, "}");

        CloseFile(handle);
        return path;
    }
}
