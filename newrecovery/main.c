#include <pspkernel.h>
#include <pspsdk.h>
#include <pspusb.h>
#include <pspusbstor.h>
#include <psploadexec_kernel.h>
#include <pspsuspend.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "mydebug.h"
#include "menu.h"
#include "conf.h"
#include <pspreg.h>
#include <psplflash_fatfmt.h>
#include <psppower.h>

PSP_MODULE_INFO("Recovery mode", 0x1000, 1, 0);
PSP_MAIN_THREAD_ATTR(0);

#define printf myDebugScreenPrintf
#define RGB(r, g, b) (0xFF000000 | ((b)<<16) | ((g)<<8) | (r))
#define setTextColor myDebugScreenSetTextColor
#define setBackColor myDebugScreenSetBackColor
#define setXY myDebugScreenSetXY
#define delay sceKernelDelayThread
#define clearScreen myDebugScreenClear

SEConfig config;
int configchanged = 0;

int usbStatus = 0;
int usbModuleStatus = 0;

PspIoDrv *lflash_driver;
PspIoDrv *msstor_driver;

int get_registry_value(const char *dir, const char *name, unsigned int *val)
{
        int ret = 0;
        struct RegParam reg;
        REGHANDLE h;

        memset(&reg, 0, sizeof(reg));
        reg.regtype = 1;
        reg.namelen = strlen("/system");
        reg.unk2 = 1;
        reg.unk3 = 1;
        strcpy(reg.name, "/system");
        if(sceRegOpenRegistry(&reg, 2, &h) == 0)
        {
                REGHANDLE hd;
                if(!sceRegOpenCategory(h, dir, 2, &hd))
                {
                        REGHANDLE hk;
                        unsigned int type, size;

                        if(!sceRegGetKeyInfo(hd, name, &hk, &type, &size))
                        {
                                if(!sceRegGetKeyValue(hd, hk, val, 4))
                                {
                                        ret = 1;
                                        sceRegFlushCategory(hd);
                                }
                        }
                        sceRegCloseCategory(hd);
                }
                sceRegFlushRegistry(h);
                sceRegCloseRegistry(h);
        }

        return ret;
}

int set_registry_value(const char *dir, const char *name, unsigned int val)
{
        int ret = 0;
        struct RegParam reg;
        REGHANDLE h;

        memset(&reg, 0, sizeof(reg));
        reg.regtype = 1;
        reg.namelen = strlen("/system");
        reg.unk2 = 1;
        reg.unk3 = 1;
        strcpy(reg.name, "/system");
        if(sceRegOpenRegistry(&reg, 2, &h) == 0)
        {
                REGHANDLE hd;
                if(!sceRegOpenCategory(h, dir, 2, &hd))
                {
                        if(!sceRegSetKeyValue(hd, name, &val, 4))
                        {
                                ret = 1;
                                sceRegFlushCategory(hd);
                        }
						else
						{
							sceRegCreateKey(hd, name, REG_TYPE_INT, 4);
							sceRegSetKeyValue(hd, name, &val, 4);
							ret = 1;
                            sceRegFlushCategory(hd);
						}
                        sceRegCloseCategory(hd);
                }
                sceRegFlushRegistry(h);
                sceRegCloseRegistry(h);
        }

        return ret;
}

int ReadLine(SceUID fd, char *str)
{
	char ch = 0;
	int n = 0;

	while (1)
	{	
		if (sceIoRead(fd, &ch, 1) != 1)
			return n;

		if (ch < 0x20)
		{
			if (n != 0)
				return n;
		}
		else
		{
			*str++ = ch;
			n++;
		}
	}
	
}

int (* Orig_IoOpen)(PspIoDrvFileArg *arg, char *file, int flags, SceMode mode);
int (* Orig_IoClose)(PspIoDrvFileArg *arg);
int (* Orig_IoRead)(PspIoDrvFileArg *arg, char *data, int len);
int (* Orig_IoWrite)(PspIoDrvFileArg *arg, const char *data, int len);
SceOff(* Orig_IoLseek)(PspIoDrvFileArg *arg, SceOff ofs, int whence);
int (* Orig_IoIoctl)(PspIoDrvFileArg *arg, unsigned int cmd, void *indata, int inlen, void *outdata, int outlen);
int (* Orig_IoDevctl)(PspIoDrvFileArg *arg, const char *devname, unsigned int cmd, void *indata, int inlen, void *outdata, int outlen);

int unit;

