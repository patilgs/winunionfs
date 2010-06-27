/*
    Copyright (c) 2010 Carol Szabo cszaboads@gmail.com

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    GPL clarification: Works hosted or built on filesystems created by this 
    work do not become "covered works" simply because this work was used in
    its object form to create them.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "stdafx.h"
#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <set>
#include <string>
using namespace std;

#include "dokan.h"

bool gDebugMode = false;

class MutexLock
{
public:
	MutexLock(HANDLE mutex) : mHandle(mutex)
	{
		if (WaitForSingleObject(mutex, INFINITE) != WAIT_OBJECT_0)
			throw 1;
	}
	~MutexLock() {
		ReleaseMutex(mHandle);
	}
private:
	HANDLE mHandle;
};
class FilePathSet : public set<wstring>
{
public:
	FilePathSet() : set<wstring>() {
		if (!(mhMutex = CreateMutex(NULL,FALSE,NULL)))
			throw 2;
	}
	~FilePathSet() {
		CloseHandle(mhMutex);
	}
	HANDLE mhMutex;
};
FilePathSet gDeletedFilesSet;

static void DbgPrint(LPCWSTR aFormat, ...)
{
	if (gDebugMode) {
		va_list argp;
		va_start(argp, aFormat);
		vfwprintf(stderr, aFormat, argp);
		va_end(argp);
	}
}
/* The length of buffers used to store file paths */
#define MAX_PATHW 32768
#define MAX_PATHB (MAX_PATHW*sizeof(WCHAR))
static WCHAR gReadRootDirectory[MAX_PATHW] = L"C:\\ReadRoot";
static WCHAR gWriteRootDirectory[MAX_PATHW] = L"C:\\WriteRoot";
size_t gReadRootDirectoryLength, gWriteRootDirectoryLength;
#define AdvanceBytes(pointer, bytes) ((WCHAR*)((char*)pointer + bytes))
/** This function concatenates aRootPath with aRelativePath and puts the result in aDest.
 * It is assumed that aDest is at least MAX_PATHB bytes long.
 * If the concatenated size of string would be too big for the minimum size buffer to hold, a NUL is put at the begining of the buffer
 * and true is returned.
 * aRootPathLength and aRelativepathLength are in bytes and do not include the terminating NUL, hence either aRootPath nor aRelativepath
 * need to be NUL terminated.
 * aDest is going to be NUL terminated.
 */
static bool PatchPath(LPWSTR aDest, LPCWSTR aRootPath, LPCWSTR aRelativePath, size_t aRootPathLength, size_t aRelativePathLength)
{
	if (aRelativePathLength + aRootPathLength >= MAX_PATHB) {
		DbgPrint(L"Path too long: %s.\n", aRelativePath);
		/* Force an error*/
		*aDest = 0;
		return true;
	}
	memcpy(aDest, aRootPath, aRootPathLength);
	aDest = AdvanceBytes(aDest, aRootPathLength);
	memcpy(aDest, aRelativePath, aRelativePathLength);
	*(AdvanceBytes(aDest, aRelativePathLength)) = L'\0';
	return false;
}

/** Constants used by GetFiepath to indicate where a file is mapped from.
 */
#define UFS_FAILED -1
#define UFS_READ_AREA 0
#define UFS_WRITE_AREA FILE_ATTRIBUTE_ARCHIVE
#define UFS_OPENED_FOR_READING FILE_ATTRIBUTE_ENCRYPTED
#define UFS_OPENED_FOR_WRITING FILE_ATTRIBUTE_HIDDEN
#define UFS_SHARE_READ FILE_ATTRIBUTE_NOT_CONTENT_INDEXED
#define UFS_SHARE_WRITE FILE_ATTRIBUTE_OFFLINE
#define UFS_SHARE_DELETE FILE_ATTRIBUTE_READONLY
#define UFS_UNSAVED_FLAGS (~(UFS_WRITE_AREA | UFS_OPENED_FOR_READING |UFS_OPENED_FOR_WRITING | UFS_SHARE_READ | UFS_SHARE_WRITE | UFS_SHARE_DELETE))

static inline bool CheckDeletedClean(const wstring& aRelativePath)
{
	MutexLock(gDeletedFilesSet.mhMutex);
	return gDeletedFilesSet.find(aRelativePath) != gDeletedFilesSet.end();
}

static inline void CleanFilename(wstring& aRelativePath) {
	wstring::iterator end = aRelativePath.end();
	for (wstring::iterator it = aRelativePath.begin(); it != end; ++it)
		if (*it == L'/')
			*it=L'\\';
		else
			*it = towupper(*it);
}

static inline bool CheckDeleted(wstring& aRelativePath) {
	CleanFilename(aRelativePath);
	return CheckDeletedClean(aRelativePath);
}

static inline bool CheckDeleted(LPCWSTR aRelativePath) {
	wstring relativePath(aRelativePath);
	return CheckDeleted(relativePath);
}
/* This function returns the target file path from a source file path
 * @params:
 * aFilepath - Pointer to a destination buffer, at least MAX_PATH bytes long.
 * aFileName - Pointer to the source file path excluding the drive letter in the mounted file system.
 * N/A aReadOnly - true if the file is opened in ReadOnly mode
 * In case of an error a 0 length path is returned in the output buffer.
 * @return If the function is successful it returns UFS_READ_AREA or UFS_WRITE_AREA, depending on where the file is found.
 * otherwise it return s UFS_FAILED.
 */
static int GetFilePath(PWCHAR aFilepath, LPCWSTR aFileName/*, bool aReadOnly*/)
{
	size_t filenameLength = wcslen(aFileName)*sizeof(WCHAR);
	if(PatchPath(aFilepath, gWriteRootDirectory, aFileName, gWriteRootDirectoryLength, filenameLength))
		return UFS_FAILED;
	if (GetFileAttributes(aFilepath) != INVALID_FILE_ATTRIBUTES)
		return UFS_WRITE_AREA;
	try {
		if (CheckDeleted(aFileName))
			return UFS_WRITE_AREA;
	} catch (...) {
		DbgPrint(L"Exception thrown in UFSGetFilepath.");
		return UFS_FAILED;
	}
	if (PatchPath(aFilepath, gReadRootDirectory, aFileName, gReadRootDirectoryLength, filenameLength))
		return UFS_FAILED;
	return UFS_READ_AREA;
}


static inline ULONG64 MakeContext(HANDLE aHandle, bool aIsInWriteArea, DWORD aAccessMode, DWORD aShareMode, DWORD aFlags)
{
	aFlags &= UFS_UNSAVED_FLAGS;
	if (aIsInWriteArea)
		aFlags |= UFS_WRITE_AREA;
	if (aAccessMode & FILE_WRITE_DATA)
		aFlags |= UFS_OPENED_FOR_WRITING;
	if (aAccessMode & FILE_READ_DATA)
		aFlags |= UFS_OPENED_FOR_READING;
	if (aShareMode & FILE_SHARE_DELETE)
		aFlags |= UFS_SHARE_DELETE;
	if (aShareMode & FILE_SHARE_READ)
		aFlags |= UFS_SHARE_READ;
	if (aShareMode & FILE_SHARE_WRITE)
		aFlags |= UFS_SHARE_WRITE;
	return ((((ULONG64)aFlags)<<32)|((ULONG64)aHandle));
}

