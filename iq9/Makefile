# IQ9 CLI: plain GNU toolchain + the SDK in ./sdk, nothing else.
#   cross build on an x86 host:  make                      (uses aarch64-linux-gnu-g++)
#   native build on the board:   make CXX=g++
# Run:  ./out/lfm3_cli --model <model dir> "prompt"   (the binary finds sdk/lib through its rpath)
ifeq ($(origin CXX),default)
  ifneq ($(shell uname -m),aarch64)
    CXX := aarch64-linux-gnu-g++
  endif
endif
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
SDK := sdk

all: out/lfm3_cli out/d1_example

out/lfm3_cli: src/main.cpp $(SDK)/include/zetic_llm.h $(SDK)/lib/libzetic_mlange_hexagon.so
	@mkdir -p out
	$(CXX) $(CXXFLAGS) -I$(SDK)/include $< -o $@ -L$(SDK)/lib -lzetic_mlange_hexagon \
	  -Wl,-rpath,'$$ORIGIN/../$(SDK)/lib' -Wl,--allow-shlib-undefined

out/d1_example: src/d1_example.cpp $(SDK)/include/zetic_decision.h $(SDK)/lib/libzetic_mlange_hexagon.so
	@mkdir -p out
	$(CXX) $(CXXFLAGS) -I$(SDK)/include $< -o $@ -L$(SDK)/lib -lzetic_mlange_hexagon \
	  -Wl,-rpath,'$$ORIGIN/../$(SDK)/lib' -Wl,--allow-shlib-undefined

clean:
	rm -rf out
