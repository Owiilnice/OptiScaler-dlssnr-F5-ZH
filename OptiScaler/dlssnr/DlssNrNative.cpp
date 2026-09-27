// Explicit-template token support; experimental FFN-only interception. CPU-built; root runs GPU validation.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <vector>
#include <stdexcept>
#include <algorithm>
#include <fstream>
#include <cstdint>
#include "nvapi.h"
#include "DlssNrNative.h"
#include <bcrypt.h>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <array>
#include <tuple>
#include <cstring>
#include <thread>
#include <atomic>
#include "DlssNrHybridBuilder.h"
#include "DlssNrHybridAssets.h"
#include "DlssNrVitReuse.h"
#include "DlssNrKernelProfile.h"
#pragma comment(lib,"bcrypt.lib")
namespace DlssNrNative {namespace {
namespace fs=std::filesystem;using Microsoft::WRL::ComPtr;void Check(HRESULT r){if(FAILED(r))throw std::runtime_error("D3D12 status "+std::to_string((unsigned)r));}void NvCheck(NvAPI_Status r){if(r!=NVAPI_OK)throw std::runtime_error("NVAPI status "+std::to_string(r));}
using Blob=std::vector<unsigned char>;
Blob Read(const fs::path&p){std::ifstream f(p,std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("Missing asset "+p.string());auto n=f.tellg();if(n<=0)throw std::runtime_error("Empty asset");Blob b((size_t)n);f.seekg(0);if(!f.read((char*)b.data(),b.size()))throw std::runtime_error("Asset read failed");return b;}
uint64_t Num(const std::string&s){size_t n=0;auto v=std::stoull(s,&n,0);if(n!=s.size()||s[0]=='-')throw std::runtime_error("Invalid asset integer");return v;}
struct Patch{size_t offset;std::string name;uint64_t add=0;};
struct Template{Blob params;fs::path cubin;std::string name;NVAPI_DIM3 grid{},block{};unsigned shared=0;uint64_t workspaceBytes=64ull<<20,resetBytes=~0ull;std::vector<Patch>patches;NVDX_ObjectHandle fn=nullptr;};
Template LoadTemplate(const fs::path&dir){Template t;std::ifstream f(dir/"manifest.txt");if(!f)throw std::runtime_error("Missing vendor manifest");std::string line;fs::path pp;
 while(std::getline(f,line)){std::istringstream in(line);std::string k;in>>k;if(k.empty()||k[0]=='#')continue;
 if(k=="cubin"||k=="params"||k=="kernel"){std::string v;in>>std::quoted(v);if(k=="cubin")t.cubin=dir/v;else if(k=="params")pp=dir/v;else t.name=v;}
 else if(k=="grid"||k=="block"){NVAPI_DIM3 d{};in>>d.x>>d.y>>d.z;if(k=="grid")t.grid=d;else t.block=d;}
 else if(k=="shared")in>>t.shared;
 else if(k=="workspace_bytes")in>>t.workspaceBytes;
 else if(k=="workspace_reset_bytes")in>>t.resetBytes;
 else if(k=="patch"){std::string a,b,c;in>>a>>b;if(in>>c)t.patches.push_back({(size_t)Num(a),b,Num(c)});else t.patches.push_back({(size_t)Num(a),b,0});}
 }
 if(t.name.empty()||pp.empty()||t.cubin.empty()||!t.grid.x||!t.grid.y||!t.grid.z||!t.block.x||!t.block.y||!t.block.z)throw std::runtime_error("Incomplete vendor template");
 t.params=Read(pp);if(t.params.size()>65536||t.patches.empty())throw std::runtime_error("Invalid vendor params");if(t.resetBytes==~0ull)t.resetBytes=t.workspaceBytes;
 if(t.resetBytes>t.workspaceBytes)throw std::runtime_error("Workspace reset exceeds allocation");
 std::vector<bool>used(t.params.size());for(auto&p:t.patches){if(p.offset>t.params.size()||t.params.size()-p.offset<8)throw std::runtime_error("Pointer patch outside params");for(size_t i=0;i<8;++i){if(used[p.offset+i])throw std::runtime_error("Overlapping patches");used[p.offset+i]=true;}}
 return t;}
struct Weight{uint64_t ep=0,es=0,cp=0,cs=0,cos=0;};
struct Buffer{ComPtr<ID3D12Resource>gpu;uint64_t bytes=0;UINT64 address()const{return gpu->GetGPUVirtualAddress();}};
// NVAPI CU entrypoints, snapshotted under S().mutex so the async init worker can call them
// without re-entering the lock.
struct NvApi{decltype(&NvAPI_D3D12_CreateCuModule)createModule=nullptr;decltype(&NvAPI_D3D12_CreateCuFunction)createFunction=nullptr;};
struct Device{FusedBuilder splitBuilder;NVDX_ObjectHandle split=nullptr,splitEpilogue=nullptr;FusedBuilder builder;Buffer norm;NVDX_ObjectHandle fused=nullptr;ComPtr<ID3D12Device>owner;std::map<uint64_t,Template> contracts;std::array<Weight,8>w{};Buffer weights;std::vector<ComPtr<ID3D12Resource>>uploads;std::vector<NVDX_ObjectHandle>modules;
 NVDX_ObjectHandle pack=nullptr,epilogue=nullptr;float gateInv=1;std::atomic<int> init{0};std::string failure;}; // init: 0 idle, 1 running, 2 ready, 3 failed
struct Session{Buffer partials;std::array<Blob,8>splitParams;std::array<FusedInfo,8>splitInfo{};std::array<Blob,8> fusedParams;std::array<FusedInfo,8> fusedInfo{};uint64_t tokens=0;unsigned block=31,pairs=0;bool active=false,pending=false;UINT64 x=0,y=0,done=0;unsigned width=0,height=0;ID3D12Device*device=nullptr;
 Buffer xp,xs,yp,ys,c,cw;ComPtr<ID3D12Resource>zero;uint64_t zeroBytes=0;bool allocated=false;uint64_t lastTouch=0;};
struct Target{ID3D12Device*device;NVDX_ObjectHandle module;unsigned kind;bool supported;};
struct State{std::recursive_mutex mutex;bool enabled=false,candidate=false,restartRequired=false,active=false;std::string status="FP8 selected";uint64_t launches=0,splitLaunches=0;uint64_t seq=0;std::map<NVDX_ObjectHandle,bool>modules;std::map<NVDX_ObjectHandle,Target>targets;std::map<std::pair<ID3D12Device*,bool>,Device>devices;std::map<std::tuple<ID3D12GraphicsCommandList*,ID3D12Device*,uint64_t,bool>,Session>sessions;
 // Evicted-but-not-provably-idle sessions: kept alive here (never freed under recorded GPU
 // work, per the file's retain-until-exit model) so the s.sessions scans stay bounded.
 std::vector<Session>graveyard;
 DlssNrKernelProfile::Profiler prof;std::map<NVDX_ObjectHandle,DlssNrKernelProfile::Info>fnInfo; // kernel census and per-group GPU timing (ini KernelProfile)
 DlssNrVitReuse::Filter vit;std::map<NVDX_ObjectHandle,std::pair<DlssNrVitReuse::Role,DlssNrVitReuse::KernelSet>>vitRole; // ViT reuse: launches of the ViT run are dropped on reused frames; each kernel set has its own rate
 decltype(&NvAPI_D3D12_CreateCuModule) createModule=nullptr;decltype(&NvAPI_D3D12_CreateCuFunction)createFunction=nullptr;decltype(&NvAPI_D3D12_LaunchCuKernelChain)launch=nullptr;
 decltype(&NvAPI_D3D12_DestroyCuModule)destroyModule=nullptr;decltype(&NvAPI_D3D12_DestroyCuFunction)destroyFunction=nullptr;};
// Deliberately retain resources until process exit, never free under recorded GPU work.
State&S(){static State*s=new State;return*s;}
// Every real launch goes through here so the kernel profiler sees it; the caller holds S().mutex.
NvAPI_Status RealLaunch(State&s,ID3D12GraphicsCommandList*c,const NVAPI_CU_KERNEL_LAUNCH_PARAMS*k,NvU32 count){
 auto rc=s.launch(c,k,count);
 if(rc==NVAPI_OK&&k&&count&&s.prof.Recording(c)){std::vector<DlssNrKernelProfile::Info>infos;infos.reserve(count);for(NvU32 i=0;i<count;++i){auto it=s.fnInfo.find(k[i].hFunction);infos.push_back(it!=s.fnInfo.end()?it->second:DlssNrKernelProfile::Classify(""));}s.prof.Launched(c,infos);}
 return rc;}
void Barrier(ID3D12GraphicsCommandList*c){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;c->ResourceBarrier(1,&b);}
unsigned SyncMask(uint64_t tokens){return S().candidate?(tokens==960?7:tokens==2160?5:0):0;}
void ScratchBarrier(ID3D12GraphicsCommandList*c,uint64_t tokens,unsigned bit,std::initializer_list<ID3D12Resource*> resources){
 if(!(SyncMask(tokens)&bit)){Barrier(c);return;}std::vector<D3D12_RESOURCE_BARRIER> barriers;
 for(auto*r:resources){if(!r)throw std::runtime_error("Missing candidate scratch resource");D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;b.UAV.pResource=r;barriers.push_back(b);}c->ResourceBarrier((UINT)barriers.size(),barriers.data());
}
void Transition(ID3D12GraphicsCommandList*c,ID3D12Resource*r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};c->ResourceBarrier(1,&b);}
ComPtr<ID3D12Resource>Resource(ID3D12Device*d,uint64_t bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state){D3D12_RESOURCE_DESC r{};r.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;r.Width=bytes;r.Height=1;r.DepthOrArraySize=1;r.MipLevels=1;r.SampleDesc.Count=1;r.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;if(type==D3D12_HEAP_TYPE_DEFAULT)r.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;D3D12_HEAP_PROPERTIES h{};h.Type=type;ComPtr<ID3D12Resource>out;Check(d->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&r,state,nullptr,IID_PPV_ARGS(&out)));return out;}
Buffer Allocate(ID3D12Device*d,uint64_t bytes){return {Resource(d,std::max<uint64_t>(bytes,256),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS),std::max<uint64_t>(bytes,256)};}
void CopyZero(ID3D12GraphicsCommandList*c,Buffer&b,ID3D12Resource*z,uint64_t n){if(!n)return;Transition(c,b.gpu.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);c->CopyBufferRegion(b.gpu.Get(),0,z,0,n);Transition(c,b.gpu.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);}
NVDX_ObjectHandle Module(Device&d,const fs::path&p,const NvApi&nv){auto b=Read(p);NVDX_ObjectHandle m=nullptr;NvCheck(nv.createModule(d.owner.Get(),b.data(),(unsigned)b.size(),&m));d.modules.push_back(m);return m;}
NVDX_ObjectHandle Function(Device&d,NVDX_ObjectHandle m,const char*n,const NvApi&nv){NVDX_ObjectHandle f=nullptr;NvCheck(nv.createFunction(d.owner.Get(),m,n,&f));return f;}
fs::path Assets(){wchar_t path[32768]{};HMODULE module=nullptr;if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&Assets),&module)||!GetModuleFileNameW(module,path,32768))throw std::runtime_error("Cannot locate OptiScaler module");return fs::path(path).parent_path()/"OptiScaler"/"nvfp4"/"hybrid";}
fs::path FusedDir(){return Assets()/"expansion";}
void VerifyAssets(){static std::atomic<bool> verified{false};if(verified.load(std::memory_order_acquire))return;auto root=Assets();for(auto&e:HybridAssets::files){auto blob=Read(root/e.path);unsigned char sha[32]{};if(blob.size()!=e.bytes||BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,blob.data(),(ULONG)blob.size(),sha,32)!=0||memcmp(sha,e.sha,32))throw std::runtime_error(std::string("Hybrid asset checksum mismatch: ")+e.path);}verified.store(true,std::memory_order_release);}
void VerifyCandidateAssets(){static std::atomic<bool> verified{false};if(verified.load(std::memory_order_acquire))return;
 {auto blob=Read(Assets()/"candidate/split-half/fused_expand.cubin");const unsigned char expected[32]={0xd5,0x16,0xfe,0x1a,0xc3,0x87,0xd5,0x9f,0x83,0xb0,0x97,0x21,0x4c,0xa9,0xb8,0x9f,0xce,0xbc,0x86,0xf2,0x63,0x67,0xf9,0xdd,0xc7,0xe8,0xcb,0x69,0x2e,0x77,0x66,0x59};unsigned char sha[32]{};if(blob.size()!=5013480||BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,blob.data(),(ULONG)blob.size(),sha,32)!=0||memcmp(sha,expected,32))throw std::runtime_error("Candidate asset checksum mismatch: candidate/split-half/fused_expand.cubin");}
 {auto blob=Read(Assets()/"candidate/split-half/params_builder.dll");const unsigned char expected[32]={0x59,0xc9,0xda,0x12,0x7e,0xf3,0xfe,0xb5,0x69,0x39,0x63,0x20,0x04,0xed,0x09,0x21,0x62,0x1d,0xcb,0x6b,0xe5,0xf8,0x63,0x0c,0x00,0xe7,0xac,0x89,0xf7,0x76,0x3a,0x04};unsigned char sha[32]{};if(blob.size()!=55808||BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,blob.data(),(ULONG)blob.size(),sha,32)!=0||memcmp(sha,expected,32))throw std::runtime_error("Candidate asset checksum mismatch: candidate/split-half/params_builder.dll");}
