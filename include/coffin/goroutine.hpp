#pragma once
#include <atomic>
#include <cassert>
#include <deque>
#include <experimental/coroutine>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <variant>

namespace cfn {

class Goroutine;
namespace detail {
class GoroutineWrapper;
struct ExceptionHandler {
  std::exception_ptr e;
};
} // namespace detail

template <class value_type = void> class Task {
public:
  struct promise_type;
  using handle_type = std::experimental::coroutine_handle<promise_type>;
  struct FinalSuspend {
    std::experimental::coroutine_handle<> parent = nullptr;
    bool await_ready() const noexcept { return false; }
    std::experimental::coroutine_handle<>
        await_suspend(std::experimental::coroutine_handle<>) noexcept;
    void await_resume() noexcept {}
  };
  struct Awaiter {
    handle_type self;
    bool await_ready() const noexcept { return false; }
    std::experimental::coroutine_handle<>
        await_suspend(std::experimental::coroutine_handle<>) noexcept;
    value_type await_resume() { return self.promise().get_or_rethrow(); }
  };

  template <class T> struct promise_type1 {
    template <class U> void return_value(U &&v) { val = std::forward<U>(v); }
    void unhandled_exception() {
      val = detail::ExceptionHandler{std::current_exception()};
    }
    std::variant<detail::ExceptionHandler, value_type> val;
    auto get_or_rethrow() {
      if (auto p = std::get_if<detail::ExceptionHandler>(&val)) {
        std::rethrow_exception(p->e);
      }
      return std::move(std::get<value_type>(val));
    }
  };
  template <> struct promise_type1<void> {
    void return_void() {}
    void unhandled_exception() { ep = std::current_exception(); }
    std::exception_ptr ep;
    auto get_or_rethrow() {
      if (ep) {
        std::rethrow_exception(ep);
      }
    }
  };
  struct promise_type : promise_type1<value_type> {
    promise_type() {}
    auto get_return_object() { return Task{handle_type::from_promise(*this)}; }
    std::experimental::suspend_always initial_suspend() { return {}; }
    FinalSuspend final_suspend() noexcept { return {parent}; }
    std::experimental::coroutine_handle<> parent =
        std::experimental::noop_coroutine();
  };
  auto operator co_await() { return Awaiter{coro_}; }

  explicit Task(handle_type c) : coro_(c) {}
  Task(Task const &src) = delete;
  Task(Task &&src) : coro_(src.coro_) { src.coro_ = nullptr; }
  ~Task() {
    if (coro_) {
      coro_.destroy();
    }
  }

private:
  friend Goroutine;
  handle_type coro_;
};

thread_local inline detail::GoroutineWrapper *current_goroutine = nullptr;

class Goroutine : public std::enable_shared_from_this<Goroutine> {
  friend detail::GoroutineWrapper;
  Task<> task;
  std::experimental::coroutine_handle<> current_handle;
  // select
  struct Seldat {
    std::size_t wakeup_id = 0;
    std::atomic<bool> on_select_detect_wait = false;
  };
  std::mutex mutex;
  Seldat seldat;

public:
  Goroutine(Task<> &&t) : task(std::move(t)), current_handle(task.coro_) {}
  void execute();
};

namespace detail {
struct GoroutineWrapper {
  std::shared_ptr<Goroutine> p;
  Goroutine::Seldat &select() { return p->seldat; }
  std::experimental::coroutine_handle<> &current_handle() {
    return p->current_handle;
  }
  std::shared_ptr<Goroutine> to_goroutine() { return p; };
};
} // namespace detail

void Goroutine::execute() {
  std::scoped_lock lock{mutex};
  detail::GoroutineWrapper c{shared_from_this()};
  ;
  current_goroutine = &c;
  current_handle.resume();
  if (task.coro_.done())
    task.coro_.promise().get_or_rethrow(); // void or rethrow
  current_goroutine = nullptr;
}

template <class value_type>
std::experimental::coroutine_handle<>
Task<value_type>::FinalSuspend::await_suspend(
    std::experimental::coroutine_handle<>) noexcept {
  current_goroutine->current_handle() = parent;
  return parent;
}
template <class value_type>
std::experimental::coroutine_handle<> Task<value_type>::Awaiter::await_suspend(
    std::experimental::coroutine_handle<> h) noexcept {
  self.promise().parent = h;
  current_goroutine->current_handle() = self;
  return self;
}

namespace detail {
template <class value_type> struct Sudog {
  value_type *val;
  GoroutineWrapper task;
  std::size_t wakeup_id;
  bool is_select;
  using Iter = typename std::list<Sudog>::iterator;
};

template <class Sudog> class SudogList {
  using Iter = typename Sudog::Iter;
  std::list<Sudog> queue;
  Iter hint_iter = queue.end();

public:
  std::optional<Sudog> dequeueSudog() {
    for (; hint_iter != queue.end(); ++hint_iter) {
      if (hint_iter->is_select) {
        bool r = hint_iter->task.p &&
                 hint_iter->task.select().on_select_detect_wait.exchange(false);
        if (r) {
          // selectの場合、要素の解放はselectの解放処理に任せる
          auto x = std::move(*hint_iter);
          hint_iter++;
          return std::move(x);
        }
      } else {
        auto it = hint_iter;
        hint_iter++;
        auto x = std::move(*it);
        queue.erase(it);
        return x;
      }
    }
    return {};
  }

  Iter enqueueSudog(Sudog sdg) {
    queue.push_back(std::move(sdg));
    auto it = queue.end();
    it--;
    if (hint_iter == queue.end()) {
      hint_iter = it;
    }
    return it;
  }
  void eraseSudog(Iter it) {
    if (hint_iter == it) {
      hint_iter++;
    }
    queue.erase(it);
  }
};

template <std::size_t... I, class F>
void integral_constant_each(std::index_sequence<I...>, F f) {
  int v[] = {(f(std::integral_constant<std::size_t, I>{}), 0)...};
  (void)v;
}
template <std::size_t... I, class F>
auto integral_constant_map(std::index_sequence<I...>, F f) {
  return std::make_tuple(f(std::integral_constant<std::size_t, I>{})...);
}
} // namespace detail

template <class T> concept ChannelStrategy = requires(T strategy) {
  // - require: thread safe
  strategy.push_goroutine(std::declval<std::shared_ptr<Goroutine>>());
};

template <ChannelStrategy Strategy, class value_type> class BasicChannel {
public:
  BasicChannel(Strategy strategy, std::size_t limit)
      : strategy(strategy), queue_limit(limit) {}
  using SendSudog = detail::Sudog<value_type>;
  using RecvSudog = detail::Sudog<std::optional<value_type>>;
  struct [[nodiscard]] SendAwaiter {
    // channelの所有権を持たない。これはSendAwaiterをもつgoroutineが持っているハズであるためである
    BasicChannel &ch;
    value_type val; // おおよそ任意のタイミングでmoveされる
    typename SendSudog::Iter it = {};
    bool await_ready() const noexcept {
      return false; // 何回もlock取得するわけにいかないので常にawait_suspendで処理する
    }
    bool await_suspend(std::experimental::coroutine_handle<>) {
      std::optional<detail::GoroutineWrapper> next_goroutine;
      bool r = false;
      {
        std::scoped_lock lock{ch.mutex};
        auto task = current_goroutine->to_goroutine();
        std::tie(r, next_goroutine) = nonblock_send();
        if (!r)
          ch.send_queue.enqueueSudog(
              SendSudog{&val, std::move(task), 0, false});
      }
      if (next_goroutine)
        ch.strategy.push_goroutine(std::move(next_goroutine->to_goroutine()));
      return !r;
    }

    void await_resume() const noexcept {}

    // select用
    auto try_nonblock_exec() { return nonblock_send(); }
    void block_exec(std::size_t wakeup_id) {
      auto task = current_goroutine->to_goroutine();
      it = ch.send_queue.enqueueSudog(
          SendSudog{&val, std::move(task), wakeup_id, true});
    }
    void release_sudog() { ch.send_queue.eraseSudog(it); }
    bool select_result(bool r) { return r; }

    std::tuple<bool, std::optional<detail::GoroutineWrapper>> nonblock_send() {
      if (ch.closed) {
        assert(false);
      } else if (auto opt = ch.recv_queue.dequeueSudog()) {
        auto &sdg = *opt;
        *sdg.val = std::move(val);
        sdg.task.select().wakeup_id = sdg.wakeup_id;
        return {true, std::move(sdg.task)};
      } else if (ch.value_queue.size() < ch.queue_limit) {
        ch.value_queue.push_back(std::move(val));
        return {true, std::nullopt};
      }
      return {false, std::nullopt};
      ;
    }
  };

  struct [[nodiscard]] RecvAwaiter {
    BasicChannel &ch;
    std::optional<value_type> val;
    std::function<void(std::optional<value_type>)> f;
    typename RecvSudog::Iter it = {};
    bool await_ready() const noexcept {
      return false;
    } // 何回もlock取得するわけにいかないので常にawait_suspendで処理する
    bool await_suspend(std::experimental::coroutine_handle<>) {
      std::optional<detail::GoroutineWrapper> next_goroutine;
      bool r = false;
      {
        std::scoped_lock lock{ch.mutex};
        auto task = current_goroutine->to_goroutine();
        std::tie(r, next_goroutine) = nonblock_recv();
        if (!r)
          ch.recv_queue.enqueueSudog(
              RecvSudog{&val, std::move(task), 0, false});
      }
      if (next_goroutine)
        ch.strategy.push_goroutine(std::move(next_goroutine->to_goroutine()));
      return !r;
    }
    auto await_resume() noexcept { return std::move(val); }

    auto try_nonblock_exec() { return nonblock_recv(); }
    void block_exec(std::size_t wakeup_id) {
      auto task = current_goroutine->to_goroutine();
      it = ch.recv_queue.enqueueSudog(
          RecvSudog{&val, std::move(task), wakeup_id, true});
    }
    void release_sudog() { ch.recv_queue.eraseSudog(it); }

    std::optional<std::optional<value_type>> select_result(bool r) {
      if (r)
        return std::move(val);
      return std::nullopt;
    }

    std::tuple<bool, std::optional<detail::GoroutineWrapper>> nonblock_recv() {
      if (ch.value_queue.size() > 0) {
        val = std::move(ch.value_queue.front());
        ch.value_queue.pop_front();
        if (auto opt = ch.send_queue.dequeueSudog()) {
          auto &sdg = *opt;
          ch.value_queue.push_back(std::move(*sdg.val));
          sdg.task.select().wakeup_id = sdg.wakeup_id;
          return {true, std::move(sdg.task)};
        }
        return {true, std::nullopt};
      } else if (ch.closed) {
        val = std::nullopt;
        return {true, std::nullopt};
      } else if (auto opt = ch.send_queue.dequeueSudog()) {
        auto &sdg = *opt;
        val = std::move(*sdg.val);
        sdg.task.select().wakeup_id = sdg.wakeup_id;
        return {true, std::move(sdg.task)};
      }
      return {false, std::nullopt};
    }
  };

  template <class T> SendAwaiter send(T &&val) {
    return SendAwaiter{*this, std::forward<T>(val)};
  }

  RecvAwaiter recv() { return RecvAwaiter{*this, std::nullopt}; }

  void close() {
    std::scoped_lock lock{mutex};
    closed = true;
    while (auto opt = send_queue.dequeueSudog()) {
      assert(false);
    }
    while (auto opt = recv_queue.dequeueSudog()) {
      auto &sdg = *opt;
      *sdg.val = std::nullopt;
      sdg.task.select().wakeup_id = sdg.wakeup_id;
      strategy.push_goroutine(sdg.task.to_goroutine());
    }
  }
  Strategy strategy;
  // mutex内で以下を行ってはならない。deadlockや処理に時間がかかる原因になりうるため
  // 1. strategy.push_goroutine()の呼び出し
  // 2.
  // goroutineの参照カウントを増やす行為(goroutineが破棄されることに~Task()が呼び出される)
  std::mutex mutex;
  std::size_t queue_limit;
  std::deque<value_type> value_queue;

  detail::SudogList<RecvSudog> recv_queue;
  detail::SudogList<SendSudog> send_queue;
  bool closed = false;
};

template <class... T>
struct [[nodiscard]] SelectAwaiter {
  std::tuple<T...> sc_list;
  static constexpr std::size_t N = sizeof...(T);

