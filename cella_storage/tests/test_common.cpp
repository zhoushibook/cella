#include "mini_test.h"

#include <string>

#include "byte_buffer.h"
#include "config.h"
#include "status.h"
#include "types.h"
#include "version.h"

using namespace cella::storage;

TEST_CASE(byte_buffer_little_endian) {
  char buf[8] = {};

  PutUint16(buf, 0x1234u);
  EXPECT_EQ(static_cast<int>(static_cast<unsigned char>(buf[0])), 0x34);
  EXPECT_EQ(static_cast<int>(static_cast<unsigned char>(buf[1])), 0x12);
  EXPECT_EQ(GetUint16(buf), 0x1234u);

  PutUint32(buf, 0x12345678u);
  EXPECT_EQ(static_cast<int>(static_cast<unsigned char>(buf[0])), 0x78);
  EXPECT_EQ(static_cast<int>(static_cast<unsigned char>(buf[3])), 0x12);
  EXPECT_EQ(GetUint32(buf), 0x12345678u);

  PutUint64(buf, 0x0102030405060708ull);
  EXPECT_EQ(static_cast<int>(static_cast<unsigned char>(buf[0])), 0x08);
  EXPECT_EQ(GetUint64(buf), 0x0102030405060708ull);
}

TEST_CASE(byte_buffer_streaming) {
  ByteBuffer bb;
  bb.PutUint8(0xAA);
  bb.PutUint16(0xBEEF);
  bb.PutUint32(0xDEADBEEF);
  bb.PutString("hello");

  EXPECT_EQ(bb.size(), size_t(1 + 2 + 4 + 2 + 5));
  EXPECT_EQ(static_cast<int>(bb.GetUint8()), 0xAA);
  EXPECT_EQ(bb.GetUint16(), 0xBEEFu);
  EXPECT_EQ(bb.GetUint32(), 0xDEADBEEFu);
  EXPECT_EQ(bb.GetString(), std::string("hello"));
}

TEST_CASE(status_ok_and_error) {
  Status ok;
  EXPECT_TRUE(ok.ok());
  EXPECT_TRUE(ok.code() == StatusCode::kOk);
  EXPECT_TRUE(ok.message().empty());

  Status err = Status::Error(StatusCode::kIoError, "磁盘读失败");
  EXPECT_TRUE(!err.ok());
  EXPECT_TRUE(err.code() == StatusCode::kIoError);
  EXPECT_TRUE(err.message() == "磁盘读失败");
}

TEST_CASE(config_validate_page_size) {
  StorageConfig c;
  EXPECT_TRUE(c.Validate().ok());

  c.page_size = 512;
  EXPECT_TRUE(c.Validate().ok());
  c.page_size = 8192;
  EXPECT_TRUE(c.Validate().ok());

  c.page_size = 768;   // 非 2 的幂
  EXPECT_TRUE(c.Validate().code() == StatusCode::kInvalidConfig);
  c.page_size = 256;   // < 512
  EXPECT_TRUE(c.Validate().code() == StatusCode::kInvalidConfig);
}

TEST_CASE(version_constants) {
  EXPECT_TRUE(std::string(kStorageVersion) == "cella-storage/0.1");
  EXPECT_EQ(kFormatVersion, 1u);
}
