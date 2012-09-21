
#include <ntddk.h>
#include <ntdddisk.h>
#include <windef.h>
#include <stdio.h>
#include <stdarg.h>
#include <ntddvol.h>
#include "ntifs.h"
#include "GenericTable.h"

#include "fatlbr.h"

#include "diskfltlib.h"

#include "ntimage.h"
#include "mempool/mempool.h"
#include "diskflt.h"
#include "md5.h"
#include "notify.h"

#define RTL_CONSTANT_STRING(s) { sizeof( s ) - sizeof( (s)[0] ), sizeof( s ), s}
#define	__free_Safe(_buffer)	{if (_buffer)__free(_buffer);}

#define dprintf	if (DBG) DbgPrint


typedef struct {
	
    LARGE_INTEGER StartingLcn;
    LARGE_INTEGER BitmapSize;
    BYTE  Buffer[1];
	
} VOLUME_BITMAP_BUFFER, *PVOLUME_BITMAP_BUFFER;

typedef struct {
	
    LARGE_INTEGER StartingLcn;
	
} STARTING_LCN_INPUT_BUFFER, *PSTARTING_LCN_INPUT_BUFFER;

#ifndef _countof
#define _countof(_Array) (sizeof(_Array) / sizeof(_Array[0]))
#endif

typedef struct _PAIR
{
	ULONGLONG	orgIndex;	// ԭʼ�ص�ַ
	ULONGLONG	mapIndex;		// �ض����ĵ�ַ
} PAIR, *PPAIR;


typedef struct _FILTER_DEVICE_EXTENSION
{
	// �Ƿ��ڱ���״̬
	BOOL					Protect;
	//������ϵı���ϵͳʹ�õ��������
	LIST_ENTRY				list_head;
	//������ϵı���ϵͳʹ�õ�������е���
	KSPIN_LOCK				list_lock;
	//������ϵı���ϵͳʹ�õ�������е�ͬ���¼�
	KEVENT					ReqEvent;
	//������ϵı���ϵͳʹ�õ�������еĴ����߳�֮�߳̾��
	PVOID					thread_read_write;
	CLIENT_ID				thread_read_write_id;

	// �����߳�
//	PVOID					thread_reclaim;	
//	CLIENT_ID				thread_reclaim_id;
	//ͳʹ�õ�������еĴ����߳�֮������־
	BOOLEAN					terminate_thread;
} FILTER_DEVICE_EXTENSION, *PFILTER_DEVICE_EXTENSION;


typedef struct _DP_BITMAP_
{
	ULONG		bitMapSize;
	// ÿ����������λ
    ULONG		regionSize;
	// ÿ����ռ����byte
	ULONG		regionBytes;
	// ���bitmap�ܹ��ж��ٸ���
    ULONG		regionNumber;
	// ָ��bitmap�洢�ռ��ָ��
    UCHAR **	buffer; 
} DP_BITMAP, * PDP_BITMAP;


typedef struct _VOLUME_INFO
{
	BOOLEAN		isValid;			// �Ƿ���Ч
	BOOLEAN		isProtect;			// �Ƿ񱣻��þ�
//	BOOLEAN		isDiskFull;			// �����Ƿ��Ѿ�����

	WCHAR		volume;				// ���

	ULONG		diskNumber;			// �˾����ڵ�Ӳ�̺�

	DWORD		partitionNumber;	// ��������
	BYTE		partitionType;		// ��������
	BOOLEAN		bootIndicator;		// �Ƿ���������

	LONGLONG	physicalStartingOffset;		// �����ڴ������ƫ��Ҳ���ǿ�ʼ��ַ

	LONGLONG	bytesTotal;			// �������ܴ�С����byteΪ��λ
	ULONG		bytesPerSector;		// ÿ�������Ĵ�С
	ULONG		bytesPerCluster;	// ÿ�ش�С
	ULONGLONG	firstDataSector;	// ��һ�������Ŀ�ʼ��ַ��Ҳָλͼ�ϵ�һ���صĿ�ʼ��ַ,NTFS�̶�Ϊ0,FATר��

	
	//������豸��Ӧ�Ĺ����豸���²��豸����
	PDEVICE_OBJECT	LowerDevObj;

	// �˾��߼����ж��ٸ���
	ULONGLONG		sectorCount;
	
	// ��ǿ��д� ���д�bitΪ0, ��ʼ����ʱ����bitMap_OR
	PDP_BITMAP		bitMap_Free;
	// ��Ǵ��Ƿ��ض���
	PDP_BITMAP		bitMap_Redirect;
	// ֱ�ӷŹ���д�Ĵ�(force write)����pagefile.sys hiberfil.sys, λͼ���Ǵ����߼�λͼ��һС����
	PDP_BITMAP		bitMap_Protect;
	
	// �ϴ�ɨ��Ŀ��дص�λ��
	ULONGLONG		last_scan_index;
	
	// ���ض����
	RTL_GENERIC_TABLE	redirectMap;

} VOLUME_INFO, *PVOLUME_INFO;


PROTECT_INFO	_protectInfo = {MAGIC_CHAR, 0, 0, 0};
// ���ȫ����Ϣ
VOLUME_INFO		_volumeList[MAX_DOS_DRIVES];

// Ӳ���²��豸������Ϣ
PDEVICE_OBJECT	_lowerDeviceObject[MAX_DOS_DRIVES];

PFILTER_DEVICE_EXTENSION	_deviceExtension = NULL;

ULONG	_processNameOfffset = 0;
ULONG	_systemProcessId = 0;

// �ܾ���������
BOOL	_sysPatchEnable = FALSE;


// �����˽��̿��Դ�͸diskflt.sys�޸�����
ULONG	_lockProcessId = -1;

// λͼһ���������2M
#define SLOT_SIZE	(1024 * 1024 * 2)

void DPBitMap_Free(DP_BITMAP * bitmap)
{
	//�ͷ�bitmap
	DWORD i = 0;
	
	if (NULL != bitmap)
	{
		if (NULL != bitmap->buffer)
		{
			for (i = 0; i < bitmap->regionNumber; i++)
			{
				if (NULL != bitmap->buffer[i])
				{
					//����ײ�Ŀ鿪ʼ�ͷţ����п鶼��ѯһ��				
					__free(bitmap->buffer[i]);
				}
			}
			//�ͷſ��ָ��
			__free(bitmap->buffer);
		}	
		//�ͷ�bitmap����
		__free(bitmap);
	}
}

NTSTATUS
DPBitMap_Create(
	DP_BITMAP ** bitmap,	// λͼ���ָ��
	ULONGLONG bitMapSize,	// λͼ�ж��ٸ���λ
	ULONGLONG regionBytes	// λͼ���ȣ��ֳ�N�飬һ��ռ����byte
	)	
{
	NTSTATUS	status = STATUS_UNSUCCESSFUL;
	int		i = 0;
	DP_BITMAP *	myBitmap = NULL;

	//������������ʹ���˴���Ĳ������·��������ȴ���
	if (NULL == bitmap || 0 == regionBytes  || 0 == bitMapSize)
	{
		return status;
	}
	__try
	{
		*bitmap = NULL;
		//����һ��bitmap�ṹ������������ζ�Ҫ����ģ�����ṹ�൱��һ��bitmap��handle	
		if (NULL == (myBitmap = (DP_BITMAP *)__malloc(sizeof(DP_BITMAP))))
		{
			__leave;
		}
		
		//��սṹ
		memset(myBitmap, 0, sizeof(DP_BITMAP));

		myBitmap->regionSize = regionBytes * 8;
		if (myBitmap->regionSize > bitMapSize)
		{
			myBitmap->regionSize = bitMapSize / 2;
		}
		//���ݲ����Խṹ�еĳ�Ա���и�ֵ
		myBitmap->bitMapSize = bitMapSize;
		myBitmap->regionBytes = (myBitmap->regionSize / 8) + sizeof(int);

		myBitmap->regionNumber = bitMapSize / myBitmap->regionSize;
		if (bitMapSize % myBitmap->regionSize)
		{
			myBitmap->regionNumber++;
		}

		//�����regionNumber��ô���ָ��region��ָ�룬����һ��ָ������
		if (NULL == (myBitmap->buffer = (UCHAR **)__malloc(sizeof(UCHAR *) * myBitmap->regionNumber)))
		{
			__leave;
		}
		//���ָ������
		memset(myBitmap->buffer, 0, sizeof(UCHAR *) * myBitmap->regionNumber);
		*bitmap = myBitmap;
		status = STATUS_SUCCESS;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		status = GetExceptionCode();
	}
	if (!NT_SUCCESS(status))
	{
		if (NULL != myBitmap)
		{
			DPBitMap_Free(myBitmap);
		}
		*bitmap = NULL;
	}
	return status;
}


ULONGLONG
DPBitMap_FindNext(DP_BITMAP * bitMap, ULONGLONG startIndex, BOOL set)
{
	LONG	jmpValue = set ? 0 : 0xFFFFFFFF;
	ULONG	slot = 0;
	
	// ����slot
	for (slot = startIndex / bitMap->regionSize; slot < bitMap->regionNumber; slot++)
	{
		ULONGLONG	max = 0;
		
		// ��û�з���
		if (!bitMap->buffer[slot])
		{
			if (set)
			{
				startIndex = (slot + 1) * bitMap->regionSize;
				continue;
			}
			else
			{
				return startIndex;
			}
		}
		
		for (max = min((slot + 1) * bitMap->regionSize, bitMap->bitMapSize); 
		startIndex < max; )
		{
			ULONG	sIndex = startIndex % bitMap->regionSize;

			// ������һ����λ������

			if (jmpValue == ((PULONG)bitMap->buffer[slot])[sIndex / 32])
			{
				// ������Խ
				startIndex += 32 - (sIndex % 32);
				continue;
			}
			
			if (set == ((((PULONG)bitMap->buffer[slot])[sIndex / 32] & (1 << (sIndex % 32))) > 0))
			{
				// �ҵ�
				return startIndex;
			}	
			startIndex++;
		}
	}
	
	return -1;
}

NTSTATUS
DPBitMap_Set(DP_BITMAP * bitMap, ULONGLONG index, BOOL set)
{
	ULONG	slot = index / bitMap->regionSize;
	if (slot > (bitMap->regionNumber-1))
	{
		dprintf("DPBitMap_Set out of range slot %d\n", slot);
		return STATUS_UNSUCCESSFUL;
	}

	if (!bitMap->buffer[slot])
	{
		if (!set)
		{
			return STATUS_SUCCESS;
		}
		bitMap->buffer[slot] = (UCHAR *)__malloc(bitMap->regionBytes);
		if (!bitMap->buffer[slot])
		{
			return STATUS_INSUFFICIENT_RESOURCES;
		}
		memset(bitMap->buffer[slot], 0, bitMap->regionBytes);
	}
	
	index %= bitMap->regionSize;
	
    if (set)
        ((ULONG *)bitMap->buffer[slot])[index / 32] |= (1 << (index % 32));
    else
        ((ULONG *)bitMap->buffer[slot])[index / 32] &= ~(1 << (index % 32));

	return STATUS_SUCCESS;
}

BOOL
DPBitMap_Test(DP_BITMAP * bitMap, ULONGLONG index)
{
	ULONG	slot = index / bitMap->regionSize;
	if (slot > (bitMap->regionNumber-1))
	{
		dprintf("DPBitMap_Test out of range slot %d\n", slot);
		return FALSE;
	}
	// ��û����
	if (!bitMap->buffer[slot])
	{
		return FALSE;
	}

	index %= bitMap->regionSize;	

	return (((ULONG *)bitMap->buffer[slot])[index / 32] & (1 << (index % 32)) ? TRUE : FALSE);
}

NTSTATUS
ksleep(ULONG microSecond)
{
	LARGE_INTEGER	timeout = RtlConvertLongToLargeInteger(-10000 * microSecond);
	KeDelayExecutionThread(KernelMode, FALSE, &timeout);
	return STATUS_SUCCESS;
}

