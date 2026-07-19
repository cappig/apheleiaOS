.PHONY: run-spike

$(SPIKE_MMIO_UART_PLUGIN): utils/arch/riscv/spike/mmio_uart_plugin.cpp
	@mkdir -p "$(dir $@)"
	@if ! command -v "$(SPIKE_MMIO_UART_CXX)" >/dev/null 2>&1; then \
		echo "Missing host C++ compiler '$(SPIKE_MMIO_UART_CXX)' for Spike MMIO UART plugin build."; \
		echo "Install one (e.g. g++ or clang++) or override with SPIKE_MMIO_UART_CXX=<compiler>."; \
		exit 1; \
	fi
	@printf "%-3s  %s\n" "CXX" "$<"
	@"$(SPIKE_MMIO_UART_CXX)" -std=c++17 -O2 -fPIC -shared -Wall -Wextra -Wpedantic \
		-I/usr/include \
		-o "$@" "$<"

run-spike: all $(SPIKE_MMIO_UART_DEPS)
	@if ! command -v "$(SPIKE)" >/dev/null 2>&1; then \
		echo "spike was not found in PATH (override with SPIKE=/path/to/spike)"; \
		exit 1; \
	fi
	@if [ ! -f "$(SPIKE_INITRD)" ]; then \
		echo "Spike initrd image not found: $(SPIKE_INITRD)"; \
		echo "Build it with: make ARCH=$(ARCH) TOOLCHAIN=$(TOOLCHAIN)"; \
		exit 1; \
	fi
	@if [ "$(SPIKE_MMIO_UART_ENABLED)" = "true" ] && [ ! -f "$(SPIKE_MMIO_UART_PLUGIN)" ]; then \
		echo "Spike MMIO UART plugin not found: $(SPIKE_MMIO_UART_PLUGIN)"; \
		exit 1; \
	fi
	@echo "Launching Spike with $(BOOT_ENTRY_ELF)"
	@echo "Using initrd rootfs: $(SPIKE_INITRD)"
	@if [ "$(SPIKE_MMIO_UART_ENABLED)" = "true" ]; then \
		echo "Spike MMIO UART: plugin=$(SPIKE_MMIO_UART_PLUGIN) device=$(SPIKE_MMIO_UART_DEVICE) base=$(SPIKE_MMIO_UART_BASE) args=$(SPIKE_MMIO_UART_ARGS)"; \
	else \
		echo "Spike MMIO UART disabled (SPIKE_MMIO_UART=false); kernel will run without UART console if UART base is 0."; \
	fi
	@if [ "$(SPIKE_RT_CLINT_ON)" = "true" ]; then \
		echo "Spike CLINT time: real-time (--real-time-clint)"; \
	fi
	@if [ "$(SPIKE_MMIO_UART_ARGS)" = "tty" ]; then \
		$(SPIKE) --isa=$(SPIKE_ISA) -m$(SPIKE_RAM_MB) $(SPIKE_MMIO_UART_FLAGS) $(SPIKE_CLINT_FLAGS) --initrd="$(SPIKE_INITRD)" "$(BOOT_ENTRY_ELF)" </dev/null; \
	else \
		$(SPIKE) --isa=$(SPIKE_ISA) -m$(SPIKE_RAM_MB) $(SPIKE_MMIO_UART_FLAGS) $(SPIKE_CLINT_FLAGS) --initrd="$(SPIKE_INITRD)" "$(BOOT_ENTRY_ELF)"; \
	fi
