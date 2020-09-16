#pragma once
#include <atomic>
#include <cassert>
#include <deque>
#include <experimental/coroutine>
#include <functional>
#include <list>
#include <mutex>
#include <optional>

namespace cfn {

struct Goroutine;
thread_local inline std::shared_ptr<Goroutine> current_goroutine = nullptr;

template <class value_type = void> struct Task {
  struct promise_type;
  using handle_type = std::experimental::coroutine_handle<promise_type>;
  struct FinalSuspend {
    std::experimental::coroutine_handle<> parent = nullptr;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::experimental::coroutine_handle<>) noexcept;
    void await_resume() noexcept {}
  };
  struct Awaiter {
    handle_type self;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::experimental::coroutine_handle<>) noexcept;
    value_type await_resume() { return self.promise().get(); }
  };

  template <class T> struct promise_type1 {
    template<class U>
    void return_value(U && v) { val = std::forward<U>(v); }
    void unhandled_exception() { std::terminate(); }
    value_type val;
    auto get() { return std::move(val); }
  };
  template <> struct promise_type1<void> {
    void return_void() {}
    void unhandled_exception() { std::terminate(); }
    auto get() {}
  };
  struct promise_type : promise_type1<value_type> {
    promise_type() {}
    auto get_return_object() { return Task{handle_type::from_promise(*this)}; }
    std::experimental::suspend_always initial_suspend() { return {}; }
    FinalSuspend final_suspend() noexcept { return {parent}; }
    std::experimental::coroutine_handle<> parent = nullptr;
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
  handle_type coro_;
};

struct Goroutine : std::enable_shared_from_this<Goroutine> {
  Goroutine(Task<> &&t) : task(std::move(t)), current_handle(task.coro_) {}
  Task<> task;
  std::experimental::coroutine_handle<> current_handle;
  bool resume = false;
  std::size_t wakeup_id = 0;
  std::atomic<bool> on_select_detect_wait;
  // mutex 何とかなくせないか。
  // たとえばgoroutineのpushやchのmutexの解除を
  // https://github.com/golang/go/blob/5c2c6d3fbf4f0a1299b5e41463847d242eae19ca/src/runtime/proc.go#L306
  // みたいな方法にするとか
  std::mutex mutex;
  void execute() {
    std::scoped_lock lock{mutex};
    resume = true;
    current_goroutine = shared_from_this();
    while (resume && current_handle) {
      resume = false;
      current_handle.resume();
    }
    current_goroutine = nullptr;
  }
};

template <class value_type>
void Task<value_type>::FinalSuspend::await_suspend(
    std::experimental::coroutine_handle<>) noexcept {
  current_goroutine->current_handle = parent;
  current_goroutine->resume = true;
}
template <class value_type>
void Task<value_type>::Awaiter::await_suspend(
    std::experimental::coroutine_handle<> h) noexcept {
  self.promise().parent = h;
  current_goroutine->current_handle = self;
  current_goroutine->resume = true;
}

namespace detail {
template <class value_type> struct Sudog {
  value_type *val;
  std::shared_ptr<Goroutine> task;
  std::size_t wakeup_id;
  bool is_select;
  using Iter = typename std::list<Sudog>::iterator;
};

template <class Sudog>
static std::optional<Sudog> dequeueSudog(std::list<Sudog> &queue) {
  for (auto it = queue.begin(); it != queue.end(); ++it) {
    if (it->is_select) {
      bool expected = false;
      bool r = it->task->on_select_detect_wait.compare_exchange_strong(
          expected, true); // TODO
      if (r) {
        return *it; // selectの場合、要素の開放はselectの解放処理に任せる
      }
    } else {
      auto x = *it;
      queue.erase(it);
      return x;
    }
  }
  return {};
}

template <class Sudog>
static typename Sudog::Iter enqueueSudog(std::list<Sudog> &queue, Sudog sdg) {
  queue.push_back(sdg);
  auto it = queue.end();
  it--;
  return it;
}
template <std::size_t... I, class F>
void integral_constant_each(std::index_sequence<I...>, F f) {
  int v[] = {(f(std::integral_constant<std::size_t, I>{}), 0)...};
  (void)v;
}
} // namespace detail

