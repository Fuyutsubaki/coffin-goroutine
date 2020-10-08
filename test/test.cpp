#define IUTEST_USE_MAIN 1
#include <coffin/goroutine.hpp>
#include <iutest.hpp>
#include <thread>
#include<vector>
#include<array>
#include<iostream>

IUTEST(Goroutine, execute){
    int n;
    auto g = std::make_shared<cfn::Goroutine>([&]() -> cfn::Task<> {
        for(int i=0;i<3;++i){
            n = i;
            co_await std::experimental::suspend_always{};
        }
    }());
    for(int i=0;i<3;++i){
        g->execute();
        IUTEST_ASSERT_EQ(i,n);
    }
}

cfn::Task<> gen_recursive_task(int &n, int x, int len){
    if(len == 1){
        n = x;
        co_await std::experimental::suspend_always{};
    }else{
        co_await gen_recursive_task(n, x, len / 2);
        co_await gen_recursive_task(n, x + len / 2, len - len / 2);
    }
 };
IUTEST(Goroutine, subtask_call){
    int n;
    auto g = std::make_shared<cfn::Goroutine>(gen_recursive_task(n,0,128));
    // co_await subtask()でsuspendせず
    // 128回で呼びきれる
    for(int i=0;i<128;++i){
        g->execute();
        IUTEST_ASSERT_EQ(i,n);
    }
}
IUTEST(Goroutine, subtask_exception){
    {
        auto g = std::make_shared<cfn::Goroutine>([&]() -> cfn::Task<> {
            co_await []() -> cfn::Task<> {
                throw 42;
                co_await std::experimental::suspend_always{};
            }();

            co_await std::experimental::suspend_always{};
        }());

        IUTEST_ASSERT_THROW_VALUE_EQ(g->execute(), int, 42);
    }
    {
        int n = 0;
        auto g = std::make_shared<cfn::Goroutine>([&]() -> cfn::Task<> {
            try
            {
                co_await []() -> cfn::Task<int> {
                    throw 42;
                    co_await std::experimental::suspend_always{};
                    co_return 5;
                }();
            }
            catch (int x)
            {
                n = x;
            }
        }());
        g->execute();
        IUTEST_ASSERT_EQ(42, n);
    }
}

IUTEST(Goroutine, subtask_return){
    int n;
    auto gen_subtask = []() -> cfn::Task<std::unique_ptr<int>> {
        co_return std::make_unique<int>(42);
    };
    auto g = std::make_shared<cfn::Goroutine>([&]() -> cfn::Task<> {
        n = *(co_await gen_subtask());
    }());
    g->execute();
    IUTEST_ASSERT_EQ(42, n);
}

struct MyScheduler{
    static inline std::deque<std::shared_ptr<cfn::Goroutine>> queue;
    static inline std::mutex mutex;
    static inline std::atomic<bool> on_ready = true;
    void spown_task(cfn::Task<>&&task){
        std::scoped_lock lock{mutex};
        queue.push_back(std::make_shared<cfn::Goroutine>(std::move(task)));
    }
    void push_goroutine(std::shared_ptr<cfn::Goroutine> && g){
        std::scoped_lock lock{mutex};
        queue.push_back(g);
    }
    std::optional<std::shared_ptr<cfn::Goroutine>> dequeue_task(){
        std::scoped_lock lock{mutex};
        if(queue.empty()){
            return std::nullopt;
        }
        auto p = queue.front();
        queue.pop_front();
        return p;
    }
    void stop(){
        on_ready.store(false);
    }
    void run(std::size_t num){
        on_ready.store(true);
        std::vector<std::thread> worker;
        for(int i=0;i<num;++i){
            std::thread th{[&]{
            while(on_ready.load()){
                auto task = MyScheduler::dequeue_task();
                if(task){
                    (*task)->execute();
                }else{
                    std::this_thread::yield();
                }
            }
            }};
            worker.emplace_back(std::move(th));
        }
        for(auto&th:worker){
            th.join();
        }
    }
};
static MyScheduler gsc;

