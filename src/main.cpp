#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include "poppler_service_impl.h"

namespace {

// Fleet message-size convention (matches gRParse).
constexpr int kMaxMessageBytes = 520 * 1024 * 1024;

}  // namespace

int main() {
  const char* port_env = std::getenv("GRPC_POPPLER_PORT");
  const std::string address =
      std::string("0.0.0.0:") + (port_env != nullptr ? port_env : "50053");

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();

  grpc_poppler::PopplerServiceImpl service;
  grpc::ServerBuilder builder;
  builder.SetMaxReceiveMessageSize(kMaxMessageBytes);
  builder.SetMaxSendMessageSize(kMaxMessageBytes);
  int selected_port = 0;
  builder.AddListeningPort(address, grpc::InsecureServerCredentials(),
                           &selected_port);
  builder.RegisterService(&service);

  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (!server || selected_port == 0) {
    std::cerr << "failed to bind " << address << std::endl;
    return 1;
  }
  std::cout << "grpc-poppler listening on 0.0.0.0:" << selected_port
            << std::endl;
  server->Wait();
  return 0;
}