static inline void GetCreateDataFromContext(ULONG64 context, DWORD* apAccessMode, DWORD* apShareMode, DWORD* apFlags)
{
	*apAccessMode = (context & (UFS_OPENED_FOR_WRITING << 32)) ? GENERIC_WRITE : 0;
	if (context & (UFS_OPENED_FOR_READING << 32))
		*apAccessMode |= GENERIC_READ;
	*apShareMode = (context & (UFS_SHARE_READ << 32)) ? FILE_SHARE_READ : 0;
	if (context & (UFS_SHARE_DELETE << 32))
		*apShareMode |= FILE_SHARE_DELETE;
	if (context & (UFS_SHARE_WRITE << 32))
		*apShareMode |= FILE_SHARE_WRITE;
	*apFlags = (DWORD)((context >> 32) & UFS_UNSAVED_FLAGS);
}
#define IsInWriteArea(context) (context & (((ULONG64)UFS_WRITE_AREA)<<32))
#define GetHandle(context) ((HANDLE)(context & 0xFFFFFFFF))

static bool CreateParentDirectories(LPWSTR aFileNamePlusRoot, LPWSTR aFileNameEnd)
{
	for (LPWSTR lpBackSlash=aFileNameEnd;lpBackSlash>aFileNamePlusRoot; --lpBackSlash)
		switch (*lpBackSlash) {
			case L'\\':
			case L'/':
				*lpBackSlash=L'\0';
				if (GetFileAttributes(aFileNamePlusRoot-gWriteRootDirectoryLength) != INVALID_FILE_ATTRIBUTES) {
					*lpBackSlash = L'\\';
					return false;
				}
				if (CreateParentDirectories(aFileNamePlusRoot, lpBackSlash-2))
					return true;
				if (!CreateDirectory(aFileNamePlusRoot-gWriteRootDirectoryLength, NULL))
					return true;
				*lpBackSlash = L'\\';
				return false;
		}
	return false;
}
/** This function creates the parent directories for aFileName.
 * aFileName must be under gWriteRootPath
 * @return false on success.
 */
static bool CreateParentDirectories(LPWSTR aFileName)
{
	aFileName = AdvanceBytes(aFileName, gWriteRootDirectoryLength);
	return CreateParentDirectories(aFileName, wcschr(aFileName, L'\0')-2);
}