template<class value_type>
using MyChannel = cfn::BasicChannel<MyScheduler, value_type>;

template<class T>
MyChannel<T> make_channel(std::size_t queue_size){
    return {gsc, queue_size};
}


IUTEST(BasicChannel, SendRecv){
    for(int i=1;i<10;++i){
        std::vector<int> tmp;
        auto ch = make_channel<std::unique_ptr<int>>(4);
        gsc.spown_task(
            [&]() -> cfn::Task<> {
                for(int i=0;i<10;++i){
                    co_await ch.send(std::make_unique<int>(i));
                }
                ch.close();
            }()
        );
        gsc.spown_task(
            [&]() -> cfn::Task<> {
                for(;;){
                    auto r = co_await ch.recv();
                    if(!r)break;
                    tmp.push_back(**r);
                }
                gsc.stop();
            }()
        );
        gsc.run(i);
        IUTEST_ASSERT_EQ((std::vector{0,1,2,3,4,5,6,7,8,9}), tmp);
    }
}

IUTEST(BasicChannel, SendRecvMultiInMultiOut){
    std::array<std::vector<int>,3> tmp;
    std::vector<int> tmp2;
    auto ch = make_channel<int>(4);
    std::atomic<int> cnt_in = 0;
    std::atomic<int> cnt_out = 0;
    for(int i=0;i<10;++i){
        gsc.spown_task(
            [&]() -> cfn::Task<> {
                for(int i=0;i<10;++i){
                    co_await ch.send(i);
                }
                if( (cnt_in+=1) == 10){
                    ch.close();
                }
            }()

        );
    }

    for(int i=0;i<3;++i){
        gsc.spown_task(
            [&](int i) -> cfn::Task<> {
                for(;;){
                    auto r = co_await ch.recv();
                    if(!r)break;
                    tmp[i].push_back(*r);
                }
                if((cnt_out+=1) == 3){
                    gsc.stop();
                }
            }(i)
        );
    }

    gsc.run(4);

    for(auto &v:tmp)
        for(auto x:v)
            tmp2.push_back(x);
    
    std::sort(tmp2.begin(), tmp2.end());

    std::vector<int> expected;
    for(int i=0;i<10;++i){
        for(int k=0;k<10;++k){
            expected.push_back(i);
        }
    }
    IUTEST_ASSERT_EQ(expected, tmp2);
}

IUTEST(BasicChannel, Select){
    std::vector<int> tmp;
    auto ch = make_channel<int>(0);
    auto done_ch = make_channel<int>(0);
    gsc.spown_task(
        [&]() -> cfn::Task<> {
            for(int i=0;i<10;++i)
            {
                co_await ch.send(i);
            }
            done_ch.close();
        }());
    gsc.spown_task(
        [&]() -> cfn::Task<> {
            while (1){
                auto [done, ret] = co_await cfn::select(
                    done_ch.recv(),
                    ch.recv());
                if(done){
                    break;
                }else if(ret){
                    tmp.push_back(**ret);
                }
            }
            gsc.stop();
        }());
    gsc.run(4);
    IUTEST_ASSERT_EQ((std::vector{0,1,2,3,4,5,6,7,8,9}), tmp);
}


IUTEST(BasicChannel, SenderRecver1){
    std::vector<int> tmp;
    {
        auto [sender, recver] = cfn::makeChannel<MyScheduler, int>(gsc,0);
        for(int i=0;i<10;++i){
            gsc.spown_task(
                [](auto sender) -> cfn::Task<> {
                    for(int i=0;i<10;++i){
                        co_await sender->send(i);
                    }
                }(sender)
            );
        }

        gsc.spown_task(
            [&](auto recver) -> cfn::Task<> {
                for(;;){
                    auto r = co_await recver->recv();
                    if(!r)break;
                    tmp.push_back(*r);
                }
                gsc.stop();
            }(recver)
        );
    }
    gsc.run(4);

    std::sort(tmp.begin(), tmp.end());

    std::vector<int> expected;
    for(int i=0;i<10;++i){
        for(int k=0;k<10;++k){
            expected.push_back(i);
        }
    }
    IUTEST_ASSERT_EQ(expected, tmp);
}

