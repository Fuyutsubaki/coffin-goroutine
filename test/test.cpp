//#include <gtest/gtest.h>
#include <coffin/goroutine.hpp>
#include <thread>
#include<vector>

struct MyScheduler{
    static inline std::deque<std::shared_ptr<cfn::Goroutine>> queue;
    static inline std::mutex mutex;
    static void spown_task(cfn::Task<>&&task){
        std::scoped_lock lock{mutex};
        auto g = std::make_shared<cfn::Goroutine>(std::move(task));
        queue.push_back(g);
    }
    void push_goroutine(std::shared_ptr<cfn::Goroutine> g, std::size_t wakeup_id){
        std::scoped_lock lock{mutex};
        g->wakeup_id = wakeup_id;
        queue.push_back(g);
    }
    static std::shared_ptr<cfn::Goroutine> dequeue_task(){
        std::scoped_lock lock{mutex};
        if(queue.empty()){
            return nullptr;
        }
        auto p = queue.front();
        queue.pop_front();
        return p;
    }
};

template<class value_type>
using MyChannel = cfn::BasicChannel<MyScheduler, value_type>;

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


//TEST(test_common, Ch)
int main()
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
