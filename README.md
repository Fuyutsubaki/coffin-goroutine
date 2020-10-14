# coffin/goroutine

"coffin/goroutine" は goroutine/channelのような非同期実行をサポートする シングルヘッダライブラリである

```C++:sample.cpp

cfn::Task<> recv(std::shared_ptr<MyRecver<int>> const &ch,std::shared_ptr<MyRecver<int>> const &done_ch){
    for(;;){
        auto [done, ret] = 
            co_await cfn::select(done_ch->recv(), ch->recv());
        if(done){
            break;
        }else if(ret){
            std::cout<<**ret<<std::endl;
        }
    }
}
```

## 準備

1. coffin/goroutineはSchedulerを含んでいない。これはアプリケーションにすでにSchedulerが存在する場合、それと共存するためである
2. 上記のSchedulerとChannnelをつなぐ、 `ChannelStrategy` を定義する必要がある

```C++:example
// Scheduler example
struct MyScheduler {
    boost::asio::io_service io_service_;
    std::shared_ptr<boost::asio::io_service::work> work_ = std::make_shared<boost::asio::io_service::work>(boost::asio::io_service::work(io_service_));
    void post(std::function<void()> && f){
        io_service_.post(std::move(f));
    }

    void run(std::size_t n){
        std::vector<std::thread> thread_list;
        for (std::size_t i = 0; i < n; ++i) {
            thread_list.push_back(std::thread{[&]{io_service_.run();}});
        }
        for(auto&th:thread_list)
            th.join();

        io_service_.reset();
    }
};

static inline MyScheduler global_scheduler;

// Strategy example
struct MyStrategy{
    void post_goroutine(cfn::Goroutine && g){
        global_scheduler.post([=]()mutable{g.execute();}); 
    }
};
```

## usage

### Task<T>

```C++
template <class value_type = void> 
class Task {
public:
  auto operator co_await();
  using promise_type = <unspecified>;
};

```

`Task` は コルーチンによる非同期実行をサポートするクラスである

1. `co_await チャンネル` のように記述することで非同期処理を行うことができる
2. `co_return` を用いて 型`T`の値をコルーチンから返すことができる
3. `Task` の `co_await`に`Task<T>`を渡すことで、`co_return` の結果を受け取ることができる
4. `Goroutine`に渡すことで実行できるようになる

```C++
// 1
auto [sender, recver] = cfn::makeChannel<MyAppStrategy, int>(MyAppStrategy{gbts}, 0);
auto t = 
    [](auto sender) -> cfn::Task<> {
        for(int i=0;i<10;++i){
            co_await sender->send(i);
        }
    }(sender);
```

```C++
// 2,3
cfn::Task<int> task2(){
    co_return 42;
};
cfn::Task<> task1(){
    auto n = co_await task2();
    std::cout<<n<<std::endl;
};

```

### makeChannel()


```
template <class T> concept ChannelStrategy = requires(T strategy) {
  // - require: thread safe
  strategy.post_goroutine(std::declval<std::shared_ptr<Goroutine>>());
};

template <ChannelStrategy Strategy, class value_type> 
class BasicChannel{
public:
    template <class T> SendAwaiter send(T && val);
    RecvAwaiter send();
    void close();
};

template <ChannelStrategy Strategy, class value_type> 
class Sender {
public:
  template <class T> auto send(T &&val);
};
template <ChannelStrategy Strategy, class value_type> 
class Recver {
public:
  auto recv();
};

template <ChannelStrategy Strategy, class value_type>
std::tuple<std::shared_ptr<Sender<Strategy, value_type>>,
           std::shared_ptr<Recver<Strategy, value_type>>>
makeChannel(Strategy strategy, std::size_t n);
```

チャンネルは非同期通信をサポートする

- makeChannelは queue size nのチャンネルを生成し、それへの参照をもつ SenderとRecverを返す
- Senderはデストラクト時にChannelをcloseする
- Channelを使用するにはChannelStrategy を定義する必要がある。詳しくは 準備 の項を参照


```C++
auto [sender, recver] = cfn::makeChannel<MyStrategy, int>(my_strategy, 0);
auto t1 = 
    [](auto sender) -> cfn::Task<> {
        for(int i=0;i<10;++i){
            co_await sender->send(i);
        }
    }(sender);
        
auto t2 = 
    [&](auto recver) -> cfn::Task<> {
        for(;;){
            auto r = co_await recver->recv();
            if(!r)break;
            tmp.push_back(*r);
        }
    }(recver);
```


### select()

selectは複数のchannel.send()/recv()を受け取る。いずれかのchannelで値取得可能になり次第、そのchannelから値を取得する

```
auto [done, ret] = 
    co_await cfn::select(done_ch->recv(), ch->recv());
if(done){
    break;
}else if(ret){
    std::cout<<**ret<<std::endl;
}
```

### Goroutine

`Goroutine` は `Task<>` を実行するためのclassである

```C++
struct Goroutine {
  Goroutine(Task<> &&t);
  void execute();
};
```

### concept ChannelStrategy  

```
template <class T> concept ChannelStrategy = requires(T strategy) {
  strategy.post_goroutine(std::declval<std::shared_ptr<Goroutine>>());
};
```

- `void post_goroutine(cfn::Goroutine &&)`  を実装する必要がある
    - この関数は Channelで blockされた GoroutineをSchedulerにpostするために使う
    - 複数threadでChannnelを使用する場合は、`post_goroutine` はthread safeで無ければならない
