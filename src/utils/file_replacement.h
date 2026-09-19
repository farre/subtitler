#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

namespace subtitler {

// A same-filesystem replacement with an explicit rollback point. The
// private directory has a short unique name, independent of NAME_MAX on
// the destination. This is ordinary I/O recovery, not a power-loss journal.
class FileReplacement {
 public:
  static std::unique_ptr<FileReplacement> Stage(
      const std::filesystem::path& target, std::string_view contents) {
    auto replacement = std::unique_ptr<FileReplacement>{new FileReplacement};
    replacement->target_ = target;
    const auto parent = target.has_parent_path() ? target.parent_path()
                                                : std::filesystem::path{"."};
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) {
      return nullptr;
    }
    auto pattern = (parent / ".subtitler-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr) {
      return nullptr;
    }
    replacement->directory_ = pattern;
    replacement->had_original_ = std::filesystem::exists(target, error);
    if (error) {
      return nullptr;
    }
    if (replacement->had_original_) {
      if (!std::filesystem::is_regular_file(target, error) || error ||
          !std::filesystem::copy_file(target, replacement->Previous(), error)) {
        return nullptr;
      }
    }
    std::ofstream file{replacement->Next(), std::ios::binary | std::ios::trunc};
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    file.close();
    if (file.fail()) {
      return nullptr;
    }
    return replacement;
  }

  ~FileReplacement() {
    // Preserve the backup for recovery if restoring it also fails.
    if (!Rollback() || directory_.empty()) {
      return;
    }
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }
  FileReplacement(const FileReplacement&) = delete;
  FileReplacement& operator=(const FileReplacement&) = delete;

  bool Install() {
    if (installed_) {
      return false;
    }
    std::error_code error;
    std::filesystem::rename(Next(), target_, error);
    installed_ = !error;
    return installed_;
  }
  bool Rollback() {
    if (!installed_) {
      return true;
    }
    std::error_code error;
    if (had_original_) {
      std::filesystem::rename(Previous(), target_, error);
    } else {
      std::filesystem::remove(target_, error);
    }
    if (error) {
      return false;
    }
    installed_ = false;
    return true;
  }
  void Commit() { installed_ = false; }

 private:
  FileReplacement() = default;
  std::filesystem::path Next() const { return directory_ / "next"; }
  std::filesystem::path Previous() const { return directory_ / "previous"; }

  std::filesystem::path target_;
  std::filesystem::path directory_;
  bool had_original_ = false;
  bool installed_ = false;
};

// The activation callback must invoke restore_file before rebuilding an
// old pipeline that refers to this same pathname. The guard also restores
// on early failures that never touched playback.
template <typename Activate>
bool ReplaceAndActivateFile(const std::filesystem::path& target,
                            std::string_view contents, Activate&& activate) {
  auto replacement = FileReplacement::Stage(target, contents);
  if (!replacement || !replacement->Install()) {
    return false;
  }
  if (!activate(target.string(), [&] { return replacement->Rollback(); })) {
    return false;
  }
  replacement->Commit();
  return true;
}

}  // namespace subtitler