  bool require_pass3 = false;
  std::size_t wakeup_id = 0;
  bool await_ready() const noexcept {
    return false; // 何回もlock取得するわけにいかないので常にawait_suspendで処理する
  }
  bool await_suspend(std::experimental::coroutine_handle<>) noexcept {
    bool r = false;
    std::optional<detail::GoroutineWrapper> next_goroutine;
    { // lock block
      auto lock = std::apply(
          [](auto &... t) { return std::scoped_lock{t.ch.mutex...}; }, sc_list);

      detail::integral_constant_each(
          std::make_index_sequence<N>{}, [&](auto I) {
            if (!r) {
              std::tie(r, next_goroutine) =
                  std::get<I()>(sc_list).try_nonblock_exec();
              if (r) {
                wakeup_id = I();
              }
            }
          });
      if (!r) {
        require_pass3 = true;
        current_goroutine->select().on_select_detect_wait.store(true);
        detail::integral_constant_each(
            std::make_index_sequence<N>{},
            [&](auto I) { std::get<I()>(sc_list).block_exec(I()); });
      }
    } // lock block
    if (next_goroutine) {
      detail::integral_constant_each(
          std::make_index_sequence<N>{}, [&](auto I) {
            if (I() == wakeup_id) {
              std::get<I()>(sc_list).ch.strategy.push_goroutine(
                  next_goroutine->to_goroutine());
            }
          });
    }
    return !r;
  }
  auto await_resume() noexcept {
    // pass 3
    if (require_pass3) {
      auto lock = std::apply(
          [](auto &... t) { return std::scoped_lock{t.ch.mutex...}; }, sc_list);
      wakeup_id = current_goroutine->select().wakeup_id;
      detail::integral_constant_each(
          std::make_index_sequence<N>{},
          [&](auto I) { std::get<I()>(sc_list).release_sudog(); });
    }
    return detail::integral_constant_map(
        std::make_index_sequence<N>{}, [&](auto I) {
          return std::get<I()>(sc_list).select_result(I() == wakeup_id);
        });
  }
};

template <class... T> SelectAwaiter<T...> select(T... select_case) {
  return {{select_case...}};
}

template <ChannelStrategy Strategy, class value_type> class Sender {
  std::shared_ptr<BasicChannel<Strategy, value_type>> ch;

public:
  Sender(std::shared_ptr<BasicChannel<Strategy, value_type>> const &ch)
      : ch(ch) {}
  ~Sender() { ch->close(); }
  template <class T> auto send(T &&val) {
    return ch->send(std::forward<T>(val));
  }
};
template <ChannelStrategy Strategy, class value_type> class Recver {
  std::shared_ptr<BasicChannel<Strategy, value_type>> ch;

public:
  Recver(std::shared_ptr<BasicChannel<Strategy, value_type>> const &ch)
      : ch(ch) {}
  auto recv() { return ch->recv(); }
};

template <ChannelStrategy Strategy, class value_type>
std::tuple<std::shared_ptr<Sender<Strategy, value_type>>,
           std::shared_ptr<Recver<Strategy, value_type>>>
makeChannel(Strategy strategy, std::size_t n) {
  using Ch = BasicChannel<Strategy, value_type>;
  auto p = std::make_shared<Ch>(strategy, n);
  return {std::make_shared<Sender<Strategy, value_type>>(p),
          std::make_shared<Recver<Strategy, value_type>>(p)};
}

} // namespace cfn
