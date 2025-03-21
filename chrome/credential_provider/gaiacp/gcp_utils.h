// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_CREDENTIAL_PROVIDER_GAIACP_GCP_UTILS_H_
#define CHROME_CREDENTIAL_PROVIDER_GAIACP_GCP_UTILS_H_

#include <memory>
#include <string>

#include "base/callback.h"
#include "base/files/file_path.h"
#include "base/strings/string16.h"
#include "base/values.h"
#include "base/version.h"
#include "base/win/scoped_handle.h"
#include "base/win/windows_types.h"
#include "chrome/credential_provider/gaiacp/scoped_handle.h"
#include "chrome/credential_provider/gaiacp/scoped_lsa_policy.h"
#include "url/gurl.h"

// These define are documented in
// https://msdn.microsoft.com/en-us/library/bb470234(v=vs.85).aspx not available
// in the user mode headers.
#define DIRECTORY_QUERY 0x00000001
#define DIRECTORY_TRAVERSE 0x00000002
#define DIRECTORY_CREATE_OBJECT 0x00000004
#define DIRECTORY_CREATE_SUBDIRECTORY 0x00000008
#define DIRECTORY_ALL_ACCESS (STANDARD_RIGHTS_REQUIRED | 0xF)

namespace base {

class CommandLine;
class FilePath;

}  // namespace base

namespace credential_provider {

// Windows supports a maximum of 20 characters plus null in username.
constexpr int kWindowsUsernameBufferLength = 21;

// Maximum domain length is 256 characters including null.
// https://support.microsoft.com/en-ca/help/909264/naming-conventions-in-active-directory-for-computers-domains-sites-and
constexpr int kWindowsDomainBufferLength = 256;

// According to:
// https://stackoverflow.com/questions/1140528/what-is-the-maximum-length-of-a-sid-in-sddl-format
constexpr int kWindowsSidBufferLength = 184;

// Max number of attempts to find a new username when a user already exists
// with the same username.
constexpr int kMaxUsernameAttempts = 10;

// First index to append to a username when another user with the same name
// already exists.
constexpr int kInitialDuplicateUsernameIndex = 2;

// Default extension used as a fallback if the picture_url returned from gaia
// does not have a file extension.
extern const wchar_t kDefaultProfilePictureFileExtension[];

// Because of some strange dependency problems with windows header files,
// define STATUS_SUCCESS here instead of including ntstatus.h or SubAuth.h
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)

// A bitfield indicating which standard handles are to be created.
using StdHandlesToCreate = uint32_t;

enum : uint32_t {
  kStdOutput = 1 << 0,
  kStdInput = 1 << 1,
  kStdError = 1 << 2,
  kAllStdHandles = kStdOutput | kStdInput | kStdError
};

// Filled in by InitializeStdHandles to return the parent side of stdin/stdout/
// stderr pipes of the login UI process.
struct StdParentHandles {
  StdParentHandles();
  ~StdParentHandles();

  base::win::ScopedHandle hstdin_write;
  base::win::ScopedHandle hstdout_read;
  base::win::ScopedHandle hstderr_read;
};

// Class used in tests to set registration data for testing.
class GoogleRegistrationDataForTesting {
 public:
  explicit GoogleRegistrationDataForTesting(base::string16 serial_number);
  ~GoogleRegistrationDataForTesting();
};

// Class used in tests to set chrome path for testing.
class GoogleChromePathForTesting {
 public:
  explicit GoogleChromePathForTesting(base::FilePath chrome_path);
  ~GoogleChromePathForTesting();
};

// Process startup options that allows customization of stdin/stdout/stderr
// handles.
class ScopedStartupInfo {
 public:
  ScopedStartupInfo();
  explicit ScopedStartupInfo(const wchar_t* desktop);
  ~ScopedStartupInfo();

  // This function takes ownership of the handles.
  HRESULT SetStdHandles(base::win::ScopedHandle* hstdin,
                        base::win::ScopedHandle* hstdout,
                        base::win::ScopedHandle* hstderr);

  LPSTARTUPINFOW GetInfo() { return &info_; }

  // Releases all resources held by this info.
  void Shutdown();

 private:
  STARTUPINFOW info_;
  base::string16 desktop_;
};

// Gets the brand specific path in which to install GCPW.
base::FilePath::StringType GetInstallParentDirectoryName();

// Gets the directory where the GCP is installed
base::FilePath GetInstallDirectory();

// Deletes versions of GCP found under |gcp_path| except for version
// |product_version|.
void DeleteVersionsExcept(const base::FilePath& gcp_path,
                          const base::string16& product_version);

