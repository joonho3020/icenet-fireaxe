RISCV_PFX = riscv64-unknown-elf-
RISCV_CXX = $(RISCV_PFX)gcc
RISCV_OBJDUMP = $(RISCV_PFX)objdump

RISCV_CFLAGS = -fno-common -fno-builtin-printf --no-builtin-rules -specs=htif_nano.specs -Wall
LD_FLAGS = -static -specs=htif_nano.specs

# .DEFAULT_GOAL := default
# $(error $(RISCV_CXX))

.PHONY: default
default: nic-loopback.riscv nic-loopback.dump \
	nic-loopback-rss.riscv nic-loopback-rss.dump \
	traffic-gen.riscv traffic-gen.dump \
	traffic-recv.riscv traffic-recv.dump \
	dummy.riscv

nic-loopback.o: nic-loopback.c nic.h nic-rss.h mmio.h marchid.h encoding.h
	$(RISCV_CXX) $(RISCV_CFLAGS) -o $@ -c $<

nic-loopback-rss.o: nic-loopback-rss.c nic.h nic-rss.h mmio.h marchid.h encoding.h
	$(RISCV_CXX) $(RISCV_CFLAGS) -o $@ -c $<

traffic-gen.o: traffic-gen.c nic-rss.h mmio.h marchid.h encoding.h
	$(RISCV_CXX) $(RISCV_CFLAGS) -o $@ -c $<

traffic-recv.o: traffic-recv.c nic-rss.h mmio.h marchid.h encoding.h
	$(RISCV_CXX) $(RISCV_CFLAGS) -o $@ -c $<

dummy.o: dummy.c
	$(RISCV_CXX) $(RISCV_CFLAGS) -o $@ -c $<

%.riscv: %.o
	$(RISCV_CXX) $(LD_FLAGS) $^ -o $@

%.dump: %.riscv
	$(RISCV_OBJDUMP) -D $< > $@

.PHONY: clean
clean:
	rm -f *.riscv *.o *.dump
