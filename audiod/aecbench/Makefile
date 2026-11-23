# SPDX-License-Identifier: AGPL-3.0-or-later
# Echo canceller comparison (issue #35, docs/aec.md). Not part of the phone's
# image: it fetches the candidates (./fetch.sh) and builds two programs.
#   make                    this PC: build/aeccpu, build/aecq
#   make quality            the quality tables (far-end echo, both talking, ...)
#   make CC=$$HOME/src-build/t48s/bin/armcc O=build-arm build-arm/aeccpu
#                           the CPU bench for the phone: copy to /tmp and run
O ?= build
SRC ?= build/src
CC ?= cc
CFLAGS ?= -O2
SX = $(SRC)/speexdsp
WAP = $(SRC)/wap
SPL = $(WAP)/webrtc/common_audio/signal_processing
AP = $(WAP)/webrtc/modules/audio_processing

# speexdsp in fixed point with its own integer FFT; SPEEX_ASM adds flags such
# as -DARM5E_ASM
SX_FLAGS = -DFIXED_POINT -DUSE_KISS_FFT -DEXPORT= $(SPEEX_ASM)
# the canceller is the copy audiod ships (the same upstream commit, plus the
# status call its "echo aec" needs); the preprocessor is only compared here
SPX = ../third_party/speexdsp
SX_SRCS = $(addprefix $(SPX)/, mdf.c kiss_fft.c kiss_fftr.c fftwrap.c) \
	$(addprefix $(SX)/libspeexdsp/, preprocess.c filterbank.c)
AECM_SRCS = $(AP)/aecm/aecm_core.c $(AP)/aecm/aecm_core_c.c $(AP)/aecm/echo_control_mobile.c \
	$(AP)/utility/delay_estimator.c $(AP)/utility/delay_estimator_wrapper.c \
	$(WAP)/webrtc/common_audio/ring_buffer.c \
	$(addprefix $(SPL)/, real_fft.c complex_fft.c complex_bit_reverse.c division_operations.c \
	spl_sqrt.c spl_sqrt_floor.c min_max_operations.c randomization_functions.c spl_init.c \
	cross_correlation.c downsample_fast.c vector_scaling_operations.c energy.c get_scaling_square.c)
OURS = ../echo.c ../aec.c ../dsp.c
INC = -I$(SPX) -I$(SPX)/speex -I$(SX)/include -I$(SX)/libspeexdsp -I$(WAP) -DWEBRTC_POSIX
ALL = -w $(CFLAGS) -std=gnu99 -D_GNU_SOURCE $(INC) $(SX_FLAGS)

all: $(O)/aeccpu $(O)/aecq

$(SX)/libspeexdsp/mdf.c:
	./fetch.sh $(SRC)

# no auto-vectorising, as ../Makefile's bench: the phone has no vector unit
$(O)/aeccpu: cpu.c cands.c cand.h $(OURS) $(SX)/libspeexdsp/mdf.c
	@mkdir -p $(O)
	$(CC) $(ALL) -fno-tree-vectorize -o $@ cpu.c cands.c $(OURS) $(SX_SRCS) $(AECM_SRCS) -lm -static

$(O)/aecq: quality.c cands.c cand.h $(OURS) $(SX)/libspeexdsp/mdf.c
	@mkdir -p $(O)
	$(CC) $(ALL) -o $@ quality.c cands.c $(OURS) $(SX_SRCS) $(AECM_SRCS) -lm

quality: $(O)/aecq
	$(O)/aecq $(SRC)/speech

clean:
	rm -rf build/aeccpu build/aecq build-arm

.PHONY: all quality clean
