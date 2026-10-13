// Wengier: LFN support
#include "stdafx.h"

#include "vdos.h"
#include "dos_system.h"
#include "Shlwapi.h"
#include "dos_inc.h"
#include "support.h"
//#include <sys/stat.h>
//#include <time.h>
//#include <direct.h>
//#include <fcntl.h>
//#include <stdlib.h>
//#include <shellapi.h>
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

bool fini=true;
extern bool filter83;

static void makeShortName(const char *longname, char *out, int outlen, int upper = 1);
static bool isOemLetter(unsigned char c);
static void resolveShortPath(const char *base, const char *name, char *win_name, int outlen, const char *cwd = NULL);
static bool isHashShortName(const char *name);
static bool mapByGenerator(const char *dir, const char *req, char *out, int outlen);
static char cp866Up(char c);
static bool sameUp(const char *a, const char *b);
static void dosExtOf(const char *n, char *ext);
static void dosNameOf(const char *n, char *out);
static bool pathExistsOem(const char *oemPath);

bool WildFileCmp(const char * file, const char * wild) 
	{
	char file_name[9];
	char file_ext[4];
	char wild_name[9];
	char wild_ext[4];
	const char * find_ext;
	Bitu r;

	strcpy(wild_name, strcpy(file_name, "        "));
	strcpy(wild_ext, strcpy(file_ext, "   "));

	find_ext = strrchr(file, '.');
	if (find_ext)
		{
		Bitu size = (Bitu)(find_ext-file);
		if (size > 8)
			size = 8;
		memcpy(file_name, file, size);
		find_ext++;
		memcpy(file_ext, find_ext, (strlen(find_ext)>3) ? 3 : strlen(find_ext)); 
		}
	else
		memcpy(file_name, file, (strlen(file) > 8) ? 8 : strlen(file));
	upcase(file_name);
	upcase(file_ext);
	find_ext = strrchr(wild, '.');
	if (find_ext)
		{
		Bitu size = (Bitu)(find_ext-wild);
		if (size > 8)
			size = 8;
		memcpy(wild_name, wild,size);
		find_ext++;
		memcpy(wild_ext, find_ext, (strlen(find_ext)>3) ? 3 : strlen(find_ext));
		}
	else
		memcpy(wild_name, wild, (strlen(wild) > 8) ? 8 : strlen(wild));
	upcase(wild_name);
	upcase(wild_ext);
	// Names are right do some checking
	r = 0;
	while (r <8)
		{
		if (wild_name[r] == '*')
			break;
		if (wild_name[r] != '?' && wild_name[r] != file_name[r])
			return false;
		r++;
		}
    r = 0;
	while (r < 3)
		{
		if (wild_ext[r] == '*')
			return true;
		if (wild_ext[r] != '?' && wild_ext[r] != file_ext[r])
			return false;
		r++;
		}
	return true;
	}

DOS_Drive::DOS_Drive(const char* startdir, Bit8u driveNo, bool use)
	{
	curdir[0] = 0;
	label[0] = driveNo+'A';
	strcpy(label+1, "_DRIVE");
	SetBaseDir(startdir);
	autouse=use;
	while (Bit8u c = *startdir++)
		VolSerial = c + (VolSerial << 6) + (VolSerial << 16) - VolSerial;
	Mem_Stosb(dWord2Ptr(dos.tables.mediaid)+driveNo*2, 0xF8);						// Set the correct media byte in the table (harddisk)
	}

void DOS_Drive::SetBaseDir(const char* startdir)
	{
	strcpy(basedir, startdir);
	if (basedir[strlen(basedir)-1] != '\\')
		strcat(basedir, "\\");

	remote = true;
	if (startdir[0] != '\\')														// Assume \\... is remote
		{
		char rootDir[] = " :\\";
		rootDir[0] = startdir[0];
		if (GetDriveType(rootDir)!=DRIVE_REMOTE)
			remote = false;
		}
	vpLog("%c: => %s) %s", *label, remote ? "(Remote" : " (Local", basedir);
	}

bool DOS_Drive::FileCreate(DOS_File * * file, char * name, Bit16u attr)
	{
vpLog("FileCreate: START. basedir='%s', name='%s'", basedir, name); // <-- ДО
	char win_name[MAX_PATH_LEN];

	int attribs = FILE_ATTRIBUTE_NORMAL;
	if (attr&7)																		// Read-only (1), Hidden (2), System (4) are the same in DOS and Windows
		attribs = FILE_ATTRIBUTE_READONLY;
	if (*name&&name[strlen(name)-1]==13) name[strlen(name)-1]=0;
	if ((int)(strlen(basedir)+strlen(name)+strlen(curdir)) >= MAX_PATH_LEN) { DOS_SetError(DOSERR_PATH_NOT_FOUND); return false; }

resolveShortPath(basedir, name, win_name, MAX_PATH_LEN, curdir);
	wchar_t wwin_name[MAX_PATH_LEN];
	MultiByteToWideChar(CP_OEMCP, 0, win_name, -1, wwin_name, MAX_PATH_LEN);
	HANDLE handle = CreateFileW(wwin_name, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, attribs, NULL);
	if (handle == INVALID_HANDLE_VALUE)
		{
		Bit32u errorno = GetLastError();
		DOS_SetError((Bit16u)errorno);
		vpLog("File creation failed: %s\nErrorno: %d", win_name, errorno);
		return false;
		}
	*file = new Disk_File(name, handle);
	(*file)->flags = OPEN_READWRITE;
    vpLog("FileCreate: END. basedir='%s'", basedir); 
	return true;
	}


static bool pathExistsOem(const char *oemPath)
{
    if (!oemPath || !*oemPath) return false;
    wchar_t w[MAX_PATH_LEN];
    // Конвертируем из OEM (CP866) в WideChar для API Windows
    MultiByteToWideChar(CP_OEMCP, 0, oemPath, -1, w, MAX_PATH_LEN);
    return GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
}

//  имя из верхнего DOS-слоя УЖЕ содержит curdir первым сегментом
// (stable: curdir='VC', name='VC\VC.EXT' -> C:\VC\VC.EXT).
// Подклейка курдира второй раз даёт VDOSPLUSR\VDOSPLUSR -> несуществующий путь.
static const char *dosJoinCwd(const char *cwd, const char *name, char *buf, int buflen)
	{
	return name;
	}

