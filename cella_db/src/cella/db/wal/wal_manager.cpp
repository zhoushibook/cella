// wal_manager.cpp —— WAL 编码、落盘与压缩的实现。
//
// 编码一律小端：本机就是 x86/ARM 小端，但显式按字节拼装可以保证换平台后
// 日志文件仍然可读（教学系统里「换个机器还能打开」比省几个周期重要）。
#include "cella/db/wal/wal_manager.h"

#include <cstring>
#include <filesystem>
#include <iterator>
#include <sstream>

namespace cella::db::wal {
namespace {

// ── 小端写入 ────────────────────────────────────────────────
void PutU8(std::string* s, uint8_t v) { s->push_back(static_cast<char>(v)); }

void PutU32(std::string* s, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    s->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  }
}

void PutU64(std::string* s, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    s->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  }
}

void PutF32(std::string* s, float v) {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  PutU32(s, bits);
}

void PutF64(std::string* s, double v) {
  uint64_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  PutU64(s, bits);
}

void PutStr(std::string* s, const std::string& v) {
  PutU32(s, static_cast<uint32_t>(v.size()));
  s->append(v);
}

// ── 小端读取（带越界检查）──────────────────────────────────
class Reader {
 public:
  Reader(const char* data, size_t size) : data_(data), size_(size) {}

  bool Take(void* dst, size_t n) {
    if (pos_ + n > size_) {
      return false;
    }
    if (n != 0) {
      std::memcpy(dst, data_ + pos_, n);
    }
    pos_ += n;
    return true;
  }
  bool TakeU8(uint8_t* v) { return Take(v, 1); }
  bool TakeU32(uint32_t* v) {
    uint8_t b[4];
    if (!Take(b, 4)) return false;
    *v = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
         (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    return true;
  }
  bool TakeU64(uint64_t* v) {
    uint32_t lo = 0, hi = 0;
    if (!TakeU32(&lo) || !TakeU32(&hi)) return false;
    *v = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    return true;
  }
  bool TakeF32(float* v) {
    uint32_t bits = 0;
    if (!TakeU32(&bits)) return false;
    std::memcpy(v, &bits, sizeof(bits));
    return true;
  }
  bool TakeF64(double* v) {
    uint64_t bits = 0;
    if (!TakeU64(&bits)) return false;
    std::memcpy(v, &bits, sizeof(bits));
    return true;
  }
  bool TakeStr(std::string* v) {
    uint32_t n = 0;
    if (!TakeU32(&n)) return false;
    if (pos_ + n > size_) return false;
    v->assign(data_ + pos_, n);
    pos_ += n;
    return true;
  }

 private:
  const char* data_;
  size_t size_;
  size_t pos_ = 0;
};

const char* kFileMagic = "CELLAWAL1";

void PutValues(std::string* out, const std::vector<storage::Value>& vs) {
  PutU32(out, static_cast<uint32_t>(vs.size()));
  for (const auto& v : vs) {
    PutValue(out, v);
  }
}

// 读取 count 个值。列数上界用于防御坏数据（否则一条坏记录会让解码一直吃到尾）。
bool TakeValues(Reader* rd, std::vector<storage::Value>* out) {
  uint32_t n = 0;
  if (!rd->TakeU32(&n)) return false;
  if (n > 4096) return false;
  out->clear();
  out->reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    storage::Value v;
    uint8_t type_byte = 0;
    if (!rd->TakeU8(&type_byte)) return false;
    v.type = static_cast<storage::ValueType>(type_byte);
    bool ok = true;
    switch (v.type) {
      case storage::ValueType::kNull:
        break;
      case storage::ValueType::kBool: {
        uint8_t b = 0;
        ok = rd->TakeU8(&b);
        v.bool_val = (b != 0);
        break;
      }
      case storage::ValueType::kInt32: {
        uint32_t x = 0;
        ok = rd->TakeU32(&x);
        v.int32_val = static_cast<int32_t>(x);
        break;
      }
      case storage::ValueType::kInt64: {
        uint64_t x = 0;
        ok = rd->TakeU64(&x);
        v.int64_val = static_cast<int64_t>(x);
        break;
      }
      case storage::ValueType::kFloat:
        ok = rd->TakeF32(&v.float_val);
        break;
      case storage::ValueType::kDouble:
        ok = rd->TakeF64(&v.double_val);
        break;
      case storage::ValueType::kVarchar:
      case storage::ValueType::kChar:
        ok = rd->TakeStr(&v.str_val);
        break;
      case storage::ValueType::kDate: {
        uint64_t x = 0;
        ok = rd->TakeU64(&x);
        v.int64_val = static_cast<int64_t>(x);
        break;
      }
      default:
        ok = false;  // 未知类型 → 拒绝（宁可截断日志，也不猜）
        break;
    }
    if (!ok) return false;
    out->push_back(std::move(v));
  }
  return true;
}

}  // namespace

