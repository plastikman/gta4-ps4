# SDL3's CMake platform detection does not recognize Orbis' CMAKE_SYSTEM_NAME
# (Generic). The SDK uses FreeBSD-derived libc and pthreads, so select SDL's
# FreeBSD/POSIX sources and thread implementation for this subproject only.
if(PS4)
    set(UNIX TRUE)
    set(FREEBSD TRUE)
endif()