/*
void cls()
{
	UCHAR SpareColor = 4;   // blue
	UCHAR BackColor = 3;    // green
	UCHAR TextColor = 15;   // white

	if (InbvIsBootDriverInstalled())
	{
		InbvAcquireDisplayOwnership();

		InbvResetDisplay();

		// c:\boot.ini ������������ϼ����� /noguiboot, �Ͳ�����ʾ����load����
		// ������ӡ��������������Ҳ������windows��logoЧ��

		//InbvSolidColorFill(0, 0, 639, 479, SpareColor);         // blue, 640x480

		InbvSetTextColor(TextColor);

	//	InbvInstallDisplayStringFilter(NULL);
		InbvEnableDisplayString(TRUE);
	}
}

ULONG
kprintf(const char *fmt, ...) 
{
	va_list args;
	int ret;
	char buff[1024];

	va_start(args, fmt);
	ret = _vsnprintf(buff, sizeof(buff), fmt, args);
	va_end(args);

	InbvDisplayString(buff);

	return ret;
}

*/
PVOID getFileClusterList(HANDLE hFile)
{
	
	NTSTATUS status;
	IO_STATUS_BLOCK iosb;
	LARGE_INTEGER StartVcn;
	PRETRIEVAL_POINTERS_BUFFER pVcnPairs;
	ULONG ulOutPutSize = 0;
	ULONG uCounts = 200;
	
	StartVcn.QuadPart=0;
	ulOutPutSize = sizeof(RETRIEVAL_POINTERS_BUFFER) + uCounts* sizeof(pVcnPairs->Extents)+sizeof(LARGE_INTEGER);
	pVcnPairs = (RETRIEVAL_POINTERS_BUFFER *)__malloc(ulOutPutSize);
	if(pVcnPairs == NULL)
	{
		return NULL;
	}
	
	while( (status = ZwFsControlFile( hFile,NULL, NULL, 0, &iosb,
		FSCTL_GET_RETRIEVAL_POINTERS,
		&StartVcn, sizeof(LARGE_INTEGER),
		pVcnPairs, ulOutPutSize ) ) == STATUS_BUFFER_OVERFLOW)
	{
		uCounts+=200;
		ulOutPutSize = sizeof(RETRIEVAL_POINTERS_BUFFER) + uCounts* sizeof(pVcnPairs->Extents)+sizeof(LARGE_INTEGER);
		__free(pVcnPairs);
		
		pVcnPairs = (RETRIEVAL_POINTERS_BUFFER *)__malloc(ulOutPutSize);
		if(pVcnPairs == NULL)
		{
			dprintf("__malloc %d bytes faild", ulOutPutSize);
			return FALSE;
		}
	}
	
	if(!NT_SUCCESS(status))
	{
		dprintf(" --ZwFsControlFile --->> FSCTL_GET_RETRIEVAL_POINTERS  failed");
		dprintf(" --status %x",status);
		__free(pVcnPairs);
		return NULL;
	}
	
	return pVcnPairs;
}



//--------------------------------------------------------------------------------------
PVOID GetSysInf(SYSTEM_INFORMATION_CLASS InfoClass)
{    
    NTSTATUS ns;
    ULONG RetSize, Size = 0x100;
    PVOID Info;
	
    while (1) 
    {    
        if ((Info = __malloc(Size)) == NULL) 
        {
            dprintf("__malloc() fails\n");
            return NULL;
        }
		
        ns = ZwQuerySystemInformation(InfoClass, Info, Size, &RetSize);
        if (ns == STATUS_INFO_LENGTH_MISMATCH)
        {       
            __free(Info);
            Size += 0x100;
        }
        else
		{
            break;    
		}
    }
	
    if (!NT_SUCCESS(ns))
    {
        dprintf("ZwQuerySystemInformation() fails; status: 0x%.8x\n", ns);
		
        if (Info)
		{
            __free(Info);
		}
		
        return NULL;
    }
	
    return Info;
}


HANDLE searchFileHandle(PUNICODE_STRING fileName)
{
	NTSTATUS status;
	ULONG i,sysBuffer =0;
	PSYSTEM_HANDLE_INFORMATION pProcesses;
	POBJECT_NAME_INFORMATION ObjectName;
	
	char ObjectNameBuf[1024];
	ULONG ReturnLen;
	HANDLE hPageFile ;
	
	ObjectName = (POBJECT_NAME_INFORMATION)ObjectNameBuf;
	ObjectName->Name.MaximumLength = 510;
	
	sysBuffer = (ULONG)GetSysInf(SystemHandleInformation);

	if(sysBuffer == NULL)
	{
		dprintf("DiskGetHandleList error\n");
		return (HANDLE)-1;
	}

	pProcesses = (PSYSTEM_HANDLE_INFORMATION)(sysBuffer + 4);
	for (i=0;i<((ULONG)(*(ULONG*)sysBuffer));i++)
    {
		if(pProcesses[i].ProcessId == _systemProcessId)
		{
			status = ZwQueryObject((HANDLE)pProcesses[i].Handle, ObjectNameInfo, 
				ObjectName, sizeof(ObjectNameBuf), &ReturnLen);

	//		dprintf("0x%x > %wZ\n", pProcesses[i].Handle, &ObjectName->Name);
			if(status == 0 && (RtlEqualUnicodeString(&ObjectName->Name, fileName,TRUE)==TRUE))
			{
				hPageFile = (HANDLE)pProcesses[i].Handle;
				__free((PVOID)sysBuffer);
				return hPageFile;
			}
		}
	}
	
	__free((PVOID)sysBuffer);

    return (HANDLE)-1;
}
//--------------------------------------------------------------------------------------
NTSTATUS
RtlAllocateUnicodeString(PUNICODE_STRING us, ULONG maxLength)
{
	NTSTATUS	status = STATUS_UNSUCCESSFUL;
	
    ULONG ulMaximumLength = maxLength;
	
    if (maxLength > 0)
    {
        if ((us->Buffer = (PWSTR)__malloc(ulMaximumLength)) != NULL)
		{
			RtlZeroMemory(us->Buffer, ulMaximumLength);
			
			us->Length = 0;
			us->MaximumLength = (USHORT)maxLength;
			
			status = STATUS_SUCCESS;
		}
		else
		{
			status = STATUS_NO_MEMORY;
		}
    }
	
    return status;
}


NTSTATUS
flt_getFileHandleReadOnly(PHANDLE fileHandle, PUNICODE_STRING fileName)
{
	OBJECT_ATTRIBUTES	oa;
	IO_STATUS_BLOCK IoStatusBlock;
		
	InitializeObjectAttributes(&oa,
		fileName,
		OBJ_CASE_INSENSITIVE,
		NULL,
		NULL);
	
	return ZwCreateFile(fileHandle,
		GENERIC_READ | SYNCHRONIZE,
		&oa,
		&IoStatusBlock,
		NULL,
		0,
		FILE_SHARE_READ,
		FILE_OPEN,
		FILE_SYNCHRONOUS_IO_NONALERT,
		NULL,
		0);
}

NTSTATUS
flt_getFatFirstSectorOffset(HANDLE fileHandle, PULONGLONG firstDataSector)
{
	NTSTATUS	status;
	IO_STATUS_BLOCK	IoStatusBlock;
	FAT_LBR		fatLBR = { 0 };

	LARGE_INTEGER	pos;
	pos.QuadPart = 0;

	if (!firstDataSector)
	{
		return STATUS_NOT_FOUND;
	}

	status = ZwReadFile(fileHandle, NULL, NULL, NULL, &IoStatusBlock, &fatLBR, sizeof(fatLBR), &pos, NULL);
	
	if (NT_SUCCESS(status) && sizeof(FAT_LBR) == IoStatusBlock.Information)
	{
		DWORD dwRootDirSectors	= 0;
		DWORD dwFATSz			= 0;
	
		// Validate jump instruction to boot code. This field has two
		// allowed forms: 
		// jmpBoot[0] = 0xEB, jmpBoot[1] = 0x??, jmpBoot[2] = 0x90 
		// and
		// jmpBoot[0] = 0xE9, jmpBoot[1] = 0x??, jmpBoot[2] = 0x??
		// 0x?? indicates that any 8-bit value is allowed in that byte.
		// JmpBoot[0] = 0xEB is the more frequently used format.
		
		if(( fatLBR.wTrailSig       != 0xAA55 ) ||
			( ( fatLBR.pbyJmpBoot[ 0 ] != 0xEB || 
			fatLBR.pbyJmpBoot[ 2 ] != 0x90 ) &&
			( fatLBR.pbyJmpBoot[ 0 ] != 0xE9 ) ) )
		{
			status = STATUS_NOT_FOUND;
			goto __faild;
		}
		
		// Compute first sector offset for the FAT volumes:		


		// First, we determine the count of sectors occupied by the
		// root directory. Note that on a FAT32 volume the BPB_RootEntCnt
		// value is always 0, so on a FAT32 volume dwRootDirSectors is
		// always 0. The 32 in the above is the size of one FAT directory
		// entry in bytes. Note also that this computation rounds up.
		
		dwRootDirSectors = 
			( ( ( fatLBR.bpb.wRootEntCnt * 32 ) + 
			( fatLBR.bpb.wBytsPerSec - 1  ) ) / 
			fatLBR.bpb.wBytsPerSec );
		
		// The start of the data region, the first sector of cluster 2,
		// is computed as follows:
		
		dwFATSz = fatLBR.bpb.wFATSz16;		
		if( !dwFATSz )
			dwFATSz = fatLBR.ebpb32.dwFATSz32;
		

		if( !dwFATSz )
		{
			status = STATUS_NOT_FOUND;
			goto __faild;
		}
		

		// �õ���������ʼ�����ǵ�һ�ص�λ��
		*firstDataSector = 
			( fatLBR.bpb.wRsvdSecCnt + 
			( fatLBR.bpb.byNumFATs * dwFATSz ) + 
			dwRootDirSectors );		
		}

	status = STATUS_SUCCESS;
__faild:

	return status;
}

/*
	��ȡ����Ϣ
	diskNumber
	LowerDevObj

	partitionNumber
	partitionType
	physicalStartingOffset
	bootIndicator
	firstSectorOffset
	bytesPerSector
	bytesPerCluster
	bytesTotal
*/
NTSTATUS
flt_getVolumeInfo(WCHAR volume, PVOLUME_INFO info)
{
	NTSTATUS	status;
	HANDLE		fileHandle;
	UNICODE_STRING	fileName;
	OBJECT_ATTRIBUTES	oa;
	IO_STATUS_BLOCK IoStatusBlock;
	
	WCHAR	volumeDosName[50];

	swprintf(volumeDosName, L"\\??\\%c:", volume);
	
	RtlInitUnicodeString(&fileName, volumeDosName);
	
	InitializeObjectAttributes(&oa,
		&fileName,
		OBJ_CASE_INSENSITIVE,
		NULL,
		NULL);
	
	status = ZwCreateFile(&fileHandle,
		GENERIC_ALL | SYNCHRONIZE,
		&oa,
		&IoStatusBlock,
		NULL,
		0,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		FILE_OPEN,
		FILE_SYNCHRONOUS_IO_NONALERT,	// ͬ����д
		NULL,
		0);

	dprintf("open %wZ ret 0x%x\n", &fileName, status);

	if (NT_SUCCESS(status))
	{		
		IO_STATUS_BLOCK				ioBlock;
		PARTITION_INFORMATION		partitionInfo;
		FILE_FS_SIZE_INFORMATION	sizeoInfo;

		ULONG	buff[256];
		PVOLUME_DISK_EXTENTS		diskExtents;

		diskExtents = (PVOLUME_DISK_EXTENTS)buff;

		// �õ��˾����ڵ�Ӳ�̺ţ������ǿ��̾�
		status = ZwDeviceIoControlFile( fileHandle, 
			NULL, 
			NULL, 
			NULL, 
			&ioBlock, 
			IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, 
			NULL, 
			0, 
			diskExtents, 
			sizeof(buff)
			);

		if (NT_SUCCESS(status))
		{
			info->diskNumber = diskExtents->Extents[0].DiskNumber;
			// �õ��²��豸
			info->LowerDevObj = _lowerDeviceObject[info->diskNumber];
		}

		// �õ��˾��һ���ͣ�������Ӳ�̵��ϵ�ƫ�Ƶ���Ϣ

		status = ZwDeviceIoControlFile( fileHandle, 
			NULL, 
			NULL, 
			NULL, 
			&ioBlock, 
			IOCTL_DISK_GET_PARTITION_INFO, 
			NULL, 
			0, 
			&partitionInfo, 
			sizeof(partitionInfo)
			);


		if (NT_SUCCESS(status))
		{
			info->partitionNumber = partitionInfo.PartitionNumber;
			info->partitionType = partitionInfo.PartitionType;
			info->physicalStartingOffset = partitionInfo.StartingOffset.QuadPart;
			info->bootIndicator = partitionInfo.BootIndicator;
			info->firstDataSector = 0;
			
			// FAT��������ȡLBR, �õ���һ���ص�ƫ��
 			if ((PARTITION_FAT_12 == partitionInfo.PartitionType) ||
 				(PARTITION_FAT_16 == partitionInfo.PartitionType) ||
				(PARTITION_HUGE == partitionInfo.PartitionType) ||
				(PARTITION_FAT32 == partitionInfo.PartitionType) ||
 				(PARTITION_FAT32_XINT13 == partitionInfo.PartitionType) ||
				(PARTITION_XINT13 == partitionInfo.PartitionType))
			{
				status = flt_getFatFirstSectorOffset(fileHandle, &info->firstDataSector);
			}
		}

		// �õ��أ������ȴ�С
		status = ZwQueryVolumeInformationFile(fileHandle,
			&IoStatusBlock,
			&sizeoInfo,
			sizeof(sizeoInfo),
			FileFsSizeInformation);

		if (NT_SUCCESS(status))
		{
			info->bytesPerSector = sizeoInfo.BytesPerSector;
			info->bytesPerCluster = sizeoInfo.BytesPerSector * sizeoInfo.SectorsPerAllocationUnit;

			// ��˵õ��Ĵ��̴�С��������LBR����Ϣ
			info->bytesTotal = partitionInfo.PartitionLength.QuadPart;
		}
		
		ZwClose(fileHandle);
	}

	return status;
}

