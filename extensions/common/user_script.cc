// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "extensions/common/user_script.h"

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <utility>

#include "base/atomic_sequence_num.h"
#include "base/command_line.h"
#include "base/pickle.h"
#include "base/strings/pattern.h"
#include "base/strings/string_util.h"
#include "extensions/common/switches.h"

namespace {

// This cannot be a plain int or int64_t because we need to generate unique IDs
// from multiple threads.
base::AtomicSequenceNumber g_user_script_id_generator;

bool UrlMatchesGlobs(const std::vector<std::string>* globs,
                     const GURL& url) {
  for (auto glob = globs->cbegin(); glob != globs->cend(); ++glob) {
    if (base::MatchPattern(url.spec(), *glob))
      return true;
  }

  return false;
}

}  // namespace

namespace extensions {

// The bitmask for valid user script injectable schemes used by URLPattern.
enum {
  kValidUserScriptSchemes = URLPattern::SCHEME_CHROMEUI |
                            URLPattern::SCHEME_HTTP |
                            URLPattern::SCHEME_HTTPS |
                            URLPattern::SCHEME_FILE |
                            URLPattern::SCHEME_FTP
};

// static
const char UserScript::kFileExtension[] = ".user.js";

// static
int UserScript::GenerateUserScriptID() {
  return g_user_script_id_generator.GetNext();
}

bool UserScript::IsURLUserScript(const GURL& url,
                                 const std::string& mime_type) {
  return base::EndsWith(url.ExtractFileName(), kFileExtension,
                        base::CompareCase::INSENSITIVE_ASCII) &&
         mime_type != "text/html";
}

// static
int UserScript::ValidUserScriptSchemes(bool canExecuteScriptEverywhere) {
  if (canExecuteScriptEverywhere)
    return URLPattern::SCHEME_ALL;
  int valid_schemes = kValidUserScriptSchemes;
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kExtensionsOnChromeURLs)) {
    valid_schemes &= ~URLPattern::SCHEME_CHROMEUI;
  }
  return valid_schemes;
}

UserScript::File::File(const base::FilePath& extension_root,
                       const base::FilePath& relative_path,
                       const GURL& url)
    : extension_root_(extension_root),
      relative_path_(relative_path),
      url_(url) {
}

UserScript::File::File() {}

// File content is not copied.
UserScript::File::File(const File& other)
    : extension_root_(other.extension_root_),
      relative_path_(other.relative_path_),
      url_(other.url_) {}

UserScript::File::~File() {}

UserScript::UserScript()
    : run_location_(DOCUMENT_IDLE),
      consumer_instance_type_(TAB),
      user_script_id_(-1),
      emulate_greasemonkey_(false),
      in_main_world_(false),
      match_all_frames_(false),
      match_about_blank_(false),
      incognito_enabled_(false) {}

UserScript::~UserScript() {
}

// static.
std::unique_ptr<UserScript> UserScript::CopyMetadataFrom(
    const UserScript& other) {
  std::unique_ptr<UserScript> script(new UserScript());
  script->run_location_ = other.run_location_;
  script->name_space_ = other.name_space_;
  script->name_ = other.name_;
  script->description_ = other.description_;
  script->version_ = other.version_;
  script->globs_ = other.globs_;
  script->exclude_globs_ = other.exclude_globs_;
  script->url_set_ = other.url_set_.Clone();
  script->exclude_url_set_ = other.exclude_url_set_.Clone();

  // Note: File content is not copied.
  for (const std::unique_ptr<File>& file : other.js_scripts()) {
    std::unique_ptr<File> file_copy(new File(*file));
    script->js_scripts_.push_back(std::move(file_copy));
  }
  for (const std::unique_ptr<File>& file : other.css_scripts()) {
    std::unique_ptr<File> file_copy(new File(*file));
    script->css_scripts_.push_back(std::move(file_copy));
  }
  script->host_id_ = other.host_id_;
  script->consumer_instance_type_ = other.consumer_instance_type_;
  script->user_script_id_ = other.user_script_id_;
  script->emulate_greasemonkey_ = other.emulate_greasemonkey_;
  script->match_all_frames_ = other.match_all_frames_;
  script->match_about_blank_ = other.match_about_blank_;
  script->incognito_enabled_ = other.incognito_enabled_;

  return script;
}