// ── 类型名 ──────────────────────────────────────────────────

const char* ToString(RecordType t) {
  switch (t) {
    case RecordType::kBegin: return "BEGIN";
    case RecordType::kCommit: return "COMMIT";
    case RecordType::kAbort: return "ABORT";
    case RecordType::kInsert: return "INSERT";
    case RecordType::kDelete: return "DELETE";
    case RecordType::kUpdate: return "UPDATE";
    case RecordType::kCheckpoint: return "CHECKPOINT";
  }
  return "?";
}

std::string Describe(const WalRecord& r) {
  std::ostringstream os;
  os << "#" << r.lsn << " " << ToString(r.type) << " txn=" << r.txn_id;
  if (r.is_data_record()) {
    os << " 表=" << r.table << " 页=" << r.page_id;
    if (!r.before.empty()) {
      os << " before=[";
      for (size_t i = 0; i < r.before.size(); ++i) {
        if (i != 0) os << ",";
        os << r.before[i].ToString();
      }
      os << "]";
    }
    if (!r.after.empty()) {
      os << " after=[";
      for (size_t i = 0; i < r.after.size(); ++i) {
        if (i != 0) os << ",";
        os << r.after[i].ToString();
      }
      os << "]";
    }
  } else if (r.type == RecordType::kCheckpoint) {
    os << " 活动事务=" << r.active_txns.size();
  }
  return os.str();
}

// ── CRC32 ───────────────────────────────────────────────────

uint32_t Crc32(const void* data, size_t size) {
  static uint32_t table[256] = {0};
  static bool built = false;
  if (!built) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1u) != 0u ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      table[i] = c;
    }
    built = true;
  }
  const auto* p = static_cast<const unsigned char*>(data);
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) {
    crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

// ── 值编码 ──────────────────────────────────────────────────

void PutValue(std::string* out, const storage::Value& v) {
  PutU8(out, static_cast<uint8_t>(v.type));
  switch (v.type) {
    case storage::ValueType::kNull:
      break;
    case storage::ValueType::kBool:
      PutU8(out, v.bool_val ? 1 : 0);
      break;
    case storage::ValueType::kInt32:
      PutU32(out, static_cast<uint32_t>(v.int32_val));
      break;
    case storage::ValueType::kInt64:
      PutU64(out, static_cast<uint64_t>(v.int64_val));
      break;
    case storage::ValueType::kFloat:
      PutF32(out, v.float_val);
      break;
    case storage::ValueType::kDouble:
      PutF64(out, v.double_val);
      break;
    case storage::ValueType::kVarchar:
    case storage::ValueType::kChar:
      PutStr(out, v.str_val);
      break;
    case storage::ValueType::kDate:
      PutU64(out, static_cast<uint64_t>(v.int64_val));
      break;
  }
}

bool ValueEqual(const storage::Value& x, const storage::Value& y) {
  if (x.type != y.type) return false;
  switch (x.type) {
    case storage::ValueType::kNull:
      return true;  // NULL 与 NULL 视为相等（与索引层一致）
    case storage::ValueType::kBool:
      return x.bool_val == y.bool_val;
    case storage::ValueType::kInt32:
      return x.int32_val == y.int32_val;
    case storage::ValueType::kInt64:
    case storage::ValueType::kDate:
      return x.int64_val == y.int64_val;
    case storage::ValueType::kFloat:
      return x.float_val == y.float_val;
    case storage::ValueType::kDouble:
      return x.double_val == y.double_val;
    case storage::ValueType::kVarchar:
    case storage::ValueType::kChar:
      return x.str_val == y.str_val;
    default:
      return false;
  }
}

bool ValuesEqual(const std::vector<storage::Value>& a,
                 const std::vector<storage::Value>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (!ValueEqual(a[i], b[i])) return false;
  }
  return true;
}

// ── payload 编解码 ──────────────────────────────────────────

