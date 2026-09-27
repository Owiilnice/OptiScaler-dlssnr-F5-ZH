#pragma once
#include <string>
#include <vector>
struct ID3D12GraphicsCommandList;
namespace DlssNrNative {
void* WrapNvapi(unsigned id,void* original);
void SetEnabled(bool enabled);
void SetPrecision(unsigned precision);
bool IsActive();
std::string Status();
// ViT reuse (see DlssNrVitReuse.h): bracket one model evaluation of `feature`. `every` = how often the ViT bottleneck is computed (1 = always) with the fp8 kernel set, `everyPlain` with the plain fp16 one; `slot` = frame number, the same for every pass of a frame, so the passes compute on the same frame.
void BeginEvaluate(const void* feature, bool reset, unsigned every, unsigned everyPlain, long long slot, ID3D12GraphicsCommandList* cmd, bool profile);
void EndEvaluate(ID3D12GraphicsCommandList* cmd);
// Kernel census / per-group GPU timing lines finished since the last call (ini [DlssNr] KernelProfile), for the log.
std::vector<std::string> TakeProfileReports();
std::string VitStatus();
// The kernel set of the last ViT run seen: "FP8", "plain FP16" or "not seen yet".
const char* VitKernelSet();
// True when the last ViT run seen used the plain fp16 kernels.
bool VitPlainKernels();
}