/*
	��ȡ��λͼ��Ϣ
	diskNumber
	LowerDevObj

	partitionNumber
	partitionType
	physicalStartingOffset
	bootIndicator
	firstSectorOffset
	bytesPerSector
	bytesPerCluster
	bytesTotal
*/
NTSTATUS
flt_getVolumeBitmapInfo(WCHAR volume, PVOLUME_BITMAP_BUFFER * bitMap)
{
	NTSTATUS	status;
	HANDLE		fileHandle;
	UNICODE_STRING	fileName;
	OBJECT_ATTRIBUTES	oa;
	IO_STATUS_BLOCK IoStatusBlock;
	
	WCHAR	volumeDosName[10];
	
	if (NULL == bitMap)
	{
		return STATUS_UNSUCCESSFUL;
	}
	
	swprintf(volumeDosName, L"\\??\\%c:", volume);
	
	RtlInitUnicodeString(&fileName, volumeDosName);
	
	InitializeObjectAttributes(&oa,
		&fileName,
		OBJ_CASE_INSENSITIVE,
		NULL,
		NULL);
	
	status = ZwCreateFile(&fileHandle,
		GENERIC_ALL | SYNCHRONIZE,
		&oa,
		&IoStatusBlock,
		NULL,
		0,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		FILE_OPEN,
		FILE_SYNCHRONOUS_IO_NONALERT,	// ͬ����д
		NULL,
		0);

	dprintf("Open %wZ ret 0x%x\n", &fileName, status);
	
	if (NT_SUCCESS(status))
	{		
		IO_STATUS_BLOCK	ioBlock;
		PVOLUME_BITMAP_BUFFER	info;
		STARTING_LCN_INPUT_BUFFER StartingLCN;

		ULONG	BitmapSize = 0;
		
		StartingLCN.StartingLcn.QuadPart = 0;
		
		
		/*
		// ���������˾�, �ڵõ���λͼǰ
		status = ZwFsControlFile( fileHandle, 
					NULL, 
					NULL, 
					NULL, 
					&ioBlock, 
					FSCTL_LOCK_VOLUME, 
					NULL, 0, NULL, 0
					);
		
		dprintf("FSCTL_LOCK_VOLUME = 0x%x\n", status);
		
		*/

		do 
		{
			BitmapSize += 10240;
			
			info = (PVOLUME_BITMAP_BUFFER)__malloc(BitmapSize);
			// �ڴ治����
			if (!info)
			{
				status = STATUS_INSUFFICIENT_RESOURCES;
				break;
			}
			
			status = ZwFsControlFile( fileHandle, 
				NULL, 
				NULL, 
				NULL, 
				&ioBlock, 
				FSCTL_GET_VOLUME_BITMAP, 
				&StartingLCN,
				sizeof (StartingLCN),
				info, 
				BitmapSize
				);

			if (STATUS_BUFFER_OVERFLOW == status)
			{
				__free(info);
			}
			
		} while(STATUS_BUFFER_OVERFLOW == status);
		
		dprintf("FSCTL_GET_VOLUME_BITMAP ret 0x%x\n", status);

		if (!NT_SUCCESS(status))
		{
			if (info)
			{
				__free(info);
			}
			*bitMap = NULL;
		}
		else
		{
			dprintf("%c: bitMapinfo (%d / %d) cluster = %I64d\n", volume, ioBlock.Information, BitmapSize, info->BitmapSize.QuadPart);

			*bitMap = info;
		}

		
		/*
		status = ZwFsControlFile( fileHandle, 
			NULL, 
			NULL, 
			NULL, 
			&ioBlock, 
			FSCTL_UNLOCK_VOLUME, 
			NULL, 0, NULL, 0
				);

		dprintf("FSCTL_UNLOCK_VOLUME ret 0x%x\n", status);
		*/
		

		ZwClose(fileHandle);
	}
	
	return status;
}

NTSTATUS
flt_SendToNextDriver(
				   IN	PDEVICE_OBJECT	TgtDevObj,
				   IN	PIRP			Irp
				   )
{	
	//������ǰ��irp stack
	IoSkipCurrentIrpStackLocation(Irp);
	//����Ŀ���豸���������irp
	return IoCallDriver(TgtDevObj, Irp);
}

NTSTATUS
flt_CompleteRequest(
				  IN	PIRP			Irp,
				  IN	NTSTATUS		Status,
				  IN	CCHAR			Priority
				  )
{	
	//��IRP��io״̬��ֵΪ����Ĳ���
	Irp->IoStatus.Status = Status;
	//����IoCompleteRequest��������Irp
	IoCompleteRequest(Irp, Priority);
	return STATUS_SUCCESS;
}


RTL_GENERIC_COMPARE_RESULTS
NTAPI CompareRoutine(
					 struct _RTL_GENERIC_TABLE *Table,
					 PVOID FirstStruct,
					 PVOID SecondStruct
					 )
{
	PPAIR first = (PPAIR) FirstStruct;
	PPAIR second = (PPAIR) SecondStruct;
	
	UNREFERENCED_PARAMETER(Table);

	if (first->orgIndex < second->orgIndex)
		return GenericLessThan;
	else if (first->orgIndex > second->orgIndex)
		return GenericGreaterThan;
	else
		return GenericEqual;
}

PVOID NTAPI AllocateRoutine (
							 struct _RTL_GENERIC_TABLE *Table,
							 LONG ByteSize
							 )
{
	UNREFERENCED_PARAMETER(Table);

	return __malloc(ByteSize);
}

VOID
NTAPI FreeRoutine (
				   struct _RTL_GENERIC_TABLE *Table,
				   PVOID Buffer
				   )
{
	
	UNREFERENCED_PARAMETER(Table);
	
	__free(Buffer);
}

BOOL bitmap_test (ULONG * bitMap, ULONGLONG index)
{
	//	return ((BYTE *)BitmapDetail)[Cluster / 8] & (1 << (Cluster % 8));
	return ((bitMap[index / 32] & (1 << (index % 32))) ? TRUE : FALSE);   
}

void bitmap_set (ULONG * bitMap, ULONGLONG index, BOOL Used)
{
    if (Used)
        bitMap[index / 32] |= (1 << (index % 32));
    else
        bitMap[index / 32] &= ~(1 << (index % 32));
}


// λͼ���Ϊ����ֱ�Ӷ�д���ļ��Ĵ�
NTSTATUS
setBitmapDirectRWFile(WCHAR volume, WCHAR * path, PDP_BITMAP bitmap)
{
	NTSTATUS	status;
	HANDLE	linkHandle = NULL;
	HANDLE	linkHandle1 = NULL;
	OBJECT_ATTRIBUTES	oa;
	ULONG	ret;
	BOOLEAN	needClose = FALSE;
	BOOLEAN	needFree = FALSE;
	UNICODE_STRING	symbol;
	UNICODE_STRING	target;
	WCHAR	tempBuffer[256];
	
	PVOID lpFileObject = NULL;
	HANDLE fileHandle = (HANDLE)-1;
	
	ULONG   Cls, r;
	LARGE_INTEGER PrevVCN, Lcn;	
	PRETRIEVAL_POINTERS_BUFFER pVcnPairs = NULL;

	PEPROCESS	eProcess = NULL;
	if (!NT_SUCCESS( PsLookupProcessByProcessId((PVOID)_systemProcessId, &eProcess)))
	{
		goto __faild;
	}

	ObDereferenceObject(eProcess);
	// ע�⣬Ҫ���뵽ϵͳ���̻�ȡ���
	KeAttachProcess(eProcess);

	swprintf(tempBuffer, L"\\??\\%c:%ls", volume, path);

	RtlInitUnicodeString(&target, tempBuffer);

	// ֱ�Ӵ�ʧ�ܣ����ԴӾ�����л�ȡ

	status = flt_getFileHandleReadOnly(&fileHandle, &target);
	if (NT_SUCCESS(status))
	{
		needClose = TRUE;
	}
	// ���ʾܾ������Դ�HANDLE���л�ȡ
	else if (STATUS_SHARING_VIOLATION == status)
	{
		dprintf("Try to open %wZ from handle list\n", &target);

		swprintf(tempBuffer, L"\\??\\%c:", volume);
		
		RtlInitUnicodeString(&symbol, tempBuffer);
		
		RtlAllocateUnicodeString(&target, 1024);

		needFree = TRUE;
		

		InitializeObjectAttributes(&oa,
			&symbol,
			OBJ_CASE_INSENSITIVE,
			NULL,
			NULL);
		
		// ��\\??\\C:ӳ��Ϊ��ʵ·��\\Device\\HarddiskVolume1 ������·��
		
		status = ZwOpenSymbolicLinkObject(&linkHandle, GENERIC_READ, &oa);
		
		if (!NT_SUCCESS(status))
		{
			dprintf("ZwOpenSymbolicLinkObject %wZ fail 0x%x\n", &symbol, status);
			goto __faild;
		}
		
		status = ZwQuerySymbolicLinkObject(linkHandle, &target, &ret);
		
		if (!NT_SUCCESS(status))
		{
			dprintf("ZwQuerySymbolicLinkObject %wZ fail 0x%x\n", &symbol, status);
			goto __faild;
		}

		while (1)
		{
			// ���Ƿ��ѯ������·��ָ��Ļ���symbolicLink
			InitializeObjectAttributes(&oa,
				&target,
				OBJ_CASE_INSENSITIVE,
				NULL,
				NULL);
			
			// ��\\??\\C:ӳ��Ϊ��ʵ·��\\Device\\HarddiskVolume1 ������·��
			
			status = ZwOpenSymbolicLinkObject(&linkHandle1, GENERIC_READ, &oa);
			
			// �����̵أ���ָ����symbollink,��
			if (NT_SUCCESS(status))
			{
				dprintf("SymbolicLink > SymbolicLink\n");
				ZwClose(linkHandle);
				linkHandle = linkHandle1;
				status = ZwQuerySymbolicLinkObject(linkHandle, &target, &ret);
				if (!NT_SUCCESS(status))
				{
					goto __faild;
				}			
			}
			else
			{
				break;
			}
		}
		
		// �ϲ�·��
		
		RtlAppendUnicodeToString(&target, path);
	
		fileHandle = searchFileHandle(&target);

		needClose = FALSE;
	}
	
	if((HANDLE)-1 == fileHandle)
	{
		dprintf("getFileHandle %wZ fail\n", &target);
		goto __faild;
	}
	
	pVcnPairs = getFileClusterList(fileHandle);
	
	if(NULL == pVcnPairs)
	{
		dprintf("getFileClusterList fail\n");
		goto __faild;
	}
	
	PrevVCN = pVcnPairs->StartingVcn;
	for (r = 0, Cls = 0; r < pVcnPairs->ExtentCount; r++)
	{
		ULONG	CnCount;
		Lcn = pVcnPairs->Extents[r].Lcn;
		
		for (CnCount = (ULONG)(pVcnPairs->Extents[r].NextVcn.QuadPart - PrevVCN.QuadPart);
		CnCount; CnCount--, Cls++, Lcn.QuadPart++) 
		{
			// ����λͼ
			DPBitMap_Set(bitmap, Lcn.QuadPart, TRUE);
		}
		
		PrevVCN = pVcnPairs->Extents[r].NextVcn;
	}

	dprintf("set %wZ force RW bit map success\n", &target);
	
	__free_Safe(pVcnPairs);
	
__faild:
	
	if (linkHandle)
		ZwClose(linkHandle);

	if (needClose && ((HANDLE)-1 != fileHandle))
		ZwClose(fileHandle);

	if (needFree)
		__free_Safe(target.Buffer);

	if (eProcess)
	{
		KeDetachProcess();		
	}
	
	return status;
}