#include<boost/asio.hpp>

struct BoostThreadpoolScheduler {
    boost::asio::io_service io_service_;
    std::shared_ptr<boost::asio::io_service::work> work_ = std::make_shared<boost::asio::io_service::work>(boost::asio::io_service::work(io_service_));

    void spown_task(cfn::Task<>&&task){
        auto g = std::make_shared<cfn::Goroutine>(std::move(task));
        io_service_.post([=]()mutable{g->execute(); });
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
static BoostThreadpoolScheduler gbts;
struct BoostThreadpoolSchedulerWrapper{
    BoostThreadpoolScheduler & ref_;
    void push_goroutine(std::shared_ptr<cfn::Goroutine> && g){
        ref_.io_service_.post([=]()mutable{g->execute(); });
    }
};

template<class value_type>
using MyChannel2 = cfn::BasicChannel<BoostThreadpoolSchedulerWrapper, value_type>;

IUTEST(WithBoostAsio, SenderRecver1){
    std::vector<int> tmp;
    {
        auto [sender, recver] = cfn::makeChannel<BoostThreadpoolSchedulerWrapper, int>(BoostThreadpoolSchedulerWrapper{gbts}, 0);
        for(int i=0;i<10;++i){
            gbts.spown_task(
                [](auto sender) -> cfn::Task<> {
                    for(int i=0;i<10;++i){
                        co_await sender->send(i);
                    }
                }(sender)
            );
        }

        gbts.spown_task(
            [&](auto recver) -> cfn::Task<> {
                for(;;){
                    auto r = co_await recver->recv();
                    if(!r)break;
                    tmp.push_back(*r);
                }
                gbts.stop();
            }(recver)
        );
    }
    gbts.run(4);

    std::sort(tmp.begin(), tmp.end());

    std::vector<int> expected;
    for(int i=0;i<10;++i){
        for(int k=0;k<10;++k){
            expected.push_back(i);
        }
    }
    IUTEST_ASSERT_EQ(expected, tmp);
}


IUTEST(Example, C100k){
    std::vector<int> tmp;
    {
        auto [sender, recver] = cfn::makeChannel<BoostThreadpoolSchedulerWrapper, int>(BoostThreadpoolSchedulerWrapper{gbts}, 0);
        gbts.spown_task(
            [](auto sender) -> cfn::Task<> {
                for(int i=0;i<10;++i){
                    co_await sender->send(i);
                }
            }(sender)
        );
        auto old_recver = recver;
        for(int i=0;i<100000;++i){
            auto [new_sender, new_recver] = cfn::makeChannel<BoostThreadpoolSchedulerWrapper, int>(BoostThreadpoolSchedulerWrapper{gbts}, 0);
            gbts.spown_task(
                [](auto recver, auto sender) -> cfn::Task<> {
                    for(;;){
                        auto r = co_await recver->recv();
                        if (!r)break;
                        co_await sender->send(*r+1);
                    }
                }(old_recver, new_sender)
            );
            old_recver = new_recver;
        }
        gbts.spown_task(
            [&](auto recver) -> cfn::Task<> {
                for(;;){
                    auto r = co_await recver->recv();
                    if (!r)break;
                    tmp.push_back(*r);
                }
            }(old_recver));
    }
    gbts.run(4);
    std::vector<int> expected =  { 100000, 100001, 100002, 100003, 100004, 100005, 100006, 100007, 100008, 100009 };


    IUTEST_ASSERT_EQ(expected, tmp);
}
