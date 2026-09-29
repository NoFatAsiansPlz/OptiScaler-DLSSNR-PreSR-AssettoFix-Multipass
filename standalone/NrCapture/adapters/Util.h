#pragma once
#include <unknwn.h>
// The shared retirement utility also serves wrapped game queues. This executable
// only creates native COM objects: there are no wrapper identities to resolve.
namespace Util {
inline bool CheckForRealObject(const char*, IUnknown*, IUnknown**) { return false; }
}