// from wdk wdm.h
#define SL_KEY_SPECIFIED                0x01
#define SL_OVERRIDE_VERIFY_VOLUME       0x02
#define SL_WRITE_THROUGH                0x04
#define SL_FT_SEQUENTIAL_WRITE          0x08
#define SL_FORCE_DIRECT_WRITE           0x10


NTSTATUS 
FltReadWriteSectorsCompletion( 
	IN PDEVICE_OBJECT DeviceObject, 
	IN PIRP Irp, 
	IN PVOID Context 
	) 
	/*++ 
	Routine Description: 
	A completion routine for use when calling the lower device objects to 
	which our filter deviceobject is attached. 

	Arguments: 

	DeviceObject - Pointer to deviceobject 
	Irp        - Pointer to a PnP Irp. 
	Context    - NULL or PKEVENT 
	Return Value: 

	NT Status is returned. 

	--*/ 
{ 
    PMDL    mdl; 
	
    UNREFERENCED_PARAMETER(DeviceObject); 
	
    // 
    // Free resources 
    // 
	
    if (Irp->AssociatedIrp.SystemBuffer && (Irp->Flags & IRP_DEALLOCATE_BUFFER)) { 
        __free(Irp->AssociatedIrp.SystemBuffer); 
    } 
	
	if (Irp->IoStatus.Status)
	{
		DbgPrint("!!!!!!!!!!Read Or Write HD Error Code====0x%x\n", Irp->IoStatus.Status);
	}
	/*
	��Ϊ��� IRP �����������ν����ģ��ϲ㱾�Ͳ�֪������ôһ�� IRP��
	��ô�������Ҿ�Ҫ�� CompleteRoutine ��ʹ�� IoFreeIrp()���ͷŵ���� IRP��
	������STATUS_MORE_PROCESSING_REQUIRED�������������ݡ�����һ��Ҫע�⣬
	�� CompleteRoutine�������غ���� IRP �Ѿ��ͷ��ˣ�
	������ʱ�������κι������ IRP �Ĳ�����ô����������Եģ��ض����� BSOD ����
	*/
    while (Irp->MdlAddress) { 
        mdl = Irp->MdlAddress; 
        Irp->MdlAddress = mdl->Next; 
        MmUnlockPages(mdl); 
        IoFreeMdl(mdl); 
    } 
	
    if (Irp->PendingReturned && (Context != NULL)) { 
        *Irp->UserIosb = Irp->IoStatus; 
        KeSetEvent((PKEVENT) Context, IO_DISK_INCREMENT, FALSE); 
    } 
	
    IoFreeIrp(Irp); 
	
    // 
    // Don't touch irp any more 
    // 
    return STATUS_MORE_PROCESSING_REQUIRED; 
} 


NTSTATUS 
fastFsdRequest( 
	IN PDEVICE_OBJECT DeviceObject, 
	ULONG majorFunction,
	IN LONGLONG ByteOffset,
	OUT PVOID Buffer, 
	IN ULONG Length, 			    
	IN BOOLEAN Wait 
	)
{ 
    PIRP                irp; 
    IO_STATUS_BLOCK        iosb; 
    KEVENT                event; 
    NTSTATUS            status; 
	
	//
    irp = IoBuildAsynchronousFsdRequest(majorFunction, DeviceObject, 
        Buffer, Length, (PLARGE_INTEGER) &ByteOffset, &iosb); 
    if (!irp) { 
        return STATUS_INSUFFICIENT_RESOURCES; 
    } 

	// vista ��ֱ�Ӵ���д������˱���, ����������Ҫ��IRP��FLAGS����SL_FORCE_DIRECT_WRITE��־
	/*
	If the SL_FORCE_DIRECT_WRITE flag is set, kernel-mode drivers can write to volume areas that they 
	normally cannot write to because of direct write blocking. Direct write blocking was implemented for 
	security reasons in Windows Vista and later operating systems. This flag is checked both at the file 
	system layer and storage stack layer. For more 
	information about direct write blocking, see Blocking Direct Write Operations to Volumes and Disks. 
	The SL_FORCE_DIRECT_WRITE flag is available in Windows Vista and later versions of Windows. 
	http://msdn.microsoft.com/en-us/library/ms795960.aspx
	*/
	if (IRP_MJ_WRITE == majorFunction)
	{
		IoGetNextIrpStackLocation(irp)->Flags |= SL_FORCE_DIRECT_WRITE;
	}
	
    if (Wait) { 
        KeInitializeEvent(&event, NotificationEvent, FALSE); 
        IoSetCompletionRoutine(irp, FltReadWriteSectorsCompletion, 
            &event, TRUE, TRUE, TRUE); 
	
        status = IoCallDriver(DeviceObject, irp); 
        if (STATUS_PENDING == status) { 
            KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL); 
            status = iosb.Status; 
        } 
    } else { 
        IoSetCompletionRoutine(irp, FltReadWriteSectorsCompletion, 
            NULL, TRUE, TRUE, TRUE); 
        irp->UserIosb = NULL; 
        status = IoCallDriver(DeviceObject, irp); 
    } 
	
	if (!NT_SUCCESS(status))
	{
		dprintf("IoCallDriver 0x%x fail 0x%x\n", majorFunction, status);
	}
    return status; 
} 

__inline
BOOL isSectorProtect (PVOLUME_INFO volumeInfo, ULONGLONG index)
{
	if (index < volumeInfo->firstDataSector)
	{
		return FALSE;
	}

	return DPBitMap_Test(volumeInfo->bitMap_Protect,
		(index - volumeInfo->firstDataSector) / (volumeInfo->bytesPerCluster / volumeInfo->bytesPerSector));
}

// ��ȡ��ʵ��Ҫ����Ĵ�
ULONGLONG
getRealSectorForRead(PVOLUME_INFO volumeInfo, ULONGLONG orgIndex)
{
	ULONGLONG	mapIndex = orgIndex;

	// �˴��Ƿ�����ֱ�Ӳ���
	if (isSectorProtect(volumeInfo, orgIndex))
	{
		return orgIndex;
	}

	// �˴��Ƿ��Ѿ����ض���
	if (DPBitMap_Test(volumeInfo->bitMap_Redirect, orgIndex))
	{
		// �ҵ��ض�������, ������	
		PAIR *	result;
		PAIR	pair;
		pair.orgIndex = orgIndex;

		result = (PAIR *)RtlLookupElementGenericTable(&volumeInfo->redirectMap, &pair);

		if (result)
		{
			mapIndex = result->mapIndex;
		}
	}
	
	return mapIndex;
}


// ��ȡ��ʵ��Ҫ����Ĵ�
ULONGLONG
getRealSectorForWrite(PVOLUME_INFO volumeInfo, ULONGLONG orgIndex)
{
	ULONGLONG	mapIndex = -1;

	// �������Ƿ�����ֱ��д
	if (isSectorProtect(volumeInfo, orgIndex))
	{
		return orgIndex;
	}

	// �˴��Ƿ��Ѿ����ض���
	if (DPBitMap_Test(volumeInfo->bitMap_Redirect, orgIndex))
	{
		// �ҵ��ض�������, ������	
		PAIR *	result;
		PAIR	pair;
		pair.orgIndex = orgIndex;
		
		result = (PAIR *)RtlLookupElementGenericTable(&volumeInfo->redirectMap, &pair);
		
		if (result)
		{
			mapIndex = result->mapIndex;
		}
	}
	else
	{
		// ������һ�����õĿ�������
		mapIndex = DPBitMap_FindNext(volumeInfo->bitMap_Free, volumeInfo->last_scan_index, FALSE);

		if (mapIndex != -1)
		{
			// lastScan = ��ǰ�õ��� + 1
			volumeInfo->last_scan_index = mapIndex + 1;

			// ���Ϊ�ǿ���
			DPBitMap_Set(volumeInfo->bitMap_Free, mapIndex, TRUE);
			
			// ��Ǵ������ѱ��ض���(orgIndex)
			DPBitMap_Set(volumeInfo->bitMap_Redirect, orgIndex, TRUE);
			
			// �����ض����б�
			{
				PAIR	pair;
				pair.orgIndex = orgIndex;
				pair.mapIndex = mapIndex;				
				RtlInsertElementGenericTable(&volumeInfo->redirectMap, &pair, sizeof(PAIR), NULL);
			}
		}
	}

	return mapIndex;
}

// ģ���
NTSTATUS
handle_disk_request(
	PVOLUME_INFO volumeInfo,
	ULONG majorFunction,
	ULONGLONG logicOffset,  
	void * buff,
	ULONG length)
{
	NTSTATUS	status;
	
	// ��ǰ����������ƫ��
	ULONGLONG	physicalOffset = 0;
	ULONGLONG	sectorIndex;
	ULONGLONG	realIndex;
	ULONG		bytesPerSector = volumeInfo->bytesPerSector;
	
	// ���¼�������Ϊ�ж�Ϊ����Ĵ��������Ĵض���
	BOOLEAN		isFirstBlock = TRUE;
	ULONGLONG	prevIndex = -1;
	ULONGLONG	prevOffset = -1;
	PVOID		prevBuffer = NULL;
	ULONG		totalProcessBytes = 0;

	// �ж��ϴ�Ҫ����Ĵظ����Ҫ����Ĵ��Ƿ������������˾�һ�������򵥶�����, �ӿ��ٶ�
	while (length)
	{
		sectorIndex = logicOffset / bytesPerSector;	
		
		if (IRP_MJ_READ == majorFunction)
		{
			realIndex = getRealSectorForRead(volumeInfo, sectorIndex);
		}
		else
		{
			// ��������������ж��Ƿ���ԭ������
			realIndex = getRealSectorForWrite(volumeInfo, sectorIndex);
		}
		
		// �粻�ǰɣ�Ӳ��̫����С��, ���Ӳ�����
		if (-1 == realIndex)
		{
			dprintf("no enough disk space\n");
			return STATUS_DISK_FULL;
		}
		
		physicalOffset = realIndex * bytesPerSector;

__reInit:		
		// ��ʼprevIndex
		if (isFirstBlock)
		{
			prevIndex = realIndex;
			prevOffset = physicalOffset;
			prevBuffer = buff;
			totalProcessBytes = bytesPerSector;
			
			isFirstBlock = FALSE;
			
			goto __next;
		}
		
		// �����Ƿ�����,  ��������������¸��ж�
		if (prevIndex == (realIndex - 1))
		{
			prevIndex = realIndex;
			totalProcessBytes += bytesPerSector;
			goto __next;
		}
		// �����ϴ�������Ҫ����Ĵ�, ����isFirstBlock
		else
		{
			isFirstBlock = TRUE;
			status = fastFsdRequest(volumeInfo->LowerDevObj, majorFunction, volumeInfo->physicalStartingOffset + prevOffset, 
				prevBuffer, totalProcessBytes, TRUE);

			// ���³�ʼ��
			goto __reInit;
		}
__next:		
		// ���һ�ء���Ҳ����������Ѿ�
		if (bytesPerSector >= length)
		{
			status = fastFsdRequest(volumeInfo->LowerDevObj, majorFunction, volumeInfo->physicalStartingOffset + prevOffset, 
				prevBuffer, totalProcessBytes, TRUE);

			// �ж��˳�
			break;
		}
		
		// ������һ��, ����ʣ�������
		logicOffset += (ULONGLONG)bytesPerSector;
		buff = (char *)buff + bytesPerSector;
		length -= bytesPerSector;
	}
	
	return status;
}
  
