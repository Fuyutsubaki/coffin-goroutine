# coffin/goroutine

"coffin/goroutine" は goroutine/channelのような非同期実行をサポートする シングルヘッダライブラリである

## チュートリアル

チュートリアルで説明するすべてのコードは、下のリンクからブラウザで試すことができる
https://wandbox.org/permlink/G8jTPwCnlPTOYXEU

### 準備

#### 1. Scheduler
もしあなたのアプリケーションがSchedulerを持っていない場合、Schedulerを用意する必要がある
- coffin/goroutineはSchedulerを含んでいない。これはアプリケーションにすでにSchedulerが存在する場合、それと共存するためである

```C++:example
// Scheduler
struct MyScheduler {
    boost::asio::io_service io_service_;
    std::shared_ptr<boost::asio::io_service::work> work_;
    void post(std::function<void()> && f){
        io_service_.post(std::move(f));
    }

    void run(std::size_t n){
        work_ = std::make_shared<boost::asio::io_service::work>(boost::asio::io_service::work(io_service_));
        std::vector<std::thread> thread_list;
        for (std::size_t i = 0; i < n; ++i) {
            thread_list.push_back(std::thread{[&]{io_service_.run();}});
        }
        for(auto&th:thread_list)
            th.join();

        io_service_.reset();
    }

    void stop(){
        work_.reset();
    }
};

static inline MyScheduler global_scheduler;
```

#### 2. ChannelStrategy
1のSchedulerとChannnelをつなぐ、 `ChannelStrategy` を定義する必要がある

するべきことは、渡された goroutineを一度だけ実行する処理をScheduler に postする関数`post_goroutine` を定義することである

```C++:example
struct MyStrategy{
    void post_goroutine(std::shared_ptr<cfn::Goroutine> && g){
        global_scheduler.post([=]()mutable{g->execute();}); 
    }
};
```

### Taskと Goroutine
- TaskとGoroutineを用いることで、Schedulerにtaskをpostすることができる
- GoroutineはTaskから変換することができる

```C++:example1
void spown_task(cfn::Task<> && task){
    global_scheduler.post([g = std::make_shared<cfn::Goroutine>(std::move(task))]{
        g->execute();
    });
}

cfn::Task<> example1_1(){
    std::cout<<1<<std::endl;
    co_return;
}
void example1(){
    spown_task(example1_1());
    spown_task([]()->cfn::Task<>{
        std::cout<<2<<std::endl;
        co_return;
    }());
}

```

### TaskとChannnelによる非同期処理

- TaskとChannelを用いることで非同期処理を行うことができる

```C++:example2
cfn::Task<> fibonacci_n(int n, std::shared_ptr<cfn::Sender<MyStrategy,int>> ch){
    int x=0;
    int y=1;
    for(int i=0;i<n;++i){
        co_await ch->send(x);
        int next = x+y;
        x=y;
        y=next;
    }
}
cfn::Task<> example2(){
    auto [sender,recver] = cfn::makeChannel<MyStrategy,int>(MyStrategy{},0);
    spown_task(fibonacci_n(10, std::move(sender)));
    for(;;){
        auto ret = co_await recver->recv();
        if(!ret) break;
        std::cout<<*ret<<std::endl;
    }
}

```

### select

selectを用いて複数のChannelについて待ち処理を行うことができる

```C++
cfn::Task<> fibonacci_seq(std::shared_ptr<cfn::Sender<MyStrategy,int>> ret_ch, std::shared_ptr<cfn::Recver<MyStrategy,int>> quit_ch){
    int x=0;
    int y=1;
    for(;;){
        auto [ret, quit] = co_await select(ret_ch->send(x), quit_ch->recv());
        if(ret){
            int next = x+y;
            x=y;
            y=next;
        }else if(quit){
            break;
        }
    }
}
cfn::Task<> example3(){
    auto [ret_send,ret_recv] = cfn::makeChannel<MyStrategy,int>(MyStrategy{},0);
    auto [quit_send,quit_recv] = cfn::makeChannel<MyStrategy,int>(MyStrategy{},0);
    spown_task(fibonacci_seq(std::move(ret_send), std::move(quit_recv)));
    for(int i=0;i<10;++i){
        auto x = co_await ret_recv->recv();
        std::cout<<*x<<std::endl;
    }
    quit_send.reset(); // 明示的にclose
}

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

```C++
auto [done, ret] = 
    co_await cfn::select(done_ch->recv(), ch->recv());
if(done){
    break;
}else if(ret){
    std::cout<<**ret<<std::endl;
}
```

### Goroutine

`Goroutine` は 以下の役割を持つ
- TaskでChannelを使えるようにする
    - TaskをSchedulerで使えるようにする

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
