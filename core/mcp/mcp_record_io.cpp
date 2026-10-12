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
#include "core/templates/vector.h"

#ifdef WINDOWS_ENABLED
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
		const bool read_ok = GetTokenInformation(token, TokenUser, token_info.ptrw(), size, &size);
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
				!SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) ||
				!SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
			return false;
		}
		attributes.nLength = sizeof(attributes);
		attributes.lpSecurityDescriptor = &descriptor;
		return true;
	}
	bool protect_directory(const String &p_path) {
		Char16String path = p_path.utf16();
		LPWSTR native_path = reinterpret_cast<LPWSTR>(path.ptrw());
		const DWORD flags = GetFileAttributesW(native_path);
		if (flags == INVALID_FILE_ATTRIBUTES || !(flags & FILE_ATTRIBUTE_DIRECTORY) || (flags & FILE_ATTRIBUTE_REPARSE_POINT)) {
			return false;
		}
		PSID owner = nullptr;
		PSECURITY_DESCRIPTOR existing = nullptr;
		if (GetNamedSecurityInfoW(native_path, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &existing) != ERROR_SUCCESS) {
			return false;
		}
		const bool own_directory = EqualSid(owner, reinterpret_cast<TOKEN_USER *>(token_info.ptrw())->User.Sid);
		LocalFree(existing);
		return own_directory && SetNamedSecurityInfoW(native_path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr) == ERROR_SUCCESS;
	}
};
#endif

} // namespace

Error MCPRecordIO::write_atomic(const String &p_path, const String &p_text, String *r_error) {
	if (r_error) {
		*r_error = String();
	}
	if (!p_path.is_absolute_path() || p_path.contains("://")) {
		return fail("MCP records require an absolute filesystem path.", r_error, ERR_INVALID_PARAMETER);
	}
	const String directory = p_path.get_base_dir();
	const Error directory_error = DirAccess::make_dir_recursive_absolute(directory);
	if (directory_error != OK && directory_error != ERR_ALREADY_EXISTS) {
		return fail("Cannot create MCP record directory.", r_error, directory_error);
	}
	String nonce;
	if (MCPProjectIdentity::generate_instance_id(nonce) != OK) {
		return fail("Cannot generate an MCP record temporary name.", r_error, ERR_CANT_CREATE);
	}
	const String temporary_path = p_path + "." + nonce + ".tmp";
	const CharString bytes = p_text.utf8();
#ifdef WINDOWS_ENABLED
	PrivateSecurity security;
	if (!security.initialize() || !security.protect_directory(directory)) {
		return fail("Cannot establish owner-only MCP record permissions.", r_error, ERR_UNAUTHORIZED);
	}
	const Char16String temporary = temporary_path.utf16();
	const Char16String target = p_path.utf16();
	HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(temporary.get_data()), GENERIC_WRITE, 0,
			&security.attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return fail("Cannot create a private MCP record.", r_error, ERR_CANT_CREATE);
	}
	DWORD written = 0;
	const bool stored = WriteFile(file, bytes.get_data(), bytes.length(), &written, nullptr) && written == DWORD(bytes.length()) && FlushFileBuffers(file);
	CloseHandle(file);
	if (!stored) {
		DeleteFileW(reinterpret_cast<LPCWSTR>(temporary.get_data()));
		return fail("Cannot write MCP record.", r_error, ERR_FILE_CANT_WRITE);
	}
	// Unlike DirAccessWindows::rename, this never deletes the destination first.
	if (!MoveFileExW(reinterpret_cast<LPCWSTR>(temporary.get_data()), reinterpret_cast<LPCWSTR>(target.get_data()), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		DeleteFileW(reinterpret_cast<LPCWSTR>(temporary.get_data()));
		return fail("Cannot atomically replace MCP record; the previous record is intact.", r_error, ERR_BUSY);
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
		return fail("Cannot open MCP record.", r_error, GetLastError() == ERROR_FILE_NOT_FOUND ? ERR_FILE_NOT_FOUND : ERR_CANT_OPEN);
	}
	LARGE_INTEGER size;
	if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || uint64_t(size.QuadPart) > p_max_bytes) {
		CloseHandle(file);
		return fail("MCP record exceeds its size limit.", r_error, ERR_FILE_CORRUPT);
	}
	Vector<uint8_t> bytes;
	bytes.resize(size.QuadPart);
	DWORD read_bytes = 0;
	const bool read_ok = ReadFile(file, bytes.ptrw(), bytes.size(), &read_bytes, nullptr) && read_bytes == DWORD(bytes.size());
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
