#include<boost/asio.hpp>
#include<thread>

namespace app{

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
}
