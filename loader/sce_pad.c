/* sce_pad.c -- keep room for vita-elf-create's SCE data.
 *
 * vita-elf-create appends ~4.6 KB of SCE data after the end of the RX segment
 * and needs that much free before the RW segment starts. The RW segment is
 * aligned to 64 KB (CMakeLists), so the free space is whatever is left of the
 * last 64 KB page -- anywhere from 0 to 64 KB depending on how big .text
 * happens to be. When a build lands just short of a boundary, tools/ci_build.sh
 * measures the gap and rebuilds with KOTOR_SCE_PAD set to push the RX end
 * past it, which leaves nearly a whole page free. 0 (the default) adds nothing. */
#ifndef KOTOR_SCE_PAD
#define KOTOR_SCE_PAD 0
#endif

#if KOTOR_SCE_PAD > 0
__attribute__((used)) const unsigned char kotor_sce_pad[KOTOR_SCE_PAD] = { 1 };
#endif

/* An empty translation unit is not ISO C. */
typedef int sce_pad_unused;
