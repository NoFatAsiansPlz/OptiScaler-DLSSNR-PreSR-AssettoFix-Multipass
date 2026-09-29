#pragma once

struct ID3D12Device;

namespace nr::AdaMfgUnlock {
// Clear capture-session eligibility; the patched NGX provider remains process-owned.
void ResetSession();
// Ordinary startup thread, after NGX initialization and before Streamline initialization.
void PrepareProvider(ID3D12Device* device);
// The actual slDLSSGGetState entry point, before the first state query or FG feature creation.
void PreparePlugin(void* getStateFunction);
bool RequestedOnAda();
bool Active();
unsigned MaximumMultiplier();
}
