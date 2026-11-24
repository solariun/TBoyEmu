# TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for the terminal.
#
#   make          build ./tboyemu
#   make clean
#
# Needs the Graphicx library (terminal graphics) in external/Graphicx, a git
# submodule.  Clone with:  git clone --recursive https://github.com/solariun/TBoyEmu
# or, in an existing clone:  git submodule update --init

# Graphicx: only its core (half blocks / ASCII rendering) is compiled in.
GRAPHICX := external/Graphicx
ifeq ($(wildcard $(GRAPHICX)/src/graphicx.cpp),)
  $(error Graphicx is missing: run "git submodule update --init" (or clone with --recursive))
endif

CXX      ?= c++
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++11 -Wall -Wextra -Wpedantic -MMD -MP -I$(GRAPHICX)/include
LDLIBS   += -lz

# OS keyboard state backends (see src/frontend/NativeKeyboard.cpp)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  LDLIBS += -framework ApplicationServices -framework IOKit -framework CoreFoundation
endif
ifeq ($(UNAME_S),Linux)
  LDLIBS += -ldl
endif

TARGET := tboyemu
SRCS   := $(wildcard src/core/*.cpp) $(wildcard src/gba/*.cpp) $(wildcard src/util/*.cpp) \
          $(wildcard src/frontend/*.cpp) src/MachineFactory.cpp src/main.cpp
OBJS   := $(SRCS:src/%.cpp=build/%.o) build/graphicx/graphicx.o
DEPS   := $(OBJS:.o=.d)

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@

build/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/graphicx/%.o: $(GRAPHICX)/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf build $(TARGET)

-include $(DEPS)