std::string EncodePayload(const WalRecord& r) {
  std::string out;
  PutU8(&out, static_cast<uint8_t>(r.type));
  switch (r.type) {
    case RecordType::kBegin:
    case RecordType::kCommit:
    case RecordType::kAbort:
      PutU64(&out, r.txn_id);
      break;
    case RecordType::kInsert:
    case RecordType::kDelete:
    case RecordType::kUpdate:
      PutU64(&out, r.txn_id);
      PutStr(&out, r.table);
      PutU32(&out, r.page_id);
      if (r.type == RecordType::kInsert) {
        PutValues(&out, r.after);
      } else if (r.type == RecordType::kDelete) {
        PutValues(&out, r.before);
      } else {
        PutValues(&out, r.before);
        PutValues(&out, r.after);
      }
      break;
    case RecordType::kCheckpoint:
      PutU32(&out, static_cast<uint32_t>(r.active_txns.size()));
      for (const auto& kv : r.active_txns) {
        PutU64(&out, kv.first);
        PutU64(&out, kv.second);
      }
      break;
  }
  return out;
}

bool DecodePayload(RecordType type, const char* data, size_t size, WalRecord* out) {
  Reader rd(data, size);
  uint8_t t = 0;
  if (!rd.TakeU8(&t)) return false;
  if (static_cast<RecordType>(t) != type) return false;
  out->type = type;
  switch (type) {
    case RecordType::kBegin:
    case RecordType::kCommit:
    case RecordType::kAbort:
      return rd.TakeU64(&out->txn_id);
    case RecordType::kInsert:
    case RecordType::kDelete:
    case RecordType::kUpdate: {
      if (!rd.TakeU64(&out->txn_id)) return false;
      if (!rd.TakeStr(&out->table)) return false;
      if (!rd.TakeU32(&out->page_id)) return false;
      if (type == RecordType::kInsert) {
        return TakeValues(&rd, &out->after);
      }
      if (type == RecordType::kDelete) {
        return TakeValues(&rd, &out->before);
      }
      return TakeValues(&rd, &out->before) && TakeValues(&rd, &out->after);
    }
    case RecordType::kCheckpoint: {
      uint32_t n = 0;
      if (!rd.TakeU32(&n)) return false;
      if (n > 100000) return false;
      out->active_txns.clear();
      for (uint32_t i = 0; i < n; ++i) {
        txn_id_t id = 0;
        lsn_t l = 0;
        if (!rd.TakeU64(&id) || !rd.TakeU64(&l)) return false;
        out->active_txns.emplace_back(id, l);
      }
      return true;
    }
  }
  return false;
}

// ── WalManager ──────────────────────────────────────────────

WalManager::WalManager() = default;

WalManager::~WalManager() { Close(); }

std::string FileHeaderBytes() {
  std::string hdr(kFileMagic, 8);
  PutU32(&hdr, kFormatVersion);
  PutU32(&hdr, 0);  // 保留
  return hdr;
}

DbStatus WalManager::WriteFileHeader() {
  const std::string hdr = FileHeaderBytes();
  out_.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));
  if (!out_) {
    return DbStatus::Error(DbCode::kWalError, "WAL 文件头写入失败: " + path_);
  }
  return DbStatus::Ok();
}