//
// For backward compatibility with Windows NT 4.0 by Bruce Engle.
//
#ifndef MmGetSystemAddressForMdlSafe
#define MmGetSystemAddressForMdlSafe(MDL, PRIORITY) MmGetSystemAddressForMdlPrettySafe(MDL)

PVOID
MmGetSystemAddressForMdlPrettySafe (
    PMDL Mdl
    )
{
    CSHORT  MdlMappingCanFail;
    PVOID   MappedSystemVa;

    MdlMappingCanFail = Mdl->MdlFlags & MDL_MAPPING_CAN_FAIL;

    Mdl->MdlFlags |= MDL_MAPPING_CAN_FAIL;

    MappedSystemVa = MmGetSystemAddressForMdl(Mdl);

    if (MdlMappingCanFail == 0)
    {
        Mdl->MdlFlags &= ~MDL_MAPPING_CAN_FAIL;
    }

    return MappedSystemVa;
}
#endif

/*
// �����߳�
VOID
flt_thread_reclaim (
	IN PVOID Context
	)
{
	ULONG	i = 0;
	ULONG	timeout = getTickCount();
	PFILTER_DEVICE_EXTENSION	device_extension = (PFILTER_DEVICE_EXTENSION)Context;

	while (!device_extension->terminate_thread)
	{
		if ((getTickCount() - timeout) > (1000 * 60))
		{
			for (i = 0; i < _countof(_volumeList); i++)
			{
				if (_volumeList[i].isProtect && _volumeList[i].isProtect && _volumeList[i].isDiskFull)
				{
					// ����
					reclaimDiskSpace(&_volumeList[i]);
				}
				
			}
			
			timeout = getTickCount();
		}
	}

	PsTerminateSystemThread(STATUS_SUCCESS);
}
*/
VOID
flt_thread_read_write (
					   IN PVOID Context
					   )
{
	//NTSTATUS���͵ĺ�������ֵ
	NTSTATUS					status = STATUS_SUCCESS;
	//����ָ������豸���豸��չ��ָ��
	PFILTER_DEVICE_EXTENSION	device_extension = (PFILTER_DEVICE_EXTENSION)Context;
	//������е����
	PLIST_ENTRY			ReqEntry = NULL;
	//irpָ��
	PIRP				Irp = NULL;
	//irp stackָ��
	PIO_STACK_LOCATION	io_stack = NULL;
	//irp�а��������ݵ�ַ
	PVOID				buffer = NULL;
	//irp�е����ݳ���
	ULONG				length = 0;
	//irpҪ�����ƫ����
	LARGE_INTEGER		offset = { 0 };

	//irpҪ�����ƫ����
	LARGE_INTEGER		cacheOffset = { 0 };

	

	//��������̵߳����ȼ�
	KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);
	//�������̵߳�ʵ�ֲ��֣����ѭ�������˳�
	for (;;)
	{	
		//�ȵȴ��������ͬ���¼������������û��irp��Ҫ�������ǵ��߳̾͵ȴ�������ó�cpuʱ��������߳�
		KeWaitForSingleObject(
			&device_extension->ReqEvent,
			Executive,
			KernelMode,
			FALSE,
			NULL
			);
		//��������߳̽�����־����ô�����߳��ڲ��Լ������Լ�
		if (device_extension->terminate_thread)
		{
			//�����̵߳�Ψһ�˳��ص�
			PsTerminateSystemThread(STATUS_SUCCESS);
			return;
		}
		//��������е��ײ��ó�һ��������׼����������ʹ�������������ƣ����Բ����г�ͻ
		while (ReqEntry = ExInterlockedRemoveHeadList(
			&device_extension->list_head,
			&device_extension->list_lock
			))
		{
			PVOLUME_INFO	volumeInfo;

			void * newbuff = NULL;

			//�Ӷ��е�������ҵ�ʵ�ʵ�irp�ĵ�ַ
			Irp = CONTAINING_RECORD(ReqEntry, IRP, Tail.Overlay.ListEntry);

			//ȡ��irp stack
			io_stack = IoGetCurrentIrpStackLocation(Irp);

			// ��ȡ����Ϣ
			volumeInfo = &_volumeList[(ULONG)Irp->IoStatus.Pointer];

			if (IRP_MJ_READ == io_stack->MajorFunction)
			{
				//����Ƕ���irp����������irp stack��ȡ����Ӧ�Ĳ�����Ϊoffset��length
				offset = io_stack->Parameters.Read.ByteOffset;
				length = io_stack->Parameters.Read.Length;
			}
			else if (IRP_MJ_WRITE == io_stack->MajorFunction)
			{
				//�����д��irp����������irp stack��ȡ����Ӧ�Ĳ�����Ϊoffset��length
				offset = io_stack->Parameters.Write.ByteOffset;
				length = io_stack->Parameters.Write.Length;				
			}
			else
			{
				//����֮�⣬offset��length����0
				cacheOffset.QuadPart = 0;
				offset.QuadPart = 0;
				length = 0;
			}	

			// �õ��ھ��е�ƫ�� ����ƫ��-���߼�ƫ��
			cacheOffset.QuadPart = offset.QuadPart - volumeInfo->physicalStartingOffset;

// 			DbgPrint("0x%x UserBuffer = 0x%x MdlAddress = 0x%x SystemBuffer = 0x%x\n", io_stack->MajorFunction,
// 				Irp->UserBuffer, Irp->MdlAddress, Irp->AssociatedIrp.SystemBuffer);

			if (Irp->MdlAddress)
			{
				buffer = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
			}
			else if (Irp->UserBuffer)
			{
				buffer = Irp->UserBuffer;
			}
			else
			{
				buffer = Irp->AssociatedIrp.SystemBuffer;
			}
	
			if (!buffer || !length)
			{
				goto __faild;
			}

			if (0 != (length % volumeInfo->bytesPerSector))
			{
				DbgPrint("fuck read %d\n", length);
			}
			
			// ���ܺ��ϴδ�����buffer��ͬһ������������Ȼ
			// ����� PFN_LIST_CORRUPT (0x99, ...) A PTE or PFN is corrupt ����
			// Ƶ�������ڴ�Ҳ���ǰ취���û���ذ�
			newbuff = __malloc(length);

			if (newbuff)
			{
				if (IRP_MJ_READ == io_stack->MajorFunction)
				{
					status = handle_disk_request(volumeInfo, io_stack->MajorFunction, cacheOffset.QuadPart,
					 newbuff, length);
					RtlCopyMemory(buffer, newbuff, length);
				}
				else
				{
					RtlCopyMemory(newbuff, buffer, length);
					status = handle_disk_request(volumeInfo, io_stack->MajorFunction, cacheOffset.QuadPart,
					 newbuff, length);
				}
				__free(newbuff);
			}
			else
			{
				status = STATUS_NO_MEMORY;
			}

			// ��ֵInformation
			if (NT_SUCCESS(status))
			{
				Irp->IoStatus.Information = length;
			}
			else
			{
				Irp->IoStatus.Information = 0;
			}

			flt_CompleteRequest(
				Irp,
				status,
				IO_NO_INCREMENT
				);
			continue;
__faild:

			flt_SendToNextDriver(volumeInfo->LowerDevObj, Irp);
			continue;			
		}
	}
}

// ����һ����
void protect_Volume(WCHAR volume, BOOLEAN protect)
{
	_volumeList[volume - L'A'].isProtect = protect;
}

NTSTATUS
flt_initVolumeLogicBitMap(PVOLUME_INFO volumeInfo)
{
	NTSTATUS	status;
	PVOLUME_BITMAP_BUFFER	bitMap = NULL;	

	// �߼�λͼ��С
	ULONGLONG	logicBitMapMaxSize = 0;
	
	ULONG		sectorsPerCluster = 0;

	ULONGLONG	index = 0;
	ULONGLONG	i = 0;

	status = flt_getVolumeBitmapInfo(volumeInfo->volume, &bitMap);
	
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	sectorsPerCluster = volumeInfo->bytesPerCluster / volumeInfo->bytesPerSector;

	// ��ȡ�˾����ж��ٸ�����, ��bytesTotal����Ƚ�׼ȷ������������ı���fsinfo,���ټ�����������
	volumeInfo->sectorCount = volumeInfo->bytesTotal / volumeInfo->bytesPerSector;
	
	// �õ��߼�λͼ�Ĵ�Сbytes
	logicBitMapMaxSize = (volumeInfo->sectorCount / 8) + 1;

	// �ϴ�ɨ��Ŀ��дص�λ��
	volumeInfo->last_scan_index = 0;

	
	dprintf("------------------------\n");
	dprintf("extend cluster = %08I64d physicalStartingOffset = 0x%08I64x bitMapSize = 0x%I64x\n"
		"bytesPerSector = %d bytesPerCluster = %d sectorsPerCluster = %d\n", 
		volumeInfo->firstDataSector, volumeInfo->physicalStartingOffset, logicBitMapMaxSize,
		volumeInfo->bytesPerSector, volumeInfo->bytesPerCluster, sectorsPerCluster);
	
	// ������Ϊ��λ��λͼ
	if (!NT_SUCCESS(DPBitMap_Create(&volumeInfo->bitMap_Redirect, volumeInfo->sectorCount, SLOT_SIZE)))
	{
		status = STATUS_UNSUCCESSFUL;
		goto __faild;	
	}
	
	// �Դ�Ϊ��λ��λͼ
	if (!NT_SUCCESS(DPBitMap_Create(&volumeInfo->bitMap_Protect, volumeInfo->sectorCount / sectorsPerCluster, SLOT_SIZE)))
	{
		status = STATUS_UNSUCCESSFUL;
		goto __faild;	
	}

	// ������Ϊ��λ��λͼ, ���һ�������ڴ���󣬻�ʧ�ܣ���dpbitmap���벻�������ڴ�
	if (!NT_SUCCESS(DPBitMap_Create(&volumeInfo->bitMap_Free, volumeInfo->sectorCount, SLOT_SIZE)))
	{
		status = STATUS_UNSUCCESSFUL;
		goto __faild;	
	}

	// ��ʽ�ؿ�ʼǰ�Ĵض����Ϊ��ʹ��
	for (i = 0; i < volumeInfo->firstDataSector; i++)
	{
		DPBitMap_Set(volumeInfo->bitMap_Free, i, TRUE);
	}

	// FAT32��ʽ�ؿ�ʼǰ����������
	for (i = 0; i < bitMap->BitmapSize.QuadPart; i++)
	{		
		if (bitmap_test((PULONG)&bitMap->Buffer, i))
		{
			ULONGLONG	j = 0;
			ULONGLONG	base = volumeInfo->firstDataSector + (i * sectorsPerCluster);
			for (j = 0; j < sectorsPerCluster; j++)
			{
				if (!NT_SUCCESS(DPBitMap_Set(volumeInfo->bitMap_Free, base + j, TRUE)))
				{
					status = STATUS_UNSUCCESSFUL;
					goto __faild;
				}
			}
		}
	}

	// �Ź��⼸���ļ���ֱ�Ӷ�д
	// bootstat.dat�������д���´���������ʾ����������

	setBitmapDirectRWFile(volumeInfo->volume, 
		(*NtBuildNumber >= 2600) ? L"\\Windows\\bootstat.dat" : L"\\WINNT\\bootstat.dat",
		volumeInfo->bitMap_Protect);
	// SAM�����ˣ�������
// 	setBitmapDirectRWFile(volumeInfo->volume, 
// 		(*NtBuildNumber >= 2600) ? L"\\Windows\\system32\\config\\sam" : L"\\WINNT\\system32\\config\\sam",
// 		volumeInfo->bitMap_Protect);

	// ҳ���ļ�
	setBitmapDirectRWFile(volumeInfo->volume, L"\\pagefile.sys", volumeInfo->bitMap_Protect);

	// �����ļ�
	setBitmapDirectRWFile(volumeInfo->volume, L"\\hiberfil.sys", volumeInfo->bitMap_Protect);	
	
	// ��ʼ��clusterMap
	RtlInitializeGenericTable(&volumeInfo->redirectMap, CompareRoutine, AllocateRoutine, FreeRoutine, NULL);

	status = STATUS_SUCCESS;

__faild:

	if (!NT_SUCCESS(status))
	{
		if (volumeInfo->bitMap_Redirect)
		{
			DPBitMap_Free(volumeInfo->bitMap_Redirect);
			volumeInfo->bitMap_Redirect = NULL;
		}
		if (volumeInfo->bitMap_Protect)
		{
			DPBitMap_Free(volumeInfo->bitMap_Protect);
			volumeInfo->bitMap_Protect = NULL;
		}
		if (volumeInfo->bitMap_Free)
		{
			DPBitMap_Free(volumeInfo->bitMap_Free);
			volumeInfo->bitMap_Free = NULL;
		}
	}

	__free_Safe(bitMap);


	return STATUS_SUCCESS;
}

