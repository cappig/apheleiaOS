DOCKER_IMAGE ?= apheleia:latest
DOCKER_PLATFORM ?= linux/amd64

docker_arg = $(if $(filter command line environment override,$(origin $(1))),$(1)=$($(1)),)

DOCKER_BUILD_VARS := \
	ARCH \
	TOOLCHAIN \
	IMAGE_FORMAT \
	PROFILE \
	USERLAND \
	SOURCE_DATE_EPOCH \
	BUILD_DATE \
	GIT_COMMIT_SHORT \
	TRACEABLE_KERNEL \
	BOOT_LOG_COLOR \
	STRIP_KERNEL \
	STRIP_USER \
	STRIP_USER_SYMBOLS \
	ROOTFS_EXTRA_BYTES \
	STRIP_KERNEL_FLAGS \
	USER_STRIP_FLAGS \
	GCC_ANALYZER \
	$(DOCKER_ARCH_VARS)

.PHONY: docker_image
docker_image:
	docker build --platform "$(DOCKER_PLATFORM)" utils -t "$(DOCKER_IMAGE)"

.PHONY: docker_build
docker_build: docker_image utils/docker_build.sh
	@utils/docker_build.sh "$(DOCKER_IMAGE)" "$(DOCKER_PLATFORM)" \
		"$(CURDIR)" "make" \
		$(foreach var,$(DOCKER_BUILD_VARS),$(if $(call docker_arg,$(var)),"$(call docker_arg,$(var))"))
