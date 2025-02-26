#include <omp.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "db/catalog/meta_types.hpp"
#include "db/table_segment_mvp.hpp"
#include "utils/atomic_counter.hpp"
#include "utils/common_util.hpp"
#include "utils/json.hpp"

namespace vectordb {
namespace engine {
// const std::chrono::minutes ROTATION_INTERVAL(10);
// const std::chrono::days LOG_RETENTION(7);
const std::chrono::seconds ROTATION_INTERVAL(600);
const std::chrono::seconds LOG_RETENTION(3600 * 24 * 7);

enum LogEntryType {
  INSERT = 1,
  DELETE = 2
};

class WriteAheadLog {
 public:
  WriteAheadLog(std::string base_path, int64_t table_id)
      : logs_folder_(base_path + "/" + std::to_string(table_id) + "/wal/"),
        last_rotation_time_(std::chrono::system_clock::now()) {
    // Load the last ID from the disk
    std::ifstream id_file(logs_folder_ + "/last_id.txt");
    if (id_file.is_open()) {
      int64_t last_global_id;
      id_file >> last_global_id;
      global_counter_.SetValue(last_global_id);
      id_file.close();
    }
    auto mkdir_status = server::CommonUtil::CreateDirectory(logs_folder_);
    if (!mkdir_status.ok()) {
      throw mkdir_status.message();
    }
    RotateFile();
  }

  ~WriteAheadLog() {
    if (file_ != nullptr) {
      fclose(file_);
    }
    // Save the last ID to the disk
    std::ofstream id_file(logs_folder_ + "/last_id.txt");
    id_file << global_counter_.Get();
    id_file.close();
  }

  int64_t WriteEntry(LogEntryType type, const std::string &entry) {
    // Skip WAL for realtime scenario
    if (!enabled_) {
      return global_counter_.Get();
    }

    auto now = std::chrono::system_clock::now();
    if (now - last_rotation_time_ > ROTATION_INTERVAL) {
      RotateFile();
    }
    int64_t next = global_counter_.IncrementAndGet();
#ifdef __APPLE__
    fprintf(file_, "%lld %d %s\n", next, type, entry.c_str());
#else
    fprintf(file_, "%ld %d %s\n", next, type, entry.c_str());
#endif
    fflush(file_);
    // Tradeoff of data consistency. We use fflush for now.
    // fsync(fileno(file_));
    return next;
  }

  void Replay(meta::TableSchema &table_schema, std::shared_ptr<TableSegmentMVP> segment) {
    std::vector<std::filesystem::path> files;
    GetSortedLogFiles(files);
    for (auto pt = 0; pt < files.size(); ++pt) {
      auto file = files[pt];
      bool update = false;
      std::ifstream in(file.string());
      std::string line;
      while (std::getline(in, line)) {
        // Entry ID
        size_t first_space = line.find(' ');
        int64_t global_id = std::stoll(line.substr(0, first_space));
        if (global_counter_.Get() < global_id) {
          global_counter_.SetValue(global_id);
        }
        // If the entry ID is less than or equal to the consumed ID, ignore it
        if (global_id <= segment->wal_global_id_) {
          continue;
        }
        update = true;
        // Entry type
        size_t second_space = line.find(' ', first_space + 1);
        LogEntryType type = static_cast<LogEntryType>(std::stoi(line.substr(first_space + 1, second_space - first_space - 1)));
        // Entry content
        std::string content = line.substr(second_space + 1);

        // Otherwise, replay the entry
        ApplyEntry(table_schema, segment, global_id, type, content);
      }
      // Close the file.
      in.close();
      // Delete the file if the whole file is already in table.
      if (!update && pt < files.size() - 1) {
        server::CommonUtil::RemoveFile(file.string());
      }
    }
    // Save the last ID to the disk
    std::ofstream id_file(logs_folder_ + "/last_id.txt");
    id_file << global_counter_.Get();
    id_file.close();
  }

