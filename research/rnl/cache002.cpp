// RNL-002 numeric-contract worker. Include the unchanged reviewed original via /I.
#define main rnl_v01_original_main
#include "rnl.cpp"
#undef main
#include <array>
#include <cfenv>
#include <filesystem>
#include <limits>
#include <type_traits>
#include <xmmintrin.h>

namespace cache002 {
using Arr = std::array<float, 3>;
static_assert(sizeof(float) == 4 && sizeof(uint32_t) == 4);
constexpr uint32_t controlsMask = 0xe040; // SSE rounding, FTZ and DAZ; not status flags.
struct Fixture { std::string name; Arr initial; std::vector<Arr> inputs; };
struct Cache { std::array<uint32_t,3> input{}; Arr product{}; uint32_t expert = 0; bool valid = false; };
struct EmptyCache {};
struct Counts {
    size_t matrix_macs=0, router_products=0, activations=0, state_updates=0;
    size_t key_comparisons=0, key_expert_comparisons=0, cache_lookups=0;
    size_t cache_hits=0, cache_misses=0, matrix_evaluations=0;
    void add(const Counts& c) {
        matrix_macs+=c.matrix_macs; router_products+=c.router_products;
        activations+=c.activations; state_updates+=c.state_updates;
        key_comparisons+=c.key_comparisons; key_expert_comparisons+=c.key_expert_comparisons;
        cache_lookups+=c.cache_lookups; cache_hits+=c.cache_hits;
        cache_misses+=c.cache_misses; matrix_evaluations+=c.matrix_evaluations;
    }
};
struct Step { Arr input{}, before{}, scores{}, product{}, output{}, after{}; uint32_t selected=0; bool hit=false; Counts counts; };
std::string quote(const std::string& s) {
    std::ostringstream out; out << '"';
    for (unsigned char c:s) {
        if (c=='"'||c=='\\') out << '\\' << char(c);
        else if (c<32) out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c) << std::dec;
        else out << char(c);
    }
    return out.str()+'"';
}
void arrayField(std::ostream& out,const char* name,const Arr& a) {
    out << quote(name) << ":[" << std::setprecision(std::numeric_limits<float>::max_digits10);
    for (size_t j=0;j<3;++j) { check(std::isfinite(a[j]),"nonfinite trajectory"); if(j)out<<','; out<<a[j]; }
    out << "]," << quote(std::string(name)+"_bits") << ":[";
    for (size_t j=0;j<3;++j) { if(j)out<<','; out<<std::bit_cast<uint32_t>(a[j]); }
    out << ']';
}
void countFields(std::ostream& out,const Counts& c) {
    out << "\"matrix_macs\":" << c.matrix_macs << ",\"router_products\":" << c.router_products
        << ",\"activations\":" << c.activations << ",\"state_updates\":" << c.state_updates
        << ",\"key_comparisons\":" << c.key_comparisons << ",\"key_expert_comparisons\":" << c.key_expert_comparisons
        << ",\"cache_lookups\":" << c.cache_lookups << ",\"cache_hits\":" << c.cache_hits
        << ",\"cache_misses\":" << c.cache_misses << ",\"matrix_evaluations\":" << c.matrix_evaluations;
}
std::vector<Fixture> readFixtures(const std::string& path) {
    std::ifstream in(path,std::ios::binary); check(bool(in),"cannot open fixtures");
    char magic[4]; in.read(magic,4); check(bool(in)&&std::string(magic,4)=="RNC2","invalid fixture signature");
    const auto count=read16(in); check(count==20,"require exactly 20 frozen trajectories");
    std::vector<Fixture> fixtures; fixtures.reserve(count); size_t total=0;
    for (unsigned i=0;i<count;++i) {
        Fixture c; c.name=readStr(in);
        check(!c.name.empty()&&c.name.size()<=32,"invalid fixture name length");
        for(unsigned char x:c.name)check(x>=32&&x<=126,"fixture name must be ASCII");
        for(auto& x:c.initial)x=readflt(in);
        const auto n=read16(in); check(n>0&&n<=17,"fixture step cap exceeded");
        total+=n; check(total<=280,"fixture total cap exceeded"); c.inputs.reserve(n);
        for(unsigned j=0;j<n;++j) { Arr input; for(auto& x:input)x=readflt(in); c.inputs.push_back(input); }
        fixtures.push_back(std::move(c));
    }
    check(total==280&&in.peek()==EOF,"fixture total or EOF mismatch");
    return fixtures;
}
template<char Mode> class VM {
    const Brain& brain;
    Arr memory{}, latest{};
    std::conditional_t<Mode=='U',EmptyCache,Cache> entry;
public:
    VM(const Brain& b,Arr initial):brain(b),memory(initial){}
    Step event(const Arr& input) {
        Step s; s.input=input; s.before=memory;
        std::vector<std::pair<float,size_t>> ranked;
        for(size_t e=0;e<brain.experts.size();++e) {
            const auto& gate=brain.experts[e].gate; float score=0;
            for(size_t j=0;j<3;++j)score+=input[j]*gate[j];
            for(size_t j=0;j<3;++j)score+=0.15f*memory[j]*gate[j];
            check(std::isfinite(score),"nonfinite router score"); s.scores[e]=score;
            ranked.push_back({score,e}); s.counts.router_products+=9;
        }
        std::stable_sort(ranked.begin(),ranked.end(),[](auto a,auto b){return a.first>b.first;});
        s.selected=uint32_t(ranked[0].second);
        if constexpr(Mode!='U') {
            s.counts.cache_lookups=1; bool equal=entry.valid;
            if(equal&&Mode=='M') { ++s.counts.key_expert_comparisons; equal=entry.expert==s.selected; }
            if(equal)for(size_t j=0;j<3;++j) {
                ++s.counts.key_comparisons;
                if(entry.input[j]!=std::bit_cast<uint32_t>(input[j])) { equal=false; break; }
            }
            s.hit=equal;
            if(s.hit) { s.product=entry.product; s.counts.cache_hits=1; }
            else s.counts.cache_misses=1;
        }
        if(!s.hit) {
            const auto& e=brain.experts[s.selected]; s.counts.matrix_evaluations=1;
            for(size_t row=0;row<3;++row)for(size_t col=0;col<3;++col) {
                if(input[col]==0)continue;
                s.product[row]+=brain.codebook[e.indices[row*3+col]]*input[col]; ++s.counts.matrix_macs;
            }
            for(float value:s.product)check(std::isfinite(value),"nonfinite matrix product");
            if constexpr(Mode!='U') {
                for(size_t j=0;j<3;++j)entry.input[j]=std::bit_cast<uint32_t>(input[j]);
                entry.product=s.product; entry.expert=s.selected; entry.valid=true;
            }
        }
        for(size_t row=0;row<3;++row) {
            latest[row]=std::tanh(s.product[row]/static_cast<float>(brain.topk)+0.25f*memory[row]);
            memory[row]=brain.decay*memory[row]+(1-brain.decay)*latest[row];
            check(std::isfinite(latest[row])&&std::isfinite(memory[row]),"nonfinite recurrent update");
            ++s.counts.activations; ++s.counts.state_updates;
        }
        s.output=latest; s.after=memory; return s;
    }
};
void stepJson(std::ostream& out,const Step& s) {
    out << '{'; arrayField(out,"input",s.input); out << ','; arrayField(out,"state_before",s.before);
    out << ','; arrayField(out,"scores",s.scores); out << ",\"selected\":[" << s.selected << "],";
    arrayField(out,"matrix_product",s.product); out << ','; arrayField(out,"output",s.output);
    out << ','; arrayField(out,"state_after",s.after); out << ",\"cache_hit\":" << (s.hit?"true":"false") << ',';
    countFields(out,s.counts); out << '}';
}
template<char Mode> std::string run(const Brain& brain,const std::vector<Fixture>& fixtures,int roundStart,uint32_t csrStart) {
    Counts aggregate; std::ostringstream out;
    out << "{\"mode\":" << quote(std::string(1,Mode)) << ",\"traces\":[";
    for(size_t i=0;i<fixtures.size();++i) {
        if(i)out<<','; const auto& c=fixtures[i]; VM<Mode> vm(brain,c.initial);
        out << "{\"name\":" << quote(c.name) << ','; arrayField(out,"initial_state",c.initial); out << ",\"steps\":[";
        for(size_t j=0;j<c.inputs.size();++j) { if(j)out<<','; const auto s=vm.event(c.inputs[j]); aggregate.add(s.counts); stepJson(out,s); }
        out << "]}";
    }
    const int roundEnd=std::fegetround(); const uint32_t csrEnd=_mm_getcsr();
    check(roundStart==roundEnd&&(csrStart&controlsMask)==(csrEnd&controlsMask),"FP control settings changed");
    constexpr bool enabled=Mode!='U';
    out << "],\"aggregate_counts\":{"; countFields(out,aggregate);
    out << "},\"cache_capacity\":{\"logical_enabled\":" << (enabled?"true":"false")
        << ",\"logical_capacity_bytes\":" << (enabled?sizeof(Cache):0)
        << ",\"input_key_bits_bytes\":" << (enabled?sizeof(Cache::input):0)
        << ",\"raw_product_bytes\":" << (enabled?sizeof(Cache::product):0)
        << ",\"expert_index_bytes\":" << (enabled?sizeof(Cache::expert):0)
        << ",\"validity_bytes\":" << (enabled?sizeof(Cache::valid):0)
        << ",\"padding_bytes\":" << (enabled?sizeof(Cache)-sizeof(Cache::input)-sizeof(Cache::product)-sizeof(Cache::expert)-sizeof(Cache::valid):0)
        << ",\"cache_struct_sizeof_bytes\":" << sizeof(Cache) << ",\"vm_object_sizeof_bytes\":" << sizeof(VM<Mode>)
        << ",\"full_vm_allocation_bytes\":null,\"process_memory_bytes\":null,\"sizeof_is_not_total_memory\":true}"
        << ",\"fp_environment\":{\"fe_round_start\":" << roundStart << ",\"fe_round_end\":" << roundEnd
        << ",\"FE_TONEAREST\":" << FE_TONEAREST << ",\"round_to_nearest\":true,\"sse_control_mask\":" << controlsMask
        << ",\"mxcsr_control_before\":" << (csrStart&controlsMask) << ",\"mxcsr_control_after\":" << (csrEnd&controlsMask)
        << ",\"sse_rounding_bits\":" << (csrStart&0x6000) << ",\"ftz\":" << ((csrStart&0x8000)?"true":"false")
        << ",\"daz\":" << ((csrStart&0x40)?"true":"false")
        << ",\"exception_status_flags_start\":" << (csrStart&0x3f) << ",\"exception_status_flags_end\":" << (csrEnd&0x3f)
        << ",\"controls_unchanged\":true,\"exception_flags_in_contract\":false}"
        << ",\"counter_scope\":\"source-level arithmetic and key equality operations; not CPU instructions\""
        << ",\"correctness_decision\":null,\"performance_claim\":null}";
    return out.str();
}
} // namespace cache002
int main(int argc,char** argv) {
    using namespace cache002;
    try {
        const int roundStart=std::fegetround(); const uint32_t csrStart=_mm_getcsr();
        check(roundStart==FE_TONEAREST&&(csrStart&0x6000)==0,"require FP round-to-nearest");
        std::string modelPath,fixturePath,outputPath; char mode=0;
        for(int i=1;i<argc;i+=2) {
            check(i+1<argc,"each option needs a value"); const std::string key=argv[i],value=argv[i+1];
            if(key=="--model")modelPath=value; else if(key=="--fixtures")fixturePath=value;
            else if(key=="--output-dir")outputPath=value;
            else if(key=="--mode") { check(value.size()==1&&std::string("UMB").find(value[0])!=std::string::npos,"mode must be U/M/B"); mode=value[0]; }
            else fail("unknown RNL-002 option");
        }
        check(!modelPath.empty()&&!fixturePath.empty()&&!outputPath.empty()&&mode,"required --model --fixtures --mode --output-dir");
        const std::filesystem::path directory(outputPath);
        check(!std::filesystem::exists(directory)&&std::filesystem::is_directory(directory.parent_path()),"output directory must be fresh");
        const Brain brain=readBrain(modelPath);
        check(brain.input==3&&brain.state==3&&brain.topk==1&&brain.experts.size()==3&&brain.codebook==std::vector<float>({-1,0,1}),"require pinned demo dimensions/codebook");
        const auto fixtures=readFixtures(fixturePath);
        const std::string json=mode=='U'?run<'U'>(brain,fixtures,roundStart,csrStart):mode=='M'?run<'M'>(brain,fixtures,roundStart,csrStart):run<'B'>(brain,fixtures,roundStart,csrStart);
        check(json.size()<1024*1024,"direct output must be below 1 MiB");
        check(std::filesystem::create_directory(directory),"cannot create fresh output directory");
        std::ofstream out(directory/"result.json",std::ios::binary); check(bool(out),"cannot create result");
        out.write(json.data(),std::streamsize(json.size())); out.flush(); check(bool(out),"incomplete result write");
        std::cout << "RNL-002 mode " << mode << ": 280 steps, " << json.size() << " direct output bytes; numeric decision external, no timing.\n";
        return 0;
    } catch(const std::exception& e) { std::cerr << "RNL-002 error: " << e.what() << '\n'; return 2; }
}
