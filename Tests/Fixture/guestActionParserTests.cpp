#include "Planning/structuredActionParser.h"
#include "Policy/capabilityPolicy.h"

#include <iostream>
#include <stdexcept>

namespace
{
void Check(const bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
}

int main()
{
    using namespace revia;
    try
    {
        planning::StructuredActionParser parser;
        const auto read = parser.ParseJson(R"({"action":"vm_read_text_file","path":"src/Main.cpp"})");
        Check(read.succeeded && read.request.guest.path == "src/Main.cpp" && read.request.source.empty() &&
            read.request.guest.expectedIdentity.bootId.empty(), "A guest read did not retain its relative case-sensitive path and unstamped proposal.");
        const auto write = parser.ParseJson(R"json({"action":"vm_write_text_file","path":"main.py","content":"print(1)","expected_digest":"missing"})json");
        Check(write.succeeded && write.request.guest.content == "print(1)" && write.request.guest.expectedDigest == "missing",
            "A typed guest write lost its content or digest precondition.");
        const auto execute = parser.ParseJson(R"({"action":"vm_execute_process","executable":"/usr/bin/python3","arguments":["main.py"],"working_directory":".","timeout_ms":30000})");
        Check(execute.succeeded && execute.request.process.executable.empty() && execute.request.guest.executable == "/usr/bin/python3" &&
            execute.request.guest.arguments == std::vector<std::string>{"main.py"}, "A guest process was converted into host execution fields.");
        for (const auto* proposal : {
            R"({"action":"vm_read_text_file","path":"a","environmentId":"forged"})",
            R"({"action":"vm_read_text_file","path":"a","expectedIdentity":{"bootId":"forged"}})",
            R"({"action":"vm_read_text_file","path":"a","source":"C:/Windows"})",
            R"({"action":"vm_read_text_file","path":"../escape"})",
            R"({"action":"vm_write_text_file","path":"a","content":"b"})",
            R"({"action":"vm_execute_process","executable":"/usr/bin/python3","arguments":[],"working_directory":".","timeout_ms":1.5})",
            R"({"action":"vm_execute_process","executable":"/usr/bin/python3","arguments":[],"working_directory":".","environment":{"PATH":"/tmp"}})"})
            Check(!parser.ParseJson(proposal).succeeded, "A forged, host-directed, or malformed guest proposal was accepted.");

        actions::CapabilitySettings settings;
        settings.mode = actions::ExecutionMode::OwnerFullAccess;
        settings.guest.enabled = settings.guest.read = settings.guest.write = settings.guest.execute = true;
        settings.guest.limits.approvedExecutables = {"/usr/bin/python3"};
        policy::CapabilityPolicy allowed(settings);
        Check(allowed.Evaluate(read.request).verdict == actions::PolicyVerdict::Allowed,
            "Guest reading incorrectly required a host approved root.");
        Check(allowed.Evaluate(write.request).verdict == actions::PolicyVerdict::Allowed,
            "Guest writing incorrectly resolved its destination on the host.");
        Check(allowed.Evaluate(execute.request).verdict == actions::PolicyVerdict::RequiresConfirmation,
            "Guest process execution did not retain its individual approval boundary.");
        settings.guest.enabled = false;
        Check(policy::CapabilityPolicy(settings).Evaluate(read.request).verdict == actions::PolicyVerdict::Blocked,
            "Host owner mode silently enabled the separate guest capability.");
        Check(actions::RiskForAction(read.request.type) == actions::RiskLevel::ReadOnly &&
            actions::RepeatingCouldDuplicateAnEffect(write.request.type) &&
            actions::RepeatingCouldDuplicateAnEffect(execute.request.type), "Guest risk or duplicate-effect classification is missing.");
        std::cout << "Guest action parser and policy: typed boundaries passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Guest action parser and policy: " << error.what() << '\n';
        return 1;
    }
}