verified.store(true,std::memory_order_release);}
struct UploadBatch{ComPtr<ID3D12CommandQueue>queue;ComPtr<ID3D12CommandAllocator>allocator;ComPtr<ID3D12GraphicsCommandList>cmd;ComPtr<ID3D12Fence>fence;UploadBatch(ID3D12Device*d){D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;Check(d->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue)));Check(d->CreateCommandAllocator(desc.Type,IID_PPV_ARGS(&allocator)));Check(d->CreateCommandList(0,desc.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&cmd)));Check(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));}void Finish(){Check(cmd->Close());ID3D12CommandList*lists[]={cmd.Get()};queue->ExecuteCommandLists(1,lists);Check(queue->Signal(fence.Get(),1));HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)throw std::runtime_error("Hybrid upload event failed");auto rc=fence->SetEventOnCompletion(1,event);if(FAILED(rc)){CloseHandle(event);Check(rc);}auto waited=WaitForSingleObject(event,10000);CloseHandle(event);if(waited!=WAIT_OBJECT_0)throw std::runtime_error("Hybrid upload completion wait failed");}}
;
const std::map<uint64_t,fs::path>&ContractPaths(){static const std::map<uint64_t,fs::path> paths{{960,Assets()/"contract"},{2160,Assets()/"contract_m2160"},{3840,Assets()/"contract_m3840"}};return paths;}
// Runs on the async init worker (see KickDeviceInit). Records only into its own private
// UploadBatch command list, and its only GPU wait (uploadBatch.Finish) is off the render
// thread. Touches no shared State; the caller publishes d via the d.init release store.
void Prepare(Device&d,ID3D12Device*device,const NvApi&nv,bool candidate){VerifyAssets();if(candidate)VerifyCandidateAssets();auto&uploadBatch=*new UploadBatch(device);ID3D12GraphicsCommandList*c=uploadBatch.cmd.Get();d.owner=device;auto root=Assets();for(auto&entry:ContractPaths())d.contracts.emplace(entry.first,LoadTemplate(entry.second));
 auto blob=Read(root/"weights.bin");std::ifstream f(root/"weights.txt");if(!f)throw std::runtime_error("Missing weights.txt");std::string line;std::array<bool,8>seen{};
 while(std::getline(f,line)){std::istringstream in(line);std::string k;in>>k;if(k.empty()||k[0]=='#')continue;if(k=="gate_inverse_tensor_scale"){in>>d.gateInv;continue;}unsigned block;std::string a,b,cname,ep,es,cp,cs,co;in>>block>>a>>ep>>es>>b>>cp>>cs>>cname>>co;if(k!="block"||block<31||block>38||a!="expand"||b!="contract"||cname!="cosine"||seen[block-31])throw std::runtime_error("Invalid weights mapping");d.w[block-31]={Num(ep),Num(es),Num(cp),Num(cs),Num(co)};seen[block-31]=true;}
 for(unsigned i=0;i<8;++i){if(!seen[i])throw std::runtime_error("Missing block weights");auto w=d.w[i];for(auto p:std::array<std::pair<uint64_t,uint64_t>,5>{{{w.ep,4096ull*1024/2},{w.es,4096ull*1024/16},{w.cp,1024ull*4096/2},{w.cs,1024ull*4096/16},{w.cos,2048}}})if(p.first>blob.size()||p.second>blob.size()-p.first)throw std::runtime_error("Weight range out of bounds");}
 d.weights=Allocate(device,blob.size());auto upload=Resource(device,blob.size(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void*p=nullptr;Check(upload->Map(0,nullptr,&p));memcpy(p,blob.data(),blob.size());upload->Unmap(0,nullptr);Transition(c,d.weights.gpu.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);c->CopyBufferRegion(d.weights.gpu.Get(),0,upload.Get(),0,blob.size());Transition(c,d.weights.gpu.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);d.uploads.push_back(upload);
 for(auto&entry:d.contracts){auto&t=entry.second;t.fn=Function(d,Module(d,t.cubin,nv),t.name.c_str(),nv);}
 fs::path io=root/"original_io.cubin";auto m=Module(d,io,nv);
 d.pack=Function(d,m,"pack_original_fp8",nv);d.epilogue=Function(d,m,"contract_epilogue_unsplit_f16_pair_publish",nv);auto fusedDir=FusedDir();d.builder.init(fusedDir);d.fused=Function(d,Module(d,fusedDir/"fused_expand.cubin",nv),"fused_expand",nv);
 d.norm=Allocate(device,256);auto normUpload=Resource(device,256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void*np=nullptr;Check(normUpload->Map(0,nullptr,&np));memset(np,0,256);float one=1;memcpy(np,&one,4);normUpload->Unmap(0,nullptr);Transition(c,d.norm.gpu.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);c->CopyBufferRegion(d.norm.gpu.Get(),0,normUpload.Get(),0,256);Transition(c,d.norm.gpu.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);d.uploads.push_back(normUpload);
 if(candidate){auto dir=Assets()/"candidate/split-half";d.splitBuilder.init(dir);d.split=Function(d,Module(d,dir/"fused_expand.cubin",nv),"fused_expand",nv);d.splitEpilogue=Function(d,m,"contract_epilogue_split4_f16_publish",nv);}
 uploadBatch.Finish();}
// Spawn the one-shot async device init. Caller holds S().mutex. Publishes d.init with a
// release store so a Ready observer in Launch sees all of Prepare's writes.
void KickDeviceInit(State&s,Device&d,ID3D12Device*device,bool candidate){
 NvApi nv{s.createModule,s.createFunction};
 d.init.store(1,std::memory_order_relaxed);
 device->AddRef();
 std::thread([&s,&d,device,nv,candidate]{
  std::string err;
  try{Prepare(d,device,nv,candidate);}catch(const std::exception&e){err=e.what();}catch(...){err="unknown hybrid init failure";}
  {std::lock_guard<std::recursive_mutex>g(s.mutex);
   if(err.empty()){d.init.store(2,std::memory_order_release);}
   else{d.failure=err;d.init.store(3,std::memory_order_release);fprintf(stderr,"Hybrid device init failed: %s\n",err.c_str());}}
  device->Release();
 }).detach();
}
void AllocateSession(Session&s,Device&d,ID3D12GraphicsCommandList*c){if(s.allocated)return;auto dev=d.owner.Get();auto&uploadBatch=*new UploadBatch(dev);c=uploadBatch.cmd.Get();const uint64_t P=((s.tokens+127)/128)*128;
 if(S().candidate&&s.tokens==960)s.partials=Allocate(dev,4ull*960*1024*2);s.xp=Allocate(dev,P*1024/2);s.xs=Allocate(dev,P*1024/16);s.yp=Allocate(dev,P*4096/2);s.ys=Allocate(dev,P*4096/16);s.c=Allocate(dev,P*1024*2);s.cw=Allocate(dev,d.contracts.at(s.tokens).workspaceBytes);
 s.zeroBytes=std::max<uint64_t>(s.partials.bytes,std::max<uint64_t>(P*4096/2,s.cw.bytes));s.zero=Resource(dev,s.zeroBytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void*p=nullptr;Check(s.zero->Map(0,nullptr,&p));memset(p,0,s.zeroBytes);s.zero->Unmap(0,nullptr);
 for(auto*b:{&s.xp,&s.xs,&s.yp,&s.ys,&s.c,&s.cw})CopyZero(c,*b,s.zero.Get(),b->bytes);if(s.partials.gpu)CopyZero(c,s.partials,s.zero.Get(),s.partials.bytes);uploadBatch.Finish();s.allocated=true;}
void LaunchBlob(ID3D12GraphicsCommandList*c,NVDX_ObjectHandle f,const void*p,unsigned size,NVAPI_DIM3 grid,NVAPI_DIM3 block,unsigned shared=0){NVAPI_CU_KERNEL_LAUNCH_PARAMS k{};k.hFunction=f;k.gridDim=grid;k.blockDim=block;k.pParams=p;k.paramSize=size;k.dynSharedMemBytes=shared;NvCheck(RealLaunch(S(),c,&k,1));}
template<class T>void IO(ID3D12GraphicsCommandList*c,NVDX_ObjectHandle f,const T&p,unsigned threads){LaunchBlob(c,f,&p,sizeof(p),{(threads+255)/256,1,1},{256,1,1});Barrier(c);}
void Vendor(ID3D12GraphicsCommandList*c,Device&d,Session&s){auto&t=d.contracts.at(s.tokens);auto w=d.w[s.block-31];auto&workspace=s.cw;CopyZero(c,workspace,s.zero.Get(),t.resetBytes);Blob p=t.params;
 std::map<std::string,std::pair<uint64_t,uint64_t>>bindings{{"A_packed",{d.weights.address()+(w.cp),2097152}},{"SF_A",{d.weights.address()+(w.cs),262144}},{"B_packed",{s.yp.address(),s.yp.bytes}},{"SF_B",{s.ys.address(),s.ys.bytes}},{"D",{s.c.address(),s.c.bytes}},{"workspace",{workspace.address(),workspace.bytes}}};
 for(auto&x:t.patches){auto it=bindings.find(x.name);if(it==bindings.end()||x.add>=it->second.second)throw std::runtime_error("Unknown/out-of-range vendor binding "+x.name);uint64_t v=it->second.first+x.add;memcpy(p.data()+x.offset,&v,8);}
 LaunchBlob(c,t.fn,p.data(),(unsigned)p.size(),t.grid,t.block,t.shared);ScratchBarrier(c,s.tokens,4,{s.c.gpu.Get()});}
bool Supported(const void*data,unsigned bytes){if(bytes!=3202680||!data)return false;const unsigned char expected[32]={0x3f,0xa6,0xf0,0x76,0xee,0xcf,0xbb,0x6e,0x19,0xc3,0x78,0xf8,0x4b,0xbb,0x70,0x25,0xcd,0x80,0x5e,0xd4,0xde,0x9a,0x2f,0x9c,0x9b,0x99,0x5e,0xb3,0x4a,0x7f,0x3d,0x76};unsigned char hash[32];return BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,(PUCHAR)data,bytes,hash,32)==0&&!memcmp(hash,expected,32);}
NvAPI_Status __cdecl CreateModule(ID3D12Device*d,const void*b,NvU32 n,NVDX_ObjectHandle*out){auto&s=S();std::lock_guard<std::recursive_mutex>apiGuard(s.mutex);auto rc=s.createModule(d,b,n,out);if(rc==0){std::lock_guard<std::recursive_mutex>g(s.mutex);s.modules[*out]=Supported(b,n);s.active=false;}return rc;}
NvAPI_Status __cdecl CreateFunction(ID3D12Device*d,NVDX_ObjectHandle m,const char*n,NVDX_ObjectHandle*out){auto&s=S();std::lock_guard<std::recursive_mutex>apiGuard(s.mutex);auto rc=s.createFunction(d,m,n,out);if(rc==0&&n){{std::lock_guard<std::recursive_mutex>g(s.mutex);s.fnInfo[*out]=DlssNrKernelProfile::Classify(n);}{auto role=DlssNrVitReuse::RoleOf(n);if(role!=DlssNrVitReuse::Role::None){std::lock_guard<std::recursive_mutex>g(s.mutex);s.vitRole[*out]={role,DlssNrVitReuse::SetOf(n)};}}unsigned kind=99;if(!strcmp(n,"cc_vit_1d_ffn_expand_publish_fp8"))kind=0;else if(!strcmp(n,"cc_vit_1d_ffn_expand_chained_fp8"))kind=1;else if(!strcmp(n,"cc_vit_1d_ffn_contract_chained_fp8"))kind=2;if(kind!=99){std::lock_guard<std::recursive_mutex>g(s.mutex);s.targets[*out]={d,m,kind,s.modules[m]};}}return rc;}
// ViT reuse: true when at least one launch of this call was dropped; `kept` then holds the others. Only launches recorded inside an NR evaluation are looked at.
// `gap`: where in `kept` the last launch of a skipped run was (or, on the plain set, which keeps that launch, where it is), or -1 when it
// was not in this call. Also true when that kept launch is the only thing that happened, so the caller still records the barrier.
bool VitDrop(State&s,const NVAPI_CU_KERNEL_LAUNCH_PARAMS*k,NvU32 count,std::vector<NVAPI_CU_KERNEL_LAUNCH_PARAMS>&kept,ptrdiff_t&gap){
 if(!k||!count||!s.vit.Evaluating())return false;bool any=false;
 for(NvU32 i=0;i<count;++i){auto it=s.vitRole.find(k[i].hFunction);auto role=it==s.vitRole.end()?DlssNrVitReuse::Role::None:it->second.first;
  const bool drop=s.vit.Drop(role,it==s.vitRole.end()?DlssNrVitReuse::KernelSet::Unknown:it->second.second);
  if(s.vit.TakeGap()){any=true;gap=(ptrdiff_t)kept.size();}
  if(drop)any=true;else kept.push_back(k[i]);}
 return any;}
NvAPI_Status __cdecl Launch(ID3D12GraphicsCommandList*c,const NVAPI_CU_KERNEL_LAUNCH_PARAMS*k,NvU32 count){auto&s=S();std::lock_guard<std::recursive_mutex>g(s.mutex);
 // A UAV barrier where the skipped run would have been (before its last kernel on the plain set, which keeps that one): without it the kernel before the gap and the decoder after it can overlap on the
 // GPU. The run's own kernels kept them apart; the plain fp16 kernel set flickered on reused frames without it (the fp8 set did not show it).
 {std::vector<NVAPI_CU_KERNEL_LAUNCH_PARAMS>kept;ptrdiff_t gap=-1;if(VitDrop(s,k,count,kept,gap)){if(gap<0)return kept.empty()?NVAPI_OK:RealLaunch(s,c,kept.data(),(NvU32)kept.size());
  // the kept launches of this call on either side of the gap go out as two chains with the barrier between them
  if(gap>0){auto r=RealLaunch(s,c,kept.data(),(NvU32)gap);if(r!=NVAPI_OK)return r;}Barrier(c);return (size_t)gap<kept.size()?RealLaunch(s,c,kept.data()+gap,(NvU32)(kept.size()-gap)):NVAPI_OK;}}
 // FP8 passthrough must stay available even after a latched hybrid failure: check `enabled`
 // first so switching Precision back to NVIDIA (FP8) fully recovers without an app restart.
 if(!s.enabled)return RealLaunch(s,c,k,count);if(s.restartRequired)return NVAPI_ERROR;if(!k||count!=1){auto pending=std::find_if(s.sessions.begin(),s.sessions.end(),[&](const auto&v){return std::get<0>(v.first)==c&&v.second.pending;});if(pending!=s.sessions.end()){s.restartRequired=true;s.status="Restart required: unsupported launch chain while hybrid pair pending; precision change unavailable";fprintf(stderr,"%s\n",s.status.c_str());return NVAPI_ERROR;}return RealLaunch(s,c,k,count);}auto target=s.targets.find(k->hFunction);if(target==s.targets.end())return RealLaunch(s,c,k,count);auto&t=target->second;
 try{if(k->paramSize!=72||!k->pParams)throw std::runtime_error("Original FFN ABI mismatch");uint64_t p[8];unsigned dims[2];memcpy(p,k->pParams,64);memcpy(dims,(char*)k->pParams+64,8);uint64_t tokens=(uint64_t)dims[0]*dims[1];for(auto&entry:s.sessions)if(std::get<0>(entry.first)==c&&entry.second.pending&&(std::get<1>(entry.first)!=t.device||std::get<2>(entry.first)!=tokens))throw std::runtime_error("Token count changed during pending pair");auto&cycle=s.sessions[{c,t.device,tokens,s.candidate}];cycle.tokens=tokens;cycle.lastTouch=++s.seq;
 // Bound the map so the two O(n) scans stay cheap as command-list pointers churn (resize,
 // alt-tab, device reset). Never evict a pending pair or the entry just touched; move the
 // victim to the graveyard rather than releasing GPU-referenced resources.
 if(s.sessions.size()>16){auto victim=s.sessions.end();for(auto it=s.sessions.begin();it!=s.sessions.end();++it)if(!it->second.pending&&std::get<0>(it->first)!=c&&(victim==s.sessions.end()||it->second.lastTouch<victim->second.lastTouch))victim=it;if(victim!=s.sessions.end()){s.graveyard.push_back(std::move(victim->second));s.sessions.erase(victim);}}
 const bool eligible=t.supported&&ContractPaths().count(tokens);
 const auto fallback=[&](){cycle.active=false;s.active=false;s.status="Original FP8 fallback: M="+std::to_string(tokens)+" dims="+std::to_string(dims[0])+"x"+std::to_string(dims[1])+(!t.supported?" unsupported original module hash":(s.devices.count({t.device,s.candidate})&&!s.devices.at({t.device,s.candidate}).failure.empty()?" initialization: "+s.devices.at({t.device,s.candidate}).failure:" no supplied contraction template"));};
 if(t.kind==0){if(cycle.pending)throw std::runtime_error("New publish before pending contraction");cycle.block=31;cycle.pairs=0;cycle.active=eligible;cycle.device=t.device;}
 if(t.kind<2){if(cycle.pending)throw std::runtime_error("Expansion while pair pending");if(!cycle.active||!eligible){fallback();return RealLaunch(s,c,k,count);}if(cycle.block>38||cycle.device!=t.device)throw std::runtime_error("FFN block/device sequence mismatch");
 if(k->gridDim.x!=32*((tokens+127)/128)||k->gridDim.y!=1||k->gridDim.z!=1||k->blockDim.x!=32||k->blockDim.y!=4||k->blockDim.z!=1||k->dynSharedMemBytes)throw std::runtime_error("Original expansion geometry mismatch");
 auto&d=s.devices[{t.device,s.candidate}];
 const int di=d.init.load(std::memory_order_acquire);
 if(di!=2){ // device init runs on a worker; record FP8 until it publishes Ready
  if(di==0)KickDeviceInit(s,d,t.device,s.candidate);
  cycle.active=false;
  s.status=di==3?("Original FP8 fallback: hybrid init failed: "+d.failure):"Hybrid device initialising; FP8 this frame";
  return RealLaunch(s,c,k,count);
 }
 try{AllocateSession(cycle,d,c);for(unsigned preflightBlock=0;preflightBlock<8;++preflightBlock){
 auto wi=preflightBlock;auto&w=d.w[wi];auto&blob=cycle.fusedParams[wi];auto&info=cycle.fusedInfo[wi];
 if(blob.empty()){FusedInput input{(int)tokens,4096,1024,170,cycle.xp.address(),d.weights.address()+w.ep,cycle.xs.address(),d.weights.address()+w.es,0,cycle.yp.address(),cycle.ys.address(),d.norm.address(),0};d.builder.encode(input,blob,info);if(info.sfa_bytes>cycle.xs.bytes||info.sfb_bytes>262144||info.sfd_bytes>cycle.ys.bytes)throw std::runtime_error("Fused scale allocation too small");fprintf(stderr,"Fused block%u Params%llu shared%llu grid%u,%u,%u block%u,%u,%u; D3D addresses encoded\n",preflightBlock+31,info.params_bytes,info.shared_bytes,info.grid[0],info.grid[1],info.grid[2],info.block[0],info.block[1],info.block[2]);}
}
 if(s.candidate&&tokens==960)for(unsigned wi=0;wi<8;++wi){auto&w=d.w[wi];auto&blob=cycle.splitParams[wi];auto&info=cycle.splitInfo[wi];if(blob.empty()){
 FusedInput input{960,1024,1024,170,cycle.yp.address(),d.weights.address()+w.cp,cycle.ys.address(),d.weights.address()+w.cs,0,cycle.partials.address(),0,0,0};d.splitBuilder.encode(input,blob,info);
 if(info.sfa_bytes>cycle.ys.bytes||info.sfb_bytes>262144||info.workspace_bytes)throw std::runtime_error("Candidate split scale/workspace bounds mismatch");}}
 }catch(const std::exception&e){d.failure=e.what();throw;}
 cycle.pending=true;cycle.x=p[0];cycle.y=p[2];cycle.done=p[7];cycle.width=dims[0];cycle.height=dims[1];Barrier(c);
 struct Pack{uint64_t input,output,sf;unsigned rows,cols;}pack{p[0],cycle.xp.address(),cycle.xs.address(),(unsigned)tokens,1024};LaunchBlob(c,d.pack,&pack,sizeof(pack),{((unsigned)tokens*1024/2+255)/256,1,1},{256,1,1});ScratchBarrier(c,tokens,1,{cycle.xp.gpu.Get(),cycle.xs.gpu.Get()});
 auto&blob=cycle.fusedParams[cycle.block-31];auto&info=cycle.fusedInfo[cycle.block-31];
 LaunchBlob(c,d.fused,blob.data(),(unsigned)blob.size(),{info.grid[0],info.grid[1],info.grid[2]},{info.block[0],info.block[1],info.block[2]},(unsigned)info.shared_bytes);ScratchBarrier(c,tokens,2,{cycle.yp.gpu.Get(),cycle.ys.gpu.Get()});
 ++s.launches;s.status=(s.candidate?"Candidate hybrid M=":"Fused hybrid M=")+std::to_string(tokens)+" dims="+std::to_string(dims[0])+"x"+std::to_string(dims[1])+" expansion block "+std::to_string(cycle.block);return NVAPI_OK;
 }
 if(!cycle.pending){if(cycle.active)throw std::runtime_error("Contraction without replaced expansion");return RealLaunch(s,c,k,count);}
 if(!t.supported||cycle.device!=t.device||tokens!=cycle.tokens||dims[0]!=cycle.width||dims[1]!=cycle.height||p[0]!=cycle.y||p[1]!=cycle.x||p[6]!=cycle.done)throw std::runtime_error("Pending contraction alias/shape mismatch; refusing partial fallback");
 if(k->gridDim.x!=8*((tokens+127)/128)||k->gridDim.y!=1||k->gridDim.z!=4||k->blockDim.x!=32||k->blockDim.y!=4||k->blockDim.z!=1||k->dynSharedMemBytes)throw std::runtime_error("Original contraction geometry mismatch");
 auto&d=s.devices[{t.device,s.candidate}];auto wi=cycle.block-31;auto w=d.w[wi];uint64_t contractionOutput=cycle.c.address();auto epilogue=d.epilogue;
 if(s.candidate&&tokens==960){auto&blob=cycle.splitParams[wi];auto&info=cycle.splitInfo[wi];
 LaunchBlob(c,d.split,blob.data(),(unsigned)blob.size(),{info.grid[0],info.grid[1],info.grid[2]},{info.block[0],info.block[1],info.block[2]},(unsigned)info.shared_bytes);
 ScratchBarrier(c,tokens,4,{cycle.partials.gpu.Get()});contractionOutput=cycle.partials.address();epilogue=d.splitEpilogue;++s.splitLaunches;
 }else Vendor(c,d,cycle);
 struct Epilogue{uint64_t gemm,skip,cosine,output;unsigned rows,pad;uint64_t done,counter,expand_done;}ep{contractionOutput,p[1],d.weights.address()+w.cos,p[2],(unsigned)tokens,0,p[7],p[4],cycle.done};IO(c,epilogue,ep,(unsigned)tokens*1024/2);
 cycle.pending=false;++cycle.block;++cycle.pairs;++s.launches;if(cycle.pairs==8)s.active=true;s.status=(s.candidate?"Candidate hybrid M=":"Fused hybrid M=")+std::to_string(tokens)+" dims="+std::to_string(dims[0])+"x"+std::to_string(dims[1])+" FFN pairs recorded "+std::to_string(cycle.pairs)+"/8 ("+std::to_string(cycle.pairs*2)+"/16 original launches); other operators FP8";return NVAPI_OK;
 }catch(const std::exception&e){s.active=false;bool pending=false;for(auto&entry:s.sessions)if(std::get<0>(entry.first)==c){pending|=entry.second.pending;if(!entry.second.pending)entry.second.active=false;}std::string shape;if(k&&k->pParams&&k->paramSize==72){unsigned dims[2];memcpy(dims,(char*)k->pParams+64,8);shape="M="+std::to_string((uint64_t)dims[0]*dims[1])+" dims="+std::to_string(dims[0])+"x"+std::to_string(dims[1])+" ";}s.restartRequired|=pending;s.status=std::string(pending?"Restart required: hybrid failed after replacement: ":"Original FP8 fallback: ")+shape+e.what();fprintf(stderr,"%s\n",s.status.c_str());return pending?NVAPI_ERROR:RealLaunch(s,c,k,count);}}
NvAPI_Status __cdecl DestroyFunction(ID3D12Device*d,NVDX_ObjectHandle f){auto&s=S();std::lock_guard<std::recursive_mutex>g(s.mutex);s.targets.erase(f);s.vitRole.erase(f);s.fnInfo.erase(f);return s.destroyFunction(d,f);}
NvAPI_Status __cdecl DestroyModule(ID3D12Device*d,NVDX_ObjectHandle m){auto&s=S();std::lock_guard<std::recursive_mutex>g(s.mutex);s.modules.erase(m);s.active=false;s.vit.Clear();for(auto i=s.targets.begin();i!=s.targets.end();)if(i->second.module==m)i=s.targets.erase(i);else++i;return s.destroyModule(d,m);}
}
void*WrapNvapi(unsigned id,void*raw){if(!raw)return raw;auto&s=S();std::lock_guard<std::recursive_mutex>apiGuard(s.mutex);switch(id){case 0xad1a677d:s.createModule=(decltype(s.createModule))raw;return(void*)&CreateModule;case 0xe2436e22:s.createFunction=(decltype(s.createFunction))raw;return(void*)&CreateFunction;case 0x24973538:s.launch=(decltype(s.launch))raw;return(void*)&Launch;case 0x41c65285:s.destroyModule=(decltype(s.destroyModule))raw;return(void*)&DestroyModule;case 0xdf295ea6:s.destroyFunction=(decltype(s.destroyFunction))raw;return(void*)&DestroyFunction;default:return raw;}}
void SetPrecision(unsigned precision){auto&s=S();std::lock_guard<std::recursive_mutex>g(s.mutex);const bool on=precision==4,candidate=on;
 const bool anyPending=std::any_of(s.sessions.begin(),s.sessions.end(),[](const auto&p){return p.second.pending;});
 // Clearing a latched restartRequired is only safe with no FFN pair mid-cycle: a pending pair
 // means an already-recorded command list still expects our contraction kernel. Reset failed
 // devices to idle so KickDeviceInit re-runs on the next eligible launch.
 const auto tryRecover=[&]{if(s.restartRequired&&!anyPending){s.restartRequired=false;s.sessions.clear();for(auto&e:s.devices)if(e.second.init.load(std::memory_order_relaxed)==3){e.second.init.store(0,std::memory_order_relaxed);e.second.failure.clear();}}};
 if(!on){s.enabled=false;s.candidate=false;s.active=false;tryRecover();s.status="Original FP8 selected";return;}
 tryRecover();
 if(s.restartRequired){s.status="Restart required: switch Precision to NVIDIA (FP8) with no frame in flight, then back, to retry";return;}
 if(s.enabled==on&&s.candidate==candidate)return;
 if(anyPending){s.restartRequired=true;s.status="Restart required: hybrid pair still pending; precision change was not applied";return;}
 s.enabled=on;s.candidate=candidate;s.active=false;s.status=candidate?"Candidate hybrid selected; waiting for original model":"Hybrid FFN selected; waiting for supported original model";
}
void SetEnabled(bool on){SetPrecision(on?4u:0u);}
bool IsActive(){auto&s=S();std::lock_guard<std::recursive_mutex>g(s.mutex);return s.enabled&&s.active&&!s.restartRequired;}
void BeginEvaluate(const void*feature,bool reset,unsigned every,unsigned everyPlain,long long slot,ID3D12GraphicsCommandList*cmd,bool profile){auto&s=S();std::lock_guard<std::recursive_mutex>g(s.mutex);s.vit.Begin(feature,reset,every,slot,everyPlain);s.prof.Begin(cmd,profile);}
void EndEvaluate(ID3D12GraphicsCommandList*cmd){auto&s=S();std::lock_guard<std::recursive_mutex>g(s.mutex);s.prof.End(cmd);const bool wasOff=s.vit.Disabled();if(!s.vit.End()&&!wasOff)fprintf(stderr,"DLSS-NR ViT reuse: unexpected launch order, reuse is off for this session\n");}
std::vector<std::string> TakeProfileReports(){auto&s=S();std::lock_guard<std::recursive_mutex>g(s.mutex);return s.prof.TakeReports();}
std::string VitStatus(){auto&s=S();std::lock_guard<std::recursive_mutex>g(s.mutex);return s.vit.Status();}
bool VitPlainKernels(){return S().vit.LastSet()==DlssNrVitReuse::KernelSet::Plain;}
const char* VitKernelSet(){switch(S().vit.LastSet()){case DlssNrVitReuse::KernelSet::Fp8:return "FP8";case DlssNrVitReuse::KernelSet::Plain:return "plain FP16";default:return "not seen yet";}}
std::string Status(){auto&s=S();std::lock_guard<std::recursive_mutex>g(s.mutex);return s.status+" | rewritten original launches: "+std::to_string(s.launches)+" | candidate split contractions: "+std::to_string(s.splitLaunches);}
}
