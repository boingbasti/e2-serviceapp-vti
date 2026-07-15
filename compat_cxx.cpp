/* libstdc++ compatibility stubs for VTi (GCC 4.9 / GLIBCXX_3.4.20).
   Provides symbols introduced in newer GCC that older libstdc++ doesn't have. */

#include <cstdlib>
#include <new>

namespace std {

/* GLIBCXX_3.4.32 (GCC 14): new iostream init mechanism.
   Old libstdc++ uses ios_base::Init constructor instead — this can be a no-op. */
void ios_base_library_init() {}

/* GLIBCXX_3.4.29 (GCC 12): bad_array_new_length exception helper.
   Triggered when new T[n] overflows. Map to bad_alloc for old libstdc++. */
void __throw_bad_array_new_length()
{
    throw std::bad_alloc();
}

} // namespace std

/* CXXABI_1.3.9: sized operator delete — delegate to regular delete.
   operator delete(void*, unsigned int) */
void operator delete(void* p, unsigned int) noexcept
{
    ::operator delete(p);
}
