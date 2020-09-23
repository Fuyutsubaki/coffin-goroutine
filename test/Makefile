iutest/include/iutest.hpp:
	git clone -b v1.17.1 --depth 1 https://github.com/srz-zumix/iutest.git 

.PHONY:test
test: iutest/include/iutest.hpp
	clang++-10 test.cpp -I iutest/include/ -I ../include/ -lpthread -lboost_system  -std=c++2a -stdlib=libc++ -g -o test
	./test