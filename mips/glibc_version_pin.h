/* Force older glibc symbol versions for VTi/glibc-2.21 compatibility (MIPS).
   Included in every compilation unit via -include. */
#ifndef __ASSEMBLER__
__asm__(".symver fcntl,fcntl@GLIBC_2.0");
__asm__(".symver pow,pow@GLIBC_2.0");
__asm__(".symver vsscanf,vsscanf@GLIBC_2.0");
__asm__(".symver strtol,strtol@GLIBC_2.0");
__asm__(".symver __isoc23_strtol,strtol@GLIBC_2.0");
__asm__(".symver __isoc23_sscanf,sscanf@GLIBC_2.0");
#endif