void UserScript::add_url_pattern(const URLPattern& pattern) {
  url_set_.AddPattern(pattern);
}

void UserScript::add_exclude_url_pattern(const URLPattern& pattern) {
  exclude_url_set_.AddPattern(pattern);
}

bool UserScript::MatchesURL(const GURL& url) const {
  if (!url_set_.is_empty()) {
    if (!url_set_.MatchesURL(url))
      return false;
  }

  if (!exclude_url_set_.is_empty()) {
    if (exclude_url_set_.MatchesURL(url))
      return false;
  }

  if (!globs_.empty()) {
    if (!UrlMatchesGlobs(&globs_, url))
      return false;
  }

  if (!exclude_globs_.empty()) {
    if (UrlMatchesGlobs(&exclude_globs_, url))
      return false;
  }

  return true;
}

bool UserScript::MatchesDocument(const GURL& effective_document_url,
                                 bool is_subframe) const {
  if (is_subframe && !match_all_frames())
    return false;

  return MatchesURL(effective_document_url);
}

void UserScript::File::Pickle(base::Pickle* pickle) const {
  pickle->WriteString(url_.spec());
  // Do not write path. It's not needed in the renderer.
  // Do not write content. It will be serialized by other means.
}

void UserScript::File::Unpickle(const base::Pickle& pickle,
                                base::PickleIterator* iter) {
  // Read the url from the pickle.
  std::string url;
  CHECK(iter->ReadString(&url));
  set_url(GURL(url));
}

void UserScript::Pickle(base::Pickle* pickle) const {
  // Write the simple types to the pickle.
  pickle->WriteInt(run_location());
  pickle->WriteInt(user_script_id_);
  pickle->WriteBool(emulate_greasemonkey());
  pickle->WriteBool(in_main_world());
  pickle->WriteBool(match_all_frames());
  pickle->WriteBool(match_about_blank());
  pickle->WriteBool(is_incognito_enabled());

  PickleHostID(pickle, host_id_);
  pickle->WriteInt(consumer_instance_type());
  PickleGlobs(pickle, globs_);
  PickleGlobs(pickle, exclude_globs_);
  PickleURLPatternSet(pickle, url_set_);
  PickleURLPatternSet(pickle, exclude_url_set_);
  PickleScripts(pickle, js_scripts_);
  PickleScripts(pickle, css_scripts_);
}

void UserScript::PickleGlobs(base::Pickle* pickle,
                             const std::vector<std::string>& globs) const {
  pickle->WriteUInt32(globs.size());
  for (auto glob = globs.cbegin(); glob != globs.cend(); ++glob) {
    pickle->WriteString(*glob);
  }
}

void UserScript::PickleHostID(base::Pickle* pickle,
                              const HostID& host_id) const {
  pickle->WriteInt(host_id.type());
  pickle->WriteString(host_id.id());
}

void UserScript::PickleURLPatternSet(base::Pickle* pickle,
                                     const URLPatternSet& pattern_list) const {
  pickle->WriteUInt32(pattern_list.patterns().size());
  for (auto pattern = pattern_list.begin(); pattern != pattern_list.end();
       ++pattern) {
    pickle->WriteInt(pattern->valid_schemes());
    pickle->WriteString(pattern->GetAsString());
  }
}

void UserScript::PickleScripts(base::Pickle* pickle,
                               const FileList& scripts) const {
  pickle->WriteUInt32(scripts.size());
  for (const std::unique_ptr<File>& file : scripts)
    file->Pickle(pickle);
}

