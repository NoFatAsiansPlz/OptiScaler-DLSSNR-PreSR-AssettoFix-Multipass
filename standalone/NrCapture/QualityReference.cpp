// Compile OptiScaler's real model wrapper into the headless check only.
#include "../../OptiScaler/dlssnr/DlssNr_Proxy.cpp"

namespace DlssNr::NgxDiagnostics {
Scope::Scope() {}
Scope::~Scope() {}
void RuntimeReport(ID3D12GraphicsCommandList*, ID3D12Device*, const char*) {}
}
namespace DlssNr {
std::shared_ptr<CompatibilityRuntime> CompatibilityRuntime::TryOpen(ID3D12Device*) { return nullptr; }
}
