#include <fstream>

#include "common/buffer/buffer_impl.h"
#include "common/filesystem/filesystem_impl.h"
#include "common/grpc/codec.h"
#include "common/grpc/common.h"
#include "common/grpc/json_transcoder_filter.h"
#include "common/http/header_map_impl.h"

#include "test/mocks/http/mocks.h"
#include "test/mocks/upstream/mocks.h"
#include "test/proto/bookstore.pb.h"
#include "test/test_common/environment.h"
#include "test/test_common/printers.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "google/protobuf/util/message_differencer.h"
#include "gtest/gtest.h"

using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::ReturnPointee;
using testing::ReturnRef;
using testing::_;

using google::api::HttpRule;
using google::grpc::transcoding::Transcoder;
using google::protobuf::FileDescriptorSet;
using google::protobuf::MethodDescriptor;
using google::protobuf::util::MessageDifferencer;
using google::protobuf::util::Status;
using google::protobuf::util::error::Code;

namespace Envoy {
namespace Grpc {

class GrpcJsonTranscoderConfigTest : public testing::Test {
public:
  GrpcJsonTranscoderConfigTest() {}

  const Json::ObjectSharedPtr configJson(const std::string& descriptor_path,
                                         const std::string& service_name) {
    std::string json_string = "{\"proto_descriptor\": \"" + descriptor_path +
                              "\",\"services\": [\"" + service_name + "\"]}";
    return Json::Factory::loadFromString(json_string);
  }

  std::string makeProtoDescriptor(std::function<void(FileDescriptorSet&)> process) {
    FileDescriptorSet descriptor_set;
    descriptor_set.ParseFromString(Filesystem::fileReadToEnd(
        TestEnvironment::runfilesPath("test/proto/bookstore.descriptor")));

    process(descriptor_set);

    mkdir(TestEnvironment::temporaryPath("envoy_test").c_str(), S_IRWXU);
    std::string path = TestEnvironment::temporaryPath("envoy_test/proto.descriptor");
    std::ofstream file(path);
    descriptor_set.SerializeToOstream(&file);

    return path;
  }

