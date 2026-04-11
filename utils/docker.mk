DOCKER_IMAGE ?= apheleia:latest

.PHONY: docker_image
docker_image:
	docker build utils -t "$(DOCKER_IMAGE)"

.PHONY: docker_build
docker_build: docker_image
	@user_args=""; \
	if uid=$$(id -u 2>/dev/null) && gid=$$(id -g 2>/dev/null); then \
		user_args="--user $$uid:$$gid"; \
	fi; \
	docker run --rm $$user_args \
		-v "$(CURDIR)":/usr/src/apheleia \
		-w /usr/src/apheleia \
		"$(DOCKER_IMAGE)" $(MAKE) ARCH=$(ARCH) TOOLCHAIN=$(TOOLCHAIN) \
		IMAGE_FORMAT=$(IMAGE_FORMAT) PROFILE=$(PROFILE) \
		GNU_CC_x86_64=gcc \
		GNU_CC_x86_32=gcc \
		GNU_LD_x86_32=ld \
		GNU_OC_x86_32=objcopy \
		GNU_ST_x86_32=strip \
		GNU_CC_riscv_64=riscv-none-elf-gcc \
		GNU_LD_riscv_64=riscv-none-elf-ld \
		GNU_OC_riscv_64=riscv-none-elf-objcopy \
		GNU_ST_riscv_64=riscv-none-elf-strip \
		GNU_CC_riscv_32=riscv-none-elf-gcc \
		GNU_LD_riscv_32=riscv-none-elf-ld \
		GNU_OC_riscv_32=riscv-none-elf-objcopy \
		GNU_ST_riscv_32=riscv-none-elf-strip \
		LLVM_CC_x86_64=clang-18 \
		LLVM_CC_x86_32=clang-18 \
		LLVM_CC_riscv_64="clang-18 --target=riscv64-unknown-elf" \
		LLVM_CC_riscv_32="clang-18 --target=riscv32-unknown-elf" \
		LLVM_OC_x86_64=llvm18-objcopy \
		LLVM_OC_x86_32=llvm18-objcopy \
		LLVM_OC_riscv_64=llvm18-objcopy \
		LLVM_OC_riscv_32=llvm18-objcopy \
		LLVM_ST_x86_64=llvm18-strip \
		LLVM_ST_x86_32=llvm18-strip \
		LLVM_ST_riscv_64=llvm18-strip \
		LLVM_ST_riscv_32=llvm18-strip
