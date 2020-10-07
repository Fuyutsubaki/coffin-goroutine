# coffin/goroutine

"coffin/goroutine" is a single header library for supporting goroutine-like concurrency in C++

```C++:sample.cpp
template<class value_type>
using MyChannel = cfn::BasicChannel<MyScheduler, value_type>;

cfn::Task<> recv(MyChannel<int>&ch, MyChannel<int>&done_ch){
    co_await cfn::select(
        done_ch.recv([&](auto){
            done = true;
        }),
        ch.recv([&](auto x){
            tmp.push_back(*x);
        })
    );
}
```



## 準備
coffin/goroutineはSchedulerを含んでいない。これはアプリケーションにすでにSchedulerが存在する場合、それと共存するためである

アプリケーション開発者は以下を用意する必要がある

- Scheduler (無ければ)
- Scheduler の Adapter
- あなたのアプリケーションに必要な便利関数(欲しければ)

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

    void stop(){
        work_.reset();
    }
};

static inline MyScheduler global_scheduler;

// Adapter example
struct Adapter{
    void push_goroutine(cfn::Goroutine && g){
        global_scheduler.post([g=std::move(g.move_as_SharedGoroutine())]()mutable{g.execute();}); 
    }
};
```

## usage

### Task<T>

`Task` は コルーチンによる非同期実行をサポートするクラスである

1. `co_await チャンネル` のように記述することで非同期処理を行うことができる
2. `co_return` を用いて 型`T`の値をコルーチンから返すことができる
3. `Task` の `co_await`に`Task<T>`を渡すことで、`co_return` の結果を受け取ることができる


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

### auto makeChannel<ChannelStrategy,T>(ChannelStrategy, queue_size)
チャンネルは非同期通信をサポートする

- makeChannelは queue size nのチャンネルを生成し、それへの参照をもつ SenderとRecverを返します
- Senderはデストラクト時にChannelをcloseします

```C++
auto [sender, recver] = cfn::makeChannel<MyScheduler, int>(gsc,0);
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


### select/try_select


### Goroutine

`Goroutine` は `Task` を実行するためのclassである。
Schedulerは Goroutineを受け取り、実行する必要がある

```C++
struct Goroutine {
  Goroutine(Task<> &&t);
  void execute();
};
```
