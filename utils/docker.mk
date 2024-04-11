.PHONY: docker_image
docker_image:
	docker build utils -t apheleia

.PHONY: docker_build
docker_build:
	docker run -it \
		-u `stat -c "%u:%g" .` \
		-v "$$PWD":/usr/src/apheleia \
		apheleia:latest $(MAKE)
