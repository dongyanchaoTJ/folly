/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <folly/portability/GTest.h>

#include <folly/fibers/FiberManager.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/fibers/async/Async.h>
#include <folly/fibers/async/Baton.h>
#include <folly/fibers/async/Future.h>
#include <folly/fibers/async/Promise.h>
#include <folly/io/async/EventBase.h>

#if FOLLY_HAS_COROUTINES
#include <folly/experimental/coro/Sleep.h>
#include <folly/fibers/async/Task.h>
#endif

using namespace folly::fibers;

namespace {
std::string getString() {
  return "foo";
}
async::Async<void> getAsyncNothing() {
  return {};
}
async::Async<std::string> getAsyncString() {
  return getString();
}
async::Async<folly::Optional<std::string>> getOptionalAsyncString() {
  // use move constructor to convert Async<std::string> to
  // Async<folly::Optional<std::string>>
  return getAsyncString();
}
async::Async<std::tuple<int, float, std::string>> getTuple() {
  return {0, 0.0, "0"};
}

struct NonCopyableNonMoveable {
  NonCopyableNonMoveable(const NonCopyableNonMoveable&) = delete;
  NonCopyableNonMoveable(NonCopyableNonMoveable&&) = delete;
  NonCopyableNonMoveable& operator=(NonCopyableNonMoveable const&) = delete;
  NonCopyableNonMoveable& operator=(NonCopyableNonMoveable&&) = delete;
};

async::Async<NonCopyableNonMoveable const&> getReference() {
  thread_local NonCopyableNonMoveable const value{};

  return value;
}

} // namespace

TEST(AsyncTest, asyncAwait) {
  folly::EventBase evb;
  auto& fm = getFiberManager(evb);

  EXPECT_NO_THROW(
      fm.addTaskFuture([&]() {
          async::init_await(getAsyncNothing());
          EXPECT_EQ(getString(), async::init_await(getAsyncString()));
          EXPECT_EQ(getString(), *async::init_await(getOptionalAsyncString()));
          async::init_await(getTuple());
          decltype(auto) ref = async::init_await(getReference());
          static_assert(
              std::is_same<decltype(ref), NonCopyableNonMoveable const&>::value,
              "");
        })
          .getVia(&evb));
}

TEST(AsyncTest, asyncBaton) {
  folly::EventBase evb;
  auto& fm = getFiberManager(evb);
  std::chrono::steady_clock::time_point start;

  EXPECT_NO_THROW(
      fm.addTaskFuture([&]() {
          constexpr auto kTimeout = std::chrono::milliseconds(230);
          {
            Baton baton;
            baton.post();
            EXPECT_NO_THROW(async::await(async::baton_wait(baton)));
          }
          {
            Baton baton;
            start = std::chrono::steady_clock::now();
            auto res = async::await(async::baton_try_wait_for(baton, kTimeout));
            EXPECT_FALSE(res);
            EXPECT_LE(start + kTimeout, std::chrono::steady_clock::now());
          }
          {
            Baton baton;
            start = std::chrono::steady_clock::now();
            auto res = async::await(
                async::baton_try_wait_until(baton, start + kTimeout));
            EXPECT_FALSE(res);
            EXPECT_LE(start + kTimeout, std::chrono::steady_clock::now());
          }
        })
          .getVia(&evb));
}

TEST(AsyncTest, asyncPromise) {
  folly::EventBase evb;
  auto& fm = getFiberManager(evb);

  fm.addTaskFuture([&]() {
      auto res = async::await(
          async::promiseWait([](Promise<int> p) { p.setValue(42); }));
      EXPECT_EQ(res, 42);
    })
      .getVia(&evb);
}

TEST(AsyncTest, asyncFuture) {
  folly::EventBase evb;
  auto& fm = getFiberManager(evb);

  // Return format: Info about where continuation runs (thread_id,
  // in_fiber_loop, on_active_fiber)
  auto getSemiFuture = []() {
    return folly::futures::sleep(std::chrono::milliseconds(1))
        .defer([](auto&&) {
          return std::make_tuple(
              std::this_thread::get_id(),
              FiberManager::getFiberManagerUnsafe() != nullptr,
              onFiber());
        });
  };

  fm.addTaskFuture([&]() {
      auto this_thread_id = std::this_thread::get_id();
      {
        // Unspecified executor: Deferred work will be executed inline on
        // producer thread
        auto res = async::init_await(
            async::futureWait(getSemiFuture().toUnsafeFuture()));
        EXPECT_NE(this_thread_id, std::get<0>(res));
        EXPECT_FALSE(std::get<1>(res));
        EXPECT_FALSE(std::get<2>(res));
      }

      {
        // Specified executor: Deferred work will be executed on evb
        auto res =
            async::init_await(async::futureWait(getSemiFuture().via(&evb)));
        EXPECT_EQ(this_thread_id, std::get<0>(res));
        EXPECT_FALSE(std::get<1>(res));
        EXPECT_FALSE(std::get<2>(res));
      }

      {
        // Unspecified executor: Deferred work will be executed inline on
        // consumer thread main-context
        auto res = async::init_await(async::futureWait(getSemiFuture()));
        EXPECT_EQ(this_thread_id, std::get<0>(res));
        EXPECT_TRUE(std::get<1>(res));
        EXPECT_FALSE(std::get<2>(res));
      }
    })
      .getVia(&evb);
}

#if FOLLY_HAS_COROUTINES
TEST(AsyncTest, asyncTask) {
  auto coroFn =
      []() -> folly::coro::Task<std::tuple<std::thread::id, bool, bool>> {
    co_await folly::coro::sleep(std::chrono::milliseconds(1));
    co_return std::make_tuple(
        std::this_thread::get_id(),
        FiberManager::getFiberManagerUnsafe() != nullptr,
        onFiber());
  };

  folly::EventBase evb;
  auto& fm = getFiberManager(evb);

  fm.addTaskFuture([&]() {
      // Coroutine should run to completion on fiber main context
      EXPECT_EQ(
          std::make_tuple(std::this_thread::get_id(), true, false),
          async::init_await(async::taskWait(coroFn())));
    })
      .getVia(&evb);
}
#endif

TEST(AsyncTest, asyncTraits) {
  static_assert(!async::is_async_v<int>);
  static_assert(async::is_async_v<async::Async<int>>);
  static_assert(
      std::is_same<int, async::async_inner_type_t<async::Async<int>>>::value);
  static_assert(
      std::is_same<int&, async::async_inner_type_t<async::Async<int&>>>::value);
}

#if __cpp_deduction_guides >= 201703
TEST(AsyncTest, asyncConstructorGuides) {
  auto getLiteral = []() { return async::Async(1); };
  // int&& -> int
  static_assert(
      std::is_same<int, async::async_inner_type_t<decltype(getLiteral())>>::
          value);

  int i = 0;
  auto tryGetRef = [&]() { return async::Async(static_cast<int&>(i)); };
  // int& -> int
  static_assert(
      std::is_same<int, async::async_inner_type_t<decltype(tryGetRef())>>::
          value);

  auto tryGetConstRef = [&]() {
    return async::Async(static_cast<const int&>(i));
  };
  // const int& -> int
  static_assert(
      std::is_same<int, async::async_inner_type_t<decltype(tryGetConstRef())>>::
          value);

  // int& explicitly constructed
  auto getRef = [&]() { return async::Async<int&>(i); };
  static_assert(
      std::is_same<int&, async::async_inner_type_t<decltype(getRef())>>::value);
}
#endif
