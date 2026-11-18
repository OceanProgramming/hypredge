# hypredge — build with plain make (no cmake/hyprpm needed).
#
#   make          build hypredge.so against the installed Hyprland headers
#   make test     build and run the geometry tests (no Hyprland needed)
#   make load     (re)load the freshly built plugin into the running Hyprland
#   make unload   unload it again (and delete the copy it was loaded from)
#   make clean    remove build output

CXX      ?= g++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++23 -fPIC -Wall -Wextra

PLUGIN_PKGS := hyprland pixman-1 libdrm libeis-1.0
PLUGIN_CFLAGS := $(shell pkg-config --cflags $(PLUGIN_PKGS))
TEST_CFLAGS   := $(shell pkg-config --cflags hyprutils)
TEST_LIBS     := $(shell pkg-config --libs hyprutils)

BUILD  := build
PLUGIN := $(BUILD)/hypredge.so
TESTS  := $(BUILD)/geometry_test

.PHONY: all test load unload clean

all: $(PLUGIN)

$(PLUGIN): src/main.cpp src/geometry.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(PLUGIN_CFLAGS) -shared src/main.cpp -o $@

$(TESTS): tests/geometry_test.cpp src/geometry.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(TEST_CFLAGS) tests/geometry_test.cpp $(TEST_LIBS) -o $@

test: $(TESTS)
	./$(TESTS)

# The dynamic loader reuses a library that is already loaded from the same
# path, so each load uses a uniquely named copy; build/loaded.path remembers it.
load: $(PLUGIN) unload
	@mkdir -p $(BUILD)/loaded
	@copy="$(abspath $(BUILD))/loaded/hypredge-$$(date +%s%N).so"; \
	 cp "$(PLUGIN)" "$$copy" && echo "$$copy" > $(BUILD)/loaded.path && \
	 echo "loading $$copy" && hyprctl plugin load "$$copy"

unload:
	@hyprctl plugin list | grep -q '^Plugin hypredge ' || exit 0; \
	 if [ -f $(BUILD)/loaded.path ]; then p=$$(cat $(BUILD)/loaded.path); \
	 else p="$(abspath $(PLUGIN))"; fi; \
	 echo "unloading $$p" && hyprctl plugin unload "$$p"; \
	 case "$$p" in "$(abspath $(BUILD))/loaded/"*) rm -f "$$p";; esac; \
	 rm -f $(BUILD)/loaded.path

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