template <class Scheduler, class value_type> class BasicChannel {
public:
  BasicChannel(Scheduler scheduler, std::size_t limit)
      : scheduler(scheduler), queue_limit(limit) {}
  using SendSudog = detail::Sudog<value_type>;
  using RecvSudog = detail::Sudog<std::optional<value_type>>;
  struct [[nodiscard]] SendAwaiter {
    // channelの所有権を持たない。これはSendAwaiterをもつgoroutineが持っているハズであるためである
    BasicChannel &ch;
    value_type val; // おおよそ任意のタイミングでmoveされる
    std::function<void()> f;
    typename SendSudog::Iter it = {};
    bool await_ready() const noexcept {
      return false;
    } // 何回もlock取得するわけにいかないので常にawait_suspendで処理する
    bool await_suspend(std::experimental::coroutine_handle<>) {
      std::scoped_lock lock{ch.mutex};
      auto task = current_goroutine;
      bool r = nonblock_send();
      if (r)
        return false;
      enqueueSudog(ch.send_queue, SendSudog{&val, task, 0, false});
      return true;
    }

    void await_resume() const noexcept {}

    // select用
    bool try_nonblock_exec() { return nonblock_send(); }
    void block_exec(std::size_t wakeup_id) {
      auto task = current_goroutine;
      it = enqueueSudog(ch.send_queue, SendSudog{&val, task, wakeup_id, true});
    }
    void release_sudog() { ch.send_queue.erase(it); }
    void invoke_callback() { f(); }

    bool nonblock_send() {
      if (ch.closed) {
        assert(false);
      } else if (auto opt = dequeueSudog(ch.recv_queue)) {
        auto sdg = *opt;
        *sdg.val = std::move(val);
        ch.scheduler.push_goroutine(sdg.task, sdg.wakeup_id);
        return true;
      } else if (ch.value_queue.size() < ch.queue_limit) {
        ch.value_queue.push_back(std::move(val));
        return true;
      }
      return false;
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
      std::scoped_lock lock{ch.mutex};
      auto task = current_goroutine;
      bool r = nonblock_recv();
      if (r)
        return false;
      enqueueSudog(ch.recv_queue, RecvSudog{&val, task, 0, false});
      return true;
    }
    auto await_resume() const noexcept { return val; }

    bool try_nonblock_exec() { return nonblock_recv(); }
    void block_exec(std::size_t wakeup_id) {
      auto task = current_goroutine;
      it = enqueueSudog(ch.recv_queue, RecvSudog{&val, task, wakeup_id, true});
    }
    void release_sudog() { ch.recv_queue.erase(it); }
    void invoke_callback() { f(val); }

    bool nonblock_recv() {
      if (ch.value_queue.size() > 0) {
        val = ch.value_queue.front();
        ch.value_queue.pop_front();
        if (auto opt = dequeueSudog(ch.send_queue)) {
          auto sdg = *opt;
          ch.value_queue.push_back(*sdg.val);
          ch.scheduler.push_goroutine(sdg.task, sdg.wakeup_id);
        }
        return true;
      } else if (ch.closed) {
        val = std::nullopt;
        return true;
      } else if (auto opt = dequeueSudog(ch.send_queue)) {
        auto sdg = *opt;
        val = std::move(*sdg.val);
        ch.scheduler.push_goroutine(sdg.task, sdg.wakeup_id);
        return true;
      }
      return false;
    }
  };

  template <class T>
  SendAwaiter send(T &&val, std::function<void()> f = [](auto...) {}) {
    return SendAwaiter{*this, std::forward<T>(val), f};
  }

  RecvAwaiter recv(std::function<void(std::optional<value_type>)> f =
                       [](auto...) {}) {
    return RecvAwaiter{*this, std::nullopt, f};
  }

  void close() {
    std::scoped_lock lock{mutex};
    closed = true;
    while (auto opt = dequeueSudog(send_queue)) {
      assert(false);
    }
    while (auto opt = dequeueSudog(recv_queue)) {
      auto sdg = *opt;
      *sdg.val = std::nullopt;
      scheduler.push_goroutine(sdg.task, sdg.wakeup_id);
    }
  }
  Scheduler scheduler;
  std::mutex mutex;
  std::size_t queue_limit;
  std::deque<value_type> value_queue;

  std::list<RecvSudog> recv_queue;
  std::list<SendSudog> send_queue;
  bool closed = false;
};

