// FIRST-001 instrumentation. Compile with the reviewed original src directory
// on the include path; the original file is not copied, patched or vendored.
#define main rnl_v01_original_main
#include "rnl.cpp"
#undef main

#include <chrono>
#include <filesystem>
#include <limits>

namespace first001 {
static_assert(sizeof(float) == 4 && sizeof(uint32_t) == 4);
using Clock = std::chrono::steady_clock;
using Vec = std::vector<float>;
constexpr size_t outputLimit = 1024 * 1024;
volatile double timingSink = 0;

std::string quote(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << char(c);
        else if (c < 32) out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c) << std::dec;
        else out << char(c);
    }
    return out.str() + '"';
}
void floats(std::ostream& out, const Vec& values) {
    out << '[' << std::setprecision(std::numeric_limits<float>::max_digits10);
    for (size_t i = 0; i < values.size(); ++i) {
        check(std::isfinite(values[i]), "nonfinite evidence value");
        if (i) out << ',';
        out << values[i];
    }
    out << ']';
}
void bits(std::ostream& out, const Vec& values) {
    out << '[';
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out << ',';
        out << std::bit_cast<uint32_t>(values[i]);
    }
    out << ']';
}
void vectorField(std::ostream& out, const char* key, const Vec& values) {
    out << quote(key) << ':';
    floats(out, values);
    out << ',' << quote(std::string(key) + "_bits") << ':';
    bits(out, values);
}
void validateDemo(const Brain& b) {
    check(b.input == 3 && b.state == 3 && b.topk == 1 && b.experts.size() == 3,
          "FIRST-001 only accepts the pinned 3x3, three-expert, top-1 demo");
    check(b.codebook == Vec({-1.0f, 0.0f, 1.0f}), "unexpected demo codebook");
}

