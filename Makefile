CXX       := g++
CXXFLAGS  := -std=c++20 -Wall -Wextra -Wpedantic -Iinclude
RELEASE   := -O3 -march=native -DNDEBUG -flto -funroll-loops
DEBUG     := -g -O0 -fsanitize=address,undefined
LDFLAGS   := -lpthread

SRCDIR    := src
BENCHDIR  := bench
BUILDDIR  := build

.PHONY: all release debug clean demo bench pgo

all: release

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# --- Release ---
release: CXXFLAGS += $(RELEASE)
release: $(BUILDDIR) $(BUILDDIR)/ome_demo $(BUILDDIR)/ome_bench

# --- Debug ---
debug: CXXFLAGS += $(DEBUG)
debug: $(BUILDDIR) $(BUILDDIR)/ome_demo $(BUILDDIR)/ome_bench

$(BUILDDIR)/order_book.o: $(SRCDIR)/order_book.cpp include/order_book.hpp include/types.hpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILDDIR)/matching_engine.o: $(SRCDIR)/matching_engine.cpp include/matching_engine.hpp include/order_book.hpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILDDIR)/ome_demo: $(SRCDIR)/main.cpp $(BUILDDIR)/order_book.o $(BUILDDIR)/matching_engine.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILDDIR)/ome_bench: $(BENCHDIR)/benchmark.cpp $(BUILDDIR)/order_book.o $(BUILDDIR)/matching_engine.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

demo: release
	./$(BUILDDIR)/ome_demo

bench: release
	./$(BUILDDIR)/ome_bench

# --- PGO (Profile-Guided Optimization) ---
pgo: $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(RELEASE) -fprofile-generate \
		$(BENCHDIR)/benchmark.cpp $(SRCDIR)/order_book.cpp $(SRCDIR)/matching_engine.cpp \
		-o $(BUILDDIR)/ome_bench $(LDFLAGS)
	./$(BUILDDIR)/ome_bench > /dev/null 2>&1 || true
	$(CXX) $(CXXFLAGS) $(RELEASE) -fprofile-use -fprofile-correction \
		$(BENCHDIR)/benchmark.cpp $(SRCDIR)/order_book.cpp $(SRCDIR)/matching_engine.cpp \
		-o $(BUILDDIR)/ome_bench $(LDFLAGS)
	@echo "PGO build complete → $(BUILDDIR)/ome_bench"

clean:
	rm -rf $(BUILDDIR) *.gcda *.gcno
