#include <rex/platform.h>
#if REX_PLATFORM_PS4
#include <rex/platform/dynlib.h>
namespace rex::platform {
DynamicLibrary::~DynamicLibrary() { Close(); }
DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
  if (this != &other) { Close(); handle_ = other.handle_; other.handle_ = nullptr; }
  return *this;
}
bool DynamicLibrary::Load(const std::filesystem::path&, SymbolResolution) { Close(); return false; }
void DynamicLibrary::Close() { handle_ = nullptr; }
void* DynamicLibrary::GetRawSymbol(const char*) const { return nullptr; }
}
#endif