// Comparable experimental containers retain the source's metadata and routing.
// F omits its unused codebook; U/C retain byte indices; P is original RNL1.
std::string serialize(const Brain& b, char variant) {
    std::ostringstream out(std::ios::out | std::ios::binary);
    out.write(variant == 'F' ? "RNLF" : variant == 'P' ? "RNL1" : "RNLU", 4);
    writeStr(out, b.name);
    u16(out, unsigned(b.input)); u16(out, unsigned(b.state));
    flt(out, b.decay); flt(out, b.novelty);
    u8(out, unsigned(b.topk));
    u8(out, variant == 'F' ? 0 : unsigned(b.codebook.size()));
    u8(out, unsigned(b.experts.size()));
    if (variant != 'F') for (float x : b.codebook) flt(out, x);
    for (const auto& e : b.experts) {
        writeStr(out, e.name);
        for (float g : e.gate) flt(out, g);
        if (variant == 'F') for (auto i : e.indices) flt(out, b.codebook[i]);
        else if (variant == 'P') {
            const auto packed = pack2(e.indices);
            out.write(reinterpret_cast<const char*>(packed.data()), std::streamsize(packed.size()));
        } else out.write(reinterpret_cast<const char*>(e.indices.data()), std::streamsize(e.indices.size()));
    }
    check(bool(out), "experimental container serialization failed");
    return out.str();
}
struct Model {
    Brain brain;
    char variant = 'U';
    std::vector<Vec> dense;
    std::vector<std::vector<uint8_t>> packed;
    float weight(size_t expert, size_t index) const {
        if (variant == 'F') return dense[expert][index];
        const auto code = variant == 'P'
            ? uint8_t((packed[expert][index / 4] >> (2 * (index % 4))) & 3)
            : brain.experts[expert].indices[index];
        return brain.codebook[code];
    }
};
Model load(std::istream& in, char variant) {
    char magic[4]; in.read(magic, 4);
    check(bool(in) && std::string(magic, 4) == (variant == 'F' ? "RNLF" : variant == 'P' ? "RNL1" : "RNLU"), "wrong experimental container");
    Model m; m.variant = variant;
    auto& b = m.brain;
    b.name = readStr(in); b.input = read16(in); b.state = read16(in);
    b.decay = readflt(in); b.novelty = readflt(in); b.topk = read8(in);
    const auto codes = read8(in), experts = read8(in);
    check(b.input == 3 && b.state == 3 && b.topk == 1 && experts == 3, "wrong experimental model dimensions");
    check(codes == (variant == 'F' ? 0u : 3u), "wrong experimental codebook size");
    check(b.decay >= 0 && b.decay < 1 && b.novelty >= 0, "invalid experimental parameters");
    b.codebook.reserve(codes);
    for (unsigned i = 0; i < codes; ++i) b.codebook.push_back(readflt(in));
    b.experts.reserve(experts);
    if (variant == 'F') m.dense.reserve(experts);
    if (variant == 'P') m.packed.reserve(experts);
    for (unsigned e = 0; e < experts; ++e) {
        Expert x; x.name = readStr(in); x.gate.reserve(b.input);
        for (size_t j = 0; j < b.input; ++j) x.gate.push_back(readflt(in));
        const size_t n = b.input * b.state;
        if (variant == 'F') {
            Vec weights; weights.reserve(n);
            for (size_t j = 0; j < n; ++j) weights.push_back(readflt(in));
            m.dense.push_back(std::move(weights));
        } else if (variant == 'P') {
            std::vector<uint8_t> packed((n + 3) / 4);
            in.read(reinterpret_cast<char*>(packed.data()), std::streamsize(packed.size()));
            check(bool(in), "truncated packed experimental matrix");
            for (size_t j = 0; j < n; ++j) check(((packed[j / 4] >> (2 * (j % 4))) & 3u) < codes, "invalid packed experimental index");
            m.packed.push_back(std::move(packed));
        } else {
            x.indices.reserve(n);
            for (size_t j = 0; j < n; ++j) {
                const auto i = read8(in); check(i < codes, "invalid experimental index");
                x.indices.push_back(uint8_t(i));
            }
        }
        b.experts.push_back(std::move(x));
    }
    check(in.peek() == EOF, "trailing experimental container bytes");
    return m;
}
Model loadBytes(const std::string& data, char variant) {
    std::istringstream in(data, std::ios::in | std::ios::binary);
    return load(in, variant);
}
struct Step {
    Vec input, before, scores, output, after;
    std::vector<size_t> selected;
    bool cached = false;
    size_t matrixMacs = 0, routerProducts = 0, activations = 0, stateUpdates = 0;
    float delta = 0;
};
class VM {
    const Model& model;
    Vec memory, latest, previous;
    bool hasPrevious = false;
public:
    size_t peakScratchCapacity = 0;
    explicit VM(const Model& m, Vec initial) : model(m), memory(std::move(initial)), latest(m.brain.state, 0) {}
    const Vec& state() const { return memory; }
    const Vec& output() const { return latest; }
    size_t stateCapacityBytes() const { return (memory.capacity() + latest.capacity() + previous.capacity()) * sizeof(float); }
    Step event(const Vec& input, bool capture) {
        const auto& b = model.brain;
        Step s;
        if (capture) { s.input = input; s.before = memory; }
        if (hasPrevious) for (size_t j = 0; j < input.size(); ++j) s.delta = std::max(s.delta, std::fabs(input[j] - previous[j]));
        if (model.variant == 'C' && hasPrevious && s.delta <= b.novelty) {
            s.cached = true;
            if (capture) { s.output = latest; s.after = memory; }
            return s;
        }
        previous = input; hasPrevious = true;
        std::vector<std::pair<float, size_t>> scores;
        for (size_t e = 0; e < b.experts.size(); ++e) {
            const auto& gate = b.experts[e].gate;
            float score = 0;
            for (size_t j = 0; j < input.size(); ++j) score += input[j] * gate[j];
            for (size_t j = 0; j < std::min(memory.size(), gate.size()); ++j) score += 0.15f * memory[j] * gate[j];
            scores.push_back({score, e});
            if (capture) s.scores.push_back(score);
            s.routerProducts += input.size() + 2 * std::min(memory.size(), gate.size());
        }
        std::stable_sort(scores.begin(), scores.end(), [](auto a, auto c) { return a.first > c.first; });
        Vec sum(b.state, 0);
        peakScratchCapacity = std::max(peakScratchCapacity, scores.capacity() * sizeof(scores[0]) + sum.capacity() * sizeof(float));
        for (size_t k = 0; k < b.topk; ++k) {
            const auto e = scores[k].second;
            if (capture) s.selected.push_back(e);
            for (size_t row = 0; row < b.state; ++row) for (size_t col = 0; col < b.input; ++col) {
                if (input[col] == 0) continue;
                sum[row] += model.weight(e, row * b.input + col) * input[col];
                ++s.matrixMacs;
            }
        }
        for (size_t row = 0; row < b.state; ++row) {
            latest[row] = std::tanh(sum[row] / static_cast<float>(b.topk) + 0.25f * memory[row]);
            memory[row] = b.decay * memory[row] + (1 - b.decay) * latest[row];
            ++s.activations;
            ++s.stateUpdates;
        }
        if (capture) { s.output = latest; s.after = memory; }
        return s;
    }
};
struct Case { std::string name; std::vector<Vec> inputs; };
std::vector<Case> corpus(float threshold) {
    const Vec z(3, 0), u = {1, 0, 0}, negative = {-1, 0, 0};
    std::vector<Case> cases = {{"Z", std::vector<Vec>(16, z)}, {"R", std::vector<Vec>(16, u)}, {"A", {}}, {"B", {}}};
    for (size_t i = 0; i < 16; ++i) cases[2].inputs.push_back(i % 2 == 0 ? u : negative);
    cases[2].inputs.push_back(u);
    for (size_t i = 0; i < 4; ++i) for (const auto& x : {z, u, z}) cases[3].inputs.push_back(x);
    for (const auto& item : std::vector<std::pair<std::string, float>>{
            {"N-below", std::nextafter(threshold, 0.0f)}, {"N-at", threshold},
            {"N-above", std::nextafter(threshold, std::numeric_limits<float>::infinity())}}) {
        Case c{item.first, {}};
        Vec perturbed = u; perturbed[1] = item.second;
        for (size_t i = 0; i < 8; ++i) { c.inputs.push_back(u); c.inputs.push_back(perturbed); }
        cases.push_back(std::move(c));
    }
    cases.push_back({"Router", {z, u, {0,1,0}, {0,0,1}, {1,1,0}, {0,1,1}, {1,0,1}, {-1,-1,-1}, {0.5f,0.5f,0.5f}}});
    return cases;
}
void stepJson(std::ostream& out, const Step& s) {
    out << '{'; vectorField(out, "input", s.input);
    out << ','; vectorField(out, "state_before", s.before);
    out << ','; vectorField(out, "scores", s.scores);
    out << ",\"selected\":[";
    for (size_t i = 0; i < s.selected.size(); ++i) { if (i) out << ','; out << s.selected[i]; }
    out << "],"; vectorField(out, "output", s.output);
    out << ','; vectorField(out, "state_after", s.after);
    out << ",\"cached\":" << (s.cached ? "true" : "false") << ",\"matrix_macs\":" << s.matrixMacs
        << ",\"router_products\":" << s.routerProducts << ",\"activations\":" << s.activations
        << ",\"state_updates\":" << s.stateUpdates
        << ",\"delta\":" << std::setprecision(9) << s.delta << '}';
}
size_t runtimeMatrixCapacity(const Model& m) {
    size_t bytes = 0;
    for (const auto& e : m.brain.experts) bytes += e.indices.capacity();
    for (const auto& x : m.packed) bytes += x.capacity();
    for (const auto& x : m.dense) bytes += x.capacity() * sizeof(float);
    return bytes;
}
size_t runtimeObjectCapacity(const Model& m) {
    return sizeof(Model) + m.brain.experts.capacity() * sizeof(Expert)
        + m.dense.capacity() * sizeof(Vec) + m.packed.capacity() * sizeof(std::vector<uint8_t>);
}
long long ns(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
}
long long timedSteps(const Model& m, const std::vector<Vec>& inputs, size_t iterations) {
    VM vm(m, Vec(3, 0)); double checksum = 0;
    const auto start = Clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        vm.event(inputs[i % inputs.size()], false);
        checksum += vm.output()[0];
    }
    const auto end = Clock::now(); timingSink = checksum;
    return ns(start, end);
}
void writeFresh(const std::filesystem::path& path, const std::string& bytes) {
    check(!std::filesystem::exists(path), "refusing existing output file");
    std::ofstream out(path, std::ios::binary);
    check(bool(out), "cannot create output file");
    out.write(bytes.data(), std::streamsize(bytes.size()));
    out.flush(); check(bool(out), "incomplete output write");
}
} // namespace first001

