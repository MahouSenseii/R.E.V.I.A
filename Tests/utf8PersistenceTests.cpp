#include "testSupport.h"

#include "Content/workingDocument.h"
#include "Initiative/curiosityJournal.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace
{
using revia::content::WorkingDocument;
using revia::initiative::CuriosityJournal;
using revia::initiative::CuriosityRecord;
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;

std::string Repeated(const std::string& unit, const std::size_t count)
{
    std::string text;
    for (std::size_t index = 0; index < count; ++index)
        text += unit;
    return text;
}

void CheckJsonText(const std::string& text)
{
    try
    {
        const nlohmann::json encoded = {{"text", text}};
        Check(nlohmann::json::parse(encoded.dump()).at("text") == text, "A bounded string did not survive strict JSON serialization.");
    }
    catch (const nlohmann::json::exception&)
    {
        Check(false, "A byte budget split a UTF-8 code point before JSON serialization.");
    }
}

void TestDocumentEditsKeepCompleteUtf8Prefixes()
{
    struct Case
    {
        std::string input;
        std::string expected;
    };
    const std::string cjk = "\xE7\x95\x8C";
    const std::string emoji = "\xF0\x9F\x98\x80";
    const std::string combining = "e\xCC\x81";
    const std::vector<Case> cases = {{Repeated(cjk, 3000), Repeated(cjk, 2666)}, {"p" + Repeated(emoji, 2100), "p" + Repeated(emoji, 1999)},
        {Repeated(combining, 3000), Repeated(combining, 2666) + "e"}};
    for (const Case& item : cases)
    {
        WorkingDocument document;
        document.Compose("Fixture", {item.input, "untouched neighbor"});
        CheckJsonText(document.Blocks().front().text);
        Check(document.Blocks().front().text == item.expected, "Compose did not keep the longest complete UTF-8 prefix.");
        const auto appended = document.Append(item.input);
        Check(appended.succeeded && appended.after == item.expected, "Append cut a UTF-8 code point at its block budget.");
        const auto replaced = document.ReplaceBlock("1", item.input);
        Check(replaced.succeeded && replaced.after == item.expected, "ReplaceBlock cut a UTF-8 code point at its block budget.");
        const auto inserted = document.InsertAfter("1", item.input);
        Check(inserted.succeeded && inserted.after == item.expected, "InsertAfter cut a UTF-8 code point at its block budget.");
        Check(document.Blocks()[2].text == "untouched neighbor", "A bounded edit changed a neighboring block.");
        CheckJsonText(document.Render());
        CheckJsonText(document.RenderNumbered());
        CheckJsonText(document.RenderNeighbourhood("1", 3));
        Check(document.Undo() && document.Blocks().size() == 3, "A UTF-8 bounded insertion could not be undone.");
        CheckJsonText(document.Render());
    }
}

void TestMalformedDocumentTextIsRepairedBeforeSerialization()
{
    const std::string malformed = std::string("a\x80", 2) + "\xED\xA0\x80";
    const std::string repaired = "a\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD";
    WorkingDocument document;
    document.SetTitle(malformed);
    CheckJsonText(document.Title());
    Check(document.Title() == repaired, "Malformed document title bytes were not replaced explicitly.");
    document.Compose(malformed, {malformed, "neighbor"});
    Check(document.Title() == repaired && document.Blocks().front().text == repaired,
        "Compose did not repair malformed title and block bytes.");
    const auto appended = document.Append(malformed);
    const auto replaced = document.ReplaceBlock("1", malformed);
    const auto inserted = document.InsertAfter("1", malformed);
    Check(appended.succeeded && appended.after == repaired && replaced.succeeded && replaced.after == repaired && inserted.succeeded &&
              inserted.after == repaired,
        "Document edits handled malformed bytes inconsistently.");
    CheckJsonText(document.RenderNumbered());
    Check(document.Undo(), "A repaired document edit could not be undone.");
    CheckJsonText(document.RenderNumbered());
}

std::string ReadBytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void TestJournalByteBudgetsKeepUtf8AndRoundTrip()
{
    const std::string cjk = "\xE7\x95\x8C";
    const std::string emoji = "\xF0\x9F\x98\x80";
    struct Case
    {
        CuriosityRecord input;
        std::string topic;
        std::string query;
        std::string outcome;
    };
    const std::vector<Case> cases = {{{"topic: " + Repeated(cjk, 400), Repeated(cjk, 400), {"source"}, Repeated(cjk, 400)},
                                         "topic: " + Repeated(cjk, 97), Repeated(cjk, 166), Repeated(cjk, 333)},
        {{"topic: " + Repeated(emoji, 400), "q" + Repeated(emoji, 400), {"source"}, "o" + Repeated(emoji, 400)},
            "topic: " + Repeated(emoji, 73), "q" + Repeated(emoji, 124), "o" + Repeated(emoji, 249)}};
    for (const Case& item : cases)
    {
        ScopedTestDirectory directory;
        const auto path = directory.root / "curiosity.jsonl";
        std::string error;
        CuriosityJournal journal;
        Check(journal.Initialize(path, error), "Could not initialize the UTF-8 journal fixture.");
        bool appended = false;
        try
        {
            appended = journal.Append(item.input, error);
        }
        catch (const nlohmann::json::exception&)
        {
            Check(false, "Valid UTF-8 journal input escaped Append as a JSON exception.");
        }
        Check(appended, "Valid UTF-8 journal input was refused: " + error);
        const auto recent = journal.Recent(1);
        Check(recent.size() == 1 && recent.front().topic == item.topic && recent.front().query == item.query &&
                  recent.front().outcome == item.outcome,
            "Journal limits did not keep complete UTF-8 prefixes.");
        const auto stored = nlohmann::json::parse(ReadBytes(path));
        Check(stored.at("topic") == item.topic && stored.at("query") == item.query && stored.at("outcome") == item.outcome,
            "Journal serialization changed the retained text.");
        CuriosityJournal reopened;
        Check(reopened.Initialize(path, error) && reopened.Recent(1).size() == 1 && reopened.Recent(1).front().query == item.query,
            "Bounded Unicode journal text did not survive reopening.");
    }
}

void TestMalformedJournalTextIsRefusedWithoutMutation()
{
    ScopedTestDirectory directory;
    const auto path = directory.root / "curiosity.jsonl";
    CuriosityJournal journal;
    std::string error;
    Check(journal.Initialize(path, error), "Could not initialize the malformed UTF-8 fixture.");
    const CuriosityRecord valid = {"retained topic", "retained query", {"retained source"}, "retained outcome"};
    Check(journal.Append(valid, error), "Could not seed the malformed UTF-8 fixture.");
    const std::string before = ReadBytes(path);
    const std::vector<std::string> malformed = {std::string(1, '\x80'), "\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xE7\x95"};
    for (const std::string& invalid : malformed)
    {
        for (int field = 0; field < 8; ++field)
        {
            CuriosityRecord record = valid;
            if (field == 0)
                record.topic += invalid;
            if (field == 1)
                record.query += invalid;
            if (field == 2)
                record.outcome += invalid;
            if (field == 3)
                record.sources.front() += invalid;
            if (field == 4)
                record.topic = std::string(310, 't') + invalid;
            if (field == 5)
                record.query = std::string(510, 'q') + invalid;
            if (field == 6)
            {
                record.sources.assign(11, "source");
                record.sources.back() += invalid;
            }
            if (field == 7)
                record.outcome = std::string(1010, 'o') + invalid;
            bool appended = true;
            try
            {
                appended = journal.Append(record, error);
            }
            catch (const nlohmann::json::exception&)
            {
                Check(false, "Malformed UTF-8 escaped the journal's error-return contract.");
            }
            Check(!appended && !error.empty(), "Malformed UTF-8 was silently accepted or truncated away.");
            const auto recent = journal.Recent(2);
            Check(ReadBytes(path) == before && recent.size() == 1 && recent.front().topic == valid.topic &&
                      recent.front().query == valid.query && recent.front().sources == valid.sources,
                "A refused journal record changed disk or recent history.");
        }
    }
}
}

void RunUtf8PersistenceTests()
{
    TestDocumentEditsKeepCompleteUtf8Prefixes();
    TestMalformedDocumentTextIsRepairedBeforeSerialization();
    TestJournalByteBudgetsKeepUtf8AndRoundTrip();
    TestMalformedJournalTextIsRefusedWithoutMutation();
    std::cout << "UTF-8 document budgets and curiosity journal persistence tests passed.\n";
}