BOOLEAN	_signal = FALSE;

// �ı䱻�����ķ�����ͼ��
VOID
changeDriveIcon(WCHAR volume)
{
	HANDLE	keyHandle;
	UNICODE_STRING	keyPath;
	OBJECT_ATTRIBUTES	objectAttributes;
	ULONG		ulResult;
	NTSTATUS	status;
	
	RtlInitUnicodeString( &keyPath, L"\\Registry\\Machine\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\DriveIcons");   
	
    //��ʼ��objectAttributes 
    InitializeObjectAttributes(&objectAttributes,   
		&keyPath,   
		OBJ_CASE_INSENSITIVE| OBJ_KERNEL_HANDLE,//�Դ�Сд����     
		NULL,
		NULL);
	
	status = ZwCreateKey( &keyHandle,   
		KEY_ALL_ACCESS,   
		&objectAttributes,   
		0,   
		NULL,   
		REG_OPTION_VOLATILE,   // ��������Ч
		&ulResult);
	
	if (NT_SUCCESS(status))
	{
		WCHAR	volumeName[10];
		HANDLE	subKey;
		swprintf(volumeName, L"%c", volume);
		
		RtlInitUnicodeString( &keyPath, volumeName);
		
		InitializeObjectAttributes(&objectAttributes,   
			&keyPath,   
			OBJ_CASE_INSENSITIVE| OBJ_KERNEL_HANDLE,//�Դ�Сд����     
			keyHandle,
			NULL);
		
		status = ZwCreateKey( &subKey,   
			KEY_ALL_ACCESS,   
			&objectAttributes,   
			0,   
			NULL,   
			REG_OPTION_VOLATILE,   // ��������Ч
			&ulResult);
		
		if (NT_SUCCESS(status))
		{
			HANDLE	subsubKey;
			RtlInitUnicodeString( &keyPath, L"DefaultIcon");
			
			InitializeObjectAttributes(&objectAttributes,   
				&keyPath,   
				OBJ_CASE_INSENSITIVE| OBJ_KERNEL_HANDLE,//�Դ�Сд����     
				subKey,
				NULL);
			
			status = ZwCreateKey( &subsubKey,   
				KEY_ALL_ACCESS,   
				&objectAttributes,   
				0,   
				NULL,   
				REG_OPTION_VOLATILE,   // ��������Ч
				&ulResult);
			
			if (NT_SUCCESS(status))
			{
				UNICODE_STRING	keyName;
				WCHAR iconPath[] = L"%SystemRoot%\\System32\\drivers\\diskflt.sys,0";
				WCHAR iconPathWin7[] = L"%SystemRoot%\\System32\\drivers\\diskflt.sys,1";

				RtlInitUnicodeString(&keyName, L"");
				
				if (*NtBuildNumber <= 2600)
				{
					status = ZwSetValueKey(subsubKey, &keyName, 0,REG_SZ, iconPath, sizeof(iconPath));
				}
				else
				{
					status = ZwSetValueKey(subsubKey, &keyName, 0,REG_SZ, iconPathWin7, sizeof(iconPathWin7));
				}				
				
				ZwClose(subsubKey);
			}
			
			ZwClose(subKey);
		}	
		
		ZwClose(keyHandle);
	}
}




