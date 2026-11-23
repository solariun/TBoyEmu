CXX      ?= c++
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++11 -Wall -Wextra -Wpedantic -MMD -MP
LDLIBS   += -lz

# OS keyboard state backends (see src/frontend/NativeKeyboard.cpp)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  LDLIBS += -framework ApplicationServices
endif
ifeq ($(UNAME_S),Linux)
  LDLIBS += -ldl
endif

TARGET := tboyemu
SRCS   := $(wildcard src/core/*.cpp) $(wildcard src/gba/*.cpp) $(wildcard src/util/*.cpp) \
          $(wildcard src/frontend/*.cpp) src/MachineFactory.cpp src/main.cpp
OBJS   := $(SRCS:src/%.cpp=build/%.o)
DEPS   := $(OBJS:.o=.d)

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@

build/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf build $(TARGET)

-include $(DEPS)
