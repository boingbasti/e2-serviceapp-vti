/* Force older glibc symbol versions for VTi/glibc-2.21 compatibility.
   Included in every compilation unit via -include. */
#ifndef __ASSEMBLER__
__asm__(".symver fcntl,fcntl@GLIBC_2.4");
__asm__(".symver pow,pow@GLIBC_2.4");
__asm__(".symver vsscanf,vsscanf@GLIBC_2.4");
__asm__(".symver strtol,strtol@GLIBC_2.4");
#endif