ULONG GetProcessNameOffset(void)
{
    PEPROCESS Process = PsGetCurrentProcess();
	
    __try
    {
		ULONG i = 0;
        for (i = 0; i < PAGE_SIZE * 3; i++)
        {
            if (!strncmp("System", (char *)Process + i, 6))
            {
                return i;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        
    }
	
    return 0;
}

wchar_t * wcsstr_n(const wchar_t *string, size_t count, const wchar_t *strCharSet)
{
	wchar_t   *cp=(wchar_t *)string;   
	wchar_t   *s1, *s2;   
    
	if(!*strCharSet)   
		return ((wchar_t *)string);   
    
	while(count && *cp  )   
	{   
		s1   =   cp;
		s2   =   (wchar_t*)strCharSet;   
		
		while(*s1 && *s2 && !(toupper(*s1)-toupper(*s2)))   
			s1++,   s2++;   
		
		if(!*s2)   
			return(cp);   
		cp++;
		count--;
	}   
    
	return(NULL);   	
}


// ���ԡ�������
NTSTATUS
testPartition(WCHAR * partitionName)
{
	NTSTATUS	status;
	HANDLE		fileHandle;
	UNICODE_STRING	fileName;
	OBJECT_ATTRIBUTES	oa;
	IO_STATUS_BLOCK IoStatusBlock;
	PVOLUME_BITMAP_BUFFER	bitMap = NULL;

	
	RtlInitUnicodeString(&fileName, partitionName);
	
	InitializeObjectAttributes(&oa,
		&fileName,
		OBJ_CASE_INSENSITIVE,
		NULL,
		NULL);
	
	status = ZwCreateFile(&fileHandle,
		GENERIC_ALL | SYNCHRONIZE,
		&oa,
		&IoStatusBlock,
		NULL,
		0,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		FILE_OPEN,
		FILE_SYNCHRONOUS_IO_NONALERT,	// ͬ����д
		NULL,
		0);

	dprintf("Open %wZ ret 0x%x\n", &fileName, status);
	
	if (NT_SUCCESS(status))
	{		
		IO_STATUS_BLOCK	ioBlock;
		PVOLUME_BITMAP_BUFFER	info;
		STARTING_LCN_INPUT_BUFFER StartingLCN;

		ULONG	BitmapSize = 0;
		
		StartingLCN.StartingLcn.QuadPart = 0;
		
		
		// ���������˾�, �ڵõ���λͼǰ
		status = ZwFsControlFile( fileHandle, 
					NULL, 
					NULL, 
					NULL, 
					&ioBlock, 
					FSCTL_LOCK_VOLUME, 
					NULL, 0, NULL, 0
					);
		
		dprintf("FSCTL_LOCK_VOLUME = 0x%x\n", status);
		

		do 
		{
			BitmapSize += 10240;
			
			info = (PVOLUME_BITMAP_BUFFER)__malloc(BitmapSize);
			
			status = ZwFsControlFile( fileHandle, 
				NULL, 
				NULL, 
				NULL, 
				&ioBlock, 
				FSCTL_GET_VOLUME_BITMAP, 
				&StartingLCN,
				sizeof (StartingLCN),
				info, 
				BitmapSize
				);

			if (STATUS_BUFFER_OVERFLOW == status)
			{
				__free(info);
			}
			
		} while(STATUS_BUFFER_OVERFLOW == status);
		
		dprintf("FSCTL_GET_VOLUME_BITMAP ret 0x%x\n", status);

		if (!NT_SUCCESS(status))
		{
			__free(info);
		}
		else
		{
			dprintf("bitMapinfo (%d / %d) cluster = %I64d\n", ioBlock.Information, BitmapSize, info->BitmapSize.QuadPart);

			bitMap = info;
		}

		
		status = ZwFsControlFile( fileHandle, 
			NULL, 
			NULL, 
			NULL, 
			&ioBlock, 
			FSCTL_UNLOCK_VOLUME, 
			NULL, 0, NULL, 0
				);

		dprintf("FSCTL_UNLOCK_VOLUME ret 0x%x\n", status);
		

		ZwClose(fileHandle);
	}

	if (bitMap)
	{
		__free(bitMap);
	}
	
	return status;
}


VOID flt_initializeVolume()
{
	NTSTATUS	status;
	ULONG	i;
	for (i = 0; i < _countof(_volumeList); i++)
	{
		_volumeList[i].volume = (WCHAR)i + L'A';
		
		// ��ѯҪ���������Ϣ
		if (_protectInfo.volumeInfo[i] && (_volumeList[i].volume > L'B')
			&& (!_volumeList[i].isValid))
		{
			status = flt_getVolumeInfo(_volumeList[i].volume, &_volumeList[i]);
			
			// ���±���״̬
			if (NT_SUCCESS(status))
			{
				status = flt_initVolumeLogicBitMap(&_volumeList[i]);
				
				_signal = TRUE;
				
				if (!NT_SUCCESS(status))
				{
					dprintf("flt_initVolumeLogicBitMap error 0x%x .\n", status);
					_protectInfo.volumeInfo[i] = 0;
					continue;
				}
				
				_volumeList[i].isValid = TRUE;
				_volumeList[i].isProtect = TRUE;
				
				dprintf("disk %c diskNumber = %d PartitionNumber: %d protect : %d\n"
					"offset = 0x%08I64x len = 0x%08I64x dataStart = 0x%08I64x\n\n", 
					_volumeList[i].volume, _volumeList[i].diskNumber, _volumeList[i].partitionNumber, _volumeList[i].isProtect,
					_volumeList[i].physicalStartingOffset, _volumeList[i].bytesTotal,
					_volumeList[i].firstDataSector);
				
			}
		}
	}
}

ULONG
getTickCount() 
{ 
	LARGE_INTEGER tickCount; 
	KeQueryTickCount(&tickCount);
	tickCount.QuadPart *= KeQueryTimeIncrement(); 
	tickCount.QuadPart /=  10000; 
	return tickCount.LowPart; 
} 

void
getRandomString(PWCHAR random)
{
	ULONG	tick = getTickCount();
	int		mask[9] = {12, 25, 36, 44, 54, 61, 78, 33, 65};
	int		i = 0;
	
	for (i = 0; i < 9; i++)
	{
		if (tick / mask[i] % 2)
			random[i] = (tick / mask[i] % 26) + 'A';
		else
			random[i] = (tick / mask[i] % 26) + 'a';
	}
	
	random[9] = '\0';
}


#define RtlInitEmptyUnicodeString(_ucStr,_buf,_bufSize) \
    ((_ucStr)->Buffer = (_buf), \
	(_ucStr)->Length = 0, \
     (_ucStr)->MaximumLength = (USHORT)(_bufSize))

VOID
ImageNotifyRoutine(
				   IN PUNICODE_STRING  FullImageName,
				   IN HANDLE  ProcessId, // where image is mapped
				   IN PIMAGE_INFO  ImageInfo
				   )
{
	static BOOL	isSetIcon = FALSE;

	NTSTATUS	status;

	if ((!isSetIcon) && FullImageName && wcsstr_n(FullImageName->Buffer, FullImageName->Length / sizeof(WCHAR), L"winlogon.exe"))
	{
		ULONG	protectNumber = 0;
		ULONG	i = 0;
		// �ٳ�ʼ��һ�Σ���ֹһЩ�ܱ����ľ�û�б���ʼ, ���ʱ���ʼ�Ƚ��ȶ�
		flt_initializeVolume();
		for (i = 0; i < _countof(_volumeList); i++) {
			if (_volumeList[i].isValid && _volumeList[i].isProtect)
			{
				// �ı��ܱ����ľ��Ĭ��ͼ��
				changeDriveIcon(_volumeList[i].volume);
				protectNumber++;
			}
		}
		isSetIcon = TRUE;
		// �������Ҫ��������������������������
		if (protectNumber)
		{
			// Ĭ�ϲ�����
			_sysPatchEnable = TRUE;	
		}
	}

	if(	(!_sysPatchEnable)
		|| (!ImageInfo->SystemModeImage)
		|| (FullImageName == NULL) 
		|| (FullImageName->Length == 0)
		|| (FullImageName->Buffer == NULL)
		)
	{
		return;
	}

	status = IsFileCreditable(FullImageName);

	if (!NT_SUCCESS(status)) 
	{
		ULONG	start;
		WCHAR	buf[512];
		WCHAR	random[50];
		UNICODE_STRING	msg;
		UNICODE_STRING	caption;

		// ������Ϊ�������
		getRandomString(random);
		RtlInitUnicodeString(&caption, random);

		RtlInitEmptyUnicodeString(&msg, buf, sizeof(buf));
		RtlAppendUnicodeToString(&msg, L"Load [");
		RtlAppendUnicodeStringToString(&msg, FullImageName);
		RtlAppendUnicodeToString(&msg, L"] ?");

		start = getTickCount();

		if (ResponseYes == kMessageBox(&msg, &caption, OptionYesNo, MB_ICONINFORMATION | MB_SETFOREGROUND | MB_DEFBUTTON2))
		{
			status = STATUS_SUCCESS;
		}

		// �˲����ܵ����ô�죬�ܾ�����
		if ((getTickCount() - start) < 500)
		{
			status = STATUS_UNSUCCESSFUL;
		}
	}

	// �����ŵ�����ȫ��XX��
	if (!NT_SUCCESS(status))
	{
		/**
		* 00410070 >    B8 220000C0   mov     eax, C0000022 // STATUS_ACCESS_DENIED
		* 00410075      C2 0800       retn    8
		*/
		BYTE	patchCode[] = {0xB8, 0x22, 0x00, 0x00, 0xC0, 0xC2, 0x08, 0x00};
		// PATCH����
		PIMAGE_DOS_HEADER	imageDosHeader = (PIMAGE_DOS_HEADER)ImageInfo->ImageBase;

		if (IMAGE_DOS_SIGNATURE == imageDosHeader->e_magic)
		{
			PIMAGE_NT_HEADERS	imageNtHeaders = (PIMAGE_NT_HEADERS)((UINT)ImageInfo->ImageBase + imageDosHeader->e_lfanew);
			if (IMAGE_NT_SIGNATURE == imageNtHeaders->Signature)
			{
				WriteReadOnlyMemory((LPBYTE)ImageInfo->ImageBase + imageNtHeaders->OptionalHeader.AddressOfEntryPoint, patchCode, sizeof(patchCode));
			}
		}
	}
}

VOID
flt_reinitializationRoutine( 
	IN	PDRIVER_OBJECT	DriverObject, 
	IN	PVOID			Context, 
	IN	ULONG			Count 
	)
{
	NTSTATUS	status;
	
	//�����豸�Ĵ����̵߳��߳̾��
	HANDLE		ThreadHandle = NULL;
	flt_initializeVolume();
	
	//��ʼ�����������������
	InitializeListHead(&_deviceExtension->list_head);
	//��ʼ����������е���
	KeInitializeSpinLock(&_deviceExtension->list_lock);
	//��ʼ����������е�ͬ���¼�
	KeInitializeEvent(
		&_deviceExtension->ReqEvent,
		SynchronizationEvent,
		FALSE
		);
	
	//��ʼ����ֹ�����̱߳�־
	_deviceExtension->terminate_thread = FALSE;
	//����������������������Ĵ����̣߳��̺߳����Ĳ��������豸��չ
	status = PsCreateSystemThread(
		&ThreadHandle,
		(ACCESS_MASK)0L,
		NULL,
		NULL,
		&_deviceExtension->thread_read_write_id,			
		flt_thread_read_write,
		_deviceExtension
		);
	
	if (!NT_SUCCESS(status))
		goto __faild;
	
	
	//��ȡ�����̵߳Ķ���
	status = ObReferenceObjectByHandle(
		ThreadHandle,
		THREAD_ALL_ACCESS,
		NULL,
		KernelMode,
		&_deviceExtension->thread_read_write,
		NULL
		);

	if (NULL != ThreadHandle)
		ZwClose(ThreadHandle);

	if (!NT_SUCCESS(status))
	{
		_deviceExtension->terminate_thread = TRUE;
		KeSetEvent(
			&_deviceExtension->ReqEvent,
			(KPRIORITY)0,
			FALSE
			);
		goto __faild;
	}

	if (*NtBuildNumber <= 3790) {
		// ���������ص�
		PsSetLoadImageNotifyRoutine(&ImageNotifyRoutine);
	}


	_deviceExtension->Protect = TRUE;
	_signal = FALSE;

__faild:
	//�ر��߳̾�������ǽ�󲻻��õ��������ж��̵߳����ö�ͨ���̶߳�����������
	if (NULL != ThreadHandle)
		ZwClose(ThreadHandle);
}


NTSTATUS
WriteReadOnlyMemory(
	LPBYTE	dest,
	LPBYTE	src,
	ULONG	count
	)
	/**
	* һ���ڴ��Ƿ��д��ֻ����������ڴ���������йء� �����Ҫдһ��ֻ���ڴ棬
	* �������½���һ����д��������,ʹ����ָ��ͬһ���ڴ�Ϳ����ˡ�
	*/
{
	NTSTATUS	status;
	KSPIN_LOCK	spinLock;
	KIRQL		oldIrql;
	PMDL		pMdlMemory;
	LPBYTE		lpWritableAddress;
	
	status = STATUS_UNSUCCESSFUL;
	
	KeInitializeSpinLock(&spinLock);
	
	pMdlMemory = IoAllocateMdl(dest, count, FALSE, FALSE, NULL);
	
	if (NULL == pMdlMemory) return status;
	
	MmBuildMdlForNonPagedPool(pMdlMemory);
    MmProbeAndLockPages(pMdlMemory, KernelMode, IoWriteAccess);
	lpWritableAddress = MmMapLockedPages(pMdlMemory, KernelMode);
    if (NULL != lpWritableAddress)
	{
		oldIrql	= 0;
		KeAcquireSpinLock(&spinLock, &oldIrql);
		
		RtlCopyMemory(lpWritableAddress, src, count);
		
		KeReleaseSpinLock(&spinLock, oldIrql);
		MmUnmapLockedPages(lpWritableAddress, pMdlMemory);
		
		status = STATUS_SUCCESS;
	}
	
	MmUnlockPages(pMdlMemory);
    IoFreeMdl(pMdlMemory);
	
	return	status;
}

// �ж��ļ��Ƿ���ţ�Ҳ�����ж��ļ��Ƿ���ԭʼû�б�����������
// ֻҪ�ж��ļ����ڵ�����û�б��ض��򼴿�

NTSTATUS
IsFileCreditable(PUNICODE_STRING filePath)
{
	NTSTATUS	status;
	HANDLE		fileHandle = (HANDLE)-1;
	PFILE_OBJECT	fileObject = NULL;	
	PRETRIEVAL_POINTERS_BUFFER	pVcnPairs = NULL;
	PVOLUME_INFO	volumeInfo = NULL;
	ULONG	sectorsPerCluster;

	PVOID	RestartKey = 0;
	PVOID	Element;

	BOOLEAN	IsCreditable = FALSE;

	status = flt_getFileHandleReadOnly(&fileHandle, filePath);

	if (!NT_SUCCESS(status))
	{
		dprintf("Open %wZ ret 0x%x\n", filePath, status);
		goto __faild;
	}

	status = ObReferenceObjectByHandle(fileHandle, 0, NULL, KernelMode, (PVOID *)&fileObject, NULL);
	
	if (!NT_SUCCESS(status))
	{
		goto __faild;
	}

	if (FILE_DEVICE_NETWORK_FILE_SYSTEM != fileObject->DeviceObject->DeviceType)
	{
		UNICODE_STRING	uniDosName;
		// �õ�����C:�������̷���Ϊ�˻�ȡVolumeInfo
		status = RtlVolumeDeviceToDosName(fileObject->DeviceObject, &uniDosName); 
		
		if (NT_SUCCESS(status))
		{
			volumeInfo = &_volumeList[toupper(*(WCHAR *)uniDosName.Buffer) - L'A'];
			ExFreePool(uniDosName.Buffer);

			if ((!volumeInfo->isValid) || (!volumeInfo->isProtect))
			{
				goto __faild;
			}
		}
	}

	if (!volumeInfo)
	{
		goto __faild;
	}

	sectorsPerCluster = volumeInfo->bytesPerCluster / volumeInfo->bytesPerSector;

	pVcnPairs = getFileClusterList(fileHandle);
	
	if(NULL == pVcnPairs)
	{
		dprintf("getFileClusterList fail\n");
		goto __faild;
	}
	
    RestartKey = NULL;
    for (Element = RtlEnumerateGenericTableWithoutSplaying(&volumeInfo->redirectMap, &RestartKey);
         Element != NULL;
         Element = RtlEnumerateGenericTableWithoutSplaying(&volumeInfo->redirectMap, &RestartKey)) 
	{
		ULONG	Cls, r;
		LARGE_INTEGER	PrevVCN = pVcnPairs->StartingVcn;
		for (r = 0, Cls = 0; r < pVcnPairs->ExtentCount; r++)
		{
			ULONG	CnCount;
			LARGE_INTEGER Lcn = pVcnPairs->Extents[r].Lcn;

			for (CnCount = (ULONG)(pVcnPairs->Extents[r].NextVcn.QuadPart - PrevVCN.QuadPart);
			CnCount; CnCount--, Cls++, Lcn.QuadPart++) 
			{
				ULONGLONG	i = 0;
				ULONGLONG	base = volumeInfo->firstDataSector + (Lcn.QuadPart * sectorsPerCluster);
				for (i = 0; i < sectorsPerCluster; i++)
				{
					// ���������ض�����, �������ļ�, ��ֹ��֤
					if (((PPAIR)Element)->orgIndex == (base + i))
					{
						// ............
						goto __exit;
					}
				}  
			}
			PrevVCN = pVcnPairs->Extents[r].NextVcn;
		}
	}

	// ��������
	IsCreditable = TRUE;

__exit:
	
	__free_Safe(pVcnPairs);
	
__faild:

	if (fileObject)
		ObDereferenceObject(fileObject);

	if (((HANDLE)-1 != fileHandle))
		ZwClose(fileHandle);

	return IsCreditable ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

//////////////////////////////////////////////////////////////////////////
PDEVICE_OBJECT on_diskperf_driver_entry(PDRIVER_OBJECT DriverObject,PUNICODE_STRING reg)
{
	NTSTATUS			status; 
	PDEVICE_OBJECT		deviceObject = NULL;
	BOOLEAN				symbolicLink = FALSE;
	UNICODE_STRING		ntDeviceName;
	PFILTER_DEVICE_EXTENSION	deviceExtension;
	UNICODE_STRING		dosDeviceName;

	RtlInitUnicodeString(&ntDeviceName, DISKFILTER_DEVICE_NAME_W);	
	
    status = IoCreateDevice(
		DriverObject,
		sizeof(FILTER_DEVICE_EXTENSION),		// DeviceExtensionSize
		&ntDeviceName,					// DeviceName
		FILE_DEVICE_DISKFLT,			// DeviceType
		0,								// DeviceCharacteristics
		TRUE,							// Exclusive �������ҪΪFALSE,Ҫ��ȻCreateFileֻ�ܴ�һ��, ��ʹ�õùرյ�
		&deviceObject					// [OUT]
		);
	
	if (!NT_SUCCESS(status))
	{
		dprintf("IoCreateDevice failed(0x%x).\n", status);
		goto failed;
	}

//	deviceObject->Flags |= DO_BUFFERED_IO; 

	deviceExtension = (PFILTER_DEVICE_EXTENSION)deviceObject->DeviceExtension;

	RtlInitUnicodeString(&dosDeviceName, DISKFILTER_DOS_DEVICE_NAME_W);

	status = IoCreateSymbolicLink(&dosDeviceName, &ntDeviceName);
	if (!NT_SUCCESS(status))
    {
        dprintf("IoCreateSymbolicLink failed(0x%x).\n", status);
		goto failed;
    }

	// ��ʼ���ڴ��
	mempool_init();

	// ��ʼ�����
	memset(&_volumeList, 0, sizeof(_volumeList));
	memset(&_lowerDeviceObject, 0, sizeof(_lowerDeviceObject));

	_sysPatchEnable = FALSE;

	// ��ʼ��Ϊ�Ǳ���״̬
	deviceExtension->Protect = FALSE;
	
	// ��ֵȫ�ֱ���
	_deviceExtension = deviceExtension;

	_systemProcessId = (ULONG)PsGetCurrentProcessId();
	_processNameOfffset = GetProcessNameOffset();

	
	//ע��һ��boot���������ص�������ص������������е�boot���������������֮����ȥִ��
  	IoRegisterBootDriverReinitialization(
  		DriverObject,
 		flt_reinitializationRoutine,
  		NULL
  		);	

    if (NT_SUCCESS(status))
	    return deviceObject;

failed:
	
	if (symbolicLink)
		IoDeleteSymbolicLink(&dosDeviceName);
	
	if (deviceObject)
		IoDeleteDevice(deviceObject);

	return deviceObject;
}


VOID on_diskperf_driver_unload(PDRIVER_OBJECT DriverObject)
{
	UNICODE_STRING dosDeviceName;

	ULONG	i = 0;
	
	//
    // Free any resources
    //

	//������Ҫ��һЩ������
	if (_deviceExtension->terminate_thread != TRUE && NULL != _deviceExtension->thread_read_write)
	{
		_deviceExtension->Protect = FALSE;
		//����̻߳������еĻ���Ҫֹͣ��������ͨ�������߳�ֹͣ���еı�־���ҷ����¼���Ϣ�����߳��Լ���ֹ����
		_deviceExtension->terminate_thread = TRUE;
		KeSetEvent(
			&_deviceExtension->ReqEvent,
			(KPRIORITY) 0,
			FALSE
			);
		//�ȴ��߳̽���
		KeWaitForSingleObject(
			_deviceExtension->thread_read_write,
			Executive,
			KernelMode,
			FALSE,
			NULL
			);

		//��������̶߳���
		ObDereferenceObject(_deviceExtension->thread_read_write);

		
		for (i = 0; i < _countof(_volumeList); i++)
		{
			// �ͷ���Դ
			DPBitMap_Free(_volumeList[i].bitMap_Redirect);
			DPBitMap_Free(_volumeList[i].bitMap_Protect);
			DPBitMap_Free(_volumeList[i].bitMap_Free);		
			{
				PVOID	RestartKey = 0;
				PVOID	Element;
				
				RestartKey = 0;  // Always get the first element
				while ((Element = RtlEnumerateGenericTableWithoutSplaying(&_volumeList[i].redirectMap, (PVOID *)&RestartKey)) != NULL) 
				{
					RtlDeleteElementGenericTable(&_volumeList[i].redirectMap, Element);		   
					RestartKey = 0;
				}
			}
		}
	}

	// �ͷ��ڴ��
	mempool_fini();

    //
    // Delete the symbolic link
    //
	
    RtlInitUnicodeString(&dosDeviceName, DISKFILTER_DOS_DEVICE_NAME_W);
	
    IoDeleteSymbolicLink(&dosDeviceName);
	
    //
    // Delete the device object
    //
	
    IoDeleteDevice(DriverObject->DeviceObject);
	
    dprintf("[disk Filter] unloaded\n");
}

// ������أ�����TRUE statusΪ״̬��
BOOLEAN on_diskperf_dispatch(
	PDEVICE_OBJECT dev,
    PIRP irp,
	NTSTATUS *status)
{
	ULONG				ioControlCode;
	PIO_STACK_LOCATION	irpSp;
	PVOID				ioBuffer;
    ULONG				inputBufferLength, outputBufferLength;
	irpSp = IoGetCurrentIrpStackLocation(irp);

	ioControlCode		= irpSp->Parameters.DeviceIoControl.IoControlCode;
	ioBuffer			= irp->AssociatedIrp.SystemBuffer;
    inputBufferLength	= irpSp->Parameters.DeviceIoControl.InputBufferLength;
    outputBufferLength	= irpSp->Parameters.DeviceIoControl.OutputBufferLength;
    ioControlCode		= irpSp->Parameters.DeviceIoControl.IoControlCode;

	irp->IoStatus.Information = 0;

	switch (ioControlCode)
    {
	case IOCTL_DISKFLT_LOCK:
		{
			BYTE	md5[16];
			CalcMD5(ioBuffer, inputBufferLength, &md5);
			if (sizeof(md5) == RtlCompareMemory(md5, _protectInfo.passWord, sizeof(md5)))
			{
				InterlockedExchange(&_lockProcessId, (ULONG)PsGetCurrentProcessId());
 				*status = STATUS_SUCCESS;
			}
			else
			{
				*status = STATUS_ACCESS_DENIED;
			}

		}
		break;
	case IOCTL_DISKFLT_UNLOCK:
		{
			InterlockedExchange(&_lockProcessId, -1);
			irp->IoStatus.Information = 0;
			*status = STATUS_SUCCESS;
		}
		break;
		
	case IOCTL_DISKFLT_GETINFO:
		{
			if (outputBufferLength >= sizeof(PROTECT_INFO))
			{
				irp->IoStatus.Information = sizeof(PROTECT_INFO);
				memcpy(ioBuffer, &_protectInfo, sizeof(PROTECT_INFO));
				*status = STATUS_SUCCESS;
			}
			else
			{
				*status = STATUS_INSUFFICIENT_RESOURCES;
			}
		}
		break;
	case IOCTL_DISKFLT_PROTECTSYS_STATE:
		{
			*status = _sysPatchEnable ? STATUS_SUCCESS : STATUS_NOT_IMPLEMENTED;
		}
		break;
	case IOCTL_DISKFLT_LOGIN:
	case IOCTL_DISKFLT_PROTECTSYS:
	case IOCTL_DISKFLT_NOPROTECTSYS:
		{
			BYTE	md5[16];
			CalcMD5(ioBuffer, inputBufferLength, &md5);
			if (sizeof(md5) == RtlCompareMemory(md5, _protectInfo.passWord, sizeof(md5)))
			{
				if (IOCTL_DISKFLT_PROTECTSYS == ioControlCode)
				{
					InterlockedExchange(&_sysPatchEnable, TRUE);
				}
				else if (IOCTL_DISKFLT_NOPROTECTSYS == ioControlCode)
				{
					InterlockedExchange(&_sysPatchEnable, FALSE);
				}
				
				*status = STATUS_SUCCESS;
			}
			else
			{
				*status = STATUS_ACCESS_DENIED;
			}
		}
		break;

	default:
		irp->IoStatus.Information = 0;
		*status = STATUS_SUCCESS;
		break;
	}

 	flt_CompleteRequest(
		irp,
		*status,
		IO_NO_INCREMENT
 		);

	return TRUE;
}

// ������أ�����TRUE statusΪ״̬��
BOOLEAN on_diskperf_read_write(
					 IN PUNICODE_STRING physics_device_name,
					 IN ULONG	device_type,
					 IN ULONG device_number,
					 IN ULONG partition_number,
					 IN PDEVICE_OBJECT device_object,
					 IN PIRP Irp,
					 IN NTSTATUS *status)
{
	
	PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation( Irp );
	ULONG	i = 0;

	//irp�е����ݳ���
	ULONG				length = 0;
	//irpҪ�����ƫ����
	LARGE_INTEGER		offset = { 0 };

	if (!_deviceExtension->Protect)
	{
		if (_signal && IRP_MJ_WRITE == irpStack->MajorFunction)
		{
			//�к��ٻ��ᴥ�����
			dprintf(">> ��ȡλͼ����ʼ�����м�������д��\n");
		}
		return FALSE;
	}

	// �Ź�ָ�����̵Ķ�д
	if (PsGetCurrentProcessId() == _lockProcessId)
	{
		return FALSE;
	}

	if (PsGetCurrentThreadId() == _deviceExtension->thread_read_write_id.UniqueThread)
	{
		return FALSE;
	}

	if (IRP_MJ_WRITE == irpStack->MajorFunction)
	{
		offset = irpStack->Parameters.Write.ByteOffset;
		length = irpStack->Parameters.Write.Length;

		// ����MBR, �����ϵĵ�һ������
		if (offset.QuadPart < 512)
		{
			flt_CompleteRequest(
				Irp,
				STATUS_ACCESS_DENIED,
				IO_NO_INCREMENT
				);
			return TRUE;
		}
	}
	else if (IRP_MJ_READ == irpStack->MajorFunction)
	{
		offset = irpStack->Parameters.Read.ByteOffset;
		length = irpStack->Parameters.Read.Length;
	}
	else
	{
		// ����֮�⣬offset��length����0
		offset.QuadPart = 0;
		length = 0;
	}

	for (i = 0; i < _countof(_volumeList); i++)
	{
		// ���Ƿ���Ч
		if ((!_volumeList[i].isValid) || (!_volumeList[i].isProtect))
			continue;

		// ���Ƿ����ܱ�����Ӳ����
		if (_volumeList[i].diskNumber != device_number)
			continue;

		if ((offset.QuadPart >= _volumeList[i].physicalStartingOffset) &&
			((offset.QuadPart - _volumeList[i].physicalStartingOffset) <= _volumeList[i].bytesTotal)
			)
		{
			//������ڱ���״̬��
			//�������Ȱ����irp��Ϊpending״̬
			IoMarkIrpPending(Irp);

			// ��IRP�е�IoStatus.Pointer���ݾ�����, ����������������ò���
			Irp->IoStatus.Pointer = (PVOID)i;

			//Ȼ�����irp�Ž���Ӧ�����������
			ExInterlockedInsertTailList(
				&_deviceExtension->list_head,
				&Irp->Tail.Overlay.ListEntry,
				&_deviceExtension->list_lock
				);
			//���ö��еĵȴ��¼���֪ͨ���ж����irp���д���
			KeSetEvent(
				&_deviceExtension->ReqEvent, 
				(KPRIORITY)0, 
				FALSE);
			//����pending״̬�����irp���㴦������
			*status = STATUS_PENDING;

			// TRUE��ʼIPR������
			return TRUE;
		}
	}


//	dprintf("offset %I64d not protected (%d)\n", offset.QuadPart, irpStack->MajorFunction);
	// //������ڱ���״̬��ֱ�ӽ����²��豸���д���
	return FALSE;
}

VOID on_diskperf_new_disk(
			IN PDEVICE_OBJECT device_object,
			IN PUNICODE_STRING physics_device_name,
			IN ULONG device_type,			
			IN ULONG disk_number,
			IN ULONG partition_number)
{
	// �����豸
	if (disk_number < _countof(_lowerDeviceObject))
	{
		_lowerDeviceObject[disk_number] = device_object;
	}
	// ��Ӳ�̹ҽ�
	dprintf("new disk %wZ %d %d %d\n", physics_device_name, device_type, disk_number, partition_number);
}

VOID
on_diskperf_remove_disk(
	IN PDEVICE_OBJECT device_object,
	IN PUNICODE_STRING physics_device_name
	)
{
	
}