// 扫描文件：把完整的记录读出来；valid_end 是「最后一条好记录的结束偏移」。
// valid_end 之后还有字节 = 上次崩溃留下的半截记录（会被后续 Append 覆盖前先截掉）。
DbStatus WalManager::Scan(std::vector<WalRecord>* out, size_t* valid_end,
                          size_t* truncated) const {
  if (out != nullptr) out->clear();
  if (valid_end != nullptr) *valid_end = 0;
  if (truncated != nullptr) *truncated = 0;
  if (!opened_) {
    return DbStatus::Error(DbCode::kWalError, "WAL 未打开，无法扫描");
  }
  std::ifstream in(path_, std::ios::binary);
  if (!in) {
    return DbStatus::Error(DbCode::kWalError, "WAL 读取失败: " + path_);
  }
  const std::string all((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
  size_t pos = 0;
  if (all.size() >= kFileHeaderSize && std::memcmp(all.data(), kFileMagic, 8) == 0) {
    pos = kFileHeaderSize;
  }
  while (pos + kRecordHeaderSize <= all.size()) {
    Reader rd(all.data() + pos, kRecordHeaderSize);
    uint32_t magic = 0, size = 0, crc = 0;
    lsn_t lsn = 0;
    if (!rd.TakeU32(&magic) || !rd.TakeU32(&size) || !rd.TakeU32(&crc)) break;
    if (!rd.TakeU64(&lsn)) break;
    if (magic != kRecordMagic) break;
    if (size < kRecordHeaderSize || pos + size > all.size()) break;
    const char* payload = all.data() + pos + kRecordHeaderSize;
    const size_t payload_len = static_cast<size_t>(size) - kRecordHeaderSize;
    if (Crc32(payload, payload_len) != crc) break;

    Reader prd(payload, payload_len);
    uint8_t t = 0;
    if (!prd.TakeU8(&t)) break;
    WalRecord rec;
    if (!DecodePayload(static_cast<RecordType>(t), payload, payload_len, &rec)) break;
    rec.lsn = lsn;
    if (out != nullptr) out->push_back(std::move(rec));
    pos += size;
  }
  if (valid_end != nullptr) *valid_end = pos;
  if (truncated != nullptr) *truncated = all.size() - pos;
  return DbStatus::Ok();
}

DbStatus WalManager::ReadAll(std::vector<WalRecord>* out) const {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  return Scan(out, nullptr, nullptr);
}

DbStatus WalManager::Open(const std::string& path, OpenResult* out) {
  Close();
  path_ = path;
  next_lsn_ = kFirstLsn;
  durable_lsn_ = kInvalidLsn;
  appended_count_ = 0;
  flush_count_ = 0;
  buffer_.clear();

  std::error_code ec;
  // ── 旧版纯文本 journal.log 的识别与归档（这就是「升级 journal.log」）──
  // 老格式每行是 "2026-09-14T... COMMIT txn=1"，前 8 字节不可能是 magic。
  if (std::filesystem::exists(path, ec)) {
    std::ifstream probe(path, std::ios::binary);
    char head[8] = {0};
    probe.read(head, 8);
    const std::streamsize got = probe.gcount();
    probe.close();
    const bool has_magic = got == 8 && std::memcmp(head, kFileMagic, 8) == 0;
    if (got > 0 && !has_magic) {
      std::error_code rec;
      std::filesystem::rename(path, path + ".legacy", rec);
      if (!rec && out != nullptr) {
        out->legacy_archived = true;
      }
    }
  }

  // 用「文件长度」而不是「是否存在」判断要不要写文件头：存在但长度为 0 的
  // 残留文件（比如上次建到一半就崩了）同样需要头。
  uint64_t existing_size = 0;
  if (std::filesystem::exists(path, ec)) {
    std::error_code sec;
    existing_size = std::filesystem::file_size(path, sec);
  }
  out_.open(path, std::ios::binary | std::ios::app);
  if (!out_) {
    opened_ = false;
    return DbStatus::Error(DbCode::kWalError, "WAL 打开失败: " + path);
  }
  opened_ = true;
  if (existing_size == 0) {
    const DbStatus hs = WriteFileHeader();
    if (!hs.ok()) return hs;
    if (out != nullptr) out->created = true;
  }

  std::vector<WalRecord> recs;
  size_t valid_end = 0;
  size_t truncated = 0;
  const DbStatus rs = Scan(&recs, &valid_end, &truncated);
  if (!rs.ok()) return rs;
  if (out != nullptr) {
    out->records = recs.size();
    out->truncated_bytes = truncated;
  }
  if (!recs.empty()) {
    next_lsn_ = recs.back().lsn + 1;
    durable_lsn_ = recs.back().lsn;
  }
  // 砍掉坏尾：否则新记录会接在半截记录后面，整段日志从此再也读不出来。
  if (truncated != 0) {
    std::error_code rec;
    std::filesystem::resize_file(path, static_cast<uint64_t>(valid_end), rec);
    if (!rec) {
      // 重开一次，让写指针回到截断后的文件尾
      out_.close();
      out_.open(path, std::ios::binary | std::ios::app);
      if (!out_) {
        opened_ = false;
        return DbStatus::Error(DbCode::kWalError, "WAL 截断后重开失败: " + path);
      }
    }
  }
  return DbStatus::Ok();
}

void WalManager::Close() {
  if (out_.is_open()) {
    out_.close();
  }
  opened_ = false;
  buffer_.clear();
}

lsn_t WalManager::Append(const WalRecord& rec) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (!opened_) {
    return kInvalidLsn;
  }
  WalRecord copy = rec;
  copy.lsn = next_lsn_++;
  const std::string payload = EncodePayload(copy);
  const uint32_t total = kRecordHeaderSize + static_cast<uint32_t>(payload.size());
  const uint32_t crc = Crc32(payload.data(), payload.size());

  std::string bytes;
  bytes.reserve(total);
  PutU32(&bytes, kRecordMagic);
  PutU32(&bytes, total);
  PutU32(&bytes, crc);
  PutU64(&bytes, copy.lsn);
  bytes.append(payload);

  buffer_.append(bytes);
  ++appended_count_;
  if (flush_each_record_) {
    Flush();
  }
  return copy.lsn;
}

void WalManager::Flush() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (buffer_.empty()) {
    return;
  }
  out_.write(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
  out_.flush();
  buffer_.clear();
  ++flush_count_;
  durable_lsn_ = next_lsn_ - 1;  // 缓冲已全落盘 → 最后一条已追加记录即水位
}

DbStatus WalManager::Compact(lsn_t keep_from, size_t* kept, size_t* dropped) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (!opened_) {
    return DbStatus::Error(DbCode::kWalError, "WAL 未打开，无法压缩");
  }
  Flush();
  std::vector<WalRecord> recs;
  const DbStatus rs = ReadAll(&recs);
  if (!rs.ok()) return rs;

  std::string body;
  size_t n_kept = 0;
  for (const auto& r : recs) {
    if (r.lsn < keep_from) continue;
    const std::string payload = EncodePayload(r);
    const uint32_t total = kRecordHeaderSize + static_cast<uint32_t>(payload.size());
    PutU32(&body, kRecordMagic);
    PutU32(&body, total);
    PutU32(&body, Crc32(payload.data(), payload.size()));
    PutU64(&body, r.lsn);
    body.append(payload);
    ++n_kept;
  }

  // 先写成独立临时文件并刷盘，再替换 —— 压到一半崩了也不会两头都没了
  const std::string tmp = path_ + ".compact";
  {
    std::ofstream tmpf(tmp, std::ios::binary | std::ios::trunc);
    if (!tmpf) {
      return DbStatus::Error(DbCode::kWalError, "WAL 压缩临时文件创建失败: " + tmp);
    }
    const std::string hdr = FileHeaderBytes();
    tmpf.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));
    if (!body.empty()) {
      tmpf.write(body.data(), static_cast<std::streamsize>(body.size()));
    }
    tmpf.flush();
    if (!tmpf) {
      return DbStatus::Error(DbCode::kWalError, "WAL 压缩临时文件写入失败: " + tmp);
    }
  }

  out_.close();
  std::error_code ec;
  std::filesystem::remove(path_, ec);
  std::error_code ec2;
  std::filesystem::rename(tmp, path_, ec2);
  if (ec2) {
    std::filesystem::remove(tmp, ec);
  }
  out_.open(path_, std::ios::binary | std::ios::app);
  if (!out_) {
    opened_ = false;
    return DbStatus::Error(DbCode::kWalError, "WAL 压缩后重开失败: " + path_);
  }
  // 注意：临时文件里**已经**写好了文件头，这里不能再补一次 ——
  // 补了就会出现两个头，扫描时第一条记录的位置对不上，整段日志读不出来。
  if (kept != nullptr) *kept = n_kept;
  if (dropped != nullptr) *dropped = recs.size() - n_kept;
  return DbStatus::Ok();
}