static int DOKAN_CALLBACK UFSCreateFile(LPCWSTR aFileName, DWORD aAccessMode,DWORD aShareMode, DWORD aCreationDisposition, DWORD aFlagsAndAttributes, PDOKAN_FILE_INFO apDokanFileInfo)
{
	DbgPrint(L"CreateFile called with %s, %d, %d, %d, %d, %p.", aFileName, aAccessMode, aShareMode, aCreationDisposition, aFlagsAndAttributes, apDokanFileInfo);
	WCHAR writeFilepath[MAX_PATHW], readFilepath[MAX_PATHW], *filePath;
	size_t filenameLength = wcslen(aFileName)*sizeof(WCHAR);
	if(PatchPath(writeFilepath, gWriteRootDirectory, aFileName, gWriteRootDirectoryLength, filenameLength)) {
		DbgPrint(L"Path too long write.\n");
		return -ERROR_NOT_SUPPORTED;
	}
	bool shouldUndelete = false;
	wstring cleanedFilename;
	DWORD fileAttributes = GetFileAttributes(writeFilepath);
	if (fileAttributes != INVALID_FILE_ATTRIBUTES) {
		filePath = writeFilepath;
		if (fileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			aFlagsAndAttributes |= FILE_FLAG_BACKUP_SEMANTICS;
	} else {
		if (PatchPath(readFilepath, gReadRootDirectory, aFileName, gReadRootDirectoryLength, filenameLength)) {
			DbgPrint(L"Path too long read.\n");
			return -ERROR_NOT_SUPPORTED;
		}
		if ((fileAttributes = GetFileAttributes(readFilepath)) != INVALID_FILE_ATTRIBUTES) {
			if (fileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				aFlagsAndAttributes |= FILE_FLAG_BACKUP_SEMANTICS;
			try {
				cleanedFilename = aFileName;
				CleanFilename(cleanedFilename);
				if (CheckDeletedClean(cleanedFilename)) {
					filePath = writeFilepath;
					shouldUndelete = true;
				} else
					switch (aCreationDisposition) {
						case CREATE_ALWAYS:
						case TRUNCATE_EXISTING:
							if(CreateParentDirectories(writeFilepath)) {
								DbgPrint(L"CreateParentDirectoriesFailed.\n");
								return -ERROR_NOT_ENOUGH_QUOTA;
							}
							filePath = writeFilepath;
							break;
						default:
							filePath = readFilepath;
					}
			} catch (...) {
				DbgPrint(L"Exception thrown in UFSCreateFile.");
				return -1;
			}
		} else
			filePath = writeFilepath;
	}
	HANDLE handle;
	DbgPrint(L"Creating file at %s.", filePath);
	// When filePath is a directory, needs to change the flag so that the file can be opened.
	handle = CreateFile(
		filePath,
		aAccessMode,//GENERIC_READ|GENERIC_WRITE|GENERIC_EXECUTE,
		aShareMode,
		NULL, // security attribute
		aCreationDisposition,
		aFlagsAndAttributes,// |FILE_FLAG_NO_BUFFERING,
		NULL); // template file handle

	if (handle == INVALID_HANDLE_VALUE) {
		DWORD error = GetLastError();
		DbgPrint(L"CreateFile failed with error code = %d\n", error);
		return error * -1; // error codes are negated value of Windows System Error codes
	}
	if (shouldUndelete)
		try {
			MutexLock(gDeletedFilesSet.mhMutex);
			gDeletedFilesSet.erase(cleanedFilename);
		} catch (...) {
			DbgPrint(L"Exception2 thrown in UFSCreateFile.\n");
			CloseHandle(handle);
			return -1;
		}
	apDokanFileInfo->Context = MakeContext(handle, filePath == writeFilepath, aAccessMode, aShareMode, aFlagsAndAttributes);
	if (fileAttributes != INVALID_FILE_ATTRIBUTES) {
		if (fileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			apDokanFileInfo->IsDirectory =TRUE;
		switch (aCreationDisposition) {
			case CREATE_ALWAYS:
			case OPEN_ALWAYS:
				DbgPrint(L"Returning ERROR_ALREADY_EXISTS.\n");
				return ERROR_ALREADY_EXISTS;
		}
	}
	DbgPrint(L"Returning success!\n");
	return 0;
}

static int DOKAN_CALLBACK UFSCreateDirectory(LPCWSTR aFileName, PDOKAN_FILE_INFO apDokanFileInfo)
{
	WCHAR writeFilepath[MAX_PATHB], readFilepath[MAX_PATHB];
	size_t filenameLength = wcslen(aFileName)*sizeof(WCHAR);
	if (PatchPath(writeFilepath, gWriteRootDirectory, aFileName, gWriteRootDirectoryLength, filenameLength))
		return -ERROR_NOT_SUPPORTED;
	if (PatchPath(readFilepath, gReadRootDirectory, aFileName, gReadRootDirectoryLength, filenameLength))
		return -ERROR_NOT_SUPPORTED;
	if (GetFileAttributes(readFilepath) != INVALID_FILE_ATTRIBUTES) {
		try {
			if (!CheckDeleted(aFileName))
				return -ERROR_ALREADY_EXISTS;
		} catch (...) {
			DbgPrint(L"Exception thrown in UFSCreateDirectory.");
			return -1;
		}
	}
	LPWSTR lpRelPathStart = AdvanceBytes(readFilepath, gReadRootDirectoryLength);
	for(LPWSTR lpRevBackslash = AdvanceBytes(lpRelPathStart, filenameLength-2); lpRevBackslash >lpRelPathStart; --lpRevBackslash)
		switch(*lpRevBackslash) {
			case L'\\':
			case L'/':
				*lpRevBackslash=L'\0';
				if (GetFileAttributes(readFilepath) == INVALID_FILE_ATTRIBUTES)
					goto TryCreate; //Shall Fail because of no parents.
				try {
					if (CheckDeleted(AdvanceBytes(readFilepath, gReadRootDirectoryLength)))
						goto TryCreate;
				} catch (...) {
					DbgPrint(L"Exception thrown in UFSCreateDirectory.");
				}
				if (CreateParentDirectories(writeFilepath))
					return -ERROR_NOT_ENOUGH_QUOTA;
				goto TryCreate;
		}
TryCreate:
	if (!CreateDirectory(writeFilepath, NULL)) {
		DWORD error = GetLastError();
		DbgPrint(L"\terror code = %d\n\n", error);
		return error * -1; // error codes are negated value of Windows System Error codes
	}
	return 0;
}

static int DOKAN_CALLBACK UFSOpenDirectory(LPCWSTR aFileName, PDOKAN_FILE_INFO apDokanFileInfo)
{
	WCHAR filePath[MAX_PATHW];
	HANDLE handle;

	int area = GetFilePath(filePath, aFileName);
	if (area == UFS_FAILED)
		return -(LONG)GetLastError();

	DbgPrint(L"OpenDirectory : %s\n", filePath);

	DWORD attributes = GetFileAttributes(filePath);
	if (attributes == INVALID_FILE_ATTRIBUTES) {
		DWORD error = GetLastError();
		DbgPrint(L"\terror code = %d\n\n", error);
		return -(LONG)error;
	}
	if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
		return -1;
	}

	handle = CreateFile(
		filePath,
		0,
		FILE_SHARE_READ|FILE_SHARE_WRITE,
		NULL,
		OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS,
		NULL);

	if (handle == INVALID_HANDLE_VALUE) {
		DWORD error = GetLastError();
		DbgPrint(L"\terror code = %d\n\n", error);
		return error * -1;
	}

	DbgPrint(L"\n");

	apDokanFileInfo->Context = MakeContext(handle, area == UFS_WRITE_AREA, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_FLAG_BACKUP_SEMANTICS);

	return 0;
}


static int DOKAN_CALLBACK UFSCloseFile(LPCWSTR aFileName, PDOKAN_FILE_INFO apDokanFileInfo)
{
	if (apDokanFileInfo->Context) {
		DbgPrint(L"CloseFile: %s\n", aFileName);
		DbgPrint(L"\terror : not cleanuped file\n\n");
		CloseHandle(GetHandle(apDokanFileInfo->Context));
		apDokanFileInfo->Context = 0;
	} else {
		DbgPrint(L"Close: %s\n\n", aFileName);
		return 0;
	}
	return 0;
}

static int DOKAN_CALLBACK UFSCleanup(LPCWSTR aFileName, PDOKAN_FILE_INFO apDokanFileInfo)
{
	if (apDokanFileInfo->Context) {
		DbgPrint(L"Cleanup: %s\n\n", aFileName);
		ULONG64 context = apDokanFileInfo->Context;
		HANDLE handle=GetHandle(context);
		if (!CloseHandle(handle)) {
			DbgPrint(L"Failed to close Handle:%p.", handle);
		};
		apDokanFileInfo->Context = 0;
		if (apDokanFileInfo->DeleteOnClose) {
			DbgPrint(L"\tDeleteOnClose\n");
			if (apDokanFileInfo->IsDirectory) {
				DbgPrint(L"\tDeleteDirectory ");
				WCHAR	filePath[MAX_PATHW];
				size_t fileNameLengthB = wcslen(aFileName)*sizeof(WCHAR);
				if (PatchPath(filePath, gWriteRootDirectory, aFileName, gWriteRootDirectoryLength, fileNameLengthB))
					return -1;
				if (GetFileAttributes(filePath) != INVALID_FILE_ATTRIBUTES) {
					if (!RemoveDirectory(filePath)) {
						int error = (int)GetLastError();
						DbgPrint(L"\tFailed to remove directory %s. Error: %d.\n", filePath, error);
						return -error;
					}
					PatchPath(filePath, gReadRootDirectory, aFileName, gReadRootDirectoryLength, fileNameLengthB);
					if (GetFileAttributes(filePath) != INVALID_FILE_ATTRIBUTES)
						goto MarkDeleted;
					return 0;
				}
				if (PatchPath(filePath, gReadRootDirectory, aFileName, gReadRootDirectoryLength, fileNameLengthB))
					return -ERROR_NOT_SUPPORTED;
				if (GetFileAttributes(filePath) != INVALID_FILE_ATTRIBUTES)
					goto MarkDeleted;
				return -ERROR_FILE_NOT_FOUND;
			} else {
				DbgPrint(L"\tDeleting File %s.", aFileName);
				WCHAR filePath[MAX_PATHW];
				size_t fileNameLengthB = wcslen(aFileName) * sizeof(WCHAR);
				if (IsInWriteArea(context)) {
					if (PatchPath(filePath, gWriteRootDirectory, aFileName, gWriteRootDirectoryLength, fileNameLengthB))
						return -ERROR_NOT_SUPPORTED;
					if (!DeleteFile(filePath)) {
						int error = (int)GetLastError();
						DbgPrint(L"Failed to delete file %s. Error %d.\n", filePath, error);
						return -error;
					}
					PatchPath(filePath, gReadRootDirectory, aFileName, gReadRootDirectoryLength, fileNameLengthB);
					if (GetFileAttributes(filePath) != INVALID_FILE_ATTRIBUTES)
MarkDeleted:
						try {
							MutexLock lock(gDeletedFilesSet.mhMutex);
							wstring filename(aFileName);
							CleanFilename(filename);
							gDeletedFilesSet.insert(filename);
						} catch(...) {
							return -1;
						}
					return 0;
				} else {
					if (PatchPath(filePath, gReadRootDirectory, aFileName, gReadRootDirectoryLength, fileNameLengthB))
						return -1;
					if (GetFileAttributes(filePath) != INVALID_FILE_ATTRIBUTES)
						goto MarkDeleted;
					return ERROR_NOT_FOUND;
				}
			}
		}

	} else {
		DbgPrint(L"Cleanup: %s\n\tinvalid handle\n\n", aFileName);
		return -1;
	}
	return 0;
}

static int DOKAN_CALLBACK UFSReadFile(LPCWSTR aFileName, LPVOID aBuffer, DWORD aBufferLength, LPDWORD aReadLength, LONGLONG aOffset, PDOKAN_FILE_INFO apDokanFileInfo)
{
	HANDLE	handle = GetHandle(apDokanFileInfo->Context);
	bool closeOnReturn = false;
	if (!handle || handle == INVALID_HANDLE_VALUE) {
		DbgPrint(L"\tinvalid handle, cleanuped?\n");
		DOKAN_FILE_INFO dokanFileInfo;
		int returnValue = UFSCreateFile(aFileName, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0, &dokanFileInfo);
		if (returnValue)
			return returnValue;
		handle = GetHandle(dokanFileInfo.Context);
		closeOnReturn = true;
	}
	if (SetFilePointer(handle, (LONG)aOffset, ((LONG*)&aOffset)+1, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
		int returnValue = GetLastError();
		if (NO_ERROR != returnValue) {
			DbgPrint(L"\tseek error %d, offset = %I64d\n\n", returnValue, aOffset);
			if (closeOnReturn)
				CloseHandle(handle);
			return -returnValue;
		}
	}
	if (!ReadFile(handle, aBuffer, aBufferLength, aReadLength,NULL)) {
		if (closeOnReturn)
			CloseHandle(handle);
		return -(LONG)GetLastError();
	}
	if (closeOnReturn)
		CloseHandle(handle);
	return 0;
}

static int DOKAN_CALLBACK UFSWriteFile(LPCWSTR aFileName, LPCVOID aBuffer,DWORD aNumberOfBytesToWrite, LPDWORD aNumberOfBytesWritten, LONGLONG aOffset, PDOKAN_FILE_INFO apDokanFileInfo)
{
	HANDLE	handle = GetHandle(apDokanFileInfo->Context);
	bool	closeOnReturn = false;
	DbgPrint(L"WriteFile : %s, offset %I64d, length %d\n", aFileName, aOffset, aNumberOfBytesToWrite);
	// reopen the file
	if (!handle || handle == INVALID_HANDLE_VALUE) {
		DbgPrint(L"\tinvalid handle, cleanuped?\n");
		WCHAR writeFilepath[MAX_PATHW], readFilepath[MAX_PATHW];
		size_t filenameLength=wcslen(aFileName)*sizeof(WCHAR);
		if (PatchPath(writeFilepath, gWriteRootDirectory, aFileName, gWriteRootDirectoryLength, filenameLength))
			return -ERROR_NOT_SUPPORTED;
		if (GetFileAttributes(writeFilepath) == INVALID_FILE_ATTRIBUTES) {
			try {
				if (CheckDeleted(aFileName))
					return -ERROR_FILE_NOT_FOUND;
			} catch (...) {
				DbgPrint(L"Exception thrown in UFSWriteFile.");
				return -1;
			}
			if (PatchPath(readFilepath, gReadRootDirectory, aFileName, gReadRootDirectoryLength, filenameLength))
				return -ERROR_NOT_SUPPORTED;
			if (GetFileAttributes(readFilepath) == INVALID_FILE_ATTRIBUTES)
				return -ERROR_FILE_NOT_FOUND;
			if (!CopyFile(readFilepath, writeFilepath, TRUE))
				return -ERROR_NOT_ENOUGH_QUOTA;
		}
		handle = CreateFile(writeFilepath, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,	NULL);
		if (handle == INVALID_HANDLE_VALUE)
			return -(LONG)GetLastError();
		closeOnReturn = true;
	} else if (!IsInWriteArea(apDokanFileInfo->Context)) {
		WCHAR writeFilepath[MAX_PATHW], readFilepath[MAX_PATHW];
		size_t filenameLength=wcslen(aFileName)*sizeof(WCHAR);
		if (PatchPath(writeFilepath, gWriteRootDirectory, aFileName, gWriteRootDirectoryLength, filenameLength))
			return -ERROR_NOT_SUPPORTED;
		PatchPath(readFilepath, gReadRootDirectory, aFileName, gReadRootDirectoryLength, filenameLength); // This must succeed since the file was open.
		CloseHandle(handle);
		DWORD accessMode, shareMode, flags;
		GetCreateDataFromContext(apDokanFileInfo->Context,&accessMode, &shareMode, &flags);
		if (!CopyFile(readFilepath, writeFilepath, TRUE)) {
			handle=CreateFile(readFilepath, accessMode, shareMode, NULL, OPEN_EXISTING, flags, NULL);
			apDokanFileInfo->Context = MakeContext(handle, false, accessMode, shareMode, flags);
			return -ERROR_NOT_ENOUGH_QUOTA;
		}
		handle = CreateFile(writeFilepath, accessMode, shareMode, NULL, OPEN_EXISTING, flags, NULL);
		apDokanFileInfo->Context = MakeContext(handle, true, accessMode, shareMode, flags);
		if (handle == INVALID_HANDLE_VALUE)
			return -(LONG)GetLastError();
	}
	if (apDokanFileInfo->WriteToEndOfFile) {
		if (SetFilePointer(handle, 0, NULL, FILE_END) == INVALID_SET_FILE_POINTER) {
			DbgPrint(L"\tseek error, offset = EOF, error = %d\n", GetLastError());
			return -1;
		}
	} else if (SetFilePointer(handle, (LONG)aOffset, ((LONG*)&aOffset) +1, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
		int returnValue = GetLastError();
		if (NO_ERROR != returnValue) {
			DbgPrint(L"\tseek error %d, offset = %I64d\n\n", returnValue, aOffset);
			if (closeOnReturn)
				CloseHandle(handle);
			return -returnValue;
		}
	}
	if (!WriteFile(handle, aBuffer, aNumberOfBytesToWrite, aNumberOfBytesWritten, NULL)) {
		int returnValue = GetLastError();
		DbgPrint(L"\twrite error = %u, buffer length = %d, write length = %d\n",
			returnValue, aNumberOfBytesToWrite, *aNumberOfBytesWritten);
		return -returnValue;
	} else {
		DbgPrint(L"\twrite %I64d, offset %d\n\n", *aNumberOfBytesWritten, aOffset);
	}
	// close the file when it is reopened
	if (closeOnReturn)
		CloseHandle(handle);
	return 0;
}

static int DOKAN_CALLBACK UFSFlushFileBuffers(LPCWSTR aFileName, PDOKAN_FILE_INFO	apDokanFileInfo)
{
	HANDLE handle = GetHandle(apDokanFileInfo->Context);
	DbgPrint(L"FlushFileBuffers : %s\n", aFileName);
	if (!handle || handle == INVALID_HANDLE_VALUE) {
		DbgPrint(L"\tinvalid handle\n\n");
		return 0;
	}
	if (FlushFileBuffers(handle))
		return 0;
	int returnValue = GetLastError();
	DbgPrint(L"\tflush error code = %d\n", returnValue);
	return -returnValue;
}


static int DOKAN_CALLBACK UFSGetFileInformation(LPCWSTR aFileName, LPBY_HANDLE_FILE_INFORMATION apHandleFileInformation, PDOKAN_FILE_INFO apDokanFileInfo)
{
	HANDLE handle = GetHandle(apDokanFileInfo->Context);
	bool closeOnReturn = false;
	DbgPrint(L"GetFileInfo : %s\n", aFileName);
	if (!handle || handle == INVALID_HANDLE_VALUE) {
		DbgPrint(L"\tinvalid handle\n\n");
		// If CreateDirectory returned FILE_ALREADY_EXISTS and 
		// it is called with FILE_OPEN_IF, that handle must be opened.
		DOKAN_FILE_INFO dokanFileInfo;
		int returnValue = UFSCreateFile(aFileName, 0, FILE_SHARE_READ, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, &dokanFileInfo);
		if (returnValue)
			return returnValue;
		handle = GetHandle(dokanFileInfo.Context);
		closeOnReturn = true;
	}
	if (!GetFileInformationByHandle(handle,apHandleFileInformation)) {
		DbgPrint(L"\terror code = %d\n", GetLastError());

		// aFileName is a root directory
		// in this case, FindFirstFile can't get directory information
		if (!aFileName[1]) {
			DbgPrint(L"  root dir\n");
			apHandleFileInformation->dwFileAttributes = GetFileAttributes(gReadRootDirectory);
		} else {
			WIN32_FIND_DATAW find;
			ZeroMemory(&find, sizeof(WIN32_FIND_DATAW));
			WCHAR filePath[MAX_PATHW];
			if (GetFilePath(filePath, aFileName) == UFS_FAILED)
				return -ERROR_NOT_SUPPORTED;
			handle = FindFirstFile(filePath, &find);
			if (handle == INVALID_HANDLE_VALUE) {
				DbgPrint(L"\tFindFirstFile error code = %d\n\n", GetLastError());
				return -1;
			}
			apHandleFileInformation->dwFileAttributes = find.dwFileAttributes;
			apHandleFileInformation->ftCreationTime = find.ftCreationTime;
			apHandleFileInformation->ftLastAccessTime = find.ftLastAccessTime;
			apHandleFileInformation->ftLastWriteTime = find.ftLastWriteTime;
			apHandleFileInformation->nFileSizeHigh = find.nFileSizeHigh;
			apHandleFileInformation->nFileSizeLow = find.nFileSizeLow;
			DbgPrint(L"\tFindFiles OK, file size = %d\n", find.nFileSizeLow);
			FindClose(handle);
		}
	} else {
		DbgPrint(L"\tGetFileInformationByHandle success, file size = %d\n",
			apHandleFileInformation->nFileSizeLow);
	}
	if (closeOnReturn)
		CloseHandle(handle);
	return 0;
}

static int DOKAN_CALLBACK UFSFindFiles(LPCWSTR aFileName, PFillFindData aFillFindData, PDOKAN_FILE_INFO apDokanFileInfo)
{
	DbgPrint(L"FindFiles called with %s, %p, %p.\n", aFileName, aFillFindData, apDokanFileInfo);
	try {
		wstring relativeFilePath(aFileName);
		CleanFilename(relativeFilePath);
		if (CheckDeletedClean(relativeFilePath)) {
			DbgPrint(L"\tDirectory deleted.\n");
			return -ERROR_FILE_NOT_FOUND;
		}
		WCHAR filePath1[MAX_PATHW + 2]; //This is done to avoid buffer overruns.
		size_t relativePathLen = relativeFilePath.size();
		switch (aFileName[relativePathLen-1]) {
			case L'\\':
			case L'/':
				break;
			default:
				relativeFilePath.append(1,L'\\');
				++relativePathLen;
		}
		size_t relativePathLenB =relativePathLen*sizeof(WCHAR);
		if (PatchPath(filePath1,gReadRootDirectory, relativeFilePath.c_str(), gReadRootDirectoryLength, relativePathLenB)) {
			DbgPrint(L"\tName too long read.\n");
			return -ERROR_NOT_SUPPORTED;
		}
		LPWSTR p1 = AdvanceBytes(filePath1, gReadRootDirectoryLength + relativePathLenB);
		*(p1++) = L'*';
		*p1 = L'\0';
		WIN32_FIND_DATAW findData1;
		HANDLE hFind1 = FindFirstFile(filePath1, &findData1);
		if (hFind1 == INVALID_HANDLE_VALUE) {
			DbgPrint(L"\tNot found in read.\n");
			if (PatchPath(filePath1,gWriteRootDirectory, relativeFilePath.c_str(), gWriteRootDirectoryLength, relativePathLenB)) {
				DbgPrint(L"\tName too long write.\n");
				return -ERROR_NOT_SUPPORTED;
			}
			p1 = AdvanceBytes(filePath1, gWriteRootDirectoryLength + relativePathLenB);
			*(p1++) = L'*';
			*p1 = L'\0';
			hFind1 = FindFirstFile(filePath1, &findData1);
			if (hFind1 == INVALID_HANDLE_VALUE) {
				int returnValue = GetLastError();
				DbgPrint(L"\tinvalid file handle. Error is %u\n\n", returnValue);
				return -returnValue;
			}
			DbgPrint(L"\twrite returning %s.\n", findData1.cFileName);
			aFillFindData(&findData1, apDokanFileInfo);//No need to check deleted files here since it is about the writeFilePath
			while (FindNextFile(hFind1, &findData1)) {
				DbgPrint(L"\twrite returning %s.\n", findData1.cFileName);
 				aFillFindData(&findData1, apDokanFileInfo);
			}
			int returnValue = GetLastError();
			FindClose(hFind1);
			if (returnValue != ERROR_NO_MORE_FILES) {
				DbgPrint(L"\tFindNextFile error. Error is %u\n\n", returnValue);
				return -returnValue;
			}
			return 0;
		}
		WCHAR filePath2[MAX_PATHW + 2]; //This is done to avoid buffer overruns.
		if (PatchPath(filePath2,gWriteRootDirectory, relativeFilePath.c_str(), gWriteRootDirectoryLength, relativePathLenB)) {
			DbgPrint(L"\tFilename too long write.\n");
			return -1;
		}
		p1 = AdvanceBytes(filePath2, gWriteRootDirectoryLength + relativePathLenB);
		*(p1++) = L'*';
		*p1 = L'\0';
		WIN32_FIND_DATAW findData2;
		HANDLE hFind2 = FindFirstFile(filePath2, &findData2);
		if (hFind2 == INVALID_HANDLE_VALUE) {
			DbgPrint(L"\tDir not found write.\n");
DoReadDir:
			if (CheckDeleted(relativeFilePath + findData1.cFileName))
				DbgPrint(L"\tFile Deleted %s.\n", findData1.cFileName);
			else {
				DbgPrint(L"\tread returning %s.\n", findData1.cFileName);
				aFillFindData(&findData1, apDokanFileInfo);
			}
DoReadDirNext:
			while (FindNextFile(hFind1, &findData1))
				if (CheckDeleted(relativeFilePath + findData1.cFileName))
					DbgPrint(L"\tFile Deleted %s.\n", findData1.cFileName);
				else {
					DbgPrint(L"\tread returning %s.\n", findData1.cFileName);
 					aFillFindData(&findData1, apDokanFileInfo);
				}
			int returnValue = GetLastError();
			FindClose(hFind1);
			if (returnValue != ERROR_NO_MORE_FILES) {
				DbgPrint(L"\tFindNextFile error. Error is %u\n\n", returnValue);
				return -returnValue;
			}
			return 0;
		}
		while(true) {
			int compareResult =wcscmp(findData1.cFileName, findData2.cFileName);
			if(compareResult < 0) {
				if (CheckDeleted(relativeFilePath + findData1.cFileName))
					DbgPrint(L"\tFile Deleted %s.\n", findData1.cFileName);
				else {
					DbgPrint(L"\tread returning %s.\n", findData1.cFileName);
					aFillFindData(&findData1, apDokanFileInfo);
				}
Move1stDir:
				if (!FindNextFile(hFind1, &findData1)) {
					int returnValue = GetLastError();
					FindClose(hFind1);
					if (returnValue != ERROR_NO_MORE_FILES) {
						DbgPrint(L"\tFindNextFile error2. Error is %u\n\n", returnValue);
						return -returnValue;
					}
					DbgPrint(L"\twrite returning %s.\n", findData2.cFileName);
					aFillFindData(&findData2, apDokanFileInfo); // No checks here as it is the writePath
					while (FindNextFile(hFind2, &findData2)) {
						DbgPrint(L"\twrite returning %s.\n", findData2.cFileName);
 						aFillFindData(&findData2, apDokanFileInfo);
					}
					returnValue = GetLastError();
					FindClose(hFind2);
					if (returnValue != ERROR_NO_MORE_FILES) {
						DbgPrint(L"\tFindNextFile error3. Error is %u\n\n", returnValue);
						return -returnValue;
					}
					return 0;
				}
			} else {
				DbgPrint(L"\twrite returning %s.\n", findData2.cFileName);
				aFillFindData(&findData2, apDokanFileInfo);
				if (!FindNextFile(hFind2, &findData2)) {
					int returnValue = GetLastError();
					FindClose(hFind2);
					if (returnValue != ERROR_NO_MORE_FILES) {
						DbgPrint(L"\tFindNextFile error4. Error is %u\n\n", returnValue);
						return -returnValue;
					}
					if (compareResult)
						goto DoReadDir;
					goto DoReadDirNext;
				}
				if (!compareResult)
					goto Move1stDir;
			}
		}
	} catch(...) {
		DbgPrint(L"Error thrown in UFSFindFiles.");
		return -1;
	}
	return 0;
}

static int DOKAN_CALLBACK UFSDeleteFile(LPCWSTR aFileName, PDOKAN_FILE_INFO apDokanFileInfo)
{
	WCHAR	filePath[MAX_PATHW];
	size_t fileNameLengthB = wcslen(aFileName)*sizeof(WCHAR);
	if (PatchPath(filePath, gWriteRootDirectory, aFileName, gWriteRootDirectoryLength, fileNameLengthB))
		return -1;
	if (GetFileAttributes(filePath) != INVALID_FILE_ATTRIBUTES)
		return 0;
	if (PatchPath(filePath, gReadRootDirectory, aFileName, gReadRootDirectoryLength, fileNameLengthB))
		return -1;
	if (GetFileAttributes(filePath) != INVALID_FILE_ATTRIBUTES)
		return 0;
	return -ERROR_FILE_NOT_FOUND;
}

static int DOKAN_CALLBACK UFSDeleteDirectory(LPCWSTR aFileName, PDOKAN_FILE_INFO apDokanFileInfo)
{
	WCHAR	filePath[MAX_PATHW];
	size_t fileNameLengthB = wcslen(aFileName)*sizeof(WCHAR);
	if (PatchPath(filePath, gWriteRootDirectory, aFileName, gWriteRootDirectoryLength, fileNameLengthB))
		return -1;
	if (GetFileAttributes(filePath) != INVALID_FILE_ATTRIBUTES) {
		WIN32_FIND_DATAW findData;
		LPWSTR p = AdvanceBytes(filePath, gWriteRootDirectoryLength + fileNameLengthB);
		*(p++) = L'\\';
		*(p++) = L'*';
		*p = L'\0';
		HANDLE hFind =FindFirstFile(filePath, &findData);
		if (hFind == INVALID_HANDLE_VALUE)
			return -ERROR_FILE_INVALID;
		do {
			if (! wcscmp(findData.cFileName, L"."))
				continue;
			if (! wcscmp(findData.cFileName, L".."))
				continue;
			FindClose(hFind);
			return ERROR_DIR_NOT_EMPTY;
		} while (FindNextFile(hFind, &findData));
		FindClose(hFind);
		if (PatchPath(filePath, gReadRootDirectory, aFileName, gReadRootDirectoryLength, fileNameLengthB))
			return -1;
		if (GetFileAttributes(filePath) == INVALID_FILE_ATTRIBUTES)
			return 0;
CheckReadFile:
		try {
			wstring cleanFilename(aFileName);
			CleanFilename(cleanFilename);
			if (CheckDeletedClean(cleanFilename))
				return 0;
			if (cleanFilename.at(fileNameLengthB/sizeof(WCHAR) - 1) != L'\\')
				cleanFilename.append(1, L'\\');
			p = AdvanceBytes(filePath, gReadRootDirectoryLength + fileNameLengthB);
			*(p++) = L'\\';
			*(p++) = L'*';
			*p = L'\0';
			HANDLE hFind =FindFirstFile(filePath, &findData);
			if (hFind == INVALID_HANDLE_VALUE)
				return -ERROR_FILE_INVALID;
			do {
				if (! wcscmp(findData.cFileName, L"."))
					continue;
				if (! wcscmp(findData.cFileName, L".."))
					continue;
				if (CheckDeleted(cleanFilename + findData.cFileName))
					continue;
				return ERROR_DIR_NOT_EMPTY;
			} while (FindNextFile(hFind, &findData));
		} catch (...) {
			DbgPrint(L"Exception throwns in DeleteDirectory.");
			return -1;
		}
		return 0;
	}
	if (PatchPath(filePath, gReadRootDirectory, aFileName, gReadRootDirectoryLength, fileNameLengthB))
		return -1;
	if (GetFileAttributes(filePath) != INVALID_FILE_ATTRIBUTES)
		goto CheckReadFile;
	return -ERROR_FILE_NOT_FOUND;
}


static int DOKAN_CALLBACK UFSMoveFile(LPCWSTR aFileName, LPCWSTR aNewFilename, BOOL aReplaceIfExisting, PDOKAN_FILE_INFO apDokanFileInfo)
{
	WCHAR filePath[MAX_PATHW], newFilepath[MAX_PATHW];
	DbgPrint(L"MoveFile %s -> %s\n\n", filePath, newFilepath);
	size_t relativeFilepathLengthB = wcslen(aFileName) * sizeof(WCHAR);
	if (PatchPath(filePath, gWriteRootDirectory, aFileName, gWriteRootDirectoryLength, relativeFilepathLengthB))
		return -ERROR_NOT_SUPPORTED;
	if (PatchPath(newFilepath, gWriteRootDirectory, aNewFilename, gWriteRootDirectoryLength, wcslen(aNewFilename) * sizeof(WCHAR)))
		return -ERROR_NOT_SUPPORTED;
	wstring cleanFilename(aFileName);
	CleanFilename(cleanFilename);
	BOOL status;
	if (GetFileAttributes(filePath) != INVALID_FILE_ATTRIBUTES) {
		status = aReplaceIfExisting ? MoveFileEx(filePath, newFilepath, MOVEFILE_REPLACE_EXISTING) : MoveFile(filePath, newFilepath);
		if ( PatchPath(filePath, gReadRootDirectory, aFileName, gReadRootDirectoryLength, relativeFilepathLengthB))
			return -ERROR_NOT_SUPPORTED;
	} else {
		wstring cleanNewFilename(aNewFilename);
		CleanFilename(cleanNewFilename);
		if (!cleanFilename.compare(cleanNewFilename))
			return -ERROR_CANNOT_COPY;
		if ( PatchPath(filePath, gReadRootDirectory, aFileName, gReadRootDirectoryLength, relativeFilepathLengthB))
			return -ERROR_NOT_SUPPORTED;
		status = CopyFile(filePath, newFilepath, ! aReplaceIfExisting);
	}
	if (status == FALSE) {
		DWORD error = GetLastError();
		DbgPrint(L"\tMoveFile failed status = %d, code = %d\n", status, error);
		return -(int)error;
	}
	if (GetFileAttributes(filePath) != INVALID_FILE_ATTRIBUTES) {
		try {
			MutexLock(gDeletedFilesSet.mhMutex);
			gDeletedFilesSet.insert(cleanFilename);
		} catch (...) {
			DbgPrint(L"Exception thrown in UFSMoveFile.");
			return -1;
		}
	}
	return 0;
}

static int DOKAN_CALLBACK UFSLockFile(LPCWSTR aFileName, LONGLONG aByteOffset, LONGLONG aLength, PDOKAN_FILE_INFO apDokanFileInfo)
{
	HANDLE	handle;
	handle = GetHandle(apDokanFileInfo->Context);
	if (!handle || handle == INVALID_HANDLE_VALUE) {
		DbgPrint(L"\tinvalid handle\n\n");
		return -1;
	}
	if (LockFile(handle, *((DWORD*)&aByteOffset), *(((DWORD*)&aByteOffset)+1), *((DWORD*)&aLength), *(((DWORD*)&aLength)+1))) {
		DbgPrint(L"\tsuccess\n\n");
		return 0;
	} else {
		DbgPrint(L"\tfail\n\n");
		return -(LONG)GetLastError();
	}
}

static int DOKAN_CALLBACK UFSSetEndOfFile(LPCWSTR aFileName, LONGLONG aByteOffset, PDOKAN_FILE_INFO apDokanFileInfo)
{
	HANDLE handle;
	handle = GetHandle(apDokanFileInfo->Context);
	if (!handle || handle == INVALID_HANDLE_VALUE) {
		DbgPrint(L"\tinvalid handle\n\n");
		return -1;
	}
	if (!SetFilePointerEx(handle, *((LARGE_INTEGER*)&aByteOffset), NULL, FILE_BEGIN)) {
		DbgPrint(L"\tSetFilePointer error: %d, offset = %I64d\n\n",
				GetLastError(), aByteOffset);
		return GetLastError() * -1;
	}
	if (!SetEndOfFile(handle)) {
		DWORD error = GetLastError();
		DbgPrint(L"\terror code = %d\n\n", error);
		return error * -1;
	}
	return 0;
}

static int DOKAN_CALLBACK UFSSetAllocationSize(LPCWSTR aFileName, LONGLONG aAllocSize, PDOKAN_FILE_INFO apDokanFileInfo)
{
	HANDLE			handle;
	LARGE_INTEGER	fileSize;
	handle = GetHandle(apDokanFileInfo->Context);
	if (!handle || handle == INVALID_HANDLE_VALUE) {
		DbgPrint(L"\tinvalid handle\n\n");
		return -1;
	}
	if (GetFileSizeEx(handle, &fileSize)) {
		if (aAllocSize < fileSize.QuadPart) {
			fileSize.QuadPart = aAllocSize;
			if (!SetFilePointerEx(handle, fileSize, NULL, FILE_BEGIN)) {
				DbgPrint(L"\tSetAllocationSize: SetFilePointer eror: %d, "
					L"offset = %I64d\n\n", GetLastError(), aAllocSize);
				return GetLastError() * -1;
			}
			if (!SetEndOfFile(handle)) {
				DWORD error = GetLastError();
				DbgPrint(L"\terror code = %d\n\n", error);
				return error * -1;
			}
		}
	} else {
		DWORD error = GetLastError();
		DbgPrint(L"\terror code = %d\n\n", error);
		return error * -1;
	}
	return 0;
}


static int DOKAN_CALLBACK UFSSetFileAttributes(LPCWSTR aFileName, DWORD aFileAttributes, PDOKAN_FILE_INFO apDokanFileInfo)
{
	WCHAR	filePath[MAX_PATHW];
	size_t relativeFilepathLengthB = wcslen(aFileName) * sizeof(WCHAR);
	if (PatchPath(filePath, gWriteRootDirectory, aFileName, gWriteRootDirectoryLength, relativeFilepathLengthB))
		return -ERROR_NOT_SUPPORTED;
	if (GetFileAttributes(filePath) == INVALID_FILE_ATTRIBUTES) {
		WCHAR filePath2[MAX_PATHW];
		if (PatchPath(filePath2, gReadRootDirectory, aFileName, gReadRootDirectoryLength, relativeFilepathLengthB))
			return -ERROR_NOT_SUPPORTED;
		if (GetFileAttributes(filePath2) == aFileAttributes)
			return 0;
		if (!CopyFile(filePath2, filePath, TRUE))
			return -(LONG)GetLastError();
	}
	DbgPrint(L"SetFileAttributes %s\n", filePath);
	if (!SetFileAttributes(filePath, aFileAttributes)) {
		DWORD error = GetLastError();
		DbgPrint(L"\terror code = %d\n\n", error);
		return error * -1;
	}
	return 0;
}

static int DOKAN_CALLBACK UFSSetFileTime(LPCWSTR aFileName, CONST FILETIME* aCreationTime, CONST FILETIME* aLastAccessTime, CONST FILETIME* aLastWriteTime, PDOKAN_FILE_INFO apDokanFileInfo)
{
	HANDLE handle = GetHandle(apDokanFileInfo->Context);
	if (!IsInWriteArea(apDokanFileInfo->Context)) {
		WCHAR	filePath[MAX_PATHW];
		size_t relativeFilepathLengthB = wcslen(aFileName) * sizeof(WCHAR);
		if (PatchPath(filePath, gWriteRootDirectory, aFileName, gWriteRootDirectoryLength, relativeFilepathLengthB))
			return -ERROR_NOT_SUPPORTED;
		WCHAR filePath2[MAX_PATHW];
		if (PatchPath(filePath2, gReadRootDirectory, aFileName, gReadRootDirectoryLength, relativeFilepathLengthB))
			return -ERROR_NOT_SUPPORTED;
		CloseHandle(handle);
		DWORD accessMode, shareMode, flags;
		GetCreateDataFromContext(apDokanFileInfo->Context, &accessMode, &shareMode, &flags);
		if (!CopyFile(filePath2, filePath, TRUE)) {
			handle = CreateFile(filePath2, accessMode, shareMode, NULL, OPEN_EXISTING, flags, NULL);
			apDokanFileInfo->Context = MakeContext(handle, false, accessMode, shareMode, flags);
			return -(LONG)GetLastError();
		}
		handle = CreateFile(filePath, accessMode, shareMode, NULL, OPEN_EXISTING, flags, NULL);
		apDokanFileInfo->Context = MakeContext(handle, true, accessMode, shareMode, flags);
	}
	if (!handle || handle == INVALID_HANDLE_VALUE) {
		DbgPrint(L"\tinvalid handle\n\n");
		return -1;
	}
	if (!SetFileTime(handle, aCreationTime, aLastAccessTime, aLastWriteTime)) {
		DWORD error = GetLastError();
		DbgPrint(L"\terror code = %d\n\n", error);
		return error * -1;
	}
	return 0;
}

static int DOKAN_CALLBACK UFSUnlockFile(LPCWSTR aFileName, LONGLONG aByteOffset, LONGLONG aLength, PDOKAN_FILE_INFO apDokanFileInfo)
{
	HANDLE	handle;

	handle = GetHandle(apDokanFileInfo->Context);
	if (!handle || handle == INVALID_HANDLE_VALUE) {
		DbgPrint(L"\tinvalid handle\n\n");
		return -1;
	}
	if (UnlockFile(handle, *((DWORD*)&aByteOffset), *(((DWORD*)&aByteOffset)+1), *((DWORD*)&aLength), *(((DWORD*)&aLength)+1))) {
		DbgPrint(L"\tsuccess\n\n");
		return 0;
	} else {
		DbgPrint(L"\tfail\n\n");
		return -1;
	}
}

// see Win32 API GetDiskFreeSpaceEx
int DOKAN_CALLBACK UFSGetDiskFreeSpace(PULONGLONG aFreeBytesAvailable, PULONGLONG aTotalNumberOfBytes, PULONGLONG aTotalNumberOfFreeBytes, PDOKAN_FILE_INFO)
{
	if (GetDiskFreeSpaceEx(gWriteRootDirectory, (PULARGE_INTEGER)aFreeBytesAvailable, (PULARGE_INTEGER)aTotalNumberOfBytes, (PULARGE_INTEGER)aTotalNumberOfFreeBytes))
		return 0;
	return -(LONG)GetLastError();
};


// see Win32 API GetVolumeInformation
int DOKAN_CALLBACK UFSGetVolumeInformation(LPWSTR aVolumeNameBuffer, DWORD	aVolumeNameSize, LPDWORD aVolumeSerialNumber, LPDWORD aMaximumComponentLength, 
										LPDWORD aFileSystemFlags, LPWSTR aFileSystemNameBuffer, DWORD aFileSystemNameSize, PDOKAN_FILE_INFO)
{
	WCHAR filePath[MAX_PATHW];
	*filePath = *gWriteRootDirectory;
	LPWSTR p = filePath + 1;
	LPCWSTR source = gWriteRootDirectory + 1;
	if ((*(p++) = *(source++)) == ':') { // Drive letter path.
		*(p++) = *source;
	} else { // UNC path
		while ((*(p++) = *(source++)) != L'\\'); // End of server
		while ((*(p++) = *(source++)) != L'\\'); // End of share
	}
	*p = L'\0';
	if (GetVolumeInformation(filePath, aVolumeNameBuffer, aVolumeNameSize, aVolumeSerialNumber, aMaximumComponentLength, aFileSystemFlags, 
		aFileSystemNameBuffer, aFileSystemNameSize))
		return 0;
	return -(LONG)GetLastError();
}

static int DOKAN_CALLBACK UFSUnmount(PDOKAN_FILE_INFO apDokanFileInfo)
{
	DbgPrint(L"Unmount\n");
	return 0;
}

int wmain(int argc, LPWSTR argv[])
{
	int status;
	PDOKAN_OPERATIONS dokanOperations = (PDOKAN_OPERATIONS)malloc(sizeof(DOKAN_OPERATIONS));
	PDOKAN_OPTIONS dokanOptions = (PDOKAN_OPTIONS)malloc(sizeof(DOKAN_OPTIONS));

	if (argc < 7) {
printHelp:
		fwprintf(stderr, L"WinUnionFS /r <ReadRoot> /w <WriteRoot> /l <driveletter> [<other options>]\n"
			L"  /r ReadRootDirectory (ex. /r c:\\read)\n"
			L"  /w WriteRootDirectory (ex. /r d:\\)\n"
			L"  /l DriveLetter (ex. /l m)\n"
			L"  /t ThreadCount (ex. /t 5)\n"
			L"  /d (enable debug output)\n"
			L"  /n (use network drive)\n"
			L"  /m (use removable drive)\n", *argv);
		return 2;
	}

	ZeroMemory(dokanOptions, sizeof(DOKAN_OPTIONS));
	dokanOptions->ThreadCount = 0; // use default
	for (++argv; --argc; ++argv) {
		switch (towupper((*argv)[1])) {
		case 'R':
			if(!--argc) goto printHelp;
			++argv;
			gReadRootDirectoryLength = wcslen(*argv) *sizeof(WCHAR);
			switch (*(AdvanceBytes(*argv, gReadRootDirectoryLength-2))) {
			case L'\\':
			case L'/':
				gReadRootDirectoryLength -= 2;
				break;
			}
			memcpy(gReadRootDirectory, *argv, gReadRootDirectoryLength);
			DbgPrint(L"ReadRootDirectory: %-*.*s\n", gReadRootDirectoryLength/2, gReadRootDirectoryLength/2, gReadRootDirectory);
			break;
		case 'W':
			if(!--argc) goto printHelp;
			++argv;
			gWriteRootDirectoryLength = wcslen(*argv) *sizeof(WCHAR);
			switch (*(AdvanceBytes(*argv, gWriteRootDirectoryLength-2))) {
			case L'\\':
			case L'/':
				*(AdvanceBytes(*argv, gWriteRootDirectoryLength-2)) = 0;
				break;
			default:
				gWriteRootDirectoryLength += 2;
			}
			memcpy(gWriteRootDirectory, *argv, gWriteRootDirectoryLength);
			gWriteRootDirectoryLength -= 2;
			DbgPrint(L"WriteRootDirectory: %s\n", gWriteRootDirectory);
			break;
		case 'L':
			if(!--argc) goto printHelp;
			++argv;
			dokanOptions->DriveLetter = **argv;
			break;
		case 'T':
			if(!--argc) goto printHelp;
			++argv;
			dokanOptions->ThreadCount = (USHORT)_wtoi(*argv);
			break;
		case 'D':
			gDebugMode = true;
			break;
		case 'N':
			dokanOptions->Options |= DOKAN_OPTION_NETWORK;
			break;
		case 'M':
			dokanOptions->Options |= DOKAN_OPTION_REMOVABLE;
			break;
		default:
			fwprintf(stderr, L"unknown option: %s\n", *argv);
			return 2;
		}
	}

	if (gDebugMode)
		dokanOptions->Options |= DOKAN_OPTION_DEBUG;
	dokanOptions->Options |= DOKAN_OPTION_KEEP_ALIVE;
	ZeroMemory(dokanOperations, sizeof(DOKAN_OPERATIONS));
	dokanOperations->CreateFile = UFSCreateFile;
	dokanOperations->OpenDirectory = UFSOpenDirectory;
	dokanOperations->CreateDirectory = UFSCreateDirectory;
	dokanOperations->Cleanup = UFSCleanup;
	dokanOperations->CloseFile = UFSCloseFile;
	dokanOperations->ReadFile = UFSReadFile;
	dokanOperations->WriteFile = UFSWriteFile;
	dokanOperations->FlushFileBuffers = UFSFlushFileBuffers;
	dokanOperations->GetFileInformation = UFSGetFileInformation;
	dokanOperations->FindFiles = UFSFindFiles;
	dokanOperations->FindFilesWithPattern = NULL;
	dokanOperations->SetFileAttributes = UFSSetFileAttributes;
	dokanOperations->SetFileTime = UFSSetFileTime;
	dokanOperations->DeleteFile = UFSDeleteFile;
	dokanOperations->DeleteDirectory = UFSDeleteDirectory;
	dokanOperations->MoveFile = UFSMoveFile;
	dokanOperations->SetEndOfFile = UFSSetEndOfFile;
	dokanOperations->SetAllocationSize = UFSSetAllocationSize;
	dokanOperations->LockFile = UFSLockFile;
	dokanOperations->UnlockFile = UFSUnlockFile;
	dokanOperations->GetDiskFreeSpace = UFSGetDiskFreeSpace;
	dokanOperations->GetVolumeInformation = UFSGetVolumeInformation;
	dokanOperations->Unmount = UFSUnmount;

	status = DokanMain(dokanOptions, dokanOperations);
	switch (status) {
		case DOKAN_SUCCESS:
			fwprintf(stderr, L"Success\n");
			break;
		case DOKAN_ERROR:
			fprintf(stderr, "Error\n");
			break;
		case DOKAN_DRIVE_LETTER_ERROR:
			fprintf(stderr, "Bad Drive letter\n");
			break;
		case DOKAN_DRIVER_INSTALL_ERROR:
			fprintf(stderr, "Can't install driver\n");
			break;
		case DOKAN_START_ERROR:
			fprintf(stderr, "Driver something wrong\n");
			break;
		case DOKAN_MOUNT_ERROR:
			fprintf(stderr, "Can't assign a drive letter\n");
			break;
		default:
			fprintf(stderr, "Unknown error: %d\n", status);
			break;
	}

	free(dokanOptions);
	free(dokanOperations);
	return 0;
}


