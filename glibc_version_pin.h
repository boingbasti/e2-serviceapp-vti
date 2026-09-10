/* Force older glibc symbol versions for VTi/glibc-2.21 compatibility.
   Included in every compilation unit via -include. */
#ifndef __ASSEMBLER__
__asm__(".symver fcntl,fcntl@GLIBC_2.4");
__asm__(".symver pow,pow@GLIBC_2.4");
__asm__(".symver vsscanf,vsscanf@GLIBC_2.4");
__asm__(".symver strtol,strtol@GLIBC_2.4");
__asm__(".symver strtoll,strtoll@GLIBC_2.4");
/* dlopen/dlsym/dlclose/dlerror wurden mit glibc 2.34 von libdl.so.2 in
   libc.so.6 zusammengefuehrt - der Build-Host exportiert sie deshalb
   standardmaessig als @GLIBC_2.34, das die Box-glibc 2.21 nicht kennt. */
__asm__(".symver dlopen,dlopen@GLIBC_2.4");
__asm__(".symver dlsym,dlsym@GLIBC_2.4");
__asm__(".symver dlclose,dlclose@GLIBC_2.4");
__asm__(".symver dlerror,dlerror@GLIBC_2.4");
#endif