void UserScript::Unpickle(const base::Pickle& pickle,
                          base::PickleIterator* iter) {
  // Read the run location.
  int run_location = 0;
  CHECK(iter->ReadInt(&run_location));
  CHECK(run_location >= 0 && run_location < RUN_LOCATION_LAST);
  run_location_ = static_cast<RunLocation>(run_location);

  CHECK(iter->ReadInt(&user_script_id_));
  CHECK(iter->ReadBool(&emulate_greasemonkey_));
  CHECK(iter->ReadBool(&in_main_world_));
  CHECK(iter->ReadBool(&match_all_frames_));
  CHECK(iter->ReadBool(&match_about_blank_));
  CHECK(iter->ReadBool(&incognito_enabled_));

  UnpickleHostID(pickle, iter, &host_id_);

  int consumer_instance_type = 0;
  CHECK(iter->ReadInt(&consumer_instance_type));
  consumer_instance_type_ =
      static_cast<ConsumerInstanceType>(consumer_instance_type);

  UnpickleGlobs(pickle, iter, &globs_);
  UnpickleGlobs(pickle, iter, &exclude_globs_);
  UnpickleURLPatternSet(pickle, iter, &url_set_);
  UnpickleURLPatternSet(pickle, iter, &exclude_url_set_);
  UnpickleScripts(pickle, iter, &js_scripts_);
  UnpickleScripts(pickle, iter, &css_scripts_);
}

void UserScript::UnpickleGlobs(const base::Pickle& pickle,
                               base::PickleIterator* iter,
                               std::vector<std::string>* globs) {
  uint32_t num_globs = 0;
  CHECK(iter->ReadUInt32(&num_globs));
  globs->clear();
  for (uint32_t i = 0; i < num_globs; ++i) {
    std::string glob;
    CHECK(iter->ReadString(&glob));
    globs->push_back(glob);
  }
}

void UserScript::UnpickleHostID(const base::Pickle& pickle,
                                base::PickleIterator* iter,
                                HostID* host_id) {
  int type = 0;
  std::string id;
  CHECK(iter->ReadInt(&type));
  CHECK(iter->ReadString(&id));
  *host_id = HostID(static_cast<HostID::HostType>(type), id);
}

void UserScript::UnpickleURLPatternSet(const base::Pickle& pickle,
                                       base::PickleIterator* iter,
                                       URLPatternSet* pattern_list) {
  uint32_t num_patterns = 0;
  CHECK(iter->ReadUInt32(&num_patterns));

  pattern_list->ClearPatterns();
  for (uint32_t i = 0; i < num_patterns; ++i) {
    int valid_schemes;
    CHECK(iter->ReadInt(&valid_schemes));

    std::string pattern_str;
    CHECK(iter->ReadString(&pattern_str));

    URLPattern pattern(kValidUserScriptSchemes);
    URLPattern::ParseResult result = pattern.Parse(pattern_str);
    CHECK(URLPattern::ParseResult::kSuccess == result)
        << URLPattern::GetParseResultString(result) << " "
        << pattern_str.c_str();

    pattern.SetValidSchemes(valid_schemes);
    pattern_list->AddPattern(pattern);
  }
}

void UserScript::UnpickleScripts(const base::Pickle& pickle,
                                 base::PickleIterator* iter,
                                 FileList* scripts) {
  uint32_t num_files = 0;
  CHECK(iter->ReadUInt32(&num_files));
  scripts->clear();
  for (uint32_t i = 0; i < num_files; ++i) {
    std::unique_ptr<File> file(new File());
    file->Unpickle(pickle, iter);
    scripts->push_back(std::move(file));
  }
}

UserScriptIDPair::UserScriptIDPair(int id, const HostID& host_id)
    : id(id), host_id(host_id) {}

UserScriptIDPair::UserScriptIDPair(int id) : id(id), host_id(HostID()) {}

bool operator<(const UserScriptIDPair& a, const UserScriptIDPair& b) {
  return a.id < b.id;
}

}  // namespace extensions