/* 1.50 specific function */
PspIoDrv *FindDriver(char *drvname)
{
	u32 *mod = (u32 *)sceKernelFindModuleByName("sceIOFileManager");

	if (!mod)
	{
		return NULL;
	}

	u32 text_addr = *(mod+27);

	u32 *(* GetDevice)(char *) = (void *)(text_addr+0x16D4);
	u32 *u;

	u = GetDevice(drvname);

	if (!u)
	{
		return NULL;
	}

	return (PspIoDrv *)u[1];
}

static int New_IoOpen(PspIoDrvFileArg *arg, char *file, int flags, SceMode mode)
{
	if (!lflash_driver->funcs->IoOpen)
		return -1;

	if (unit == 0)
		file = "0,0";
	else if (unit == 1)
		file = "0,1";
	else if (unit == 2)
		file = "0,2";
	else
		file = "0,3";

	return lflash_driver->funcs->IoOpen(arg, file, flags, mode);
}

static int New_IoClose(PspIoDrvFileArg *arg)
{
	if (!lflash_driver->funcs->IoClose)
		return -1;

	return lflash_driver->funcs->IoClose(arg);
}

static int New_IoRead(PspIoDrvFileArg *arg, char *data, int len)
{
	if (!lflash_driver->funcs->IoRead)
		return -1;

	return lflash_driver->funcs->IoRead(arg, data, len);
}
static int New_IoWrite(PspIoDrvFileArg *arg, const char *data, int len)
{
	if (!lflash_driver->funcs->IoWrite)
		return -1;

	return lflash_driver->funcs->IoWrite(arg, data, len);
}

static SceOff New_IoLseek(PspIoDrvFileArg *arg, SceOff ofs, int whence)
{
	if (!lflash_driver->funcs->IoLseek)
		return -1;

	return lflash_driver->funcs->IoLseek(arg, ofs, whence);
}