DbStatus WalManager::TruncateAll() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (!opened_) {
    return DbStatus::Ok();
  }
  buffer_.clear();
  out_.close();
  {
    std::ofstream f(path_, std::ios::binary | std::ios::trunc);
    if (!f) {
      return DbStatus::Error(DbCode::kWalError, "WAL 清空失败: " + path_);
    }
    const std::string hdr = FileHeaderBytes();
    f.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));
    f.flush();
  }
  out_.open(path_, std::ios::binary | std::ios::app);
  if (!out_) {
    opened_ = false;
    return DbStatus::Error(DbCode::kWalError, "WAL 清空后重开失败: " + path_);
  }
  next_lsn_ = kFirstLsn;
  durable_lsn_ = kInvalidLsn;
  return DbStatus::Ok();
}

std::string WalManager::Describe() const {
  std::ostringstream os;
  os << "WAL 文件=" << (path_.empty() ? "(未设置)" : path_)
     << " 已打开=" << (opened_ ? "是" : "否")
     << " 下个LSN=" << next_lsn_
     << " 已落盘LSN=" << durable_lsn_
     << " 本次追加=" << appended_count_
     << " 刷盘=" << flush_count_
     << " 未落盘字节=" << buffer_.size()
     << " 每记录刷盘=" << (flush_each_record_ ? "开" : "关");
  return os.str();
}

}  // namespace cella::db::wal
