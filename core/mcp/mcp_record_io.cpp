/**************************************************************************/
/*  mcp_record_io.cpp                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "mcp_record_io.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/mcp/mcp_project_identity.h"

#ifdef WINDOWS_ENABLED
#include "core/templates/vector.h"

#include <windows.h>

#include <aclapi.h>
#elif defined(UNIX_ENABLED)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#endif

namespace {

Error fail(const String &p_message, String *r_error, Error p_error) {
	if (r_error) {
		*r_error = p_message;
	}
	return p_error;
}

#ifdef WINDOWS_ENABLED
// Godot's Windows shim undefines DELETE; retain its exact standard access bit.
constexpr DWORD FILE_DELETE_ACCESS = 0x00010000;

class PrivateSecurity {
	Vector<uint8_t> token_info;
	PACL acl = nullptr;
	SECURITY_DESCRIPTOR descriptor;

public:
	SECURITY_ATTRIBUTES attributes = {};
	~PrivateSecurity() {
		if (acl) {
			LocalFree(acl);
		}
	}
	bool initialize() {
		HANDLE token = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
			return false;
		}
		DWORD size = 0;
		GetTokenInformation(token, TokenUser, nullptr, 0, &size);
		if (size == 0) {
			CloseHandle(token);
			return false;
		}
		token_info.resize(size);
		const bool read_ok = GetTokenInformation(token, TokenUser, token_info.ptrw(), size, &size) != FALSE;
		CloseHandle(token);
		if (!read_ok) {
			return false;
		}
		EXPLICIT_ACCESSW access = {};
		access.grfAccessPermissions = GENERIC_ALL;
		access.grfAccessMode = SET_ACCESS;
		access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
		access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
		access.Trustee.TrusteeType = TRUSTEE_IS_USER;
		access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(reinterpret_cast<TOKEN_USER *>(token_info.ptrw())->User.Sid);
		if (SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS ||
				!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
				!SetSecurityDescriptorOwner(&descriptor, reinterpret_cast<TOKEN_USER *>(token_info.ptrw())->User.Sid, FALSE) ||
				!SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) ||
				!SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
			return false;
		}
		attributes.nLength = sizeof(attributes);
		attributes.lpSecurityDescriptor = &descriptor;
		return true;
	}
	bool protect_directory(const String &p_path, DWORD &r_error) {
		const Char16String path = p_path.utf16();
		HANDLE directory = CreateFileW(reinterpret_cast<LPCWSTR>(path.get_data()), READ_CONTROL | WRITE_DAC,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
				FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if (directory == INVALID_HANDLE_VALUE) {
			r_error = GetLastError();
			return false;
		}
		BY_HANDLE_FILE_INFORMATION info;
		if (!GetFileInformationByHandle(directory, &info)) {
			r_error = GetLastError();
			CloseHandle(directory);
			return false;
		}
		if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
			r_error = ERROR_DIRECTORY;
			CloseHandle(directory);
			return false;
		}
		PSID owner = nullptr;
		PSECURITY_DESCRIPTOR existing = nullptr;
		r_error = GetSecurityInfo(directory, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &existing);
		if (r_error != ERROR_SUCCESS) {
			CloseHandle(directory);
			return false;
		}
		const bool own_directory = EqualSid(owner, reinterpret_cast<TOKEN_USER *>(token_info.ptrw())->User.Sid) != FALSE;
		LocalFree(existing);
		r_error = own_directory ? SetSecurityInfo(directory, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr) : ERROR_INVALID_OWNER;
		CloseHandle(directory);
		return r_error == ERROR_SUCCESS;
	}
};
#endif

} // namespace

Error MCPRecordIO::make_private_directory(const String &p_path) {
	if (!p_path.is_absolute_path() || p_path.contains("://")) {
		return ERR_INVALID_PARAMETER;
	}
#ifdef WINDOWS_ENABLED
	PrivateSecurity security;
	if (!security.initialize()) {
		return ERR_UNAUTHORIZED;
	}
	const Char16String path = p_path.utf16();
	if (CreateDirectoryW(reinterpret_cast<LPCWSTR>(path.get_data()), &security.attributes)) {
		return OK;
	}
	return GetLastError() == ERROR_ALREADY_EXISTS ? ERR_ALREADY_EXISTS : ERR_CANT_CREATE;
#elif defined(UNIX_ENABLED)
	const CharString path = p_path.utf8();
	if (mkdir(path.get_data(), 0700) == 0) {
		return OK;
	}
	return errno == EEXIST ? ERR_ALREADY_EXISTS : ERR_CANT_CREATE;
#else
	return ERR_UNAVAILABLE;
#endif
}

Error MCPRecordIO::ensure_private_directory(const String &p_path) {
	if (!p_path.is_absolute_path() || p_path.contains("://")) {
		return ERR_INVALID_PARAMETER;
	}
	if (DirAccess::dir_exists_absolute(p_path)) {
		return OK;
	}
	const String parent = p_path.get_base_dir();
	if (parent.is_empty() || parent == p_path) {
		return ERR_CANT_CREATE;
	}
	const Error parent_error = ensure_private_directory(parent);
	if (parent_error != OK) {
		return parent_error;
	}
	const Error error = make_private_directory(p_path);
	if (error == ERR_ALREADY_EXISTS && DirAccess::dir_exists_absolute(p_path)) {
		return OK;
	}
	return error;
}

Error MCPRecordIO::write_atomic(const String &p_path, const String &p_text, String *r_error) {
	if (r_error) {
		*r_error = String();
	}
	if (!p_path.is_absolute_path() || p_path.contains("://")) {
		return fail("MCP records require an absolute filesystem path.", r_error, ERR_INVALID_PARAMETER);
	}
	const String directory = p_path.get_base_dir();
#ifdef WINDOWS_ENABLED
	PrivateSecurity security;
	if (!security.initialize()) {
		return fail("Cannot initialize private MCP record permissions.", r_error, ERR_UNAUTHORIZED);
	}
	const Error directory_error = ensure_private_directory(directory.get_base_dir());
	if (directory_error != OK && directory_error != ERR_ALREADY_EXISTS) {
		return fail("Cannot create MCP record parent directory.", r_error, directory_error);
	}
	const Error create_error = make_private_directory(directory);
	if (create_error != OK && create_error != ERR_ALREADY_EXISTS) {
		return fail("Cannot create a private MCP record directory.", r_error, create_error);
	}
	DWORD security_error = ERROR_SUCCESS;
	if (!security.protect_directory(directory, security_error)) {
		return fail(vformat("Cannot establish owner-only MCP record permissions (Windows error %d).", security_error), r_error, ERR_UNAUTHORIZED);
	}
#else
	const Error directory_error = ensure_private_directory(directory);
	if (directory_error != OK && directory_error != ERR_ALREADY_EXISTS) {
		return fail("Cannot create MCP record directory.", r_error, directory_error);
	}
#endif
	String nonce;
	if (MCPProjectIdentity::generate_instance_id(nonce) != OK) {
		return fail("Cannot generate an MCP record temporary name.", r_error, ERR_CANT_CREATE);
	}
	const String temporary_path = p_path + "." + nonce + ".tmp";
	const CharString bytes = p_text.utf8();
#ifdef WINDOWS_ENABLED
	const Char16String temporary = temporary_path.utf16();
	const Char16String target = p_path.replace("/", "\\").utf16();
	// This handle survives publication until CloseHandle below. Its sharing must
	// already permit readers and later replacements at the published filename.
	HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(temporary.get_data()), GENERIC_WRITE | FILE_DELETE_ACCESS,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &security.attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return fail("Cannot create a private MCP record.", r_error, ERR_CANT_CREATE);
	}
	DWORD written = 0;
	const bool stored = WriteFile(file, bytes.get_data(), DWORD(bytes.length()), &written, nullptr) && written == DWORD(bytes.length()) && FlushFileBuffers(file);
	if (!stored) {
		CloseHandle(file);
		DeleteFileW(reinterpret_cast<LPCWSTR>(temporary.get_data()));
		return fail("Cannot write MCP record.", r_error, ERR_FILE_CANT_WRITE);
	}
	// FILE_SHARE_DELETE alone does not let MoveFileEx replace an open target.
	// POSIX rename semantics keep existing readers on the old complete record
	// while new opens see the replacement. Readers denying delete still block it.
	const uint64_t rename_size = sizeof(FILE_RENAME_INFO) + uint64_t(target.length()) * sizeof(WCHAR);
	if (rename_size > MAXDWORD) {
		CloseHandle(file);
		DeleteFileW(reinterpret_cast<LPCWSTR>(temporary.get_data()));
		return fail("MCP record path exceeds the native rename size limit.", r_error, ERR_INVALID_PARAMETER);
	}
	Vector<uint8_t> rename_buffer;
	rename_buffer.resize(rename_size);
	memset(rename_buffer.ptrw(), 0, rename_buffer.size());
	FILE_RENAME_INFO *rename_info = reinterpret_cast<FILE_RENAME_INFO *>(rename_buffer.ptrw());
	rename_info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
	rename_info->FileNameLength = DWORD(target.length() * sizeof(WCHAR));
	memcpy(rename_info->FileName, target.get_data(), rename_info->FileNameLength);
	bool renamed = SetFileInformationByHandle(file, FileRenameInfoEx, rename_info, DWORD(rename_buffer.size())) != FALSE;
	DWORD rename_error = renamed ? ERROR_SUCCESS : GetLastError();
	CloseHandle(file);
	if (!renamed && (rename_error == ERROR_INVALID_PARAMETER || rename_error == ERROR_NOT_SUPPORTED || rename_error == ERROR_INVALID_FUNCTION)) {
		// Older Windows/filesystems may lack extended rename support. Retain a
		// safe atomic fallback; never delete the destination to make it succeed.
		renamed = MoveFileExW(reinterpret_cast<LPCWSTR>(temporary.get_data()), reinterpret_cast<LPCWSTR>(target.get_data()), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
		rename_error = renamed ? ERROR_SUCCESS : GetLastError();
	}
	if (!renamed) {
		DeleteFileW(reinterpret_cast<LPCWSTR>(temporary.get_data()));
		return fail(vformat("Cannot atomically replace MCP record (Windows error %d); the previous record is intact.", rename_error), r_error, ERR_BUSY);
	}
	return OK;
#elif defined(UNIX_ENABLED)
	const CharString directory_path = directory.utf8();
	const int directory_fd = ::open(directory_path.get_data(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (directory_fd < 0) {
		return fail("Cannot open MCP record directory safely.", r_error, ERR_CANT_OPEN);
	}
	struct stat directory_stat;
	if (fstat(directory_fd, &directory_stat) != 0 || directory_stat.st_uid != geteuid() || fchmod(directory_fd, 0700) != 0) {
		::close(directory_fd);
		return fail("Cannot establish owner-only MCP record permissions.", r_error, ERR_UNAUTHORIZED);
	}
	const CharString temporary_name = temporary_path.get_file().utf8();
	const CharString target_name = p_path.get_file().utf8();
	const int file = openat(directory_fd, temporary_name.get_data(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (file < 0) {
		::close(directory_fd);
		return fail("Cannot create a private MCP record.", r_error, ERR_CANT_CREATE);
	}
	int offset = 0;
	bool stored = true;
	while (offset < bytes.length()) {
		const ssize_t written = ::write(file, bytes.get_data() + offset, bytes.length() - offset);
		if (written < 0 && errno == EINTR) {
			continue;
		}
		if (written <= 0) {
			stored = false;
			break;
		}
		offset += written;
	}
	stored = stored && fsync(file) == 0;
	if (::close(file) != 0) {
		stored = false;
	}
	Error result = OK;
	if (!stored) {
		result = fail("Cannot write MCP record.", r_error, ERR_FILE_CANT_WRITE);
	} else if (renameat(directory_fd, temporary_name.get_data(), directory_fd, target_name.get_data()) != 0) {
		result = fail("Cannot atomically replace MCP record; the previous record is intact.", r_error, ERR_CANT_CREATE);
	}
	if (result != OK) {
		unlinkat(directory_fd, temporary_name.get_data(), 0);
	}
	::close(directory_fd);
	return result;
#else
	return fail("Private MCP record storage is unsupported on this platform.", r_error, ERR_UNAVAILABLE);
#endif
}

Error MCPRecordIO::read(const String &p_path, uint64_t p_max_bytes, String &r_text, String *r_error) {
	r_text = String();
#ifdef WINDOWS_ENABLED
	const Char16String path = p_path.utf16();
	HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(path.get_data()), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		const DWORD error = GetLastError();
		return fail("Cannot open MCP record.", r_error, error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ERR_FILE_NOT_FOUND : ERR_CANT_OPEN);
	}
	LARGE_INTEGER size;
	if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || uint64_t(size.QuadPart) > p_max_bytes || uint64_t(size.QuadPart) > MAXDWORD) {
		CloseHandle(file);
		return fail("MCP record exceeds its size limit.", r_error, ERR_FILE_CORRUPT);
	}
	Vector<uint8_t> bytes;
	bytes.resize(size.QuadPart);
	DWORD read_bytes = 0;
	const bool read_ok = ReadFile(file, bytes.ptrw(), DWORD(bytes.size()), &read_bytes, nullptr) && read_bytes == DWORD(bytes.size());
	CloseHandle(file);
	if (!read_ok) {
		return fail("Cannot read complete MCP record.", r_error, ERR_FILE_CANT_READ);
	}
	return r_text.append_utf8(reinterpret_cast<const char *>(bytes.ptr()), bytes.size());
#else
	Error error = OK;
	const Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ, &error);
	if (file.is_null()) {
		return fail("Cannot open MCP record.", r_error, error == OK ? ERR_CANT_OPEN : error);
	}
	if (file->get_length() > p_max_bytes) {
		return fail("MCP record exceeds its size limit.", r_error, ERR_FILE_CORRUPT);
	}
	r_text = file->get_as_utf8_string();
	return OK;
#endif
}