  void CleanUpOldFiles() {
    // Get the current time
    auto now = std::chrono::system_clock::now();
    // Convert LOG_RETENTION to seconds for comparison
    auto retention_period_seconds = std::chrono::duration_cast<std::chrono::seconds>(LOG_RETENTION).count();

    // Get all log files
    std::vector<std::filesystem::path> files;
    GetSortedLogFiles(files);

    for (const auto &file : files) {
      // Extract the timestamp from the filename
      auto filename = file.filename().string();
      auto pos = filename.find_last_of('.');
      auto timestamp_str = filename.substr(0, pos);
      auto timestamp = std::stoll(timestamp_str);

      // Convert now to seconds since epoch
      auto now_in_seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

      // If the file is older than LOG_RETENTION, delete it
      if (now_in_seconds - timestamp > retention_period_seconds) {
        server::CommonUtil::RemoveFile(file.string());
      } else {
        // Since the files are sorted, we can break as soon as we encounter a file that's not old enough to be deleted
        break;
      }
    }
  }

  void SetEnabled(bool enabled) {
    enabled_ = enabled;
  }

 private:
  void ApplyEntry(meta::TableSchema &table_schema, std::shared_ptr<TableSegmentMVP> segment, int64_t global_id, LogEntryType &type, std::string &content) {
    vectordb::Json record;
    record.LoadFromString(content);
    switch (type) {
      case LogEntryType::INSERT: {
        auto status = segment->Insert(table_schema, record, global_id);
        if (!status.ok()) {
          std::cout << "Fail to apply wal entry: " << status.message() << std::endl;
        }
        break;
      }
      case LogEntryType::DELETE: {
        auto status = segment->DeleteByPK(record, global_id);
        if (!status.ok()) {
          std::cout << "Fail to apply wal entry: " << status.message() << std::endl;
        }
        break;
      }
      default: {
        break;
      }
    }
  }

  // void GetSortedLogFiles(std::vector<std::filesystem::path>& files) {
  //   std::cout << "Get shorted log files by thread: " << omp_get_thread_num() << std::endl;

  //   std::filesystem::directory_iterator end_itr;  // Default ctor yields past-the-end
  //   for (std::filesystem::directory_iterator i(logs_folder_); i != end_itr; ++i) {
  //     if (i->path().extension() == ".log") {
  //       files.push_back(i->path());
  //     }
  //   }
  //   std::sort(files.begin(), files.end());
  // }

  void GetSortedLogFiles(std::vector<std::filesystem::path> &files) {
    // Check if logs_folder_ exists and is a directory.
    if (!std::filesystem::exists(logs_folder_) || !std::filesystem::is_directory(logs_folder_)) {
      std::cout << "Directory " << logs_folder_ << " does not exist or is not a directory.\n";
      return;
    }

    try {
      std::filesystem::directory_iterator end_itr;  // Default ctor yields past-the-end
      for (std::filesystem::directory_iterator i(logs_folder_); i != end_itr; ++i) {
        // Print out the path of each file being processed.
        std::cout << "Processing file: " << i->path().string() << std::endl;

        if (i->path().extension() == ".log") {
          files.push_back(i->path());
        }
      }
    } catch (const std::filesystem::filesystem_error &ex) {
      std::cout << "Caught exception: " << ex.what() << '\n';
    }

    std::sort(files.begin(), files.end());
  }

  void RotateFile() {
    if (file_ != nullptr) {
      fclose(file_);
    }

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    std::string filename = logs_folder_ + std::to_string(time) + ".log";
    file_ = fopen(filename.c_str(), "a");

    last_rotation_time_ = now;
  }

  std::string logs_folder_;
  std::chrono::time_point<std::chrono::system_clock> last_rotation_time_;
  FILE *file_ = nullptr;
  AtomicCounter global_counter_;
  bool enabled_ = true;
};
}  // namespace engine
}  // namespace vectordb
