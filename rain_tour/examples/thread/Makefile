CXX ?= c++
CXXFLAGS ?= -std=c++23 -Wall -Wextra -O0 -g -pthread

PROGRAMS := thread_pool_futures

.PHONY: all clean run-pool-futures

all: $(PROGRAMS)

%: %.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

run-pool-futures: thread_pool_futures
	./thread_pool_futures

clean:
	rm -f $(PROGRAMS)