template <class... T>
struct [[nodiscard]] SelectAwaiter {
  std::tuple<T...> sc_list;
  static constexpr std::size_t N = sizeof...(T);

  bool require_pass3 = false;
  std::size_t wakeup_id = 0;
  bool await_ready() const noexcept {
    return false;
  } // 何回もlock取得するわけにいかないので常にawait_suspendで処理する
  bool await_suspend(std::experimental::coroutine_handle<>) noexcept {
    auto lock = std::apply(
        [](auto &... t) { return std::scoped_lock{t.ch.mutex...}; }, sc_list);
    bool r = false;
    detail::integral_constant_each(std::make_index_sequence<N>{}, [&](auto I) {
      if (!r) {
        r = std::get<I()>(sc_list).try_nonblock_exec();
        if (r) {
          wakeup_id = I();
        }
      }
    });
    if (r)
      return false;
    require_pass3 = true;
    current_goroutine->on_select_detect_wait.store(false); // 初期化
    detail::integral_constant_each(std::make_index_sequence<N>{}, [&](auto I) {
      std::get<I()>(sc_list).block_exec(I());
    });
    return true;
  }
  void await_resume() noexcept {
    // pass 3
    if (require_pass3) {
      auto lock = std::apply(
          [](auto &... t) { return std::scoped_lock{t.ch.mutex...}; }, sc_list);
      wakeup_id = current_goroutine->wakeup_id;
      detail::integral_constant_each(
          std::make_index_sequence<N>{},
          [&](auto I) { std::get<I()>(sc_list).release_sudog(); });
    }
    detail::integral_constant_each(std::make_index_sequence<N>{}, [&](auto I) {
      if (I() == wakeup_id) {
        std::get<I()>(sc_list).invoke_callback();
      }
    });
  }
};

template <class... T> SelectAwaiter<T...> select(T... select_case) {
  return {{select_case...}};
}

template<class Channel>
class Sender{
public:
  Sender(std::shared_ptr<Channel> const &ch):ch(ch){}
  ~Sender(){
    std::cout<<ch.use_count()<<std::endl;
    ch->close();
  }
  template<class T>
  auto send(T && v){
    return ch->send(std::forward<T>(v));
  }
private:
  std::shared_ptr<Channel> ch;
};
template<class Channel>
class Recver{
public:
  Recver(std::shared_ptr<Channel> const &ch):ch(ch){}
  auto recv(){
    return ch->recv();
  }
private:
  std::shared_ptr<Channel> ch;
};

template <class Scheduler, class value_type>
struct SenderRecver{
  std::shared_ptr<Sender<BasicChannel<Scheduler,value_type>>> sender;
  std::shared_ptr<Recver<BasicChannel<Scheduler,value_type>>> recver;
};

template <class Scheduler, class value_type>
SenderRecver<Scheduler,value_type> makeChannel(Scheduler scheduler, std::size_t n){
  using Ch = BasicChannel<Scheduler,value_type>;
  auto p = std::make_shared<Ch>(scheduler, n);
  return {std::make_shared<Sender<Ch>>(p), std::make_shared<Recver<Ch>>(p)};
}

} // namespace cfn
// もしかするとこっちのほうが速いかもしれないが文法がアレになってしまう
#define CFN_INLINE_SUBTASK(TASK, RETURN_VALUE_REF)                             \
  do {                                                                         \
    auto &&t = TASK;                                                           \
    while (!t.coro_.done()) {                                                  \
      t.coro_.resume();                                                        \
      co_await std::experimental::suspend_always{};                            \
    }                                                                          \
    RETURN_VALUE_REF = t.coro_.promise().val;                                  \
  } while (0)