  void setGetBookHttpRule(FileDescriptorSet& descriptor_set, const HttpRule& http_rule) {
    for (auto& file : *descriptor_set.mutable_file()) {
      for (auto& service : *file.mutable_service()) {
        for (auto& method : *service.mutable_method()) {
          if (method.name() == "GetBook") {
            method.mutable_options()->MutableExtension(google::api::http)->MergeFrom(http_rule);
            return;
          }
        }
      }
    }
  }
};

TEST_F(GrpcJsonTranscoderConfigTest, ParseConfig) {
  EXPECT_NO_THROW(JsonTranscoderConfig config(*configJson(
      TestEnvironment::runfilesPath("test/proto/bookstore.descriptor"), "bookstore.Bookstore")));
}

TEST_F(GrpcJsonTranscoderConfigTest, UnknownService) {
  EXPECT_THROW_WITH_MESSAGE(
      JsonTranscoderConfig config(
          *configJson(TestEnvironment::runfilesPath("test/proto/bookstore.descriptor"),
                      "grpc.service.UnknownService")),
      EnvoyException,
      "transcoding_filter: Could not find 'grpc.service.UnknownService' in the proto descriptor");
}

TEST_F(GrpcJsonTranscoderConfigTest, IncompleteProto) {
  EXPECT_THROW_WITH_MESSAGE(
      JsonTranscoderConfig config(
          *configJson(TestEnvironment::runfilesPath("test/proto/bookstore_bad.descriptor"),
                      "grpc.service.UnknownService")),
      EnvoyException, "transcoding_filter: Unable to build proto descriptor pool");
}

TEST_F(GrpcJsonTranscoderConfigTest, NonProto) {
  EXPECT_THROW_WITH_MESSAGE(JsonTranscoderConfig config(*configJson(
                                TestEnvironment::runfilesPath("test/proto/bookstore.proto"),
                                "grpc.service.UnknownService")),
                            EnvoyException, "transcoding_filter: Unable to parse proto descriptor");
}

TEST_F(GrpcJsonTranscoderConfigTest, InvalidHttpTemplate) {
  HttpRule http_rule;
  http_rule.set_get("/book/{");
  EXPECT_THROW_WITH_MESSAGE(
      JsonTranscoderConfig config(*configJson(
          makeProtoDescriptor([&](FileDescriptorSet& pb) { setGetBookHttpRule(pb, http_rule); }),
          "bookstore.Bookstore")),
      EnvoyException,
      "transcoding_filter: Cannot register 'bookstore.Bookstore.GetBook' to path matcher");
}

TEST_F(GrpcJsonTranscoderConfigTest, CreateTranscoder) {
  JsonTranscoderConfig config(*configJson(
      TestEnvironment::runfilesPath("test/proto/bookstore.descriptor"), "bookstore.Bookstore"));

  Http::TestHeaderMapImpl headers{{":method", "GET"}, {":path", "/shelves"}};

  TranscoderInputStreamImpl request_in, response_in;
  std::unique_ptr<Transcoder> transcoder;
  const MethodDescriptor* method_descriptor;
  auto status =
      config.createTranscoder(headers, request_in, response_in, transcoder, method_descriptor);

  EXPECT_TRUE(status.ok());
  EXPECT_TRUE(transcoder);
  EXPECT_EQ("bookstore.Bookstore.ListShelves", method_descriptor->full_name());
}

TEST_F(GrpcJsonTranscoderConfigTest, InvalidVariableBinding) {
  HttpRule http_rule;
  http_rule.set_get("/book/{b}");
  JsonTranscoderConfig config(*configJson(
      makeProtoDescriptor([&](FileDescriptorSet& pb) { setGetBookHttpRule(pb, http_rule); }),
      "bookstore.Bookstore"));

  Http::TestHeaderMapImpl headers{{":method", "GET"}, {":path", "/book/1"}};

  TranscoderInputStreamImpl request_in, response_in;
  std::unique_ptr<Transcoder> transcoder;
  const MethodDescriptor* method_descriptor;
  auto status =
      config.createTranscoder(headers, request_in, response_in, transcoder, method_descriptor);

  EXPECT_EQ(google::protobuf::util::error::Code::INVALID_ARGUMENT, status.error_code());
  EXPECT_EQ("Could not find field \"b\" in the type \"bookstore.GetBookRequest\".",
            status.error_message());
  EXPECT_FALSE(transcoder);
}

class GrpcJsonTranscoderFilterTest : public testing::Test {
public:
  GrpcJsonTranscoderFilterTest() : config_(*bookstoreJson()), filter_(config_) {
    filter_.setDecoderFilterCallbacks(decoder_callbacks_);
    filter_.setEncoderFilterCallbacks(encoder_callbacks_);
  }

  const Json::ObjectSharedPtr bookstoreJson() {
    std::string json_string = "{\"proto_descriptor\": \"" + bookstoreDescriptorPath() +
                              "\",\"services\": [\"bookstore.Bookstore\"]}";
    return Json::Factory::loadFromString(json_string);
  }

  const std::string bookstoreDescriptorPath() {
    return TestEnvironment::runfilesPath("test/proto/bookstore.descriptor");
  }

  // TODO(lizan): Add a mock of JsonTranscoderConfig and test more error cases.
  JsonTranscoderConfig config_;
  JsonTranscoderFilter filter_;
  NiceMock<Http::MockStreamDecoderFilterCallbacks> decoder_callbacks_;
  NiceMock<Http::MockStreamEncoderFilterCallbacks> encoder_callbacks_;
};

TEST_F(GrpcJsonTranscoderFilterTest, NoTranscoding) {
  Http::TestHeaderMapImpl request_headers{{"content-type", "application/grpc"},
                                          {":method", "POST"},
                                          {":path", "/grpc.service/UnknownGrpcMethod"}};

  Http::TestHeaderMapImpl expected_request_headers{{"content-type", "application/grpc"},
                                                   {":method", "POST"},
                                                   {":path", "/grpc.service/UnknownGrpcMethod"}};

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.decodeHeaders(request_headers, false));
  EXPECT_EQ(expected_request_headers, request_headers);