int main(int argc, char** argv) {
    using namespace first001;
    try {
        std::string input, outputDirectory; char variant = 0;
        size_t iterations = 10000; int singleBatch = -1;
        for (int i = 1; i < argc; i += 2) {
            check(i + 1 < argc, "each option requires one value");
            const std::string option = argv[i], value = argv[i + 1];
            if (option == "--model") input = value;
            else if (option == "--output-dir") outputDirectory = value;
            else if (option == "--variant") { check(value.size() == 1 && std::string("FUPC").find(value[0]) != std::string::npos, "variant must be F/U/P/C"); variant = value[0]; }
            else if (option == "--iterations") { size_t end = 0; const auto n = std::stoul(value, &end); check(end == value.size() && n <= 1000000, "iterations outside 0..1000000"); iterations = size_t(n); }
            else if (option == "--batch") { check(value.size() == 1 && value[0] >= '0' && value[0] <= '4', "batch outside 0..4"); singleBatch = value[0] - '0'; }
            else fail("unknown FIRST-001 option");
        }
        check(!input.empty() && !outputDirectory.empty() && variant, "required: --model PATH --output-dir FRESH_DIR --variant F|U|P|C");
        const std::filesystem::path directory(outputDirectory);
        check(!std::filesystem::exists(directory) && std::filesystem::is_directory(directory.parent_path()), "output directory must be fresh with an existing parent");
        std::string modelBytes;
        { const auto original = readBrain(input); validateDemo(original); modelBytes = serialize(original, variant); }
        check(modelBytes.size() < outputLimit, "serialized model output limit");
        const Model model = loadBytes(modelBytes, variant);
        const auto cases = corpus(model.brain.novelty);
        const std::vector<Vec> initialStates = {Vec(3,0), {0.25f,-0.5f,0.75f}};
        size_t peakScratch = 0, peakStateCapacity = 0, traceCount = 0;
        std::ostringstream result;
        result << "{\"variant\":" << quote(std::string(1,variant)) << ",\"source_sha256\":\"d5ee18bbe9df39eb9d99b571c93ae1f0bbeec97bf19b387587a71d4563d61b0f\",\"threshold\":" << std::setprecision(9) << model.brain.novelty
               << ",\"threshold_bits\":" << std::bit_cast<uint32_t>(model.brain.novelty) << ",\"decoded_weights\":[";
        for (size_t e = 0; e < 3; ++e) {
            if (e) result << ',';
            Vec weights; for (size_t i = 0; i < 9; ++i) weights.push_back(model.weight(e,i));
            floats(result, weights);
        }
        result << "],\"traces\":[";
        bool firstTrace = true;
        for (const auto& initial : initialStates) for (const auto& c : cases) {
            if (!firstTrace) result << ','; firstTrace = false;
            result << "{\"name\":" << quote(c.name) << ','; vectorField(result,"initial_state",initial);
            result << ",\"steps\":["; VM vm(model,initial);
            for (size_t i = 0; i < c.inputs.size(); ++i) {
                if (i) result << ',';
                stepJson(result, vm.event(c.inputs[i],true)); ++traceCount;
            }
            peakScratch = std::max(peakScratch, vm.peakScratchCapacity);
            peakStateCapacity = std::max(peakStateCapacity, vm.stateCapacityBytes());
            result << "]}";
        }
        const size_t matrixBytes = variant == 'F' ? 108 : variant == 'P' ? 9 : 27;
        const size_t codebookBytes = variant == 'F' ? 0 : 12;
        size_t gateCapacity = 0;
        for (const auto& e : model.brain.experts) gateCapacity += e.gate.capacity() * sizeof(float);
        result << "],\"storage\":{\"schema\":" << quote(variant == 'P' ? "original RNL1" : variant == 'F' ? "experimental RNLF" : "experimental RNLU")
            << ",\"serialized_bytes\":" << modelBytes.size() << ",\"matrix_payload_bytes\":" << matrixBytes
            << ",\"codebook_bytes\":" << codebookBytes << ",\"gates_bytes\":36,\"headers_names_bytes\":" << modelBytes.size()-matrixBytes-codebookBytes-36
            << ",\"padding_bits\":" << (variant == 'P' ? 18 : 0)
            << ",\"runtime_matrix_capacity_bytes\":" << runtimeMatrixCapacity(model)
            << ",\"runtime_codebook_capacity_bytes\":" << model.brain.codebook.capacity()*sizeof(float)
            << ",\"runtime_gate_capacity_bytes\":" << gateCapacity
            << ",\"runtime_object_capacity_bytes\":" << runtimeObjectCapacity(model)
            << ",\"runtime_state_cache_capacity_bytes\":" << peakStateCapacity
            << ",\"runtime_event_scratch_capacity_peak_bytes\":" << peakScratch
            << ",\"runtime_allocator_overhead_bytes\":null,\"runtime_string_allocation_bytes\":null,\"peak_process_rss_bytes\":null"
            << ",\"resident_experts\":3,\"capacity_sums_are_not_process_memory\":true,\"trace_and_serialization_buffers_excluded_from_capacity_sums\":true}"
            << ",\"timing\":{\"status\":" << quote(iterations ? "collected_for_external_gate" : "skipped")
            << ",\"iterations_per_batch\":" << iterations << ",\"clock_period_num\":" << Clock::period::num << ",\"clock_period_den\":" << Clock::period::den
            << ",\"load_boundary\":\"in-memory container parse and allocation; no disk IO; destruction excluded\""
            << ",\"step_boundary\":\"event computation and checksum; VM construction and trace serialization excluded\""
            << ",\"ordering\":" << quote(singleBatch >= 0 ? "single batch for external interleaving" : "five consecutive variant batches; not cross-variant interleaved") << ",\"batches\":[";
        if (iterations) {
            std::vector<Vec> timedInputs;
            for (const auto& c : cases) for (const auto& x : c.inputs) timedInputs.push_back(x);
            { const auto warm = loadBytes(modelBytes,variant); timingSink = warm.weight(0,0); }
            (void)timedSteps(model,timedInputs,iterations);
            const int begin = singleBatch >= 0 ? singleBatch : 0, end = singleBatch >= 0 ? singleBatch+1 : 5;
            for (int batch = begin; batch < end; ++batch) {
                if (batch != begin) result << ',';
                const auto start = Clock::now();
                const auto loaded = loadBytes(modelBytes,variant);
                const auto stop = Clock::now(); timingSink = loaded.weight(0,0);
                const auto duration = timedSteps(model,timedInputs,iterations);
                result << "{\"batch\":" << batch << ",\"load_ns\":" << ns(start,stop) << ",\"step_total_ns\":" << duration << '}';
            }
        }
        result << "]},\"trace_steps\":" << traceCount << ",\"correctness_decision\":null,\"performance_claim\":null}";
        const auto json = result.str();
        check(json.size() + modelBytes.size() < outputLimit, "combined direct outputs must remain below 1 MiB");
        check(std::filesystem::create_directory(directory), "cannot create fresh output directory");
        writeFresh(directory / "model.rnb",modelBytes);
        writeFresh(directory / "result.json",json);
        std::cout << "FIRST-001 variant " << variant << ": " << traceCount << " traced steps, " << json.size()+modelBytes.size() << " direct output bytes; no self-assigned correctness or performance verdict.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FIRST-001 error: " << e.what() << '\n'; return 2; }
}
