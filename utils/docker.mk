DOCKER_IMAGE ?= apheleia:latest

.PHONY: docker_image
docker_image:
	docker build utils -t "$(DOCKER_IMAGE)"

.PHONY: docker_build
docker_build: docker_image utils/docker_build.sh
	@utils/docker_build.sh "$(DOCKER_IMAGE)" "$(CURDIR)" "make" \
		"$(ARCH)" "$(TOOLCHAIN)" "$(IMAGE_FORMAT)" "$(PROFILE)" \
		"$(RISCV_FRISC)" "$(RISCV_UART_STRIDE)" "$(USERLAND)"
