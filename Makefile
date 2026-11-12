#=============================================================================
# Makefile - 16-Bit OMF Object/Library Manipulation Library & Tools (Unix)
#
# Usage Examples:
#   make                                   (Defaults: c++, RELEASE)
#   make BUILD=DEBUG                       (Unoptimized build with -g3)
#   make CXX=clang++ BUILD=DEBUG SAN=1     (Clang debug build + ASan/UBSan)
#   make clean
#=============================================================================

# --- User-Configurable Build Options ---
CXX    ?= c++
AR     ?= ar
BUILD  ?= RELEASE
SAN    ?= 0

OBJDIR  = obj
LIBDIR  = lib
BINDIR  = bin

# --- 1. Base Compiler & Archiver Flags ---
CXXFLAGS_BASE = -std=gnu++20 -Wall -Wextra -fPIC -MMD -MP
LDFLAGS_BASE  =
ARFLAGS       = rcs

# --- 2. Map Build Configuration (RELEASE vs DEBUG) ---
ifeq ($(BUILD),RELEASE)
	CXXFLAGS_BUILD = -O2 -DNDEBUG
	LDFLAGS_BUILD  =
else ifeq ($(BUILD),DEBUG)
	CXXFLAGS_BUILD = -O0 -g3 -DDEBUG
	LDFLAGS_BUILD  =
else
	$(error Invalid BUILD="$(BUILD)". Choose RELEASE or DEBUG)
endif

# --- 3. Optional Address/Undefined-Behavior Sanitizers (SAN=1) ---
ifeq ($(SAN),1)
	SAN_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer
else
	SAN_FLAGS =
endif

CXXFLAGS := $(CXXFLAGS_BASE) $(CXXFLAGS_BUILD) $(SAN_FLAGS) $(CXXFLAGS)
LDFLAGS  := $(LDFLAGS_BASE) $(LDFLAGS_BUILD) $(SAN_FLAGS) $(LDFLAGS)

# --- 4. Targets & Object Files ---
LIB_STATIC  = $(LIBDIR)/libomf.a
LIB_SHARED  = $(LIBDIR)/libomf.so
LIB_TARGETS = $(LIB_STATIC) $(LIB_SHARED)
LIB_OBJS    = $(OBJDIR)/OmfFile.o

BIN_TARGETS = $(BINDIR)/omfdump $(BINDIR)/omfedit $(BINDIR)/omfexplode
ALL_OBJS    = $(LIB_OBJS) $(OBJDIR)/OmfDump.o $(OBJDIR)/OmfEdit.o $(OBJDIR)/OmfExplode.o
DEPS        = $(ALL_OBJS:.o=.d)

# Automatically include omfedit if OmfEdit.cpp exists in the directory
ifneq ($(wildcard OmfEdit.cpp),)
	BIN_TARGETS += $(BINDIR)/omfedit
endif

# --- 5. Build Rules ---
all: info dirs $(LIB_TARGETS) $(BIN_TARGETS)

info:
	@echo "Building 16-bit OMF library & tools"
	@echo "Compiler:   $(CXX)"
	@echo "Build mode: $(BUILD)"

dirs: $(OBJDIR) $(LIBDIR) $(BINDIR)

$(OBJDIR) $(LIBDIR) $(BINDIR):
	@mkdir -p $@

$(LIB_STATIC): $(LIB_OBJS) | $(LIBDIR)
	$(AR) $(ARFLAGS) $@ $^

$(LIB_SHARED): $(LIB_OBJS) | $(LIBDIR)
	$(CXX) $(LDFLAGS) -shared -o $@ $^

$(BINDIR)/omfdump: $(OBJDIR)/OmfDump.o $(LIB_STATIC) | $(BINDIR)
	$(CXX) $(LDFLAGS) -o $@ $^

$(BINDIR)/omfexplode: $(OBJDIR)/OmfExplode.o $(LIB_STATIC) | $(BINDIR)
	$(CXX) $(LDFLAGS) -o $@ $^

$(BINDIR)/omfedit: $(OBJDIR)/OmfEdit.o $(LIB_STATIC) | $(BINDIR)
	$(CXX) $(LDFLAGS) -o $@ $^

$(OBJDIR)/%.o: %.cpp | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJDIR)/*.o $(OBJDIR)/*.d
	rm -f $(LIBDIR)/*.a $(LIBDIR)/*.so
	rm -f $(BINDIR)/omfdump $(BINDIR)/omfexplode $(BINDIR)/omfedit
	rm -f *.o *.d *.a *.so omfdump omfexplode omfedit
	@if [ -d $(OBJDIR) ]; then rmdir --ignore-fail-on-non-empty $(OBJDIR) 2>/dev/null || rmdir $(OBJDIR) 2>/dev/null || true; fi
	@if [ -d $(LIBDIR) ]; then rmdir --ignore-fail-on-non-empty $(LIBDIR) 2>/dev/null || rmdir $(LIBDIR) 2>/dev/null || true; fi
	@if [ -d $(BINDIR) ]; then rmdir --ignore-fail-on-non-empty $(BINDIR) 2>/dev/null || rmdir $(BINDIR) 2>/dev/null || true; fi

-include $(DEPS)

.PHONY: all info dirs clean