// Waits for the process specified by |procinfo| to terminate.  The handles
// in |read_handles| can be used to read stdout/err from the process.  Upon
// return, |exit_code| contains one of the UIEC_xxx constants listed above,
// and |stdout_buffer| and |stderr_buffer| contain the output, if any.
// Both buffers must be at least |buffer_size| characters long.
HRESULT WaitForProcess(base::win::ScopedHandle::Handle process_handle,
                       const StdParentHandles& parent_handles,
                       DWORD* exit_code,
                       char* output_buffer,
                       int buffer_size);

// Creates a restricted, batch or interactive login token for the given user.
HRESULT CreateLogonToken(const wchar_t* domain,
                         const wchar_t* username,
                         const wchar_t* password,
                         bool interactive,
                         base::win::ScopedHandle* token);

HRESULT CreateJobForSignin(base::win::ScopedHandle* job);

// Creates a pipe that can be used by a parent process to communicate with a
// child process.  If |child_reads| is false, then it is expected that the
// parent process will read from |reading| anything the child process writes
// to |writing|.  For example, this is used to read stdout/stderr of child.
//
// If |child_reads| is true, then it is expected that the child process will
// read from |reading| anything the parent process writes to |writing|.  For
// example, this is used to write to stdin of child.
//
// If |use_nul| is true, then the parent's handle is not used (can be passed
// as nullptr).  The child reads from or writes to the null device.
HRESULT CreatePipeForChildProcess(bool child_reads,
                                  bool use_nul,
                                  base::win::ScopedHandle* reading,
                                  base::win::ScopedHandle* writing);

// Initializes 3 pipes for communicating with a child process.  On return,
// |startupinfo| will be set with the handles needed by the child.  This is
// used when creating the child process.  |parent_handles| contains the
// corresponding handles to be used by the parent process.
//
// Communication direction is used to optimize handle creation.  If
// communication occurs in only one direction then some pipes will be directed
// to the nul device.
enum class CommDirection {
  kParentToChildOnly,
  kChildToParentOnly,
  kBidirectional,
};
HRESULT InitializeStdHandles(CommDirection direction,
                             StdHandlesToCreate to_create,
                             ScopedStartupInfo* startupinfo,
                             StdParentHandles* parent_handles);

// Fills |path_to_dll| with the short path to the dll referenced by
// |dll_handle|. The short path is needed to correctly call rundll32.exe in
// cases where there might be quotes or spaces in the path.
HRESULT GetPathToDllFromHandle(HINSTANCE dll_handle,
                               base::FilePath* path_to_dll);

// This function gets a correctly formatted entry point argument to pass to
// rundll32.exe for a dll referenced by the handle |dll_handle| and an entry
// point function with the name |entrypoint|. |entrypoint_arg| will be filled
// with the argument value.
HRESULT GetEntryPointArgumentForRunDll(HINSTANCE dll_handle,
                                       const wchar_t* entrypoint,
                                       base::string16* entrypoint_arg);

// This function is used to build the command line for rundll32 to call an
// exported entrypoint from the DLL given by |dll_handle|.
// Returns S_FALSE if a command line can successfully be built but if the
// path to the "dll" actually points to a non ".dll" file. This allows
// detection of calls to this function via a unit test which will be
// running under an ".exe" module.
HRESULT GetCommandLineForEntrypoint(HINSTANCE dll_handle,
                                    const wchar_t* entrypoint,
                                    base::CommandLine* command_line);

// Looks up the name associated to the |sid| (if any). Returns an error on any
// failure or no name is associated with the |sid|.
HRESULT LookupLocalizedNameBySid(PSID sid, base::string16* localized_name);

// Looks up the name associated to the well known |sid_type| (if any). Returns
// an error on any failure or no name is associated with the |sid_type|.
HRESULT LookupLocalizedNameForWellKnownSid(WELL_KNOWN_SID_TYPE sid_type,
                                           base::string16* localized_name);

// Handles the writing and deletion of a startup sentinel file used to ensure
// that the GCPW does not crash continuously on startup and render the
// winlogon process unusable.
bool VerifyStartupSentinel();
void DeleteStartupSentinel();
void DeleteStartupSentinelForVersion(const base::string16& version);

// Gets a string resource from the DLL with the given id.
base::string16 GetStringResource(int base_message_id);

// Gets a string resource from the DLL with the given id after replacing the
// placeholders with the provided substitutions.
base::string16 GetStringResource(int base_message_id,
                                 const std::vector<base::string16>& subst);

// Gets the language selected by the base::win::i18n::LanguageSelector.
base::string16 GetSelectedLanguage();

// Securely clear a base::Value that may be a dictionary value that may
// have a password field.
void SecurelyClearDictionaryValue(base::Optional<base::Value>* value);
void SecurelyClearDictionaryValueWithKey(base::Optional<base::Value>* value,
                                         const std::string& password_key);

