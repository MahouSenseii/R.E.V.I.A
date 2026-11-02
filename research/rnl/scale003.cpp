// RNL-003 synthetic CPU size study. Uploaded source and previous workers unchanged.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <xmmintrin.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
namespace scale003 {
constexpr uint32_t controlMask=0xe040;
constexpr uint64_t basis=14695981039346656037ull,prime=1099511628211ull;
void require(bool okay,const char* why){if(!okay)throw std::runtime_error(why);}
std::string hex64(uint64_t n){std::ostringstream s;s<<std::hex<<std::setw(16)<<std::setfill('0')<<n;return s.str();}
void byte(uint64_t& h,uint8_t x){h^=x;h*=prime;}
void word(uint64_t& h,uint32_t x){for(unsigned i=0;i<4;++i)byte(h,uint8_t(x>>(i*8)));}
void floats(uint64_t& h,const std::vector<float>& v){for(float x:v)word(h,std::bit_cast<uint32_t>(x));}
uint64_t floatHash(const std::vector<float>& v){uint64_t h=basis;floats(h,v);return h;}
uint32_t next(uint32_t& x){x^=x<<13;x^=x>>17;x^=x<<5;return x;}
bool allowedD(size_t d){return d==16||d==64||d==256||d==1024;}
bool allowedL(size_t l){return l==1||l==2||l==10;}
struct Expert{std::vector<float> gate;std::vector<uint8_t> indices;};
struct Model{size_t d;std::array<Expert,3> experts;std::array<float,3> book{-1.f,0.f,1.f};};
Model model(size_t d){
    Model b{d,{}};uint32_t rng=0x524e4c33;
    for(auto& e:b.experts){
        e.gate.resize(d);e.indices.resize(d*d);
        for(auto& g:e.gate)g=float(int(next(rng)%3)-1);
        for(auto& w:e.indices)w=uint8_t(next(rng)%3);
    }return b;
}
using Corpus=std::vector<std::vector<float>>;
Corpus corpus(size_t d,size_t l){
    Corpus c;c.reserve(100);uint32_t rng=0x494e5033;
    for(size_t block=0;block<100/l;++block){
        std::vector<float> v(d);
        for(auto& x:v)x=(next(rng)&1)?0.25f:-0.25f;
        v[0]=0.25f+float(block)/1024.f;
        for(size_t i=0;i<l;++i)c.push_back(v);
    }return c;
}
uint64_t modelHash(const Model& b){
    uint64_t h=basis;
    for(const auto& e:b.experts){floats(h,e.gate);for(auto x:e.indices)byte(h,x);}return h;
}
uint64_t corpusHash(const Corpus& c){uint64_t h=basis;for(const auto& v:c)floats(h,v);return h;}
void bitsJson(std::ostream& out,const std::vector<float>& v){
    out<<'[';for(size_t i=0;i<v.size();++i){if(i)out<<',';out<<std::bit_cast<uint32_t>(v[i]);}out<<']';
}
void scoresJson(std::ostream& out,const std::array<float,3>& v){
    out<<'[';for(size_t i=0;i<3;++i){if(i)out<<',';out<<std::bit_cast<uint32_t>(v[i]);}out<<']';
}
struct Counts{uint64_t events=0,cache_hits=0,matrix_evaluations=0,cache_lookups=0,key_comparisons=0,expert_comparisons=0;};
void countJson(std::ostream& out,const Counts& c){
    out<<"{\"events\":"<<c.events<<",\"cache_hits\":"<<c.cache_hits<<",\"matrix_evaluations\":"<<c.matrix_evaluations
       <<",\"cache_lookups\":"<<c.cache_lookups<<",\"key_comparisons\":"<<c.key_comparisons<<",\"expert_comparisons\":"<<c.expert_comparisons<<'}';
}
// Both modes share this ordinary decoding kernel; no per-MAC timed instrumentation.
__declspec(noinline) void matrix(const Model& b,size_t selected,const std::vector<float>& input,std::vector<float>& product){
    const auto& indices=b.experts[selected].indices;
    for(size_t row=0;row<b.d;++row){
        float sum=0;
        for(size_t col=0;col<b.d;++col){if(input[col]==0)continue;sum+=b.book[indices[row*b.d+col]]*input[col];}
        product[row]=sum;
    }
}
class VM{
    const Model& b;
    bool enabled,valid=false;
    uint32_t cachedExpert=0;
public:
    std::vector<float> state,output,product,cachedProduct;
    std::vector<uint32_t> inputKey;
    std::array<float,3> scores{};
    uint32_t selected=0;
    bool hit=false;
    Counts counts;
    VM(const Model& brain,bool cache):b(brain),enabled(cache),state(b.d),output(b.d),product(b.d),cachedProduct(cache?b.d:0),inputKey(cache?b.d:0){}
    void reset(bool tiled=false){
        constexpr float seed[3]={.25f,-.5f,.75f};
        for(size_t j=0;j<b.d;++j)state[j]=tiled?seed[j%3]:0.f;
        valid=false;counts={};hit=false;
    }
    __declspec(noinline) void event(const std::vector<float>& input){
        ++counts.events;
        for(size_t e=0;e<3;++e){
            float score=0;
            for(size_t j=0;j<b.d;++j)score+=input[j]*b.experts[e].gate[j];
            for(size_t j=0;j<b.d;++j)score+=0.15f*state[j]*b.experts[e].gate[j];
            scores[e]=score;
        }
        selected=0;
        for(uint32_t e=1;e<3;++e)if(scores[e]>scores[selected])selected=e;
        hit=false;
        if(enabled){
            ++counts.cache_lookups;bool equal=valid;
            if(equal){++counts.expert_comparisons;equal=cachedExpert==selected;}
            if(equal)for(size_t j=0;j<b.d;++j){
                ++counts.key_comparisons;
                if(inputKey[j]!=std::bit_cast<uint32_t>(input[j])){equal=false;break;}
            }
            hit=equal;
        }
        if(hit){++counts.cache_hits;std::copy(cachedProduct.begin(),cachedProduct.end(),product.begin());}
        else{
            ++counts.matrix_evaluations;matrix(b,selected,input,product);
            if(enabled){
                for(size_t j=0;j<b.d;++j)inputKey[j]=std::bit_cast<uint32_t>(input[j]);
                std::copy(product.begin(),product.end(),cachedProduct.begin());
                cachedExpert=selected;valid=true;
            }
        }
        for(size_t j=0;j<b.d;++j){
            output[j]=std::tanh(product[j]+0.25f*state[j]);
            state[j]=0.55f*state[j]+(1-0.55f)*output[j];
        }
    }
};
void finite(const std::vector<float>& v){for(float x:v)require(std::isfinite(x),"nonfinite vector");}
void equalBits(const std::vector<float>& u,const std::vector<float>& m){
    require(u.size()==m.size(),"vector size disagreement");
    for(size_t j=0;j<u.size();++j)require(std::bit_cast<uint32_t>(u[j])==std::bit_cast<uint32_t>(m[j]),"U/M vector bit disagreement");
}
void trajectoryHash(uint64_t& h,const std::vector<float>& input,const std::vector<float>& before,const VM& vm){
    floats(h,input);floats(h,before);for(float s:vm.scores)word(h,std::bit_cast<uint32_t>(s));
    word(h,vm.selected);floats(h,vm.product);floats(h,vm.output);floats(h,vm.state);
}
void integerOracle(const Model& b,const std::vector<float>& input,const VM& vm){
    const auto& indices=b.experts[vm.selected].indices;
    for(size_t row=0;row<b.d;++row){
        int64_t sum=0;
        for(size_t col=0;col<b.d;++col){
            const auto q=int64_t(input[col]*1024.f);
            require(q>=-355&&q<=355&&float(q)/1024.f==input[col],"Q10 input bound failed");
            sum+=(int64_t(indices[row*b.d+col])-1)*q;
            require(sum>=-363520&&sum<=363520,"integer partial bound failed");
        }
        const float expected=float(sum)/1024.f;
        require(std::bit_cast<uint32_t>(expected)==std::bit_cast<uint32_t>(vm.product[row]),"integer product oracle disagreement");
    }
}
void common(std::ostream& out,const char* kind){out<<"{\"experiment\":\"RNL-003\",\"kind\":\""<<kind<<"\",\"compiler_version\":"<<_MSC_FULL_VER;}
void fingerprints(std::ostream& out,const Model& b,const Corpus& c,size_t l){
    out<<",\"dimension\":"<<b.d<<",\"block\":"<<l<<",\"model_fingerprint\":\""<<hex64(modelHash(b))<<"\",\"corpus_fingerprint\":\""<<hex64(corpusHash(c))<<'"';
}
struct WitnessError:std::runtime_error{
    std::string json;
    explicit WitnessError(std::string value):std::runtime_error("validation failed; bounded witness retained"),json(std::move(value)){}
};
void vmWitness(std::ostream& out,const VM& vm){
    out<<"{\"scores_bits\":";scoresJson(out,vm.scores);out<<",\"selected\":"<<vm.selected;
    out<<",\"matrix_product_bits\":";bitsJson(out,vm.product);out<<",\"output_bits\":";bitsJson(out,vm.output);
    out<<",\"state_after_bits\":";bitsJson(out,vm.state);out<<'}';
}
std::string validation(){
    std::ostringstream out;common(out,"validate");out<<",\"scenarios\":[";bool first=true;
    for(size_t d:{16u,64u,256u,1024u})for(size_t l:{1u,2u,10u}){
        const auto b=model(d);const auto c=corpus(d,l);
        if(!first)out<<',';first=false;out<<'{';
        out<<"\"dimension\":"<<d<<",\"block\":"<<l<<",\"model_fingerprint\":\""<<hex64(modelHash(b))<<"\",\"corpus_fingerprint\":\""<<hex64(corpusHash(c))<<"\",\"traces\":[";
        for(unsigned seed=0;seed<2;++seed){
            if(seed)out<<',';VM u(b,false),m(b,true);u.reset(seed!=0);m.reset(seed!=0);
            uint64_t uh=basis,mh=basis;std::ostringstream trace;trace<<'[';
            for(size_t i=0;i<100;++i){
                const auto before=u.state;
                try{
                    equalBits(before,m.state);u.event(c[i]);m.event(c[i]);
                    finite(u.product);finite(u.output);finite(u.state);finite(m.product);finite(m.output);finite(m.state);
                    for(size_t e=0;e<3;++e)require(std::isfinite(u.scores[e])&&std::bit_cast<uint32_t>(u.scores[e])==std::bit_cast<uint32_t>(m.scores[e]),"router disagreement/nonfinite");
                    require(u.selected==m.selected,"selected expert disagreement");
                    equalBits(u.product,m.product);equalBits(u.output,m.output);equalBits(u.state,m.state);
                    integerOracle(b,c[i],u);integerOracle(b,c[i],m);
                }catch(const std::exception& e){
                    std::ostringstream witness;
                    witness<<"{\"experiment\":\"RNL-003\",\"kind\":\"validation_failure\",\"reason\":\""<<e.what()<<"\",\"dimension\":"<<d<<",\"block\":"<<l<<",\"seed\":\""<<(seed?"tiled":"zero")<<"\",\"step\":"<<i<<",\"input_bits\":";
                    bitsJson(witness,c[i]);witness<<",\"state_before_bits\":";bitsJson(witness,before);
                    witness<<",\"U\":";vmWitness(witness,u);witness<<",\"M\":";vmWitness(witness,m);witness<<'}';
                    throw WitnessError(witness.str());
                }
                trajectoryHash(uh,c[i],before,u);trajectoryHash(mh,c[i],before,m);
                if(d==16){
                    if(i)trace<<',';trace<<"{\"input_bits\":";bitsJson(trace,c[i]);trace<<",\"state_before_bits\":";bitsJson(trace,before);
                    trace<<",\"scores_bits\":";scoresJson(trace,u.scores);trace<<",\"selected\":"<<u.selected<<",\"matrix_product_bits\":";bitsJson(trace,u.product);
                    trace<<",\"output_bits\":";bitsJson(trace,u.output);trace<<",\"state_after_bits\":";bitsJson(trace,u.state);trace<<",\"m_cache_hit\":"<<(m.hit?"true":"false")<<'}';
                }
            }
            trace<<']';require(l!=1||m.counts.cache_hits==0,"L1 unexpected hit");
            out<<"{\"seed\":\""<<(seed?"tiled":"zero")<<"\",\"steps\":100,\"equal\":true,\"integer_oracle\":true,\"all_finite\":true,\"u_hash\":\""<<hex64(uh)<<"\",\"m_hash\":\""<<hex64(mh)<<"\",\"u_counts\":";
            countJson(out,u.counts);out<<",\"m_counts\":";countJson(out,m.counts);out<<",\"u_trace\":"<<trace.str()<<'}';
        }out<<"]}";
    }out<<']';return out.str();
}
int64_t qpc(){LARGE_INTEGER x;require(QueryPerformanceCounter(&x)!=0,"QPC failed");return x.QuadPart;}
struct Clock{int64_t frequency,minTicks;};
Clock clockInfo(){
    LARGE_INTEGER f;require(QueryPerformanceFrequency(&f)!=0&&f.QuadPart>0,"invalid QPC frequency");
    int64_t previous=qpc(),minimum=std::numeric_limits<int64_t>::max();
    for(unsigned i=0;i<1999;++i){
        const auto current=qpc();require(current>=previous,"nonmonotonic QPC");
        if(current>previous)minimum=std::min(minimum,current-previous);previous=current;
    }
    require(minimum!=std::numeric_limits<int64_t>::max(),"no positive QPC delta");return {f.QuadPart,minimum};
}
struct Batch{char mode;uint64_t events;int64_t ticks;double checksum;uint64_t stateHash;Counts counts;uint64_t repeated;};
Batch batch(VM& vm,bool enabled,const Corpus& c,uint64_t n,size_t l){
    vm.reset();double checksum=0;const int64_t start=qpc();
    for(uint64_t i=0;i<n;++i){vm.event(c[size_t(i%100)]);checksum+=double(vm.output[0]);}
    const int64_t stop=qpc();require(stop>start,"nonpositive timed interval");finite(vm.state);require(std::isfinite(checksum),"nonfinite checksum");
    require(l!=1||vm.counts.cache_hits==0,"L1 timing unexpected hit");
    return {enabled?'M':'U',n,stop-start,checksum,floatHash(vm.state),vm.counts,n/l*(l-1)};
}
void batchJson(std::ostream& out,const Batch& b){
    out<<"{\"mode\":\""<<b.mode<<"\",\"events\":"<<b.events<<",\"ticks\":"<<b.ticks<<",\"checksum_bits\":\""<<hex64(std::bit_cast<uint64_t>(b.checksum))<<"\",\"state_hash\":\""<<hex64(b.stateHash)<<"\",\"counts\":";
    countJson(out,b.counts);out<<",\"repeated_events\":"<<b.repeated<<'}';
}
void sameOutcome(const Batch& u,const Batch& m){
    require(u.events==m.events&&std::bit_cast<uint64_t>(u.checksum)==std::bit_cast<uint64_t>(m.checksum)&&u.stateHash==m.stateHash,"timing outcome disagreement");
    require(u.counts.matrix_evaluations==u.events&&m.counts.matrix_evaluations+m.counts.cache_hits==m.events,"timing counts disagreement");
}
std::string timing(size_t d,size_t l){
    const auto b=model(d);const auto c=corpus(d,l);VM u(b,false),m(b,true);const auto clock=clockInfo();
    std::ostringstream out;common(out,"timing");fingerprints(out,b,c,l);
    out<<",\"clock\":{\"frequency_hz\":"<<clock.frequency<<",\"empirical_min_ticks\":"<<clock.minTicks<<",\"sample_reads\":2000,\"nonmonotonic\":false}";
    const uint64_t cap=std::min(uint64_t(100000),uint64_t(200000000)/(d*d))/100*100;
    require(cap>=100,"calibration cap too small");uint64_t n=100;std::vector<Batch> calibration;
    for(unsigned i=0;i<4;++i){
        calibration.push_back(batch(u,false,c,n,l));const double seconds=double(calibration.back().ticks)/double(clock.frequency);
        if(seconds>=0.050||n==cap||i==3)break;
        const double proposed=std::ceil(double(n)*0.050/seconds/100.0)*100.0;
        n=std::min(cap,std::max(n+100,proposed>=double(cap)?cap:uint64_t(proposed)));
    }
    const auto wu=batch(u,false,c,n,l),wm=batch(m,true,c,n,l);sameOutcome(wu,wm);
    out<<",\"event_count\":"<<n<<",\"cap_event_count\":"<<cap<<",\"calibration\":[";
    for(size_t i=0;i<calibration.size();++i){if(i)out<<',';batchJson(out,calibration[i]);}
    out<<"],\"warmup\":[";batchJson(out,wu);out<<',';batchJson(out,wm);out<<"],\"pairs\":[";
    for(unsigned i=0;i<5;++i){
        Batch bu{},bm{};
        if(i%2==0){bu=batch(u,false,c,n,l);bm=batch(m,true,c,n,l);}
        else{bm=batch(m,true,c,n,l);bu=batch(u,false,c,n,l);}
        sameOutcome(bu,bm);require(bm.counts.cache_hits==wm.counts.cache_hits,"M hits changed across identical batches");
        if(i)out<<',';out<<"{\"order\":\""<<(i%2?"MU":"UM")<<"\",\"U\":";batchJson(out,bu);out<<",\"M\":";batchJson(out,bm);out<<'}';
    }out<<']';return out.str();
}
void memoryJson(std::ostream& out){
    PROCESS_MEMORY_COUNTERS_EX p{};p.cb=sizeof(p);
    require(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&p),sizeof(p))!=0,"process memory API failed");
    out<<"{\"working_set_bytes\":"<<p.WorkingSetSize<<",\"private_bytes\":"<<p.PrivateUsage<<",\"peak_working_set_bytes\":"<<p.PeakWorkingSetSize<<",\"pagefile_usage_bytes\":"<<p.PagefileUsage<<",\"peak_pagefile_usage_bytes\":"<<p.PeakPagefileUsage<<'}';
}
std::string memory(size_t d,bool cache){
    const auto b=model(d);const auto c=corpus(d,10);VM vm(b,cache);vm.reset();
    for(const auto& input:c)vm.event(input);finite(vm.state);
    // Snapshot precedes result serialization buffers (small snapshot stream itself exists).
    std::ostringstream measured;memoryJson(measured);const std::string snapshot=measured.str();
    size_t gateBytes=0,indexBytes=0,corpusBytes=0;
    for(const auto& e:b.experts){gateBytes+=e.gate.capacity()*sizeof(float);indexBytes+=e.indices.capacity();}
    for(const auto& input:c)corpusBytes+=input.capacity()*sizeof(float);
    const size_t baseVectors=(vm.state.capacity()+vm.output.capacity()+vm.product.capacity())*sizeof(float);
    const size_t cacheVectors=vm.cachedProduct.capacity()*sizeof(float)+vm.inputKey.capacity()*sizeof(uint32_t);
    const size_t corpusObjects=c.capacity()*sizeof(Corpus::value_type);
    const size_t objects=sizeof(Model)+sizeof(Corpus)+sizeof(VM);
    std::ostringstream out;common(out,"memory");fingerprints(out,b,c,10);
    out<<",\"mode\":\""<<(cache?'M':'U')<<"\",\"counts\":";countJson(out,vm.counts);
    out<<",\"memory\":"<<snapshot<<",\"allocation\":{\"model_object_bytes\":"<<sizeof(Model)<<",\"corpus_object_bytes\":"<<sizeof(Corpus)<<",\"vm_object_bytes\":"<<sizeof(VM)
       <<",\"gates_capacity_bytes\":"<<gateBytes<<",\"indices_capacity_bytes\":"<<indexBytes<<",\"corpus_vectors_capacity_bytes\":"<<corpusObjects<<",\"corpus_float_capacity_bytes\":"<<corpusBytes
       <<",\"vm_base_float_capacity_bytes\":"<<baseVectors<<",\"cache_input_capacity_bytes\":"<<vm.inputKey.capacity()*sizeof(uint32_t)<<",\"cache_product_capacity_bytes\":"<<vm.cachedProduct.capacity()*sizeof(float)
       <<",\"partial_total_bytes\":"<<objects+gateBytes+indexBytes+corpusObjects+corpusBytes+baseVectors+cacheVectors
       <<",\"generated_matrix_payload_bytes\":"<<3*d*d<<",\"generated_gate_payload_bytes\":"<<3*d*sizeof(float)<<",\"model_disk_file_bytes\":null,\"allocator_overhead_bytes\":null,\"crt_overhead_bytes\":null}"
       <<",\"state_hash\":\""<<hex64(floatHash(vm.state))<<'"';return out.str();
}
std::string empty(){std::ostringstream out;common(out,"empty");out<<",\"memory\":";memoryJson(out);return out.str();}
void fpJson(std::ostream& out,int roundStart,uint32_t csrStart){
    const int roundEnd=std::fegetround();const uint32_t csrEnd=_mm_getcsr();
    require(roundStart==roundEnd&&(csrStart&controlMask)==(csrEnd&controlMask),"FP controls changed");
    out<<",\"fp_environment\":{\"fe_round_start\":"<<roundStart<<",\"fe_round_end\":"<<roundEnd<<",\"FE_TONEAREST\":"<<FE_TONEAREST<<",\"mxcsr_control_before\":"<<(csrStart&controlMask)<<",\"mxcsr_control_after\":"<<(csrEnd&controlMask)
       <<",\"ftz\":"<<((csrStart&0x8000)?"true":"false")<<",\"daz\":"<<((csrStart&0x40)?"true":"false")<<",\"controls_unchanged\":true,\"exception_status_flags_start\":"<<(csrStart&0x3f)<<",\"exception_status_flags_end\":"<<(csrEnd&0x3f)<<",\"exception_flags_in_contract\":false}";
}
} // namespace scale003
int main(int argc,char** argv){
    using namespace scale003;
    std::string outputPath;
    try{
        const int roundStart=std::fegetround();const uint32_t csrStart=_mm_getcsr();
        require(roundStart==FE_TONEAREST&&(csrStart&0x6000)==0,"require round-to-nearest");
        std::string kind,mode;size_t d=0,l=0;
        for(int i=1;i<argc;i+=2){
            require(i+1<argc,"option missing value");const std::string key=argv[i],value=argv[i+1];
            if(key=="--kind")kind=value;else if(key=="--output-dir")outputPath=value;else if(key=="--dimension")d=std::stoul(value);else if(key=="--block")l=std::stoul(value);else if(key=="--mode")mode=value;else require(false,"unknown option");
        }
        require(!outputPath.empty(),"missing output directory");const std::filesystem::path destination(outputPath);
        require(!std::filesystem::exists(destination)&&std::filesystem::is_directory(destination.parent_path()),"output must be fresh under existing parent");
        std::string json;
        if(kind=="validate"){require(d==0&&l==0&&mode.empty(),"validate accepts no scenario");json=validation();}
        else if(kind=="timing"){require(allowedD(d)&&allowedL(l)&&mode.empty(),"invalid timing scenario");json=timing(d,l);}
        else if(kind=="memory"){require(allowedD(d)&&l==10&&(mode=="U"||mode=="M"),"invalid memory scenario");json=memory(d,mode=="M");}
        else if(kind=="empty"){require(d==0&&l==0&&mode.empty(),"empty accepts no scenario");json=empty();}
        else require(false,"invalid kind");
        std::ostringstream ending;fpJson(ending,roundStart,csrStart);json+=ending.str()+"}";
        require(json.size()<1024*1024,"direct result exceeds 1MiB cap");
        require(std::filesystem::create_directory(destination),"cannot create result directory");
        std::ofstream out(destination/"result.json",std::ios::binary);require(bool(out),"cannot create result");
        out.write(json.data(),std::streamsize(json.size()));out.flush();require(bool(out),"result write failed");
        std::cout<<"RNL-003 "<<kind<<": "<<json.size()<<" direct bytes; interpretation external.\n";return 0;
    }catch(const WitnessError& e){
        try{
            require(e.json.size()<1024*1024,"failure witness exceeds cap");
            const std::filesystem::path destination(outputPath);
            require(!outputPath.empty()&&!std::filesystem::exists(destination)&&std::filesystem::is_directory(destination.parent_path()),"failure output must be fresh");
            require(std::filesystem::create_directory(destination),"cannot create failure directory");
            std::ofstream out(destination/"failure.json",std::ios::binary);
            out.write(e.json.data(),std::streamsize(e.json.size()));out.flush();require(bool(out),"failure witness write failed");
        }catch(const std::exception& writeError){std::cerr<<"Witness not written: "<<writeError.what()<<'\n';}
        std::cerr<<"RNL-003 stopped: "<<e.what()<<'\n';return 2;
    }catch(const std::exception& e){std::cerr<<"RNL-003 stopped: "<<e.what()<<'\n';return 2;}
}
