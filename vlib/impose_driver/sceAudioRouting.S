	.set noreorder

#include "pspimport.s"

// Build List
// sceAudioRouting_0000.o sceAudioRouting_0001.o sceAudioRouting_0002.o sceAudioRouting_0003.o sceAudioRouting_0004.o 

#ifdef F_sceAudioRouting_0000
	IMPORT_START	"sceAudioRouting",0x40010011
#endif
#ifdef F_sceAudioRouting_0001
	IMPORT_FUNC	"sceAudioRouting",0x28235C56,sceAudioRoutingGetVolumeMode
#endif
#ifdef F_sceAudioRouting_0002
	IMPORT_FUNC	"sceAudioRouting",0x36FD8AA9,sceAudioRoutingSetMode
#endif
#ifdef F_sceAudioRouting_0003
	IMPORT_FUNC	"sceAudioRouting",0x39240E7D,sceAudioRoutingGetMode
#endif
#ifdef F_sceAudioRouting_0004
	IMPORT_FUNC	"sceAudioRouting",0xBB548475,sceAudioRoutingSetVolumeMode
#endif
