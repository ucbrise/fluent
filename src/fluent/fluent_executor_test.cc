#include "fluent/fluent_executor.h"

#include <cstddef>
#include <cstdint>

#include <set>
#include <tuple>
#include <utility>

#include "glog/logging.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "zmq.hpp"

#include "common/status.h"
#include "common/status_or.h"
#include "common/string_util.h"
#include "fluent/channel.h"
#include "fluent/fluent_builder.h"
#include "fluent/infix.h"
#include "fluent/mock_pickler.h"
#include "lineagedb/connection_config.h"
#include "lineagedb/mock_client.h"
#include "lineagedb/mock_to_sql.h"
#include "lineagedb/noop_client.h"
#include "ra/all.h"
#include "testing/captured_stdout.h"
#include "testing/mock_clock.h"

namespace ldb = fluent::lineagedb;

using ::testing::UnorderedElementsAreArray;

namespace fluent {
namespace {

auto noopfluent(const std::string& name, const std::string& address,
                zmq::context_t* context,
                const ldb::ConnectionConfig& connection_config) {
  return fluent<ldb::NoopClient, Hash, ldb::ToSql, MockPickler, MockClock>(
      name, address, context, connection_config);
}

}  // namespace

TEST(FluentExecutor, SimpleProgram) {
  zmq::context_t context(1);
  lineagedb::ConnectionConfig connection_config;
  auto fb_or = noopfluent("name", "inproc://yolo", &context, connection_config);
  ASSERT_EQ(Status::OK, fb_or.status());
  auto fe_or = fb_or.ConsumeValueOrDie()
                   .table<std::size_t>("t", {{"x"}})
                   .scratch<int, int, float>("s", {{"x", "y", "z"}})
                   .channel<std::string, float, char>("c", {{"addr", "x", "y"}})
                   .RegisterRules([](auto& t, auto& s, auto& c) {
                     using namespace fluent::infix;
                     return std::make_tuple(t <= (t.Iterable() | ra::count()),
                                            t <= (s.Iterable() | ra::count()),
                                            t <= (c.Iterable() | ra::count()));
                   });
  ASSERT_EQ(Status::OK, fe_or.status());
  auto f = fe_or.ConsumeValueOrDie();

  using T = std::set<std::tuple<int>>;
  using S = std::set<std::tuple<int, int, float>>;
  using C = std::set<std::tuple<std::string, float, char>>;

  ASSERT_EQ(Status::OK, f.Tick());
  EXPECT_THAT(f.Get<0>().Get(), UnorderedElementsAreArray(T{{0}}));
  EXPECT_THAT(f.Get<1>().Get(), UnorderedElementsAreArray(S{}));
  EXPECT_THAT(f.Get<2>().Get(), UnorderedElementsAreArray(C{}));

  ASSERT_EQ(Status::OK, f.Tick());
  EXPECT_THAT(f.Get<0>().Get(), UnorderedElementsAreArray(T{{0}, {1}}));
  EXPECT_THAT(f.Get<1>().Get(), UnorderedElementsAreArray(S{}));
  EXPECT_THAT(f.Get<2>().Get(), UnorderedElementsAreArray(C{}));

  ASSERT_EQ(Status::OK, f.Tick());
  EXPECT_THAT(f.Get<0>().Get(), UnorderedElementsAreArray(T{{0}, {1}, {2}}));
  EXPECT_THAT(f.Get<1>().Get(), UnorderedElementsAreArray(S{}));
  EXPECT_THAT(f.Get<2>().Get(), UnorderedElementsAreArray(C{}));
}

TEST(FluentExecutor, SimpleBootstrap) {
  using Tuples = std::set<std::tuple<int>>;
  Tuples xs = {{1}, {2}, {3}, {4}, {5}};

  zmq::context_t context(1);
  lineagedb::ConnectionConfig connection_config;
  auto fb_or = noopfluent("name", "inproc://yolo", &context, connection_config);
  ASSERT_EQ(Status::OK, fb_or.status());
  auto fe_or =
      fb_or.ConsumeValueOrDie()
          .table<int>("t", {{"x"}})
          .scratch<int>("s", {{"x"}})
          .RegisterBootstrapRules([&xs](auto& t, auto& s) {
            using namespace fluent::infix;
            return std::make_tuple(t <= ra::make_iterable("xs", &xs),
                                   s <= ra::make_iterable("xs", &xs));
          })
          .RegisterRules([&xs](auto&, auto&) { return std::make_tuple(); });
  ASSERT_EQ(Status::OK, fe_or.status());
  auto f = fe_or.ConsumeValueOrDie();

  EXPECT_THAT(f.Get<0>().Get(), UnorderedElementsAreArray(Tuples{}));
  EXPECT_THAT(f.Get<1>().Get(), UnorderedElementsAreArray(Tuples{}));
  ASSERT_EQ(Status::OK, f.BootstrapTick());
  EXPECT_THAT(f.Get<0>().Get(), UnorderedElementsAreArray(xs));
  EXPECT_THAT(f.Get<1>().Get(), UnorderedElementsAreArray(Tuples{}));
}

TEST(FluentExecutor, MildlyComplexProgram) {
  auto int_tuple_to_string = [](const std::tuple<int>& t) {
    return std::tuple<std::string>(std::to_string(std::get<0>(t)));
  };

  zmq::context_t context(1);
  lineagedb::ConnectionConfig connection_config;
  auto fb_or = noopfluent("name", "inproc://yolo", &context, connection_config);
  ASSERT_EQ(Status::OK, fb_or.status());
  auto fe_or =
      fb_or.ConsumeValueOrDie()
          .table<std::size_t>("t", {{"x"}})
          .scratch<std::size_t>("s", {{"x"}})
          .stdout()
          .RegisterRules([&](auto& t, auto& s, auto& stdout) {
            using namespace fluent::infix;
            auto a = t <= (t.Iterable() | ra::count());
            auto b = t += t.Iterable();
            auto c = t -= s.Iterable();
            auto d = s <= (t.Iterable() | ra::count());
            auto e = stdout <= (s.Iterable() | ra::map(int_tuple_to_string));
            auto f = stdout += (s.Iterable() | ra::map(int_tuple_to_string));
            return std::make_tuple(a, b, c, d, e, f);
          });
  ASSERT_EQ(Status::OK, fb_or.status());
  auto f = fe_or.ConsumeValueOrDie();

  using T = std::set<std::tuple<int>>;
  CapturedStdout captured;

  EXPECT_THAT(f.Get<0>().Get(), UnorderedElementsAreArray(T{}));
  EXPECT_THAT(f.Get<1>().Get(), UnorderedElementsAreArray(T{}));
  EXPECT_STREQ("", captured.Get().c_str());

  ASSERT_EQ(Status::OK, f.Tick());
  EXPECT_THAT(f.Get<0>().Get(), UnorderedElementsAreArray(T{{0}}));
  EXPECT_THAT(f.Get<1>().Get(), UnorderedElementsAreArray(T{}));
  EXPECT_STREQ("1\n1\n", captured.Get().c_str());

  ASSERT_EQ(Status::OK, f.Tick());
  EXPECT_THAT(f.Get<0>().Get(), UnorderedElementsAreArray(T{{0}, {1}}));
  EXPECT_THAT(f.Get<1>().Get(), UnorderedElementsAreArray(T{}));
  EXPECT_STREQ("1\n1\n2\n2\n", captured.Get().c_str());
}

TEST(FluentExecutor, ComplexProgram) {
  using Tuple = std::tuple<int>;
  using T = std::set<Tuple>;
  using S = std::set<Tuple>;

  auto plus_one_times_two = [](const std::tuple<std::size_t>& t) {
    return std::tuple<std::size_t>((1 + std::get<0>(t)) * 2);
  };
  auto is_even = [](const auto& t) { return std::get<0>(t) % 2 == 0; };

  zmq::context_t context(1);
  lineagedb::ConnectionConfig connection_config;
  auto fb_or = noopfluent("name", "inproc://yolo", &context, connection_config);
  ASSERT_EQ(Status::OK, fb_or.status());
  auto fe_or =
      fb_or.ConsumeValueOrDie()
          .table<std::size_t>("t", {{"x"}})
          .scratch<std::size_t>("s", {{"x"}})
          .RegisterRules([plus_one_times_two, is_even](auto& t, auto& s) {
            using namespace fluent::infix;
            auto a = t += (s.Iterable() | ra::count());
            auto b = t <= (t.Iterable() | ra::map(plus_one_times_two));
            auto c = s <= t.Iterable();
            auto d = t -= (s.Iterable() | ra::filter(is_even));
            return std::make_tuple(a, b, c, d);
          });
  ASSERT_EQ(Status::OK, fe_or.status());
  auto f = fe_or.ConsumeValueOrDie();

  EXPECT_THAT(f.Get<0>().Get(), UnorderedElementsAreArray(T{}));
  EXPECT_THAT(f.Get<1>().Get(), UnorderedElementsAreArray(S{}));

  ASSERT_EQ(Status::OK, f.Tick());
  EXPECT_THAT(f.Get<0>().Get(), UnorderedElementsAreArray(T{{0}}));
  EXPECT_THAT(f.Get<1>().Get(), UnorderedElementsAreArray(S{}));

  ASSERT_EQ(Status::OK, f.Tick());
  EXPECT_THAT(f.Get<0>().Get(), UnorderedElementsAreArray(T{}));
  EXPECT_THAT(f.Get<1>().Get(), UnorderedElementsAreArray(S{}));

  ASSERT_EQ(Status::OK, f.Tick());
  EXPECT_THAT(f.Get<0>().Get(), UnorderedElementsAreArray(T{{0}}));
  EXPECT_THAT(f.Get<1>().Get(), UnorderedElementsAreArray(S{}));
}

TEST(FluentExecutor, SimpleCommunication) {
  auto reroute = [](const std::string& s) {
    return [s](const std::tuple<std::string, int>& t) {
      return std::make_tuple(s, std::get<1>(t));
    };
  };

  zmq::context_t context(1);
  lineagedb::ConnectionConfig connection_config;

  auto ping_fb_or =
      noopfluent("name", "inproc://ping", &context, connection_config);
  ASSERT_EQ(Status::OK, ping_fb_or.status());
  auto ping_fe_or =
      ping_fb_or.ConsumeValueOrDie()
          .channel<std::string, int>("c", {{"addr", "x"}})
          .RegisterRules([&reroute](auto& c) {
            using namespace fluent::infix;
            return std::make_tuple(
                c <= (c.Iterable() | ra::map(reroute("inproc://pong"))));
          });
  ASSERT_EQ(Status::OK, ping_fe_or.status());
  auto ping = ping_fe_or.ConsumeValueOrDie();

  auto pong_fb_or =
      noopfluent("name", "inproc://pong", &context, connection_config);
  ASSERT_EQ(Status::OK, pong_fb_or.status());
  auto pong_fe_or =
      pong_fb_or.ConsumeValueOrDie()
          .channel<std::string, int>("c", {{"addr", "x"}})
          .RegisterRules([&reroute](auto& c) {
            using namespace fluent::infix;
            return std::make_tuple(
                c <= (c.Iterable() | ra::map(reroute("inproc://ping"))));
          });
  ASSERT_EQ(Status::OK, pong_fe_or.status());
  auto pong = pong_fe_or.ConsumeValueOrDie();

  using C = std::set<std::tuple<std::string, int>>;
  C catalyst = {{"inproc://pong", 42}};
  ping.MutableGet<0>().Merge(catalyst, 9001);

  for (int i = 0; i < 3; ++i) {
    ASSERT_EQ(Status::OK, pong.Receive());
    EXPECT_THAT(pong.Get<0>().Get(),
                UnorderedElementsAreArray(C{{"inproc://pong", 42}}));
    ASSERT_EQ(Status::OK, pong.Tick());
    EXPECT_THAT(pong.Get<0>().Get(), UnorderedElementsAreArray(C{}));

    ASSERT_EQ(Status::OK, ping.Receive());
    EXPECT_THAT(ping.Get<0>().Get(),
                UnorderedElementsAreArray(C{{"inproc://ping", 42}}));
    ASSERT_EQ(Status::OK, ping.Tick());
    EXPECT_THAT(ping.Get<0>().Get(), UnorderedElementsAreArray(C{}));
  }
}

TEST(FluentExecutor, SimpleProgramWithLineage) {
  zmq::context_t context(1);
  lineagedb::ConnectionConfig connection_config;
  auto fb_or =
      fluent<ldb::MockClient, Hash, ldb::MockToSql, MockPickler, MockClock>(
          "name", "inproc://yolo", &context, connection_config);
  ASSERT_EQ(Status::OK, fb_or.status());
  auto fe_or = fb_or.ConsumeValueOrDie()
                   .table<std::size_t>("t", {{"x"}})
                   .scratch<std::size_t>("s", {{"x"}})
                   .channel<std::string, float, char>("c", {{"addr", "x", "y"}})
                   .RegisterRules([](auto& t, auto& s, auto& c) {
                     using namespace fluent::infix;
                     return std::make_tuple(t <= (t.Iterable() | ra::count()),
                                            t <= (s.Iterable() | ra::count()),
                                            s <= (c.Iterable() | ra::count()));
                   });
  ASSERT_EQ(Status::OK, fe_or.status());
  auto f = fe_or.ConsumeValueOrDie();
  const ldb::MockClient<Hash, ldb::MockToSql, MockClock>& client =
      f.GetLineageDbClient();
  Hash<std::tuple<std::size_t>> hash;

  using T = std::set<std::tuple<int>>;
  using S = std::set<std::tuple<std::size_t>>;
  using C = std::set<std::tuple<std::string, float, char>>;

  using MockClient = ldb::MockClient<Hash, ldb::MockToSql, MockClock>;
  using AddCollectionTuple = MockClient::AddCollectionTuple;
  using AddRuleTuple = MockClient::AddRuleTuple;
  using InsertTupleTuple = MockClient::InsertTupleTuple;
  using DeleteTupleTuple = MockClient::DeleteTupleTuple;
  using AddDerivedLineageTuple = MockClient::AddDerivedLineageTuple;

  using time_point = std::chrono::time_point<MockClock>;
  using std::chrono::seconds;

  ASSERT_EQ(client.GetAddRule().size(), static_cast<std::size_t>(3));
  EXPECT_EQ(client.GetAddCollection()[0],
            AddCollectionTuple("t", "Table", {"x"}, {"unsigned long"}));
  EXPECT_EQ(client.GetAddCollection()[1],
            AddCollectionTuple("s", "Scratch", {"x"}, {"unsigned long"}));
  EXPECT_EQ(client.GetAddCollection()[2],
            AddCollectionTuple("c", "Channel", {"addr", "x", "y"},
                               {"string", "float", "char"}));
  ASSERT_EQ(client.GetAddCollection().size(), static_cast<std::size_t>(3));
  EXPECT_EQ(client.GetAddRule()[0], AddRuleTuple(0, false, "t <= Count(t)"));
  EXPECT_EQ(client.GetAddRule()[1], AddRuleTuple(1, false, "t <= Count(s)"));
  EXPECT_EQ(client.GetAddRule()[2], AddRuleTuple(2, false, "s <= Count(c)"));

  ASSERT_EQ(Status::OK, f.Tick());
  EXPECT_THAT(f.Get<0>().Get(), UnorderedElementsAreArray(T{{0}}));
  EXPECT_THAT(f.Get<1>().Get(), UnorderedElementsAreArray(S{}));
  EXPECT_THAT(f.Get<2>().Get(), UnorderedElementsAreArray(C{}));

  ASSERT_EQ(client.GetAddRule().size(), static_cast<std::size_t>(3));
  ASSERT_EQ(client.GetAddCollection().size(), static_cast<std::size_t>(3));
  ASSERT_EQ(client.GetInsertTuple().size(), static_cast<std::size_t>(3));
  EXPECT_EQ(client.GetInsertTuple()[0],
            InsertTupleTuple("t", 1, time_point(), {"0"}));
  EXPECT_EQ(client.GetInsertTuple()[1],
            InsertTupleTuple("t", 2, time_point(), {"0"}));
  EXPECT_EQ(client.GetInsertTuple()[2],
            InsertTupleTuple("s", 3, time_point(), {"0"}));
  ASSERT_EQ(client.GetDeleteTuple().size(), static_cast<std::size_t>(1));
  EXPECT_EQ(client.GetDeleteTuple()[0],
            DeleteTupleTuple("s", 4, time_point(), {"0"}));
  ASSERT_EQ(client.GetAddNetworkedLineage().size(),
            static_cast<std::size_t>(0));
  ASSERT_EQ(client.GetAddDerivedLineage().size(), static_cast<std::size_t>(0));

  MockClock::Advance(seconds(1));
  ASSERT_EQ(Status::OK, f.Tick());
  EXPECT_THAT(f.Get<0>().Get(), UnorderedElementsAreArray(T{{0}, {1}}));
  EXPECT_THAT(f.Get<1>().Get(), UnorderedElementsAreArray(S{}));
  EXPECT_THAT(f.Get<2>().Get(), UnorderedElementsAreArray(C{}));

  ASSERT_EQ(client.GetAddRule().size(), static_cast<std::size_t>(3));
  ASSERT_EQ(client.GetAddCollection().size(), static_cast<std::size_t>(3));
  ASSERT_EQ(client.GetInsertTuple().size(), static_cast<std::size_t>(6));
  EXPECT_EQ(client.GetInsertTuple()[3],
            InsertTupleTuple("t", 5, time_point(seconds(1)), {"1"}));
  EXPECT_EQ(client.GetInsertTuple()[4],
            InsertTupleTuple("t", 6, time_point(seconds(1)), {"0"}));
  EXPECT_EQ(client.GetInsertTuple()[5],
            InsertTupleTuple("s", 7, time_point(seconds(1)), {"0"}));
  ASSERT_EQ(client.GetDeleteTuple().size(), static_cast<std::size_t>(2));
  EXPECT_EQ(client.GetDeleteTuple()[1],
            DeleteTupleTuple("s", 8, time_point(seconds(1)), {"0"}));
  ASSERT_EQ(client.GetAddNetworkedLineage().size(),
            static_cast<std::size_t>(0));
  ASSERT_EQ(client.GetAddDerivedLineage().size(), static_cast<std::size_t>(1));
  EXPECT_EQ(client.GetAddDerivedLineage()[0],
            AddDerivedLineageTuple("t", hash({0}), 0, true,
                                   time_point(seconds(1)), "t", hash({1}), 5));

  MockClock::Advance(seconds(1));
  ASSERT_EQ(Status::OK, f.Tick());
  EXPECT_THAT(f.Get<0>().Get(), UnorderedElementsAreArray(T{{0}, {1}, {2}}));
  EXPECT_THAT(f.Get<1>().Get(), UnorderedElementsAreArray(S{}));
  EXPECT_THAT(f.Get<2>().Get(), UnorderedElementsAreArray(C{}));

  ASSERT_EQ(client.GetAddRule().size(), static_cast<std::size_t>(3));
  ASSERT_EQ(client.GetAddCollection().size(), static_cast<std::size_t>(3));
  ASSERT_EQ(client.GetInsertTuple().size(), static_cast<std::size_t>(9));
  EXPECT_EQ(client.GetInsertTuple()[6],
            InsertTupleTuple("t", 9, time_point(seconds(2)), {"2"}));
  EXPECT_EQ(client.GetInsertTuple()[7],
            InsertTupleTuple("t", 10, time_point(seconds(2)), {"0"}));
  EXPECT_EQ(client.GetInsertTuple()[8],
            InsertTupleTuple("s", 11, time_point(seconds(2)), {"0"}));
  ASSERT_EQ(client.GetDeleteTuple().size(), static_cast<std::size_t>(3));
  EXPECT_EQ(client.GetDeleteTuple()[2],
            DeleteTupleTuple("s", 12, time_point(seconds(2)), {"0"}));
  ASSERT_EQ(client.GetAddNetworkedLineage().size(),
            static_cast<std::size_t>(0));
  EXPECT_THAT(
      client.GetAddDerivedLineage(),
      UnorderedElementsAreArray(std::set<AddDerivedLineageTuple>{
          {"t", hash({0}), 0, true, time_point(seconds(1)), "t", hash({1}), 5},
          {"t", hash({0}), 0, true, time_point(seconds(2)), "t", hash({2}), 9},
          {"t", hash({1}), 0, true, time_point(seconds(2)), "t", hash({2}), 9},
      }));
}

TEST(FluentExecutor, BlackBoxLineage) {
  zmq::context_t context(1);
  lineagedb::ConnectionConfig connection_config;
  auto fb_or =
      fluent<ldb::MockClient, Hash, ldb::MockToSql, MockPickler, MockClock>(
          "name", "inproc://yolo", &context, connection_config);
  ASSERT_EQ(Status::OK, fb_or.status());
  auto fe_or =
      fb_or.ConsumeValueOrDie()
          .channel<std::string, std::string, std::int64_t, int>(
              "f_request", {{"dst_addr", "src_addr", "id", "x"}})
          .channel<std::string, std::int64_t, int>("f_response",
                                                   {{"addr", "id", "y"}})
          .RegisterRules([](auto&, auto&) { return std::make_tuple(); });
  ASSERT_EQ(Status::OK, fe_or.status());
  auto f = fe_or.ConsumeValueOrDie();
  ASSERT_EQ(Status::OK, (f.RegisterBlackBoxLineage<0, 1>(
                            [](const std::string& time_inserted,
                               const std::string& x, const std::string& y) {
                              (void)time_inserted;
                              (void)x;
                              (void)y;
                              return "hello world";
                            })));

  const ldb::MockClient<Hash, ldb::MockToSql, MockClock>& client =
      f.GetLineageDbClient();
  ASSERT_EQ(client.GetExec().size(), static_cast<std::size_t>(2));
  EXPECT_EQ(CrunchWhitespace(std::get<0>(client.GetExec()[0])),
            CrunchWhitespace(R"(
    CREATE FUNCTION name_f_response_lineage_impl(integer, int, int)
    RETURNS TABLE(collection_name text, hash bigint, time_inserted integer)
    AS $$hello world$$ LANGUAGE SQL;
  )"));
  EXPECT_EQ(CrunchWhitespace(std::get<0>(client.GetExec()[1])),
            CrunchWhitespace(R"(
    CREATE FUNCTION name_f_response_lineage(bigint)
    RETURNS TABLE(collection_name text, hash bigint, time_inserted integer)
    AS $$
      SELECT name_f_response_lineage_impl(Req.time_inserted, Req.x, Resp.y)
      FROM name_f_request Req, name_f_response Resp
      WHERE Req.id = $1 AND Resp.id = $1
    $$ LANGUAGE SQL;
  )"));
}

// TODO(mwhittaker): Test the lineage of a fluent program with communication
// and deletion.

}  // namespace fluent

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