u8 data_5803[96] = 
{
	0x02, 0x00, 0x08, 0x00, 0x08, 0x00, 0x07, 0x9F, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x21, 0x21, 0x00, 0x00, 0x20, 0x01, 0x08, 0x00, 0x02, 0x00, 0x02, 0x00, 
	0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static int New_IoIoctl(PspIoDrvFileArg *arg, unsigned int cmd, void *indata, int inlen, void *outdata, int outlen)
{
	if (cmd == 0x02125008)
	{
		u32 *x = (u32 *)outdata;
		*x = 1; /* Enable writing */
		return 0;
	}
	else if (cmd == 0x02125803)
	{
		memcpy(outdata, data_5803, 96);
		return 0;
	}

	return -1;
}

static int New_IoDevctl(PspIoDrvFileArg *arg, const char *devname, unsigned int cmd, void *indata, int inlen, void *outdata, int outlen)
{
	if (cmd == 0x02125801)
	{
		u8 *data8 = (u8 *)outdata;

		data8[0] = 1;
		data8[1] = 0;
		data8[2] = 0,
		data8[3] = 1;
		data8[4] = 0;
				
		return 0;
	}

	return -1;
}

int UnassignFlashes(int f0only)
{
	if (sceIoUnassign("flash0:") < 0)
		return -1;

	if (!f0only)
	{
		if (sceIoUnassign("flash1:") < 0)
			return -1;
	}

	return 0;
}

int AssignFlashes(int f0only)
{
	if (sceIoAssign("flash0:", "lflash0:0,0", "flashfat0:", IOASSIGN_RDWR, NULL, 0) < 0)
		return -1;

	if (!f0only)
	{
		if (sceIoAssign("flash1:", "lflash0:0,1", "flashfat1:", IOASSIGN_RDWR, NULL, 0) < 0)
			return -1;
	}

	return 0;
}

void disableUsb(void) 
{ 
	if(usbStatus) 
	{
		sceUsbDeactivate(0);
		sceUsbStop(PSP_USBSTOR_DRIVERNAME, 0, 0);
		sceUsbStop(PSP_USBBUS_DRIVERNAME, 0, 0);

		msstor_driver->funcs->IoOpen = Orig_IoOpen;
		msstor_driver->funcs->IoClose = Orig_IoClose;
		msstor_driver->funcs->IoRead = Orig_IoRead;
		msstor_driver->funcs->IoWrite = Orig_IoWrite;
		msstor_driver->funcs->IoLseek = Orig_IoLseek;
		msstor_driver->funcs->IoIoctl = Orig_IoIoctl;
		msstor_driver->funcs->IoDevctl = Orig_IoDevctl;

		usbStatus = 0;

		if (unit >= 0)
		{
			AssignFlashes(0);
		}

		scePowerUnlock(0);
	}
}

void enableUsb(int device) 
{
	if (usbStatus)
	{
		disableUsb();
		sceKernelDelayThread(300000);
	}

	if(!usbModuleStatus) 
	{
		pspSdkLoadStartModule("flash0:/kd/semawm.prx", PSP_MEMORY_PARTITION_KERNEL);
		pspSdkLoadStartModule("flash0:/kd/usbstor.prx", PSP_MEMORY_PARTITION_KERNEL);
		pspSdkLoadStartModule("flash0:/kd/usbstormgr.prx", PSP_MEMORY_PARTITION_KERNEL);
		pspSdkLoadStartModule("flash0:/kd/usbstorms.prx", PSP_MEMORY_PARTITION_KERNEL);
		pspSdkLoadStartModule("flash0:/kd/usbstorboot.prx", PSP_MEMORY_PARTITION_KERNEL);
		
		lflash_driver = FindDriver("lflash");
		msstor_driver = FindDriver("msstor");
		
		Orig_IoOpen = msstor_driver->funcs->IoOpen;
		Orig_IoClose = msstor_driver->funcs->IoClose;
		Orig_IoRead = msstor_driver->funcs->IoRead;
		Orig_IoWrite = msstor_driver->funcs->IoWrite;
		Orig_IoLseek = msstor_driver->funcs->IoLseek;
		Orig_IoIoctl = msstor_driver->funcs->IoIoctl;
		Orig_IoDevctl = msstor_driver->funcs->IoDevctl;

		usbModuleStatus = 1;
	}

	if (device != 0)
	{
		unit = device-1;

		UnassignFlashes(0);

		msstor_driver->funcs->IoOpen = New_IoOpen;
		msstor_driver->funcs->IoClose = New_IoClose;
		msstor_driver->funcs->IoRead = New_IoRead;
		msstor_driver->funcs->IoWrite = New_IoWrite;
		msstor_driver->funcs->IoLseek = New_IoLseek;
		msstor_driver->funcs->IoIoctl = New_IoIoctl;
		msstor_driver->funcs->IoDevctl = New_IoDevctl;
	}
	else
	{
		unit = -1;
	}

	scePowerLock(0);

	sceUsbStart(PSP_USBBUS_DRIVERNAME, 0, 0);
	sceUsbStart(PSP_USBSTOR_DRIVERNAME, 0, 0);
	sceUsbstorBootSetCapacity(0x800000);
	sceUsbActivate(0x1c8);
	usbStatus = 1;
}

int FormatFlash1()
{
	char *argv[2];

	pspSdkLoadStartModule("flash0:/kd/lflash_fatfmt.prx", PSP_MEMORY_PARTITION_KERNEL);
	
	if (UnassignFlashes(0) < 0)
	{
		return -1;
	}	

	argv[0] = "fatfmt";
	argv[1] = "lflash0:0,1";

	if (sceLflashFatfmtStartFatfmt(2, argv) < 0)
	{
		return -1;
	}

	if (AssignFlashes(0) < 0)
	{
		return -1;
	}	

	return 0;
}

static int ValidCpu(int cpu)
{
	return (cpu == 0 || cpu == 20 || cpu == 75 || cpu == 100 || 
		    cpu == 133 || cpu == 222 || cpu == 266 || cpu == 300 || cpu == 333);
}

static int ValidBus(int bus)
{
	return (bus == 0 || bus == 10 || bus == 37 || bus == 50 ||
		    bus == 66 ||  bus == 111 || bus == 133 || bus == 150 || bus == 166);
}

#define PROGRAM "ms0:/PSP/GAME/RECOVERY/EBOOT.PBP"

char  hide[56], game[56], bootprog[76], noumd[56], region[56];
char patch[96], bootbin[96];
char plugins[21][50];
char vshspeed[48], umdspeed[48];
char freereg[48];
char hbreset[40];
char *plugins_p[21];
u8	plugcon[20];
char button[48];
char vshmenu[40];
char usbdev[50];
int nvsh = 0, ngame = 0, npops = 0, ngame150 = 0;

char *regions[12] =
{
	"Disabled",
	"Japan",
	"America",
	"Europe",
	"",
	"",
	"",
	"Australia/New Zealand",
	"",
	"",
	"Russia",
	""
};

char *umd_modes[] =
{
	"Normal -UMD required-",
	"OE isofs legacy -NO UMD-",
	"M33 driver -NO UMD-",
	"Sony NP9660 -NO UMD-"
};

char *items[] = { "Toggle USB", "Configuration -> ", "Run program at /PSP/GAME/RECOVERY/EBOOT.PBP", "Advanced -> ", "CPU Speed ->", "Plugins ->", "Registry hacks ->", "Exit", 0 };
char *advitems[] = { "Back", "Advanced configuration -> ", "Toggle USB (flash0)", "Toggle USB (flash1)", "Toggle USB (flash2)", "Toggle USB (flash3)", "Format flash1 and reset settings" };
char *conitems[] = { "Back", "", "", "", "", "", "", "", "", "", "" };
char *conmsg = "Configuration";
char *adconitems[] = { "Back", "", "" };
char *speeditems[] = { "Back", "", "" };
char *regitems[] = { "Back", "", "Activate WMA", "Activate Flash Player" };

int main(void) {
	myDebugScreenInit();
	
	int oldselection;

	//ReassignFlash0();
	//myDebugScreenSetBackColor(RGB(164, 0, 164));
	UnassignFlashes(1);
	AssignFlashes(1);

	myDebugScreenSetBackColor(RGB(0, 0, 0));
	while(1) 
	{
		clearScreen();
		int result;
		result = doMenu(items, 8, 0, "Main menu", 0);
		if(result == 0) {
			if(!usbStatus) {
				printf(" > USB enabled"); enableUsb(0); delay(1000000);
			} else {
				printf(" > USB disabled"); disableUsb(); delay(1000000);
			}
		}
		else if(result == 3) 
		{
			while(1) 
			{
				clearScreen();
				result = doMenu(advitems, 7, 0, "Advanced", 2);
				
				if(result == 0) 
				{ 
					printf(" > Back..."); 
					disableUsb();
					delay(1000000); 
					break;
				}

				else if (result == 3)
				{
					if(!usbStatus) 
					{
						printf(" > USB enabled"); enableUsb(2); delay(1000000);
					} else {
						printf(" > USB disabled"); disableUsb(); delay(1000000);
					}
				}

				else if (result == 2)
				{
					if(!usbStatus) 
					{
						printf(" > USB enabled"); enableUsb(1); delay(1000000);
					} else {
						printf(" > USB disabled"); disableUsb(); delay(1000000);
					}
				}

				else if (result == 4)
				{
					if(!usbStatus) 
					{
						printf(" > USB enabled"); enableUsb(3); delay(1000000);
					} else {
						printf(" > USB disabled"); disableUsb(); delay(1000000);
					}
				}

				else if (result == 5)
				{
					if(!usbStatus) 
					{
						printf(" > USB enabled"); enableUsb(4); delay(1000000);
					} else {
						printf(" > USB disabled"); disableUsb(); delay(1000000);
					}
				}

				else if (result == 6)
				{
					disableUsb();
					printf(" > Formatting flash1...");
					if (FormatFlash1() < 0)
						sceKernelSleepThread();

					delay(1000000);
					sceKernelExitVSHVSH(NULL);
				}

				else if (result == 1)
				{
					while (1)
					{
						clearScreen();
						
						if (!configchanged)
						{
							SE_GetConfig(&config);
						}

						sprintf(patch, "Plain modules in UMD/ISO (currently: %s)", config.umdactivatedplaincheck ? "Enabled" : "Disabled");
						sprintf(bootbin, "Execute BOOT.BIN in UMD/ISO (currently: %s)", config.executebootbin ? "Enabled" : "Disabled");

						adconitems[1] = patch;
						adconitems[2] = bootbin;
						
						result = doMenu(adconitems, 3, 0, "Advanced configuration", 2);

						if (result != 0 && result != -1)
						{
							if (!configchanged)
							{
								configchanged = 1;
							}						
						}

						if(result == 0) 
						{ 
							printf(" > Back..."); 
							delay(1000000); 
							break;
						}

						if (result == 1)
						{
							config.umdactivatedplaincheck = !config.umdactivatedplaincheck;
							printf(" > Plain modules in UMD/ISO: %s", config.umdactivatedplaincheck ? "Enabled" : "Disabled");
							delay(1000000);
						}
						else if (result == 2)
						{							
							config.executebootbin = !config.executebootbin;
							printf(" > Execute BOOT.BIN in UMD/ISO: %s", config.executebootbin ? "Enabled" : "Disabled");
							delay(1000000);
						}											
					}
				}				
			}
		}
		else if(result == 1) 
		{
			oldselection = 0;

			while(1) 
			{
				char skip[64];				

				clearScreen();
				if (!configchanged)
				{
					SE_GetConfig(&config);
				}

				sprintf(skip, "Skip Sony logo (currently: %s)", config.skiplogo ? "Enabled" : "Disabled");
				sprintf(hide, "Hide corrupt icons (currently: %s)", config.hidecorrupt ? "Enabled" : "Disabled");
				sprintf(game, "Game folder homebrew (currently: %s)", config.gamekernel150 ? "1.50 Kernel" : "3.52 Kernel");
				sprintf(bootprog, "Autorun program at /PSP/GAME/BOOT/EBOOT.PBP (currently: %s)", config.startupprog ? "Enabled" : "Disabled");
				
				if (config.umdmode > MODE_NP9660)
				{
					config.umdmode = MODE_UMD;
				}
				
				sprintf(noumd, "UMD Mode (currently: %s)", umd_modes[config.umdmode]);
				
				if (config.fakeregion > 10)
					config.fakeregion = 0;
				
				sprintf(region, "Fake region (currently: %s)", regions[config.fakeregion]);
				sprintf(freereg, "Free UMD Region (currently: %s)", config.freeumdregion ? "Enabled" : "Disabled");
				sprintf(hbreset, "Hard Reset on homebrew (currently: %s)", config.hardresetHB ? "Enabled" : "Disabled");
				sprintf(vshmenu, "Use VshMenu (currently: %s)", config.novshmenu ? "Disabled" : "Enabled");
				
				strcpy(usbdev, "XMB Usb Device (currently: ");

				if (config.usbdevice <= 0 || config.usbdevice > 5)
				{
					strcat(usbdev, "Memory Stick)");

					if (config.usbdevice != 0)
					{
						config.usbdevice = 0;
						configchanged = 1;
					}
				}
				else if (config.usbdevice == 5)
				{
					strcat(usbdev, "UMD Disc)");
				}
				else
				{
					sprintf(usbdev+strlen(usbdev), "Flash %d)", config.usbdevice-1);
				}

				conitems[1] = skip;
				conitems[2] = hide;
				conitems[3] = game;
				conitems[4] = bootprog;
				conitems[5] = noumd;
				conitems[6] = region;
				conitems[7] = freereg;
				conitems[8] = hbreset;
				conitems[9] = vshmenu;
				conitems[10] = usbdev;

				result = doMenu(conitems, 11, oldselection, conmsg, 2);
				
				if (result != 0 && result != -1)
				{
					if (!configchanged)
					{
						configchanged = 1;
					}
				}

				if(result == 0) 
				{ 
					printf(" > Back..."); 
					delay(1000000); 
					break;
				}
				else if(result == 1) 
				{ 
					config.skiplogo = !config.skiplogo;
					printf(" > Skip SCE logo: %s", (config.skiplogo) ? "Enabled" : "Disabled");
					delay(1000000); 
				}
				else if(result == 2) 
				{ 
					config.hidecorrupt = !config.hidecorrupt;
					printf(" > Hide corrupt icons: %s", (config.hidecorrupt) ? "Enabled" : "Disabled"); 
					delay(1000000); 
				}
				else if (result == 3)
				{
					config.gamekernel150 = !config.gamekernel150;
					printf(" > Game folder homebrew: %s", (config.gamekernel150) ? "1.50 Kernel" : "3.52 Kernel"); 
					delay(1000000); 
				}
				else if (result == 4)
				{
					config.startupprog = !config.startupprog;
					printf(" > Autorun program at /PSP/GAME/BOOT/EBOOT.PBP: %s", (config.startupprog) ? "Enabled" : "Disabled"); 
					
					u8 *buffer;
					int size;
					SceUID fd; 

					sceKernelVolatileMemLock(0, &buffer, &size);
					fd = sceIoOpen("flash0:/kd/rtc.prx", PSP_O_RDONLY, 0777);
					size = sceIoRead(fd, buffer, size);
					sceIoClose(fd);

					if (config.startupprog)
					{
						_sw(0x10000003, buffer+0x60+0x804);
					}
					else
					{
						_sw(0, buffer+0x60+0x804);
					}

					fd = sceIoOpen("flash0:/kd/rtc.prx", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
					sceIoWrite(fd, buffer, size);
					sceIoClose(fd);

					sceKernelVolatileMemUnlock();
					
					delay(1000000); 
				}
				else if (result == 8)
				{
					config.hardresetHB = !config.hardresetHB;
					printf(" > Hard Reset on homebrew: %s", (config.hardresetHB) ? "Enabled" : "Disabled");
					delay(1000000);
				}
				else if (result == 7)
				{
					config.freeumdregion = !config.freeumdregion;
					printf(" > Free UMD Region: %s", (config.freeumdregion) ? "Enabled" : "Disabled");
					delay(1000000);
				}
				else if(result == 9) 
				{ 
					config.novshmenu = !config.novshmenu;
					printf(" > Use VshMenu: %s", !(config.novshmenu) ? "Enabled" : "Disabled");
					delay(1000000); 
				}

				if (result == 6)
				{
					config.fakeregion++;

					if (config.fakeregion == FAKE_REGION_KOREA)
						config.fakeregion = FAKE_REGION_AUSTRALIA;
					else if (config.fakeregion == FAKE_REGION_HONGKONG)
						config.fakeregion = FAKE_REGION_RUSSIA;
					else if (config.fakeregion == FAKE_REGION_CHINA)
						config.fakeregion = FAKE_REGION_DISABLED;

					oldselection = 6;

					delay(300000);
				}
				else if (result == 5)
				{
					if (config.umdmode == MODE_UMD)
						config.umdmode = MODE_MARCH33;
					else if (config.umdmode == MODE_MARCH33)
						config.umdmode = MODE_NP9660;
					else if (config.umdmode == MODE_NP9660)
						config.umdmode = MODE_OE_LEGACY;
					else
						config.umdmode = MODE_UMD;					

					printf(" > UMD Mode: %s", umd_modes[config.umdmode]);
					oldselection = 5;
					delay(550000);
				}
				
				else if (result == 10)
				{
					if (config.usbdevice == 5)
					{
						config.usbdevice = 0;						
					}
					else if (config.usbdevice == 4)
					{
						config.usbdevice++;						
					}
					else
					{
						config.usbdevice++;						
					}	
					
					oldselection = 10;
					delay(300000);
				}

				else if (result != -1)
				{
					oldselection = 0;
				}				
			}
		}

		else if (result == 2)
		{
			struct SceKernelLoadExecVSHParam param;

			pspSdkLoadStartModule("flash0:/kd/systemctrl.prx", PSP_MEMORY_PARTITION_KERNEL);

			memset(&param, 0, sizeof(param));
			
			param.size = sizeof(param);
			param.args = strlen(PROGRAM)+1;
			param.argp = PROGRAM;
			param.key = "updater";

			sceKernelLoadExecVSHMs1(PROGRAM, &param);
		}

		else if (result == 4)
		{
			oldselection = 0;
			
			while (1)
			{			
				clearScreen();

				if (!configchanged)
				{
					SE_GetConfig(&config);
				}

				if (!ValidCpu(config.vshcpuspeed))
				{
					config.vshcpuspeed = 0;
					config.vshbusspeed = 0;
					configchanged = 1;
				}

				else if (!ValidBus(config.vshbusspeed))
				{
					config.vshcpuspeed = 0;
					config.vshbusspeed = 0;
					configchanged = 1;
				}

				if (!ValidCpu(config.umdisocpuspeed))
				{
					config.umdisocpuspeed = 0;
					config.umdisobusspeed = 0;
					configchanged = 1;
				}

				else if (!ValidBus(config.umdisobusspeed))
				{
					config.umdisobusspeed = 0;
					config.umdisobusspeed = 0;
					configchanged = 1;
				}

				sprintf(vshspeed, "Speed in XMB (currently: %d)", config.vshcpuspeed);
				sprintf(umdspeed, "Speed in UMD/ISO (currently: %d)", config.umdisocpuspeed);
				
				if (config.vshcpuspeed == 0)
				{
					strcpy(vshspeed+strlen(vshspeed)-2, "Default)");
				}
				
				if (config.umdisocpuspeed == 0)
				{
					strcpy(umdspeed+strlen(umdspeed)-2, "Default)");
				}

				speeditems[1] = vshspeed;
				speeditems[2] = umdspeed;				

				result = doMenu(speeditems, 3, oldselection, "CPU Speed", 2);

				if (result != 0)
				{
					if (!configchanged)
						configchanged = 1;
				}

				if(result == 0)
				{
					printf(" > Back..."); 
					delay(1000000); 
					break; 
				}

				else if (result == 1)
				{
					if (config.vshcpuspeed == 0)
					{
						config.vshcpuspeed = 20;
						config.vshbusspeed = 10;
					}

					else if (config.vshcpuspeed == 20)
					{
						config.vshcpuspeed = 75;
						config.vshbusspeed = 37;
					}

					else if (config.vshcpuspeed == 75)
					{
						config.vshcpuspeed = 100;
						config.vshbusspeed = 50;
					}

					else if (config.vshcpuspeed == 100)
					{
						config.vshcpuspeed = 133;
						config.vshbusspeed = 66;
					}
					
					else if (config.vshcpuspeed == 133)
					{
						config.vshcpuspeed = 222;
						config.vshbusspeed = 111;
					}

					else if (config.vshcpuspeed  == 222)
					{
						config.vshcpuspeed = 266;
						config.vshbusspeed = 133;
					}
					else if (config.vshcpuspeed == 266)
					{
						config.vshcpuspeed = 300;
						config.vshbusspeed = 150;
					}
					else if (config.vshcpuspeed  == 300)
					{
						config.vshcpuspeed = 333;
						config.vshbusspeed = 166;
					}
					else if (config.vshcpuspeed  == 333)
					{
						config.vshcpuspeed = 0;
						config.vshbusspeed = 0;
					}

					sprintf(vshspeed, "Speed in XMB: %d", config.vshcpuspeed);
					if (config.vshcpuspeed == 0)
					{
						strcpy(vshspeed+strlen(vshspeed)-1, "Default");
					}
					
					oldselection = 1;
					printf("%s", vshspeed);
					delay(1000000);
				}
				
				else if (result == 2)
				{
					if (config.umdisocpuspeed == 0)
					{
						config.umdisocpuspeed = 20;
						config.umdisobusspeed = 10;
					}

					else if (config.umdisocpuspeed == 20)
					{
						config.umdisocpuspeed = 100;
						config.umdisobusspeed = 50;
					}

					else if (config.umdisocpuspeed == 100)
					{
						config.umdisocpuspeed = 222;
						config.umdisobusspeed = 111;
					}

					else if (config.umdisocpuspeed  == 222)
					{
						config.umdisocpuspeed = 266;
						config.umdisobusspeed = 133;
					}
					else if (config.umdisocpuspeed == 266)
					{
						config.umdisocpuspeed = 300;
						config.umdisobusspeed = 150;
					}
					else if (config.umdisocpuspeed  == 300)
					{
						config.umdisocpuspeed = 333;
						config.umdisobusspeed = 166;
					}
					else if (config.umdisocpuspeed  == 333)
					{
						config.umdisocpuspeed = 0;
						config.umdisobusspeed = 0;
					}

					sprintf(umdspeed, "Speed in UMD/ISO: %d", config.umdisocpuspeed);
					if (config.umdisocpuspeed == 0)
					{
						strcpy(umdspeed+strlen(umdspeed)-1, "Default");
					}
					
					oldselection = 2;
					printf("%s", umdspeed);					
					delay(1000000);
				}				
			}

		}

		if (result == 5)
		{
			
			while (1)
			{
				clearScreen();

				SceUID fd; 
				int i;
				char *p;

				ngame = 0;
				nvsh = 0;
				npops = 0;
				ngame150 = 0;
			
				memset(plugcon, 0, 20);

				fd = sceIoOpen("ms0:/seplugins/conf.bin", PSP_O_RDONLY, 0777);
				sceIoRead(fd, plugcon, 20);
				sceIoClose(fd);

				memset(plugins, 0, sizeof(plugins));

				fd= sceIoOpen("ms0:/seplugins/vsh.txt", PSP_O_RDONLY, 0777);
				if (fd >= 0)
				{
					for (i = 0; i < 5; i++)
					{
						if (ReadLine(fd, plugins[i+1]) > 0)
						{
							p = strrchr(plugins[i+1], '/');
							if (p)
							{
								strcpy(plugins[i+1], p+1);
							}
						
							strcat(plugins[i+1], " [VSH]");							
							nvsh++;	
						}
						else
						{
							break;
						}
					}
				
					sceIoClose(fd);
				}

				fd = sceIoOpen("ms0:/seplugins/game.txt", PSP_O_RDONLY, 0777);

				if (fd >= 0)
				{
					for (i = 0; i < 5; i++)
					{
						if (ReadLine(fd, plugins[i+nvsh+1]) > 0)
						{
							p = strrchr(plugins[i+nvsh+1], '/');
							if (p)
							{
								strcpy(plugins[i+nvsh+1], p+1);
							}
						
							strcat(plugins[i+nvsh+1], " [GAME]");
							ngame++;	
						}
						else
						{
							break;
						}
					}

					sceIoClose(fd);
				}
				
				fd = sceIoOpen("ms0:/seplugins/pops.txt", PSP_O_RDONLY, 0777);

				if (fd >= 0)
				{
					for (i = 0; i < 5; i++)
					{
						if (ReadLine(fd, plugins[i+nvsh+ngame+1]) > 0)
						{
							p = strrchr(plugins[i+nvsh+ngame+1], '/');
							if (p)
							{
								strcpy(plugins[i+nvsh+ngame+1], p+1);
							}
						
							strcat(plugins[i+nvsh+ngame+1], " [POPS]");
							npops++;	
						}
						else
						{
							break;
						}
					}

					sceIoClose(fd);
				}

				fd = sceIoOpen("ms0:/seplugins/game150.txt", PSP_O_RDONLY, 0777);

				if (fd >= 0)
				{
					for (i = 0; i < 5; i++)
					{
						if (ReadLine(fd, plugins[i+nvsh+ngame+npops+1]) > 0)
						{
							p = strrchr(plugins[i+nvsh+ngame+npops+1], '/');
							if (p)
							{
								strcpy(plugins[i+nvsh+ngame+npops+1], p+1);
							}
						
							strcat(plugins[i+nvsh+ngame+npops+1], " [GAME150]");
							ngame150++;	
						}
						else
						{
							break;
						}
					}

					sceIoClose(fd);
				}

				strcpy(plugins[0], "Back");

				for (i = 0; i < (ngame+nvsh+npops+ngame150+1); i++)
				{
					if (i != 0)
						strcat(plugins[i], (plugcon[i-1]) ? " (Enabled) " : " (Disabled)");
				
					plugins_p[i] = plugins[i];
				}

				result = doMenu(plugins_p, ngame+nvsh+npops+ngame150+1, 0, "Plugins", 2);

				if(result == 0) { printf(" > Back..."); delay(1000000); break; }
				else if (result != -1)
				{
					char str[256];

					strcpy(str, plugins_p[result]);
					str[strlen(str)-11] = 0;
					
					plugcon[result-1] = !plugcon[result-1];

					if (plugcon[result-1])
						strcat(str, ": Enabled");
					else
						strcat(str, ": Disabled");

					printf(str);
					SceUID fd = sceIoOpen("ms0:/seplugins/conf.bin", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);

					sceIoWrite(fd, plugcon, 20);
					sceIoClose(fd);
					delay(1000000);
				}
			}
		}

		else if (result == 6)
		{
			while (1)
			{
				unsigned int value = 0;

				clearScreen();

				strcpy(button, "Button assign (currently ");

				get_registry_value("/CONFIG/SYSTEM/XMB", "button_assign", &value); 

				if (value == 0)
				{
					strcat(button, "O is enter)");
				}
				else
				{
					strcat(button, "X is enter)");
				}

				regitems[1] = button;

				result = doMenu(regitems, 4, 0, "Registry Hacks", 2);

				if(result == 0) 
				{ 
					printf(" > Back..."); 
					delay(800000); 
					break;
				}
				else if (result == 1)
				{
					value = !value;
					set_registry_value("/CONFIG/SYSTEM/XMB", "button_assign", value); 
					
					strcpy(button, " > Button assign: ");
					if (value == 0)
					{
						strcat(button, "O is enter");
					}
					else
					{
						strcat(button, "X is enter");
					}

					printf("%s", button);
					
					delay(1000000);
				}
				else if (result == 2)
				{
					get_registry_value("/CONFIG/MUSIC", "wma_play", &value);
					
					if (value == 1)
					{
						printf("WMA was already activated.");
					}
					else
					{
						printf("Activating WMA...\n");
						set_registry_value("/CONFIG/MUSIC", "wma_play", 1);
					}

					delay(1000000);
				}
				else if (result == 3)
				{
					get_registry_value("/CONFIG/BROWSER", "flash_activated", &value);

					if (value == 1)
					{
						printf("Flash player was already activated.\n");
					}
					else
					{
						printf("Activating Flash Player...");
						set_registry_value("/CONFIG/BROWSER", "flash_activated", 1);
						set_registry_value("/CONFIG/BROWSER", "flash_play", 1);
					}

					delay(1000000);
				}				
			}
		}

		else if(result == 7) 
		{ 
			printf(" > Exiting recovery"); 
			delay(700000); 
			
			if (configchanged) 
				SE_SetConfig(&config); 			
			
			sceKernelExitVSHVSH(NULL); 
		}		
	}

	return 0;
}

