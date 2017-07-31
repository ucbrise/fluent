#ifndef LINEAGEDB_MOCK_PQXX_CLIENT_H_
#define LINEAGEDB_MOCK_PQXX_CLIENT_H_

#include <cstdint>

#include <string>
#include <utility>
#include <vector>

#include "common/macros.h"
#include "common/status.h"
#include "common/status_or.h"
#include "lineagedb/mock_connection.h"
#include "lineagedb/mock_work.h"
#include "lineagedb/pqxx_client.h"

namespace fluent {
namespace lineagedb {

// A MockPqxxClient is like a PqxxClient, except that instead of issuing SQL
// queries, it stores them in a vector. For example:
//
//   // Create and initialize a MockPqxxClient.
//   using Client = MockPqxxClient<Hash, ToSql>;
//   std::string name = "seanconnery";
//   std::size_t id = 42;
//   std::string address = "inproc://zardoz";
//   ConnectionConfig config;
//   StatusOr<Client> mock_client_or = Client::Make(name, id, address, config);
//   Client mock_client = mock_client_or.ConsumeValueOrDie();
//
//   // All the queries generated by the client are stored.
//   for (const auto& query : mock_client.Queries()) {
//     // Print the name of the query.
//     std::cout << query.first << std::endl;
//
//     // Print the SQL of the query.
//     std::cout << query.second << std::endl;
//   }
//
// A MockPqxxClient is used to unit test a PqxxClient. See
// `mock_pqxx_client_test.cc`.
template <template <typename> class Hash, template <typename> class ToSql,
          typename Clock>
class MockPqxxClient : public InjectablePqxxClient<MockConnection, MockWork,
                                                   Hash, ToSql, Clock> {
 public:
  DISALLOW_COPY_AND_ASSIGN(MockPqxxClient);
  DISALLOW_MOVE_AND_ASSIGN(MockPqxxClient);
  virtual ~MockPqxxClient() = default;

  static WARN_UNUSED StatusOr<std::unique_ptr<MockPqxxClient>> Make(
      std::string name, std::size_t id, std::string address,
      const ConnectionConfig& connection_config) {
    std::unique_ptr<MockPqxxClient> mock_client(new MockPqxxClient(
        std::move(name), id, std::move(address), connection_config));
    RETURN_IF_ERROR(mock_client->Init());
    return std::move(mock_client);
  }

  WARN_UNUSED Status ExecuteQuery(const std::string& name,
                                  const std::string& query) override {
    queries_.push_back({name, query});
    return Status::OK;
  }

  const std::vector<std::pair<std::string, std::string>>& Queries() {
    return queries_;
  }

 private:
  MockPqxxClient(std::string name, std::size_t id, std::string address,
                 const ConnectionConfig& connection_config)
      : InjectablePqxxClient<MockConnection, MockWork, Hash, ToSql, Clock>(
            std::move(name), id, std::move(address), connection_config) {}

  std::vector<std::pair<std::string, std::string>> queries_;
};

}  // namespace lineagedb
}  // namespace fluent

#endif  // LINEAGEDB_MOCK_PQXX_CLIENT_H_