// Securely clear base:string16 and std::string.
void SecurelyClearString(base::string16& str);
void SecurelyClearString(std::string& str);

// Securely clear a given |buffer| with size |length|.
void SecurelyClearBuffer(void* buffer, size_t length);

// Helpers to get strings from base::Values that are expected to be
// DictionaryValues.

base::string16 GetDictString(const base::Value& dict, const char* name);
base::string16 GetDictString(const std::unique_ptr<base::Value>& dict,
                             const char* name);
// Perform a recursive search on a nested dictionary object. Note that the
// names provided in the input should be in order. Below is an example : Lets
// say the json object is {"key1": {"key2": {"key3": "value1"}}, "key4":
// "value2"}. Then to search for the key "key3", this method should be called
// by providing the |path| as {"key1", "key2", "key3"}.
std::string SearchForKeyInStringDictUTF8(
    const std::string& json_string,
    const std::initializer_list<base::StringPiece>& path);

// Perform a recursive search on a nested dictionary object. Note that the
// names provided in the input should be in order. Below is an example : Lets
// say the json object is
// {"key1": {"key2": {"value": "value1", "value": "value2"}}}.
// Then to search for the key "key2" and list_key as "value", then this method
// should be called by providing |list_key| as "value", |path| as
// ["key1", "key2"] and the result returned would be ["value1", "value2"].
HRESULT SearchForListInStringDictUTF8(
    const std::string& list_key,
    const std::string& json_string,
    const std::initializer_list<base::StringPiece>& path,
    std::vector<std::string>* output);
std::string GetDictStringUTF8(const base::Value& dict, const char* name);
std::string GetDictStringUTF8(const std::unique_ptr<base::Value>& dict,
                              const char* name);

// Returns the major build version of Windows by reading the registry.
// See:
// https://stackoverflow.com/questions/31072543/reliable-way-to-get-windows-version-from-registry
base::string16 GetWindowsVersion();

// Returns the minimum supported version of Chrome for GCPW.
base::Version GetMinimumSupportedChromeVersion();

class OSUserManager;
class OSProcessManager;

// This structure is used in tests to set fake objects in the credential
// provider dll.  See the function SetFakesForTesting() for details.
struct FakesForTesting {
  FakesForTesting();
  ~FakesForTesting();

  ScopedLsaPolicy::CreatorCallback scoped_lsa_policy_creator;
  OSUserManager* os_user_manager_for_testing = nullptr;
  OSProcessManager* os_process_manager_for_testing = nullptr;
};

// DLL entrypoint signature for settings testing fakes.  This is used by
// the setup tests to install fakes into the dynamically loaded gaia1_0 DLL
// static data.  This way the production DLL does not need to include binary
// code used only for testing.
typedef void CALLBACK (*SetFakesForTestingFn)(const FakesForTesting* fakes);

// Initializes the members of a Windows STRING struct (UNICODE_STRING or
// LSA_STRING) to point to the string pointed to by |string|.
template <class WindowsStringT,
          class WindowsStringCharT = decltype(WindowsStringT().Buffer[0])>
void InitWindowsStringWithString(const WindowsStringCharT* string,
                                 WindowsStringT* windows_string) {
  constexpr size_t buffer_char_size = sizeof(WindowsStringCharT);
  windows_string->Buffer = const_cast<WindowsStringCharT*>(string);
  windows_string->Length = static_cast<USHORT>(
      std::char_traits<WindowsStringCharT>::length((windows_string->Buffer)) *
      buffer_char_size);
  windows_string->MaximumLength = windows_string->Length + buffer_char_size;
}

// Extracts the provided keys from the given dictionary. Returns true if all
// keys are found. If any of the key isn't found, returns false.
bool ExtractKeysFromDict(
    const base::Value& dict,
    const std::vector<std::pair<std::string, std::string*>>& needed_outputs);

// Gets the bios serial number of the windows device.
base::string16 GetSerialNumber();

// Gets the obfuscated device_id that is a combination of multiple device
// identifiers.
HRESULT GenerateDeviceId(std::string* device_id);

// Overrides the gaia_url and gcpw_endpoint_path that is used to load GLS.
HRESULT SetGaiaEndpointCommandLineIfNeeded(const wchar_t* override_registry_key,
                                           const std::string& default_endpoint,
                                           bool provide_deviceid,
                                           bool show_tos,
                                           base::CommandLine* command_line);

// Returns the file path to installed chrome.exe.
base::FilePath GetChromePath();

// Returns the file path to system installed chrome.exe.
base::FilePath GetSystemChromePath();

}  // namespace credential_provider

#endif  // CHROME_CREDENTIAL_PROVIDER_GAIACP_GCP_UTILS_H_