bool DOS_Drive::FileOpen(DOS_File **file, char *name, Bit32u flags)
{
    // Очистка имени от возможного CR (13) в конце
    if (*name && name[strlen(name)-1] == 13) 
        name[strlen(name)-1] = 0;

    int ohFlag, shhFlag;

    // --- Логика флагов доступа (ОСТАВЛЕНО БЕЗ ИЗМЕНЕНИЙ) ---
    switch (flags & 0xf)
    {
        case OPEN_READ:
            ohFlag = GENERIC_READ;
            break;
        case OPEN_WRITE:
            ohFlag = GENERIC_WRITE;
            break;
        case OPEN_READWRITE:
            ohFlag = GENERIC_READ | GENERIC_WRITE;
            break;
        default:
            DOS_SetError(DOSERR_ACCESS_CODE_INVALID);
            return false;
    }

    switch (flags & 0x70)
    {
        case 0x10:
            shhFlag = 0;
            break;
        case 0x20:
            shhFlag = FILE_SHARE_READ;
            break;
        case 0x30:
            shhFlag = FILE_SHARE_WRITE;
            break;
        default:
            shhFlag = FILE_SHARE_READ | FILE_SHARE_WRITE;
    }

    char win_name[MAX_PATH_LEN];
    int len = strlen(name);

    // --- Логика редиректа системных файлов (ОСТАВЛЕНО БЕЗ ИЗМЕНЕНИЙ) ---
    if (!stricmp(name, "AUTOEXEC.BAT")) {
        strcpy(curauto ? win_name : strrchr(strcpy(win_name, _pgmptr), '\\') + 1, autoexec);
    }
    else if (!stricmp(name, "4DOS.HLP") || (len > 8 && !strnicmp(name + len - 9, "\\4DOS.HLP", 9))) {
        strcpy(strrchr(strcpy(win_name, _pgmptr), '\\'), "\\4DOS.HLP");
    }
    else if (fini && (!stricmp(name, "4DOS.INI") || (len > 8 && !strnicmp(name + len - 9, "\\4DOS.INI", 9)))) {
        fini = false;
        strcpy(strrchr(strcpy(win_name, _pgmptr), '\\'), "\\4DOS.INI");
    }
    else 
    {
        // --- НАЧАЛО ИЗМЕНЕННОГО БЛОКА (НОРМАЛИЗАЦИЯ ПУТИ) ---
	if ((int)(strlen(basedir)+strlen(name)+strlen(curdir)) >= MAX_PATH_LEN)
		{ DOS_SetError(DOSERR_PATH_NOT_FOUND); return false; }
	resolveShortPath(basedir, name, win_name, MAX_PATH_LEN, curdir);
	vpLog("FileOpen: win_name='%s'", win_name);
	}


    // --- Логика открытия файла (ОСТАВЛЕНО БЕЗ ИЗМЕНЕНИЙ) ---
    wchar_t wwin_name[MAX_PATH_LEN];
    MultiByteToWideChar(CP_OEMCP, 0, win_name, -1, wwin_name, MAX_PATH_LEN);
    
    HANDLE handle = CreateFileW(wwin_name, ohFlag, shhFlag, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    
    if (handle == INVALID_HANDLE_VALUE)
    {
        vpLog("FileOpen: FAILED win_name='%s', error=%d", win_name, GetLastError());
        DOS_SetError((Bit16u)GetLastError());
        return false;
    }

    *file = new Disk_File(name, handle);
    (*file)->flags = flags;
    
    return true;
}

bool DOS_Drive::SetFileAttr(char* name, Bit16u attr)
	{
	char win_name[MAX_PATH_LEN];
	if ((int)(strlen(basedir)+strlen(name)+strlen(curdir)) >= MAX_PATH_LEN) { DOS_SetError(DOSERR_PATH_NOT_FOUND); return false; }
	resolveShortPath(basedir, name, win_name, MAX_PATH_LEN, curdir);
		wchar_t wwin_name[MAX_PATH_LEN];
	MultiByteToWideChar(CP_OEMCP, 0, win_name, -1, wwin_name, MAX_PATH_LEN);
	if (!SetFileAttributesW(wwin_name, attr))
		{
		DOS_SetError((Bit16u)GetLastError());
		return false;
		}
	return true;
	}

bool DOS_Drive::GetFileAttr(char* name, Bit16u* attr)
	{
	char win_name[MAX_PATH_LEN];
if ((int)(strlen(basedir)+strlen(name)+strlen(curdir)) >= MAX_PATH_LEN) { DOS_SetError(DOSERR_PATH_NOT_FOUND); return false; }
	resolveShortPath(basedir, name, win_name, MAX_PATH_LEN, curdir);
	
		wchar_t wwin_name[MAX_PATH_LEN];
	MultiByteToWideChar(CP_OEMCP, 0, win_name, -1, wwin_name, MAX_PATH_LEN);
	Bit32u attribs = GetFileAttributesW(wwin_name);
	if (attribs == INVALID_FILE_ATTRIBUTES)
		{
		DOS_SetError((Bit16u)GetLastError());
		return false;
		}
	*attr = attribs&0x3f;															// Only return lower 6 bits
	return true;
	}


bool DOS_Drive::GetFileAttrEx(char* name, LPVOID fad)
	{
	char win_name[MAX_PATH_LEN];
if ((int)(strlen(basedir)+strlen(name)+strlen(curdir)) >= MAX_PATH_LEN) { DOS_SetError(DOSERR_PATH_NOT_FOUND); return false; }
	resolveShortPath(basedir, name, win_name, MAX_PATH_LEN, curdir);
	
	
	wchar_t wwin_name[MAX_PATH_LEN];
	MultiByteToWideChar(CP_OEMCP, 0, win_name, -1, wwin_name, MAX_PATH_LEN);
	if (!GetFileAttributesExW(wwin_name, GetFileExInfoStandard, fad))
		{
		DOS_SetError((Bit16u)GetLastError());
		return false;
		}
	return true;
	}

bool DOS_Drive::GetVolumeInfo(char* name, char* volume_label, DWORD *serial_number)
	{
	char win_name[MAX_PATH_LEN], Drive[] = "A:\\";
	if ((int)(strlen(basedir)+strlen(name)+strlen(curdir)) >= MAX_PATH_LEN) { DOS_SetError(DOSERR_PATH_NOT_FOUND); return false; }
	resolveShortPath(basedir, name, win_name, MAX_PATH_LEN);
	Drive[0]=*win_name;

	if (!GetVolumeInformation(Drive, volume_label, MAX_PATH, serial_number, NULL, NULL, NULL, 0))
		{
		DOS_SetError((Bit16u)GetLastError());
		return false;
		}
	return true;
	}

bool DOS_Drive::GetFreeDiskSpace(struct _diskfree_t* df)
	{
	char win_name[MAX_PATH_LEN];

	strcpy(win_name, basedir);
	return win_name[0]=='\\' ? NULL : _getdiskfree(toupper(win_name[0])-'A'+1, df) == 0;
	}

bool DOS_Drive::GetDiskFreeSpace32(char* name, DWORD* sectors_per_cluster, DWORD* bytes_per_sector, DWORD* free_clusters, DWORD* total_clusters)
	{
	char win_name[MAX_PATH_LEN];
	if ((int)(strlen(basedir)+strlen(name)+strlen(curdir)) >= MAX_PATH_LEN) return false;
	resolveShortPath(basedir, name, win_name, MAX_PATH_LEN, curdir);
	return GetDiskFreeSpace(win_name, sectors_per_cluster, bytes_per_sector, free_clusters, total_clusters)?true:false;
	}

DWORD DOS_Drive::GetCompressedSize(char* name)
	{
	char win_name[MAX_PATH_LEN];
if ((int)(strlen(basedir)+strlen(name)+strlen(curdir)) >= MAX_PATH_LEN) { DOS_SetError((Bit16u)ERROR_FILENAME_EXCED_RANGE); return (DWORD)-1; }
	resolveShortPath(basedir, name, win_name, MAX_PATH_LEN, curdir);
	
	wchar_t wwin_name[MAX_PATH_LEN];
	MultiByteToWideChar(CP_OEMCP, 0, win_name, -1, wwin_name, MAX_PATH_LEN);
	DWORD size = GetCompressedFileSizeW(wwin_name, NULL);
	if (size != INVALID_FILE_SIZE)
		{
		if (size != 0 && size == GetFileSize(win_name, NULL))
			{
			DWORD sectors_per_cluster, bytes_per_sector, free_clusters, total_clusters;
			if (GetDiskFreeSpace(win_name, &sectors_per_cluster, &bytes_per_sector, &free_clusters, &total_clusters))
				size = ((size - 1) | (sectors_per_cluster * bytes_per_sector - 1)) + 1;
			}
		return size;
		}
	else
		{
		DOS_SetError((Bit16u)GetLastError());
		return -1;
		}
	}

HANDLE DOS_Drive::CreateOpenFile(const char* name)
	{
	char win_name[MAX_PATH_LEN];
	if ((int)(strlen(basedir)+strlen(name)+strlen(curdir)) >= MAX_PATH_LEN) { DOS_SetError((Bit16u)GetLastError()); return INVALID_HANDLE_VALUE; }
	resolveShortPath(basedir, name, win_name, MAX_PATH_LEN, curdir);
	
	
	wchar_t wwin_name[MAX_PATH_LEN];
	MultiByteToWideChar(CP_OEMCP, 0, win_name, -1, wwin_name, MAX_PATH_LEN);
	HANDLE handle=CreateFileW(wwin_name, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (handle==INVALID_HANDLE_VALUE)
		DOS_SetError((Bit16u)GetLastError());
	return handle;
	}

bool DOS_Drive::FileExists(const char* name)
	{
	char win_name[MAX_PATH_LEN];
	if ((int)(strlen(basedir)+strlen(name)+strlen(curdir)) >= MAX_PATH_LEN) return false;
	resolveShortPath(basedir, name, win_name, MAX_PATH_LEN, curdir);

	wchar_t wwin_name[MAX_PATH_LEN];
	MultiByteToWideChar(CP_OEMCP, 0, win_name, -1, wwin_name, MAX_PATH_LEN);
	Bit32u attribs = GetFileAttributesW(wwin_name);
	return (attribs != INVALID_FILE_ATTRIBUTES && !(attribs&FILE_ATTRIBUTE_DIRECTORY));
	}


bool DOS_Drive::FileUnlink(char * name, bool wildcard)
	{
	char win_name[MAX_PATH_LEN];
	if ((int)(strlen(basedir)+strlen(name)+strlen(curdir)) >= MAX_PATH_LEN)
		{
		DOS_SetError(DOSERR_PATH_NOT_FOUND);
		return false;
		}
	resolveShortPath(basedir, name, win_name, MAX_PATH_LEN, curdir);

	wchar_t wwin_name[MAX_PATH_LEN];
	MultiByteToWideChar(CP_OEMCP, 0, win_name, -1, wwin_name, MAX_PATH_LEN);
DWORD attribs = GetFileAttributesW(wwin_name);
if (attribs != INVALID_FILE_ATTRIBUTES && (attribs & FILE_ATTRIBUTE_READONLY)) {
    SetFileAttributesW(wwin_name, attribs & ~FILE_ATTRIBUTE_READONLY);
}
	if (DeleteFileW(wwin_name))
		return true;
	vpLog("DeleteFile failed: %s, error: %d", win_name, GetLastError());
	if (wildcard)
		{
		SHFILEOPSTRUCTW op={0};
		op.wFunc = FO_DELETE;
		wwin_name[wcslen(wwin_name)+1]=0;
		op.pFrom = wwin_name;
		op.pTo = NULL;
		op.fFlags = FOF_FILESONLY | FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_NORECURSION;
		int err=SHFileOperationW(&op);
		if (err) DOS_SetError(err);
		return !err;
		}

	DOS_SetError((Bit16u)GetLastError());
	return false;
	}

#define maxWinSearches 512															// Should be ennough!
static struct {																		// To keep track of open Window searches
	PhysPt	dta_pt;																	// DTA address
	Bit16u	psp;																	// PSP segment of program initiating search
	int 	handle;																	// LFN search handle
	HANDLE	sHandle;																// Search handle
	bool	root;																	// Search is at root DOS drive (no "." or "..")
	bool	wildcard;																// Search has wildcard
	char	sfnPrefixes[256][7];													// Collision tracking: 6-char prefix + null
	int		sfnCounts[256];														// Collision tracking: count per prefix
	int		sfnCount;																// Collision tracking: number of prefixes tracked
} WinSearches[maxWinSearches];

static int openWinSearches = 0;														// Number of active Windows serach handles
static char srch_dir[MAX_PATH_LEN];													// Windows search directory

static void closeWinSearch(DOS_DTA &dta, int handle)
	{
	PhysPt dta_Addr = dta.GetPt();
	for (int i = 0; i < openWinSearches; i++)
		{
		if ((WinSearches[i].dta_pt == dta_Addr && WinSearches[i].handle == -1) || 
		(WinSearches[i].handle == handle && handle != -1))	// Match found
			{
			if (WinSearches[i].sHandle != INVALID_HANDLE_VALUE)						// If search started, close handle
				FindClose(WinSearches[i].sHandle);
			openWinSearches--;
			if (i < openWinSearches)												// Compact WinSearches down
				memmove(&WinSearches[i], &WinSearches[i+1], sizeof(WinSearches[1])*(openWinSearches-i));
			return;
			}
		}
	}

void closeWinSearchByPSP(Bit16u psp)
	{
	for (int i = 0; i < openWinSearches; i++)
		if (WinSearches[i].psp == psp)												// Match found
			{
			if (WinSearches[i].sHandle != INVALID_HANDLE_VALUE)						// If serach started, close handle
				FindClose(WinSearches[i].sHandle);
			openWinSearches--;
			if (i < openWinSearches)												// Compact WinSearches down
				memmove(&WinSearches[i], &WinSearches[i+1], sizeof(WinSearches[1])*(openWinSearches-i));
			return;
			}
	}



bool DOS_Drive::FindFirst(char* _dir, DOS_DTA & dta, int handle)
	{
	closeWinSearch(dta, handle);														// Close Windows search handle if entry for this dta
	if (!*_dir && curdir[0])
		{
		strncpy(_dir, curdir, MAX_PATH_LEN-1);
		_dir[MAX_PATH_LEN-1] = 0;
		}
	if (!TestDir(_dir))

		{
		if (uselfn) for (int i=0;i<(signed)strlen(_dir);i++) _dir[i]=tolower(_dir[i]);
		if (!uselfn||!TestDir(_dir))
			{
			DOS_SetError(DOSERR_PATH_NOT_FOUND);
			return false;
			}
		}

	if ((int)(strlen(basedir)+strlen(_dir)+strlen(curdir)+2) >= MAX_PATH_LEN) { DOS_SetError(DOSERR_PATH_NOT_FOUND); return false; }
resolveShortPath(basedir, _dir, srch_dir, MAX_PATH_LEN, curdir);
vpLog("FindFirst: srch_dir='%s'", srch_dir);
	if (srch_dir[strlen(srch_dir)-1] != '\\')
		strcat(srch_dir, "\\");
	
	Bit8u sAttr, rAttr;
	char srch_pattern[LFN_NAMELENGTH+1];
	dta.GetSearchParams(handle, sAttr, rAttr, srch_pattern);
	if (!strcmp(srch_pattern, "        .   ")) {										// Special complete empty, what about abc. ? (abc.*?)
		strcat(srch_dir, "*.*");
	}

	else
		{

		// Работаем напрямую в CP866 — srch_dir уже в OEM
		char clean_pattern[LFN_NAMELENGTH+1];
		char *pdot = strchr(srch_pattern, '.');
		if (pdot)
			{
			int nlen = pdot - srch_pattern;
			while (nlen > 0 && srch_pattern[nlen-1] == ' ') nlen--;
			int estart = (pdot - srch_pattern) + 1;
			int elen = strlen(srch_pattern) - estart;
			while (elen > 0 && srch_pattern[estart+elen-1] == ' ') elen--;
			int cl = 0;
			if (nlen > 0)
				{ memcpy(clean_pattern, srch_pattern, nlen); cl = nlen; }
			else
				clean_pattern[cl++] = '*';
			clean_pattern[cl++] = '.';
			if (elen > 0)
				{ memcpy(clean_pattern + cl, srch_pattern + estart, elen); cl += elen; }
			else
				clean_pattern[cl++] = '*';
			clean_pattern[cl] = 0;
			}
		else
			{
			int len = strlen(srch_pattern);
			while (len > 0 && srch_pattern[len-1] == ' ') len--;
			if (len == 0)
				strcpy(clean_pattern, "*.*");
			else
				{ memcpy(clean_pattern, srch_pattern, len); clean_pattern[len] = 0; }
			}

		int j = strlen(srch_dir), k=j;
 		for (int i = 0; i < (signed)strlen(clean_pattern); i++)
			if (clean_pattern[i] != 0)
				srch_dir[j++] = clean_pattern[i];
		srch_dir[j]=0;


		if (uselfn)
			{
			wchar_t wsrch_dir[MAX_PATH_LEN * 2];
			MultiByteToWideChar(CP_OEMCP, 0, srch_dir, -1, wsrch_dir, MAX_PATH_LEN * 2);
			if (!PathFileExistsW(wsrch_dir))
				{
				j=k;
				for (int i = 0; i < (signed)strlen(clean_pattern); i++)
					if (clean_pattern[i] != 0)
						srch_dir[j++] = tolower(clean_pattern[i]);
				MultiByteToWideChar(CP_OEMCP, 0, srch_dir, -1, wsrch_dir, MAX_PATH_LEN * 2);
				if (!PathFileExistsW(wsrch_dir))
					{
					j=k;
					for (int i = 0; i < (signed)strlen(clean_pattern); i++)
						if (clean_pattern[i] != 0)
							srch_dir[j++] = clean_pattern[i];
					}
				}
			}
  	}



	// Windows "finds" LPT1-9/COM1-9 which are never returned in a (DOS)DIR
	if (((!strnicmp(srch_pattern, "LPT", 3) || !strnicmp(srch_pattern, "COM", 3)) && (srch_pattern[3] > '0' && srch_pattern[3] <= '9' && srch_pattern[4] ==' '))
		|| ((!strnicmp(srch_pattern, "PRN", 3) || !strnicmp(srch_pattern, "AUX", 3)) && srch_pattern[3] ==' '))
		{
		DOS_SetError(DOSERR_NO_MORE_FILES);
		return false;
		}
	DOS_SetError(DOSERR_NONE);
	char Drive[] = "C:\\", Label[MAX_PATH], VolLabel[14];
	DWORD fill;
	Drive[0]=*basedir;
	if (GetVolumeInformation(Drive[0]=='\\'?basedir:Drive,Label,sizeof(Label),NULL,&fill,&fill,NULL,0))
		{
		strncpy(VolLabel,Label,13);
		VolLabel[13]=0;
		}
	else {
		VolLabel[0] = dta.GetSearchDrive(handle)+'A';
		strcpy(VolLabel+1, "_DRIVE");
		}
	if (sAttr&DOS_ATTR_VOLUME && !strlen(VolLabel) && !(sAttr&DOS_ATTR_DIRECTORY && sAttr&DOS_ATTR_ARCHIVE))
		{
		DOS_SetError(DOSERR_NO_MORE_FILES);
		return false;		
		}
	if ((sAttr&DOS_ATTR_VOLUME) && WildFileCmp(VolLabel, srch_pattern))
		{
		dta.SetResult(handle, VolLabel, VolLabel, 0, 0, 0, 0, 0, 0, 0, 0, DOS_ATTR_VOLUME);
		if (!(sAttr&DOS_ATTR_DIRECTORY && sAttr&DOS_ATTR_ARCHIVE))
			return true;
		}
	if (!(sAttr&(DOS_ATTR_VOLUME)))
	{
		// Remove
		int fLen = strlen(srch_dir);
		if (fLen > 11 && !stricmp("\\4HELP.EXE", srch_dir+fLen-10))					// Global check for "4help.exe" to start by 4DOS help function
			{
			dta.SetResult(handle, "4HELP.EXE", "4HELP.EXE", 0, 16384, 0, 0, 0, 0, 0, 0, DOS_ATTR_ARCHIVE);
			return true;
			}
		if ((fLen > 11 && !stricmp("\\COMMAND.*", srch_dir+fLen-10))
			|| (fLen > 13 && !stricmp("\\COMMAND.COM", srch_dir+fLen-12)))			// Global check for "COMMAND.COM"
			{
			dta.SetResult(handle, "COMMAND.COM", "COMMAND.COM", 0, 16384, 0, 0, 0, 0, 0, 0, DOS_ATTR_ARCHIVE);
			return true;
			}
		if ((fLen > 14 && !stricmp("\\AUTOEXEC.BAT", srch_dir+fLen-13)))
			{
			dta.SetResult(handle, "AUTOEXEC.BAT", "AUTOEXEC.BAT", 0, 16384, 0, 0, 0, 0, 0, 0, DOS_ATTR_ARCHIVE);
			return true;
			}
		}
	if (openWinSearches == maxWinSearches)											// We'll need a new Windows search handle
		E_Exit("Maximum number of Windows search handles exceeded");				// Shouldn't happen of course
	WinSearches[openWinSearches].dta_pt = dta.GetPt();
	WinSearches[openWinSearches].psp = dos.psp();
	WinSearches[openWinSearches].handle = handle;	
	WinSearches[openWinSearches].sHandle = INVALID_HANDLE_VALUE;
	WinSearches[openWinSearches].root = !strncmp(basedir, srch_dir, strrchr(srch_dir, '\\')-srch_dir);
	WinSearches[openWinSearches].wildcard = (strpbrk(srch_dir, "?*") != NULL);
	WinSearches[openWinSearches].sfnCount = 0;
	openWinSearches++;
	bool r=DoFindNext(handle);
	return r;
	}

bool isDosName(char* fName)															// Check for valid DOS filename, 8.3 and no forbidden characters
	{
	if (!strcmp(fName, ".") || !strcmp(fName, ".."))								// "." and ".." specials
		return true;
	if (strpbrk(fName, "+[] "))
		return false;
	char* pos = strchr(fName, '.');
	if (pos)																		// If extension
		{
		if (strlen(pos) > 4 || strchr(pos+1, '.') || pos - fName > 8)				// Max 3 chars,  max 1 "extension", max name = 8 chars
			return false;
		}
	else if (strlen(fName) > 8)														// Max name = 8 chars
		return false;
	return true;
	}



static void resolveShortPath(const char *base, const char *name_in, char *win_name, int outlen, const char *cwd)
{
	char joined[MAX_PATH_LEN];
	const char *name = dosJoinCwd(cwd, name_in, joined, MAX_PATH_LEN);
	const char *_origname = name;
// vpLog("resolveShortPath: base='%s', cwd='%s', name='%s'", base, cwd ? cwd : "", name);
	static bool codepageLogged = false;
	if (!codepageLogged) {
		vpLog("System codepages: ACP=%d, OEMCP=%d", GetACP(), GetOEMCP());
		codepageLogged = true;
	}
	strncpy(win_name, base, outlen - 1);
	win_name[outlen - 1] = 0;

	int blen = (int)strlen(win_name);
	if (blen > 0 && win_name[blen-1] != '\\')
		{ strcat(win_name, "\\"); blen++; }

	const char *p = name;
	while (*p == '\\') p++;

	while (*p)
		{
		char comp[MAX_PATH];
		const char *bs = strchr(p, '\\');
		int clen;
		if (bs)
			{
			clen = (int)(bs - p);
			if (clen >= MAX_PATH) clen = MAX_PATH - 1;
			memcpy(comp, p, clen);
			comp[clen] = 0;
			}
		else
			{
			clen = (int)strlen(p);
			if (clen >= MAX_PATH) clen = MAX_PATH - 1;
			memcpy(comp, p, clen);
			comp[clen] = 0;
			}

		
		// Clean 8.3 padding spaces from component
		if (strcmp(comp, ".") && strcmp(comp, ".."))
			{
			char *dot = strchr(comp, '.');
			if (dot)
				{
				int nlen = dot - comp;
				while (nlen > 0 && comp[nlen-1] == ' ') nlen--;
				int estart = (dot - comp) + 1;
				int elen = strlen(comp) - estart;
				while (elen > 0 && comp[estart+elen-1] == ' ') elen--;
				char tmp[MAX_PATH];
				int tl = 0;
				if (nlen > 0) { memcpy(tmp, comp, nlen); tl = nlen; }
				tmp[tl++] = '.';
				if (elen > 0) { memcpy(tmp + tl, comp + estart, elen); tl += elen; }
				tmp[tl] = 0;
				strcpy(comp, tmp);
				}
			else
				{
				int len = strlen(comp);
				while (len > 0 && comp[len-1] == ' ') len--;
				comp[len] = 0;
				}
			}


		// Fallback: if comp has underscores and direct path doesn't exist,
		// try replacing _ with . (DoFindNext replaces dots with _ in directory names)
		if (clen > 0 && strcmp(comp, ".") && strcmp(comp, "..") && !strpbrk(comp, "*?"))
			{
			char fprobe[MAX_PATH_LEN];
			snprintf(fprobe, MAX_PATH_LEN, "%s%s", win_name, comp);
			wchar_t wfprobe[MAX_PATH_LEN];
			MultiByteToWideChar(CP_OEMCP, 0, fprobe, -1, wfprobe, MAX_PATH_LEN);
			DWORD fa = GetFileAttributesW(wfprobe);
			if (fa == INVALID_FILE_ATTRIBUTES)
				{
				bool has_ = false;
				char altcomp[MAX_PATH_LEN];
				strncpy(altcomp, comp, MAX_PATH_LEN-1);
				altcomp[MAX_PATH_LEN-1] = 0;
				for (char *q = altcomp; *q; q++)
					if (*q == '_') { *q = '.'; has_ = true; }
				if (has_)
					{
					snprintf(fprobe, MAX_PATH_LEN, "%s%s", win_name, altcomp);
					MultiByteToWideChar(CP_OEMCP, 0, fprobe, -1, wfprobe, MAX_PATH_LEN);
					DWORD fa2 = GetFileAttributesW(wfprobe);
					vpLog("Fallback _->.: comp='%s' altcomp='%s' fa2=%d", comp, altcomp, fa2);
					if (fa2 != INVALID_FILE_ATTRIBUTES)
						strcpy(comp, altcomp);
					}
				}
//			else
//				vpLog("Fallback _->.: direct hit, comp='%s' fa=%d", comp, fa);
			}

		if (clen > 0 && !strchr(comp, '~') && !strpbrk(comp, "*?"))
		{
			// Если имя короткое и без тильды, пытаемся найти его в текущей папке win_name
			// Формируем шаблон поиска: путь + имя
			char searchPattern[MAX_PATH_LEN];
			snprintf(searchPattern, MAX_PATH_LEN, "%s%s", win_name, comp);
			
			// Конвертируем в ANSI и WideChar для API
			wchar_t wPattern[MAX_PATH_LEN];
			MultiByteToWideChar(CP_OEMCP, 0, searchPattern, -1, wPattern, MAX_PATH_LEN);

			WIN32_FIND_DATAW fd;
			// Ищем точное совпадение имени (регистр не важен для Windows, но важно для логики)
			// Мы ищем именно папку с таким именем.
			HANDLE h = FindFirstFileW(wPattern, &fd);
			
			if (h != INVALID_HANDLE_VALUE) 
			{
				// Нашли! Теперь берем РЕАЛЬНОЕ имя из Windows (fd.cFileName)
				// Это решит проблему регистра (DN640R -> dn640r)
				char realName[MAX_PATH];
				WideCharToMultiByte(CP_OEMCP, 0, fd.cFileName, -1, realName, MAX_PATH, NULL, NULL);

//				vpLog("resolveShortPath: Resolved short name '%s' -> '%s'", comp, realName);
				
				// Копируем реальное имя обратно в comp
				strcpy(comp, realName);
				
				FindClose(h);
			}
			else 
			{
//				vpLog("resolveShortPath: NOT FOUND: comp='%s', win_name='%s', searchPattern='%s'", comp, win_name, searchPattern);
			}
		}

		if (clen > 0 && strchr(comp, '~'))
			{
			char *tilde = strchr(comp, '~');
			char targetPrefix[7] = {0};
			int targetNum = 1;
			int prefixLen = tilde - comp;
			if (prefixLen > 6) prefixLen = 6;
			memcpy(targetPrefix, comp, prefixLen);
			targetPrefix[prefixLen] = 0;
			for (int i = 0; targetPrefix[i]; i++)
				{
				unsigned char c = (unsigned char)targetPrefix[i];
				if (c >= 'a' && c <= 'z') targetPrefix[i] = c - 32;
				else if (c >= 0xA0 && c <= 0xAF) targetPrefix[i] = c - 0x20;
				else if (c >= 0xE0 && c <= 0xEF) targetPrefix[i] = c - 0x50;
				else if (c == 0xF1) targetPrefix[i] = 0xF0;
				}
			targetNum = atoi(tilde + 1);
			if (targetNum < 1) targetNum = 1;

			char searchPath[MAX_PATH_LEN];
			snprintf(searchPath, MAX_PATH_LEN, "%s*", win_name);

			wchar_t wPath[MAX_PATH_LEN];
			MultiByteToWideChar(CP_OEMCP, 0, searchPath, -1, wPath, MAX_PATH_LEN);

			char entries[512][MAX_PATH];
			int nentries = 0;

			WIN32_FIND_DATAW fd;
			HANDLE h = FindFirstFileW(wPath, &fd);
			if (h != INVALID_HANDLE_VALUE)
				{
				do	{
					if (nentries < 512)
						{
						WideCharToMultiByte(CP_OEMCP, 0, fd.cFileName, -1,
							entries[nentries], MAX_PATH, NULL, NULL);
						nentries++;
						}
					} while (FindNextFileW(h, &fd));
				FindClose(h);
				}

			struct { char prefix[7]; int count; } colliders[256];
			int ncolliders = 0;

			for (int i = 0; i < nentries; i++)
				{
				if (!isDosName(entries[i]))
					{
					char gen[13];
					makeShortName(entries[i], gen, sizeof(gen));
					char *gt = strchr(gen, '~');
					if (!gt) continue;					// генератор не давал ~N — имя не наше

					char prefix[7] = {0};
					int plen = (int)(gt - gen);
					if (plen > 6) plen = 6;
					memcpy(prefix, gen, plen);
					prefix[plen] = 0;
						int idx = -1;
						for (int j = 0; j < ncolliders; j++)
							if (!strcmp(colliders[j].prefix, prefix))
								{ idx = j; break; }
						if (idx < 0 && ncolliders < 256)
							{
							idx = ncolliders++;
							strcpy(colliders[idx].prefix, prefix);
							colliders[idx].count = 0;
							}
						if (idx >= 0)
							{
							colliders[idx].count++;

							if (colliders[idx].count == targetNum && !strcmp(colliders[idx].prefix, targetPrefix))
								{ strcpy(comp, entries[i]); break; }
							}
						}
					}
				}
			



		char mapped[MAX_PATH_LEN]; mapped[0] = 0;
		if (clen > 0 && strcmp(comp, ".") && strcmp(comp, "..") && !strpbrk(comp, "*?"))
			{
			char probe[MAX_PATH_LEN];
			snprintf(probe, MAX_PATH_LEN, "%s%s", win_name, comp);
			wchar_t wprobe[MAX_PATH_LEN];
			MultiByteToWideChar(CP_OEMCP, 0, probe, -1, wprobe, MAX_PATH_LEN);
			if (GetFileAttributesW(wprobe) == INVALID_FILE_ATTRIBUTES)
				{
				mapByGenerator(win_name, comp, mapped, sizeof(mapped));
				}
			}

		char *app = mapped[0] ? mapped : comp;
		strncat(win_name, app, MAX_PATH_LEN - strlen(win_name) - 1);
		if (bs)
			{ strcat(win_name, "\\"); p = bs + 1; }
		else
			break;
		}



	// Log the result (still in CP866/OEM)
//	vpLog("resolveShortPath: result='%s' (OEM)", win_name);
// vpLog("resolveShortPath: result='%s'", win_name);
}


static bool isOemLetter(unsigned char c)
	{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		(c >= 0x80 && c <= 0x9F) || (c >= 0xA0 && c <= 0xAF) ||
		(c >= 0xE0 && c <= 0xEF) || c == 0xF0 || c == 0xF1;
	}


static bool isHashShortName(const char *name)
{
	const char *tilde = strchr(name, '~');
	if (!tilde) return false;
	int prefixLen = (int)(tilde - name);
	if (prefixLen < 4 || prefixLen > 6) return false;

	// 1. Старый случай: чистый hex-хэш (5296~1, 1DF0~1)
	bool isHexHash = true;
	for (int i = prefixLen - 4; i < prefixLen; i++)
	{
		unsigned char c = (unsigned char)name[i];
		if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
		{
			isHexHash = false;
			break;
		}
	}
	if (isHexHash) return true;

	// 2. Новый случай: мусорные префиксы от спецсимволов (--roya~1, ()-roy~1, &#039_~1)
	// Если первые 3 символа — не буквы (A-Z, a-z, кириллица), считаем это мусором
	int nonLetterCount = 0;
	for (int i = 0; i < 3 && i < prefixLen; i++)
	{
		unsigned char c = (unsigned char)name[i];
		bool isLetter = isOemLetter(c);
		if (!isLetter) nonLetterCount++;
	}
	if (nonLetterCount == 3) return true;

	return false;
}


static void makeShortName(const char *longname, char *out, int outlen, int upper)
{
	if (!strcmp(longname, ".") || !strcmp(longname, ".."))
	{
		strncpy(out, longname, outlen-1);
		out[outlen-1] = 0;
		return;
	}
	// Find extension, but ignore a leading dot (like ".gitignore")
	const char *dot = strrchr(longname, '.');
	if (dot == longname) dot = NULL;
	int namelen = dot ? (int)(dot - longname) : (int)strlen(longname);

	// Skip leading non-letter characters to find a meaningful start
	int start = 0;
	while (start < namelen)
	{
		unsigned char c = (unsigned char)longname[start];

		bool isLetter = isOemLetter(c);
		if (isLetter) break;
		start++;
	}

	// Если ни одной буквы не нашли, откатываемся к началу имени
	if (start >= namelen)
		start = 0;

	char name[9], ext[4];
	int availLen = namelen - start;
	if (availLen <= 8)
	{
		memcpy(name, longname + start, availLen);
		name[availLen] = 0;
	}
	else
	{
		memcpy(name, longname + start, 6);
		name[6] = '~';
		name[7] = '1';
		name[8] = 0;
	}
	// Точка в имени 8.3 недопустима — заменяем на _
	for (int i = 0; name[i]; i++)
		if (name[i] == '.')
			name[i] = '_';
	if (dot && dot[1])
	{
		int el = (int)strlen(dot+1);
		if (el > 3) el = 3;
		memcpy(ext, dot+1, el);
		ext[el] = 0;
	}
	else
		ext[0] = 0;
	if (ext[0])
		snprintf(out, outlen, "%s.%s", name, ext);
	else
	{
		strncpy(out, name, outlen);
		out[outlen-1] = 0;
	}
	for (int i = 0; out[i]; i++)
	{
		unsigned char c = (unsigned char)out[i];
		if (c==' '||c=='?'||c=='*'||c=='+'||c==','||c==';'||c=='='||c=='['||c==']'||c=='"'||c=='/'||c=='\\'||c=='|'||c==':'||c=='<'||c=='>')
		{
			out[i] = '_';
			continue;
		}
		if (!upper) continue;
	if (c >= 'a' && c <= 'z') out[i] = c - 32;
	else if (c >= 0xA0 && c <= 0xAF) out[i] = c - 0x20;   // а–п –> А–П
	else if (c >= 0xE0 && c <= 0xEF) out[i] = c - 0x50;   // р–я –> Р–Я
	else if (c == 0xF1) out[i] = 0xF0;                    // ё –> Ё
	}
}


static char cp866Up(char c)
	{
	unsigned char u = (unsigned char)c;
	if (u >= 'a' && u <= 'z')   return (char)(u - 32);
	if (u >= 0xA0 && u <= 0xAF) return (char)(u - 0x20);	// а–п > А–П
	if (u >= 0xE0 && u <= 0xEF) return (char)(u - 0x50);	// р–я > Р–Я
	if (u == 0xF1)              return (char)0xF0;		// ё > Ё
	return c;
	}

static bool sameUp(const char *a, const char *b)
	{
	while (*a && *b)
		if (cp866Up(*a++) != cp866Up(*b++)) return false;
	return *a == 0 && *b == 0;
	}

// Расширение из "ИМЯ~1.TXT": после точки, не больше 3 символов
static void dosExtOf(const char *n, char *ext)
	{
	const char *d = strrchr(n, '.');
	if (!d) { ext[0] = 0; return; }
	d++;
	int i = 0;
	while (d[i] && i < 3) { ext[i] = d[i]; i++; }
	ext[i] = 0;
	}

// Тело имени без расширения и без "~N"
static void dosNameOf(const char *n, char *out)
	{
	const char *stop = strchr(n, '~');
	if (!stop) stop = strrchr(n, '.');
	if (!stop) stop = n + strlen(n);
	int len = (int)(stop - n);
	if (len > 8) len = 8;
	memcpy(out, n, len); out[len] = 0;
	}

static bool mapByGenerator(const char *dir, const char *req, char *out, int outlen)
	{
	out[0] = 0;
	if (!req || !req[0] || strpbrk(req, "*?")) return false;

	char dirpart[MAX_PATH_LEN];
	strncpy(dirpart, dir, sizeof(dirpart)-1); dirpart[sizeof(dirpart)-1] = 0;
	int dl = (int)strlen(dirpart);
	if (dl > 0 && dirpart[dl-1] != '\\')
		{ dirpart[dl++] = '\\'; dirpart[dl] = 0; }

	char testPath[MAX_PATH_LEN];
	snprintf(testPath, sizeof(testPath), "%s*", dirpart);
	wchar_t wTest[MAX_PATH_LEN];
	MultiByteToWideChar(CP_OEMCP, 0, testPath, -1, wTest, MAX_PATH_LEN);

	char requested[MAX_PATH_LEN];
	strncpy(requested, req, sizeof(requested)-1); requested[sizeof(requested)-1] = 0;

	char reqName[10], reqExt[4];
	int reqNum = 1;
	dosExtOf(requested, reqExt);
	dosNameOf(requested, reqName);
	char *tilde = strchr(requested, '~');
	if (tilde)
		{
		int q = (int)(tilde - requested) + 1;
		reqNum = 0;
		while (requested[q] >= '0' && requested[q] <= '9') { reqNum = reqNum*10 + (requested[q]-'0'); q++; }
		if (reqNum < 1) reqNum = 1;
		}

	char cands[64][MAX_PATH];
	int ncand = 0, hit = -1;

	WIN32_FIND_DATAW tfd;
	HANDLE th = FindFirstFileW(wTest, &tfd);
	if (th == INVALID_HANDLE_VALUE) return false;

	for (;;)
		{
		if (wcscmp(tfd.cFileName, L".") && wcscmp(tfd.cFileName, L".."))
			{
			char entryName[MAX_PATH];
			entryName[0] = 0;
			WideCharToMultiByte(CP_OEMCP, 0, tfd.cFileName, -1, entryName, sizeof(entryName), NULL, NULL);
			if (entryName[0])
				{
				char g[13], gName[10], gExt[4];
				bool real = false;
				{
				char fullE[MAX_PATH_LEN];
				snprintf(fullE, sizeof(fullE), "%s%s", dirpart, entryName);
				wchar_t wFull[MAX_PATH_LEN], wShort[MAX_PATH];
				MultiByteToWideChar(CP_OEMCP, 0, fullE, -1, wFull, MAX_PATH_LEN);
				DWORD sl = GetShortPathNameW(wFull, wShort, MAX_PATH);
				if (sl > 0 && sl < MAX_PATH)
					{
					wchar_t *sf = wcsrchr(wShort, L'\\');
					if (sf) sf++; else sf = wShort;
					g[0] = 0;
					WideCharToMultiByte(CP_OEMCP, 0, sf, -1, g, sizeof(g), NULL, NULL);
					if (g[0] && isDosName((char *)g)) real = true;
					}
				}
				if (!real) makeShortName(entryName, g, sizeof(g));
				dosExtOf(g, gExt);
				dosNameOf(g, gName);
				if (sameUp(gExt, reqExt) && sameUp(gName, reqName)
					&& ncand < (int)(sizeof(cands)/sizeof(cands[0])))
					{
					strcpy(cands[ncand], entryName);
					if (hit < 0 && (real || !strcasecmp(g, requested))) hit = ncand;
					ncand++;
					}
				}
			}
		if (!FindNextFileW(th, &tfd)) break;
		}
	FindClose(th);

	if (hit < 0 && ncand >= reqNum) hit = reqNum - 1;

	if (hit >= 0 && (int)strlen(cands[hit]) < outlen)
		{
		strcpy(out, cands[hit]);
		vpLog("mapByGenerator: '%s' -> '%s' (cand %d of %d, ext '%s')", requested, out, reqNum, ncand, reqExt);
		return true;
		}
	vpLog("mapByGenerator: no mapping for '%s' (ext '%s', cands %d, num %d)", requested, reqExt, ncand, reqNum);
	return false;
	}

// Номер "~N" для конкретной записи: тот же алгоритм и тот же набор записей,
// что в цикле colliders[] внутри resolveShortPath
static void genShortNumber(const char *dirWithSlash, const char *entryOem, char *out, int outlen, int upper)
	{
	char testPath[MAX_PATH_LEN];
	snprintf(testPath, sizeof(testPath), "%s*", dirWithSlash);
	wchar_t wTest[MAX_PATH_LEN];
	MultiByteToWideChar(CP_OEMCP, 0, testPath, -1, wTest, MAX_PATH_LEN);

	struct { char prefix[7]; int count; } seen[256];
	int nseen = 0;
	int myNum = 1;
	bool found = false;

	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(wTest, &fd);
	if (h != INVALID_HANDLE_VALUE)
		{
		do	{
			char e[MAX_PATH]; e[0] = 0;
			WideCharToMultiByte(CP_OEMCP, 0, fd.cFileName, -1, e, sizeof(e), NULL, NULL);
			if (!e[0] || isDosName(e)) continue;			// тот же предикат, что в resolveShortPath
			char gen[13]; makeShortName(e, gen, sizeof(gen));
			char *gt = strchr(gen, '~');
			if (!gt) continue;
			char pfx[7] = {0};
			int pl = (int)(gt - gen); if (pl > 6) pl = 6;
			memcpy(pfx, gen, pl); pfx[pl] = 0;			// без upcase — как в resolveShortPath
			int idx = -1;
			for (int j = 0; j < nseen; j++) if (!strcmp(seen[j].prefix, pfx)) { idx = j; break; }
			if (idx < 0 && nseen < 256)
				{ idx = nseen++; strcpy(seen[idx].prefix, pfx); seen[idx].count = 0; }
			if (idx < 0) continue;
			seen[idx].count++;
			if (!strcmp(e, entryOem)) { myNum = seen[idx].count; found = true; break; }
			} while (FindNextFileW(h, &fd));
		FindClose(h);
		}


	char base[13]; makeShortName(entryOem, base, sizeof(base), upper);
	vpLog("genShortNumber: '%s' -> num %d (found=%d)", entryOem, myNum, found);

	if (!found) { out[0] = 0; return; }


	char *bt = strchr(base, '~');
	if (!bt) { out[0] = 0; return; }
	char num[4]; snprintf(num, sizeof(num), "%d", myNum);
	char *after = bt + 1; while (*after >= '0' && *after <= '9') after++;
	char reb[13]; int hl = (int)(bt - base) + 1;
	memcpy(reb, base, hl);
	if (hl + (int)strlen(num) + (int)strlen(after) < 13)
		{
		memcpy(reb + hl, num, strlen(num));
		strcpy(reb + hl + strlen(num), after);
		reb[12] = 0;
		strncpy(out, reb, outlen-1); out[outlen-1] = 0;
		return;
		}
	out[0] = 0;
	}

bool DoFindNext(int handle)
	{
	Bit8u srch_attr, rattr;
	char srch_pattern[LFN_NAMELENGTH+1];

	DOS_DTA dta(dos.dta());
	dta.GetSearchParams(handle, srch_attr, rattr, srch_pattern);
	srch_attr ^= (DOS_ATTR_VOLUME);
	WIN32_FIND_DATAW search_data;
	Bit8u find_attr;
	PhysPt dtaAddress = dta.GetPt();

	DOS_SetError(DOSERR_NO_MORE_FILES);
	int winSearchEntry = -1;
	for (int i = 0; i < openWinSearches; i++)
		if (WinSearches[i].dta_pt == dtaAddress && handle == -1 || WinSearches[i].handle == handle && handle != -1)
			winSearchEntry = i;
	if (handle == -1 && winSearchEntry == -1 && dtaAddress > 200000)
		for (int i = 0; i < openWinSearches; i++)
			if (WinSearches[i].dta_pt == dtaAddress - 4)
				winSearchEntry = i;
	if (winSearchEntry == -1)
		return false;

	if (WinSearches[winSearchEntry].sHandle == INVALID_HANDLE_VALUE)
		{

		// Resolve ~ in the search path before calling FindFirstFileW

		{
		char probeDir[MAX_PATH_LEN];
		strcpy(probeDir, srch_dir);
		char *ps = strrchr(probeDir, '\\');
		if (ps) *ps = 0;										// отрезаем маску: проверяем сам каталог
		bool need = (strchr(srch_dir, '~') != NULL);
		if (!need && probeDir[0])
			{
			wchar_t wneed[MAX_PATH_LEN];
			MultiByteToWideChar(CP_OEMCP, 0, probeDir, -1, wneed, MAX_PATH_LEN);
			need = (GetFileAttributesW(wneed) == INVALID_FILE_ATTRIBUTES);
			}
		if (need)
			{
			vpLog("DoFindNext: resolve needed, srch_dir='%s'", srch_dir);
			char root[4] = {0};
			int rootLen = 0;
			if (srch_dir[1] == ':')
				{ root[0] = srch_dir[0]; root[1] = ':'; root[2] = '\\'; root[3] = 0; rootLen = 3; }
			else if (srch_dir[0] == '\\')
				{ root[0] = '\\'; root[1] = 0; rootLen = 1; }

			char inPath[MAX_PATH_LEN];
			strncpy(inPath, srch_dir, MAX_PATH_LEN-1);
			inPath[MAX_PATH_LEN-1] = 0;
			char *rest = inPath + rootLen;
			while (*rest == '\\') rest++;
			if (*rest)
				resolveShortPath(root, rest, srch_dir, MAX_PATH_LEN);
			vpLog("DoFindNext: AFTER resolve srch_dir='%s'", srch_dir);
			}
		}

	wchar_t wsrch_dir[MAX_PATH_LEN];
	MultiByteToWideChar(CP_OEMCP, 0, srch_dir, -1, wsrch_dir, MAX_PATH_LEN);
vpLog("DoFindNext: FindFirstFileW srch_dir='%s'", srch_dir);
	if ((WinSearches[winSearchEntry].sHandle = FindFirstFileW(wsrch_dir, &search_data)) == INVALID_HANDLE_VALUE)
		{
		// Fallback: try replacing _ with . in the pattern part (directories with dots shown as _ in listings)
		char *last_bs = strrchr(srch_dir, '\\');
		if (last_bs && !strpbrk(last_bs + 1, "*?"))
			{
			bool has_ = false;
			char alt_srchn[MAX_PATH_LEN];
			strncpy(alt_srchn, srch_dir, MAX_PATH_LEN-1);
			alt_srchn[MAX_PATH_LEN-1] = 0;
			char *alt_bs = strrchr(alt_srchn, '\\');
			if (alt_bs)
				for (char *q = alt_bs + 1; *q; q++)
					if (*q == '_') { *q = '.'; has_ = true; }
			if (has_)
				{
				wchar_t walt_srchn[MAX_PATH_LEN];
				MultiByteToWideChar(CP_OEMCP, 0, alt_srchn, -1, walt_srchn, MAX_PATH_LEN);
				WinSearches[winSearchEntry].sHandle = FindFirstFileW(walt_srchn, &search_data);
				vpLog("DoFindNext: Fallback _->. '%s' success=%d", alt_srchn, WinSearches[winSearchEntry].sHandle != INVALID_HANDLE_VALUE);
				}
			}
		}
	if (WinSearches[winSearchEntry].sHandle == INVALID_HANDLE_VALUE)
		{
	vpLog("DoFindNext: FindFirstFileW FAILED, error=%d", GetLastError());
		closeWinSearch(dta, handle);

		DOS_SetError((Bit16u)GetLastError());
		if (dos.errorcode == DOSERR_FILE_NOT_FOUND)
			DOS_SetError(DOSERR_NO_MORE_FILES);
		return false;
		}
		}
	else if (!FindNextFileW(WinSearches[winSearchEntry].sHandle, &search_data))
		{
		closeWinSearch(dta, handle);
		return false;
		}


	do
		{
		if (search_data.cFileName[0] == '.' && WinSearches[winSearchEntry].root)
			continue;
		bool sfn = false;

		char orgname_buf[MAX_PATH], shortname_buf[13];
		WideCharToMultiByte(CP_OEMCP, 0, search_data.cFileName, -1,
			orgname_buf, MAX_PATH, NULL, NULL);

		char *orgname = orgname_buf;
		char *shortname = shortname_buf;

		if (!isDosName(orgname))
			{
			bool useMakeShort = true;

			char altName[14];
			altName[0] = 0;
			WideCharToMultiByte(CP_OEMCP, 0, search_data.cAlternateFileName, -1, altName, sizeof(altName), NULL, NULL);
			vpLog("DEBUG: Long='%s', WinAlt='%s'", orgname, altName);

			if (altName[0] && isDosName(altName) && !isHashShortName(altName))
				{

				unsigned char firstChar = (unsigned char)altName[0];
				bool firstIsLetter = isOemLetter(firstChar);

				bool isGoodName = false;
				if (firstIsLetter)
					{
					int letterCount = 0;
					for (int i = 0; i < 3 && altName[i] && altName[i] != '~'; i++)
						{
						unsigned char c = (unsigned char)altName[i];
						bool isLetter = isOemLetter(c);
						if (isLetter) letterCount++;
						}
					if (letterCount >= 2)
						isGoodName = true;
					}

				if (isGoodName)
					{
					strncpy(shortname_buf, altName, 12);
					shortname_buf[12] = 0;
					useMakeShort = false;
					}
				}

			if (useMakeShort)
				{
				char dirPart[MAX_PATH_LEN];
				strncpy(dirPart, srch_dir, MAX_PATH_LEN-1);
				dirPart[MAX_PATH_LEN-1] = 0;
				char *lastSlash = strrchr(dirPart, '\\');
				if (lastSlash)
					{
					*lastSlash = 0;
					if (strlen(dirPart) == 2 && dirPart[1] == ':')
						{ dirPart[2] = '\\'; dirPart[3] = 0; }

					wchar_t wDirPart[MAX_PATH];
					MultiByteToWideChar(CP_OEMCP, 0, dirPart, -1, wDirPart, MAX_PATH);
					wchar_t wFullPath[MAX_PATH];
					_snwprintf(wFullPath, MAX_PATH, L"%s\\%s", wDirPart, search_data.cFileName);
					wchar_t wShortPath[MAX_PATH];

					DWORD shortLen = GetShortPathNameW(wFullPath, wShortPath, MAX_PATH);
					char dbgFullPath[MAX_PATH];
					WideCharToMultiByte(CP_OEMCP, 0, wFullPath, -1, dbgFullPath, MAX_PATH, NULL, NULL);
					vpLog("DEBUG: GetShortPathNameW called: path='%s', shortLen=%d", dbgFullPath, shortLen);
					if (shortLen > 0 && shortLen < MAX_PATH)
						{
						wchar_t *shortFile = wcsrchr(wShortPath, L'\\');
						if (shortFile) shortFile++; else shortFile = wShortPath;
						char oemShort[14];
						WideCharToMultiByte(CP_OEMCP, 0, shortFile, -1, oemShort, sizeof(oemShort), NULL, NULL);
						vpLog("DEBUG: GetShortPathNameW result: oemShort='%s', orgname='%s', isDos=%d", oemShort, orgname, isDosName(oemShort));

						if (*oemShort && isDosName(oemShort) && stricmp(oemShort, orgname) != 0
							&& !isHashShortName(oemShort))
							{
							unsigned char firstChar = (unsigned char)oemShort[0];
							bool firstIsLetter = isOemLetter(firstChar);		
							bool isGoodName = false;
							if (firstIsLetter)
								{
								int letterCount2 = 0;
								for (int i = 0; i < 3 && oemShort[i] && oemShort[i] != '~'; i++)
									{
									unsigned char c = (unsigned char)oemShort[i];
									bool isLetter = isOemLetter(c);
									if (isLetter) letterCount2++;
									}
								if (letterCount2 >= 2)
									isGoodName = true;
								}

							if (isGoodName)
								{
								strncpy(shortname_buf, oemShort, 12);
								shortname_buf[12] = 0;
								useMakeShort = false;
								}
							}
						}
					}
				}

			if (useMakeShort)
				{
				char dirOem[MAX_PATH_LEN];
				strncpy(dirOem, srch_dir, MAX_PATH_LEN-1); dirOem[MAX_PATH_LEN-1] = 0;
				char *ds = strrchr(dirOem, '\\');
				if (ds) *ds = 0;
				if (strlen(dirOem) == 2 && dirOem[1] == ':') { dirOem[2] = '\\'; dirOem[3] = 0; }
				if (dirOem[0] && dirOem[strlen(dirOem)-1] != '\\')
					{ strncat(dirOem, "\\", MAX_PATH_LEN-strlen(dirOem)-1); }


				char genNum[13];
				genShortNumber(dirOem, orgname, genNum, sizeof(genNum), 0);
				if (genNum[0] && strchr(genNum, '~') && (int)strlen(genNum) < 13)
					strcpy(shortname_buf, genNum);
				else
					makeShortName(orgname, shortname_buf, 13, 0);
				}
			}
		else
			{
			strncpy(shortname_buf, orgname, 12);
			shortname_buf[12] = 0;
			}



		find_attr = search_data.dwFileAttributes&0xff;
		if (uselfn && fflfn && handle > -1 && rattr && (find_attr&rattr) != rattr)
			continue;
 		if (~srch_attr & find_attr & DOS_ATTR_DIRECTORY)
			continue;
 		if (~srch_attr & find_attr & (DOS_ATTR_HIDDEN | DOS_ATTR_SYSTEM) && !(srch_attr & find_attr & DOS_ATTR_DIRECTORY && strchr(srch_pattern, '*') == NULL && strchr(srch_pattern, '?') == NULL))
			continue;


		char find_name[13], lfind_name_buf[MAX_PATH];
		Bit16u find_date,find_time,find_adate,find_atime,find_cdate,find_ctime;
		Bit32u find_hsize, find_size;

		memset(find_name, 0, 13);
		strncpy(find_name, sfn ? srch_pattern : shortname, 12);
		find_name[12] = 0;



		if (find_attr & DOS_ATTR_DIRECTORY)
		for (int i = 0; find_name[i]; i++) {
			unsigned char c = (unsigned char)find_name[i];
			if (c >= 'a' && c <= 'z') find_name[i] = c - 32;
			else if (c >= 0x80 && c <= 0x9F) ; // уже заглавные
			else if (c >= 0xA0 && c <= 0xAF) find_name[i] = c - 0x20;
			else if (c >= 0xE0 && c <= 0xEF) find_name[i] = c - 0x50;
			else if (c == 0xF1) find_name[i] = 0xF0;
		}

		strcpy(lfind_name_buf, orgname);
		char *lfind_name = lfind_name_buf;

		// Replace dots with underscores in directory long names for display
		if (find_attr & DOS_ATTR_DIRECTORY && strcmp(lfind_name, ".") && strcmp(lfind_name, ".."))
			for (char *p = lfind_name; *p; p++)
				if (*p == '.')
					*p = '_';

		find_hsize = search_data.nFileSizeHigh;
		find_size = search_data.nFileSizeLow;
		FILETIME fTime;
		FileTimeToLocalFileTime(&search_data.ftLastWriteTime, &fTime);
		FileTimeToDosDateTime(&fTime, &find_date, &find_time);
		FileTimeToLocalFileTime(&search_data.ftLastAccessTime, &fTime);
		FileTimeToDosDateTime(&fTime, &find_adate, &find_atime);
		FileTimeToLocalFileTime(&search_data.ftCreationTime, &fTime);
		FileTimeToDosDateTime(&fTime, &find_cdate, &find_ctime);

		dta.SetResult(handle, find_name, lfind_name, find_hsize, find_size, find_date, find_time, find_adate, find_atime, find_cdate, find_ctime, find_attr);

		if (!WinSearches[winSearchEntry].wildcard)
			closeWinSearch(dta, handle);
		DOS_SetError(DOSERR_NONE);
		return true;
		}
	while (FindNextFileW(WinSearches[winSearchEntry].sHandle, &search_data));
	closeWinSearch(dta, handle);
	return false;
	}




bool DOS_Drive::MakeDir(char * dir)
	{
	char win_dir[MAX_PATH_LEN];
	if ((int)(strlen(basedir)+strlen(dir)+strlen(curdir)) >= MAX_PATH_LEN) { DOS_SetError(DOSERR_PATH_NOT_FOUND); return false; }
	resolveShortPath(basedir, dir, win_dir, MAX_PATH_LEN, curdir);
	

	wchar_t wwin_dir[MAX_PATH_LEN];
	MultiByteToWideChar(CP_OEMCP, 0, win_dir, -1, wwin_dir, MAX_PATH_LEN);
	if (CreateDirectoryW(wwin_dir, NULL))
		return true;
	Bit16u error=(Bit16u)GetLastError();
	DOS_SetError(error==ERROR_ALREADY_EXISTS?DOSERR_ACCESS_DENIED:error);
	return false;
	}

bool DOS_Drive::RemoveDir(char * dir)
	{
	char win_dir[MAX_PATH_LEN];

	if ((int)(strlen(basedir)+strlen(dir)+strlen(curdir)) >= MAX_PATH_LEN) { DOS_SetError(DOSERR_PATH_NOT_FOUND); return false; }
	resolveShortPath(basedir, dir, win_dir, MAX_PATH_LEN, curdir);

	wchar_t wwin_dir[MAX_PATH_LEN];
	MultiByteToWideChar(CP_OEMCP, 0, win_dir, -1, wwin_dir, MAX_PATH_LEN);
	if (RemoveDirectoryW(wwin_dir))
		return true;
	Bit16u error=(Bit16u)GetLastError();
	DOS_SetError((error==ERROR_DIRECTORY||error==ERROR_DIR_NOT_EMPTY)?DOSERR_ACCESS_DENIED:error);
	return false;
	}

bool DOS_Drive::Rename(char* oldname, char* newname)
	{
	char winold[MAX_PATH_LEN];
	char winnew[MAX_PATH_LEN];
	if ((int)(strlen(basedir)+strlen(oldname)+strlen(curdir)) >= MAX_PATH_LEN || (int)(strlen(basedir)+strlen(newname)+strlen(curdir)) >= MAX_PATH_LEN)
		{ DOS_SetError(DOSERR_PATH_NOT_FOUND); return false; }

	resolveShortPath(basedir, oldname, winold, MAX_PATH_LEN, curdir);
	resolveShortPath(basedir, newname, winnew, MAX_PATH_LEN, curdir);


	wchar_t wwinold[MAX_PATH_LEN], wwinnew[MAX_PATH_LEN];
	MultiByteToWideChar(CP_OEMCP, 0, winold, -1, wwinold, MAX_PATH_LEN);
	MultiByteToWideChar(CP_OEMCP, 0, winnew, -1, wwinnew, MAX_PATH_LEN);
	if (MoveFileW(wwinold, wwinnew))
		return true;
	Bit16u error = (Bit16u)GetLastError();
	if (error == ERROR_ALREADY_EXISTS)												// Not kwnown by DOS
		error = DOSERR_ACCESS_DENIED;
	DOS_SetError(error);
	return false;
	}



bool DOS_Drive::TestDir(const char* dir)
	{
	char win_dir[MAX_PATH_LEN];
	if ((int)(strlen(basedir)+strlen(dir)+strlen(curdir)+2) >= MAX_PATH_LEN) { DOS_SetError(DOSERR_PATH_NOT_FOUND); return false; }
	resolveShortPath(basedir, dir, win_dir, MAX_PATH_LEN, curdir);
	if (win_dir[strlen(win_dir)-1] != '\\')
		strcat(win_dir, "\\");
	wchar_t wdir[MAX_PATH_LEN];
	MultiByteToWideChar(CP_OEMCP, 0, win_dir, -1, wdir, MAX_PATH_LEN);
	DWORD attr = GetFileAttributesW(wdir);
	vpLog("TestDir: dir='%s' win_dir='%s' attr=%d", dir, win_dir, attr);
	vpLog("TestDir: dir='%s' win_dir='%s' attr=%d", dir, win_dir, attr);
	if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
		return true;
	DOS_SetError((Bit16u)GetLastError());
	return false;
	}

bool Disk_File::Read(Bit8u* data, Bit16u* size)
	{
	if ((flags&0xf) == OPEN_WRITE)													// Check if file opened in write-only mode
		{
		DOS_SetError(DOSERR_ACCESS_DENIED);
		return false;
		}
	Bit32u bytesRead;
	for (int tries = 3; tries; tries--)												// Try three times
		{
		if (ReadFile(hFile,static_cast<LPVOID>(data), (Bit32u)*size, &bytesRead, NULL))
			{
			*size = (Bit16u)bytesRead;
			if (bytesRead)															// Only if something is indeed read, skip the Idle function
				idleSkip = true;
			return true;
			}
		Sleep(25);																	// If failed (should be region locked), wait 25 millisecs
		}
	DOS_SetError((Bit16u)GetLastError());
	*size = 0;																		// Is this OK ??
	return false;
	}

bool Disk_File::Write(Bit8u* data, Bit16u* size)
	{
	if ((flags&0xf) == OPEN_READ)													// Check if file opened in read-only mode
		{
		DOS_SetError(DOSERR_ACCESS_DENIED);
		return false;
		}
	if (*size == 0)
		if (SetEndOfFile(hFile))
			return true;
		else
			{
			DOS_SetError((Bit16u)GetLastError());
			return false;
			}
	Bit32u bytesWritten;
	for (int tries = 3; tries; tries--)												// Try three times
		{
		if (WriteFile(hFile, data, (Bit32u)*size, &bytesWritten, NULL))
			{
			*size = (Bit16u)bytesWritten;
			idleSkip = true;
			return true;
			}
		Sleep(25);																	// If failed (should be region locked? (not documented in MSDN)), wait 25 millisecs
		}
	DOS_SetError((Bit16u)GetLastError());
	*size = 0;																		// Is this OK ??
	return false;
	}

bool Disk_File::LockFile(Bit8u mode, Bit32u pos, Bit32u size)
	{
	if (mode > 1)
		{
		DOS_SetError(DOSERR_FUNCTION_NUMBER_INVALID);
		return false;
		}
	BOOL bRet = false;
	//auto lockFunct = mode == 0 ? ::LockFile : ::UnlockFile;
	for (int tries = 3; tries; tries--)												// Try three times
		{
		if ((bRet = mode == 0 ? ::LockFile(hFile, pos, 0, size, 0) : UnlockFile(hFile, pos, 0, size, 0)))
			return true;
		Sleep(25);																	// If failed, wait 25 millisecs
		}
	DOS_SetError((Bit16u)GetLastError());
	return false;
	}

bool Disk_File::Seek(Bit32u* pos, Bit32u type)
	{
	if (type > 2)
		{
		DOS_SetError(DOSERR_FUNCTION_NUMBER_INVALID);
		return false;
		}
	Bit32s dwPtr = SetFilePointer(hFile, *pos, NULL, type);
	if (dwPtr != INVALID_SET_FILE_POINTER)											// If succes
		{
		*pos = (Bit32u)dwPtr;
		return true;
		}
	DOS_SetError((Bit16u)GetLastError());
	return false;
	}

void Disk_File::Close()
	{
	if (refCtr == 1)																// Only close if one reference left
		CloseHandle(hFile);
	}

Bit16u Disk_File::GetInformation(void)
	{
	return 0;
	}

Disk_File::Disk_File(const char* _name, HANDLE handle)
	{
	hFile = handle;
	UpdateDateTimeFromHost();

	attr = DOS_ATTR_ARCHIVE;

	name = 0;
	SetName(_name);
	}

void Disk_File::UpdateDateTimeFromHost()
	{
    FILETIME ftWrite, ftAccess, ftCreate;
	SYSTEMTIME stUTC, stLocal;
	atime = adate = 1;
	ctime = cdate = 1;
	time = date = 1;
	if (GetFileTime(hFile, &ftCreate, &ftAccess, &ftWrite))
		{
	    FileTimeToSystemTime(&ftCreate, &stUTC);									// Convert the creation time to local time
		SystemTimeToTzSpecificLocalTime(NULL, &stUTC, &stLocal);
		SystemTimeToFileTime(&stLocal, &ftCreate);
		FileTimeToDosDateTime(&ftCreate, &cdate, &ctime);
	    FileTimeToSystemTime(&ftAccess, &stUTC);									// Convert the access time to local time
		SystemTimeToTzSpecificLocalTime(NULL, &stUTC, &stLocal);
		SystemTimeToFileTime(&stLocal, &ftAccess);
		FileTimeToDosDateTime(&ftAccess, &adate, &atime);
	    FileTimeToSystemTime(&ftWrite, &stUTC);										// Convert the last-write time to local time
		SystemTimeToTzSpecificLocalTime(NULL, &stUTC, &stLocal);
		SystemTimeToFileTime(&stLocal, &ftWrite);
		FileTimeToDosDateTime(&ftWrite, &date, &time);
		}
	return;
	}

#if (_MSC_VER == 1500)
	#define LL2FILETIME( ll, pft )\
		(pft)->dwLowDateTime = (UINT)(ll); \
		(pft)->dwHighDateTime = (UINT)((ll) >> 32);
	#define FILETIME2LL( pft, ll) \
		ll = (((LONGLONG)((pft)->dwHighDateTime))<<32) + (pft)-> dwLowDateTime ;

	static const int MonthLengths[2][12] =
	{
		{ 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
		{ 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
	};

	static inline BOOL IsLeapYear(int Year)
	{
		return Year % 4 == 0 && (Year % 100 != 0 || Year % 400 == 0);
	}

	static int TIME_DayLightCompareDate( const SYSTEMTIME *date,
		const SYSTEMTIME *compareDate )
	{
		int limit_day, dayinsecs;

		if (date->wMonth < compareDate->wMonth)
			return -1; // We are in a month before the date limit.

		if (date->wMonth > compareDate->wMonth)
			return 1; // We are in a month after the date limit.

		// if year is 0 then date is in day-of-week format, otherwise
		// it's absolute date.
		if (compareDate->wYear == 0)
		{
			WORD First;
			// compareDate->wDay is interpreted as number of the week in the month
			// 5 means: the last week in the month
			int weekofmonth = compareDate->wDay;
			  // calculate the day of the first DayOfWeek in the month
			First = ( 6 + compareDate->wDayOfWeek - date->wDayOfWeek + date->wDay
				   ) % 7 + 1;
			limit_day = First + 7 * (weekofmonth - 1);
			// check needed for the 5th weekday of the month
			if(limit_day > MonthLengths[date->wMonth==2 && IsLeapYear(date->wYear)]
					[date->wMonth - 1])
				limit_day -= 7;
		}
		else
		{
		   limit_day = compareDate->wDay;
		}

		// convert to seconds
		limit_day = ((limit_day * 24  + compareDate->wHour) * 60 +
				compareDate->wMinute ) * 60;
		dayinsecs = ((date->wDay * 24  + date->wHour) * 60 +
				date->wMinute ) * 60 + date->wSecond;
		// and compare
		return dayinsecs < limit_day ? -1 :
			   dayinsecs > limit_day ? 1 :
			   0;   // date is equal to the date limit.
	}

	static DWORD TIME_CompTimeZoneID ( const TIME_ZONE_INFORMATION *pTZinfo,
		FILETIME *lpFileTime, BOOL islocal )
	{
		int ret, year;
		BOOL beforeStandardDate, afterDaylightDate;
		DWORD retval = TIME_ZONE_ID_INVALID;
		LONGLONG llTime = 0; // initialized to prevent gcc complaining
		SYSTEMTIME SysTime;
		FILETIME ftTemp;

		if (pTZinfo->DaylightDate.wMonth != 0)
		{
			// if year is 0 then date is in day-of-week format, otherwise
			// it's absolute date.
			if (pTZinfo->StandardDate.wMonth == 0 ||
				(pTZinfo->StandardDate.wYear == 0 &&
				(pTZinfo->StandardDate.wDay<1 ||
				pTZinfo->StandardDate.wDay>5 ||
				pTZinfo->DaylightDate.wDay<1 ||
				pTZinfo->DaylightDate.wDay>5)))
			{
				SetLastError(ERROR_INVALID_PARAMETER);
				return TIME_ZONE_ID_INVALID;
			}

			if (!islocal) {
				FILETIME2LL( lpFileTime, llTime );
				llTime -= pTZinfo->Bias * (LONGLONG)600000000;
				LL2FILETIME( llTime, &ftTemp)
				lpFileTime = &ftTemp;
			}

			FileTimeToSystemTime(lpFileTime, &SysTime);
			year = SysTime.wYear;

			if (!islocal) {
				llTime -= pTZinfo->DaylightBias * (LONGLONG)600000000;
				LL2FILETIME( llTime, &ftTemp)
				FileTimeToSystemTime(lpFileTime, &SysTime);
			}

			// check for daylight savings
			if(year == SysTime.wYear) {
				ret = TIME_DayLightCompareDate( &SysTime, &pTZinfo->StandardDate);
				if (ret == -2)
					return TIME_ZONE_ID_INVALID;

				beforeStandardDate = ret < 0;
			} else
				beforeStandardDate = SysTime.wYear < year;

			if (!islocal) {
				llTime -= ( pTZinfo->StandardBias - pTZinfo->DaylightBias )
					* (LONGLONG)600000000;
				LL2FILETIME( llTime, &ftTemp)
				FileTimeToSystemTime(lpFileTime, &SysTime);
			}

			if(year == SysTime.wYear) {
				ret = TIME_DayLightCompareDate( &SysTime, &pTZinfo->DaylightDate);
				if (ret == -2)
					return TIME_ZONE_ID_INVALID;

				afterDaylightDate = ret >= 0;
			} else
				afterDaylightDate = SysTime.wYear > year;

			retval = TIME_ZONE_ID_STANDARD;
			if( pTZinfo->DaylightDate.wMonth <  pTZinfo->StandardDate.wMonth ) {
				// Northern hemisphere
				if( beforeStandardDate && afterDaylightDate )
					retval = TIME_ZONE_ID_DAYLIGHT;
			} else    // Down south
				if( beforeStandardDate || afterDaylightDate )
				retval = TIME_ZONE_ID_DAYLIGHT;
		} else
			// No transition date
			retval = TIME_ZONE_ID_UNKNOWN;

		return retval;
	}

	static BOOL TIME_GetTimezoneBias( const TIME_ZONE_INFORMATION *pTZinfo,
		FILETIME *lpFileTime, BOOL islocal, LONG *pBias )
	{
		LONG bias = pTZinfo->Bias;
		DWORD tzid = TIME_CompTimeZoneID( pTZinfo, lpFileTime, islocal);

		if( tzid == TIME_ZONE_ID_INVALID)
			return FALSE;
		if (tzid == TIME_ZONE_ID_DAYLIGHT)
			bias += pTZinfo->DaylightBias;
		else if (tzid == TIME_ZONE_ID_STANDARD)
			bias += pTZinfo->StandardBias;
		*pBias = bias;
		return TRUE;
	}

	BOOL WINAPI TzSpecificLocalTimeToSystemTime(
		const TIME_ZONE_INFORMATION *lpTimeZoneInformation,
		LPSYSTEMTIME lpLocalTime, LPSYSTEMTIME lpUniversalTime)
	{
		FILETIME ft;
		LONG lBias;
		LONGLONG t;
		TIME_ZONE_INFORMATION tzinfo;

		if (lpTimeZoneInformation != NULL)
		{
			tzinfo = *lpTimeZoneInformation;
		}
		else
		{
			if (GetTimeZoneInformation(&tzinfo) == TIME_ZONE_ID_INVALID)
				return FALSE;
		}

		if (!SystemTimeToFileTime(lpLocalTime, &ft))
			return FALSE;
		FILETIME2LL( &ft, t)
		if (!TIME_GetTimezoneBias(&tzinfo, &ft, TRUE, &lBias))
			return FALSE;
		// convert minutes to 100-nanoseconds-ticks
		t += (LONGLONG)lBias * 600000000;
		LL2FILETIME( t, &ft)
		return FileTimeToSystemTime(&ft, lpUniversalTime);
	}
#endif

bool Disk_File::UpdateHostDateTime(Bit16u ntime, Bit16u ndate, int type)
	{
	FILETIME ftTime;
	SYSTEMTIME stUTC, stLocal;
	DosDateTimeToFileTime(ndate, ntime, &ftTime);
	FileTimeToSystemTime(&ftTime, &stLocal);
	TzSpecificLocalTimeToSystemTime(NULL, &stLocal, &stUTC);
	SystemTimeToFileTime(&stUTC, &ftTime);
	return SetFileTime(hFile, type==7?&ftTime:NULL, type==5?&ftTime:NULL, type==1?&ftTime:NULL)?true:false;
	}
