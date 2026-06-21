DOCKER_IMAGE ?= apheleia:latest
DOCKER_PLATFORM ?= linux/amd64

docker_arg = $(if $(filter command line environment override,$(origin $(1))),$($(1)),)

.PHONY: docker_image
docker_image:
	docker build --platform "$(DOCKER_PLATFORM)" utils -t "$(DOCKER_IMAGE)"

.PHONY: docker_build
docker_build: docker_image utils/docker_build.sh
	@utils/docker_build.sh "$(DOCKER_IMAGE)" "$(DOCKER_PLATFORM)" \
		"$(CURDIR)" "make" \
		"$(call docker_arg,ARCH)" "$(call docker_arg,TOOLCHAIN)" \
		"$(call docker_arg,IMAGE_FORMAT)" "$(call docker_arg,PROFILE)" \
		"$(call docker_arg,RISCV_FRISC)" \
		"$(call docker_arg,RISCV_UART_STRIDE)" \
		"$(call docker_arg,USERLAND)" "$(call docker_arg,BUILD_DATE)" \
		"$(call docker_arg,GIT_COMMIT_SHORT)" \
		"$(call docker_arg,TRACEABLE_KERNEL)" \
		"$(call docker_arg,BOOT_LOG_COLOR)" \
		"$(call docker_arg,STRIP_KERNEL)" "$(call docker_arg,STRIP_USER)" \
		"$(call docker_arg,STRIP_USER_SYMBOLS)" \
		"$(call docker_arg,ROOTFS_EXTRA_BYTES)" \
		"$(call docker_arg,STRIP_KERNEL_FLAGS)" \
		"$(call docker_arg,USER_STRIP_FLAGS)" \
		"$(call docker_arg,GCC_ANALYZER)" "$(call docker_arg,RISCV_UART0)" \
		"$(call docker_arg,RISCV_SCRATCH_OFFSET)"