  Buffer::OwnedImpl request_data{"{}"};
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_.decodeData(request_data, false));
  EXPECT_EQ(2, request_data.length());

  Http::TestHeaderMapImpl request_trailers;
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_.decodeTrailers(request_trailers));

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.encodeHeaders(request_headers, false));
  EXPECT_EQ(expected_request_headers, request_headers);

  Buffer::OwnedImpl response_data{"{}"};
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_.encodeData(response_data, false));
  EXPECT_EQ(2, response_data.length());

  Http::TestHeaderMapImpl response_trailers{{"grpc-status", "0"}};
  Http::TestHeaderMapImpl expected_response_trailers{{"grpc-status", "0"}};
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_.encodeTrailers(response_trailers));
  EXPECT_EQ(expected_response_trailers, response_trailers);
}

TEST_F(GrpcJsonTranscoderFilterTest, TranscodingUnaryPost) {
  Http::TestHeaderMapImpl request_headers{
      {"content-type", "application/json"}, {":method", "POST"}, {":path", "/shelf"}};

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.decodeHeaders(request_headers, false));
  EXPECT_EQ("application/grpc", request_headers.get_("content-type"));
  EXPECT_EQ("/bookstore.Bookstore/CreateShelf", request_headers.get_(":path"));
  EXPECT_EQ("trailers", request_headers.get_("te"));

  Buffer::OwnedImpl request_data{"{\"theme\": \"Children\"}"};

  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_.decodeData(request_data, true));

  Decoder decoder;
  std::vector<Frame> frames;
  decoder.decode(request_data, frames);

  EXPECT_EQ(1, frames.size());

  bookstore::CreateShelfRequest expected_request;
  expected_request.mutable_shelf()->set_theme("Children");

  bookstore::CreateShelfRequest request;
  request.ParseFromString(TestUtility::bufferToString(*frames[0].data_));

  EXPECT_EQ(expected_request.ByteSize(), frames[0].length_);
  EXPECT_TRUE(MessageDifferencer::Equals(expected_request, request));

  Http::TestHeaderMapImpl response_headers{{"content-type", "application/grpc"},
                                           {":status", "200"}};

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_.encodeHeaders(response_headers, false));
  EXPECT_EQ("application/json", response_headers.get_("content-type"));

  bookstore::Shelf response;
  response.set_id(20);
  response.set_theme("Children");

  auto response_data = Common::serializeBody(response);

  EXPECT_EQ(Http::FilterDataStatus::StopIterationAndBuffer,
            filter_.encodeData(*response_data, false));

  std::string response_json = TestUtility::bufferToString(*response_data);

  EXPECT_EQ("{\"id\":\"20\",\"theme\":\"Children\"}", response_json);

  Http::TestHeaderMapImpl response_trailers{{"grpc-status", "0"}, {"grpc-message", ""}};

  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_.decodeTrailers(response_trailers));
}

TEST_F(GrpcJsonTranscoderFilterTest, TranscodingUnaryError) {
  Http::TestHeaderMapImpl request_headers{
      {"content-type", "application/json"}, {":method", "POST"}, {":path", "/shelf"}};

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.decodeHeaders(request_headers, false));
  EXPECT_EQ("application/grpc", request_headers.get_("content-type"));
  EXPECT_EQ("/bookstore.Bookstore/CreateShelf", request_headers.get_(":path"));
  EXPECT_EQ("trailers", request_headers.get_("te"));

  Buffer::OwnedImpl request_data{"{\"theme\": \"Children\""};

  EXPECT_CALL(decoder_callbacks_, encodeHeaders_(_, false))
      .WillOnce(Invoke([](Http::HeaderMap& headers, bool end_stream) {
        EXPECT_STREQ("400", headers.Status()->value().c_str());
        EXPECT_FALSE(end_stream);
      }));
  EXPECT_CALL(decoder_callbacks_, encodeData(_, true));

  EXPECT_EQ(Http::FilterDataStatus::StopIterationNoBuffer, filter_.decodeData(request_data, true));
  EXPECT_EQ(0, request_data.length());
}

} // namespace Grpc
} // namespace Envoy