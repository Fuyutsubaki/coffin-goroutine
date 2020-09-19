#define IUTEST_USE_MAIN 1
#include <coffin/goroutine.hpp>
#include <iutest.hpp>
#include <thread>
#include<vector>
#include<array>

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
        auto g = std::make_shared<cfn::Goroutine>(std::move(task));
        queue.push_back(g);
    }
    void push_goroutine(std::shared_ptr<cfn::Goroutine> g, std::size_t wakeup_id){
        std::scoped_lock lock{mutex};
        g->wakeup_id = wakeup_id;
        queue.push_back(g);
    }
    std::shared_ptr<cfn::Goroutine> dequeue_task(){
        std::scoped_lock lock{mutex};
        if(queue.empty()){
            return nullptr;
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
        for(int i=0;i<3;++i){
            std::thread th{[&]{
            while(on_ready.load()){
                auto task = MyScheduler::dequeue_task();
                if(task){
                    task->execute();
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
    for(int i=0;i<10;++i){
        std::vector<int> tmp;
        auto ch = make_channel<int>(4);
        gsc.spown_task(
            [&]() -> cfn::Task<> {
                for(int i=0;i<10;++i){
                    co_await ch.send(i);
                }
                ch.close();
            }()
        );
        gsc.spown_task(
            [&]() -> cfn::Task<> {
                for(;;){
                    auto r = co_await ch.recv();
                    if(!r)break;
                    tmp.push_back(*r);
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
            bool done = false;
            while (!done){
                co_await cfn::select(
                    done_ch.recv([&](auto) {
                        done = true;
                    }),
                    ch.recv([&](auto x) {
                        tmp.push_back(*x);
                    }));
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

/*
cfn::Task<> send1(MyChannel<int>&ch){
    for(int i=0;i<3;++i){
        co_await ch.send(i);
    }
}

cfn::Task<> send2(MyChannel<int>&ch, MyChannel<int>&done_ch){
    co_await send1(ch);
    co_await send1(ch);
    co_await send1(ch);

    done_ch.close();
}

cfn::Task<> recv(MyChannel<int>&ch, MyChannel<int>&done_ch, std::atomic<bool> & all_done, std::vector<int>&tmp){
    int  done = false;
    while(!done){
        co_await cfn::select(
            done_ch.recv([&](auto){
                done = true;
            }),
            ch.recv([&](auto x){
                tmp.push_back(*x);
            })
        );
    }
    all_done.store(true);
}

// move only type
cfn::Task<> send_move(MyChannel<std::unique_ptr<int>>&ch){
    co_await ch.send(std::make_unique<int>(32));
}

cfn::Task<> recv_move(MyChannel<int>&ch){
    auto x = co_await ch.recv();
}


IUTEST(test_common, Ch)
{
    MyChannel<int> ch(MyScheduler{},0);
    MyChannel<int> done_ch(MyScheduler{},0);
    std::atomic<bool> all_done = false;
    std::vector<int> tmp;

    MyScheduler::spown_task(send2(ch, done_ch));
    MyScheduler::spown_task(recv(ch, done_ch, all_done, tmp));
    
    // ガバガバスレッドプール
    std::vector<std::thread> worker;
    for(int i=0;i<3;++i){
        std::thread th{[&]{
           while(!all_done.load()){
               auto task = MyScheduler::dequeue_task();
               if(task){
                   task->execute();
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
    assert(tmp == (std::vector{0,1,2,0,1,2,0,1,2})); // 本当に？
}
*/