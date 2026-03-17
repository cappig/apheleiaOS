.PHONY: docker_image
docker_image:
	docker build utils -t apheleia

.PHONY: docker_build
docker_build:
	docker run \
		-u `stat -c "%u:%g" .` \
		-v "$$PWD":/usr/src/apheleia \
		apheleia:latest $(MAKE) ARCH=$(ARCH) TOOLCHAIN=$(TOOLCHAIN) \
		IMAGE_FORMAT=$(IMAGE_FORMAT) PROFILE=$(PROFILE) \
		GNU_CC_x86_64=gcc GNU_CC_x86_32=gcc
