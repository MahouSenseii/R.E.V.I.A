#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace revia::quality
{
// Evaluation accepts an operator-selected source only. It never discovers app-owned
// credentials, persists the value, or accepts a secret in command-line arguments.
struct LocalModelCredentialSource
{
    std::filesystem::path file;
    std::string environment;

    [[nodiscard]] bool Empty() const { return file.empty() && environment.empty(); }
    [[nodiscard]] std::string Kind() const { return !file.empty() ? "file" : (!environment.empty() ? "environment" : "none"); }

    [[nodiscard]] std::string Read() const
    {
        if (!file.empty() && !environment.empty())
            throw std::runtime_error("Choose one credential source per model endpoint.");
        if (Empty())
            return {};
        std::string value;
        if (!file.empty())
        {
            std::ifstream input(file, std::ios::binary);
            if (!input)
                throw std::runtime_error("Cannot read the explicitly selected model credential file.");
            char bytes[4099];
            input.read(bytes, sizeof(bytes));
            value.assign(bytes, static_cast<std::size_t>(input.gcount()));
            if (input.bad())
                throw std::runtime_error("The selected model credential file could not be read completely.");
            if (value.size() > 4098 || input.peek() != std::char_traits<char>::eof())
                throw std::runtime_error("The selected model credential file exceeds the bounded token size.");
        }
        else
        {
            const char* selected = std::getenv(environment.c_str());
            if (!selected)
                throw std::runtime_error("The explicitly selected model credential environment variable is unset.");
            value = selected;
        }
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
            value.pop_back();
        Validate(value);
        return value;
    }

    static void Validate(const std::string& value)
    {
        if (value.empty() || value.size() > 4096)
            throw std::runtime_error("The selected model credential must contain 1 to 4096 bytes.");
        for (const unsigned char ch : value)
            if (ch <= 32 || ch == 127)
                throw std::runtime_error("The selected model credential contains an invalid header character.");
    }
};
}
